#include "platform_handler.h"

#include "dashboard_assets.h"
#include "http_response.h"
#include "participant_projection.h"
#include "radar_api_server.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#endif

#include <algorithm>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace esphome {
namespace radar_api_server {

struct PlatformParticipantConfigureScratch {
  ParticipantReadinessConfig config{};
  uint32_t max_targets{0};
  uint32_t geometry_version{0};
  uint32_t local_pose_version{0};
  uint32_t pose_version{0};
  uint32_t calibration_version{0};
  int32_t origin_x_mm{0};
  int32_t origin_y_mm{0};
  int32_t rotation_cdeg{0};
  uint32_t residual_mm{0};
  uint32_t calibration_samples{0};
  bool point_tracks{false};
  bool velocity{false};
  bool uncertainty{false};
  bool range{false};
  bool presence{false};
  bool motion{false};
  bool zone_presence{false};
  bool bridge{false};
  bool automatic_group{false};
};

static_assert(sizeof(PlatformParticipantConfigureScratch) <= 256,
              "Participant configure scratch exceeded its budget");

struct PlatformResponseWorkspace {
  static constexpr std::size_t CHUNK_SIZE = 768;
  char chunk[CHUNK_SIZE]{};
  PlatformParticipantConfigureScratch participant_configure{};
};

static_assert(sizeof(PlatformResponseWorkspace) <= 1024,
              "Platform response workspace exceeded its budget");

namespace {

static const char *const TAG = "platform_handler";
constexpr uint32_t DEFAULT_PAIRING_WINDOW_SECONDS = 120;
constexpr uint32_t MIN_PAIRING_WINDOW_SECONDS = 30;
constexpr uint32_t MAX_PAIRING_WINDOW_SECONDS =
    ParticipantEnrollment::MAX_PAIRING_WINDOW_MS / 1000U;

#ifdef USE_ESP32
class PlatformJsonChunkWriter {
 public:
  PlatformJsonChunkWriter(httpd_req_t *request, char *buffer,
                          std::size_t capacity)
      : request_(request), buffer_(buffer), capacity_(capacity) {}

  bool write(const char *value) {
    if (!this->ok_ || value == nullptr)
      return false;
    return this->append_(value, std::strlen(value));
  }

  bool printf(const char *format, ...) {
    if (!this->ok_ || format == nullptr)
      return false;
    va_list args;
    va_start(args, format);
    const bool result = this->vprintf_(format, args);
    va_end(args);
    return result;
  }

  bool json_string(const char *value) {
    if (!this->ok_ || value == nullptr || !this->append_char_('"'))
      return false;
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(value);
         *cursor != '\0'; ++cursor) {
      switch (*cursor) {
        case '"':
          if (!this->write("\\\""))
            return false;
          break;
        case '\\':
          if (!this->write("\\\\"))
            return false;
          break;
        case '\b':
          if (!this->write("\\b"))
            return false;
          break;
        case '\f':
          if (!this->write("\\f"))
            return false;
          break;
        case '\n':
          if (!this->write("\\n"))
            return false;
          break;
        case '\r':
          if (!this->write("\\r"))
            return false;
          break;
        case '\t':
          if (!this->write("\\t"))
            return false;
          break;
        default:
          if (*cursor < 0x20U) {
            static constexpr char HEX[] = "0123456789abcdef";
            char escaped[6]{'\\', 'u', '0', '0',
                            HEX[(*cursor >> 4U) & 0x0FU],
                            HEX[*cursor & 0x0FU]};
            if (!this->append_(escaped, sizeof(escaped)))
              return false;
          } else if (!this->append_char_(static_cast<char>(*cursor))) {
            return false;
          }
          break;
      }
    }
    return this->append_char_('"');
  }

  bool unsigned_number(uint64_t value) {
    char digits[20]{};
    std::size_t used = 0;
    do {
      digits[used++] = static_cast<char>('0' + value % 10U);
      value /= 10U;
    } while (value != 0 && used < sizeof(digits));
    while (used != 0) {
      if (!this->append_char_(digits[--used]))
        return false;
    }
    return this->ok_;
  }

  bool signed_number(int64_t value) {
    if (value >= 0)
      return this->unsigned_number(static_cast<uint64_t>(value));
    if (!this->append_char_('-'))
      return false;
    const uint64_t magnitude =
        static_cast<uint64_t>(-(value + 1)) + static_cast<uint64_t>(1);
    return this->unsigned_number(magnitude);
  }

  bool boolean(bool value) { return this->write(value ? "true" : "false"); }

  bool fixed_hex(uint64_t value, std::size_t digits) {
    static constexpr char HEX[] = "0123456789abcdef";
    if (digits == 0 || digits > 16)
      return false;
    char encoded[16]{};
    for (std::size_t index = 0; index < digits; ++index) {
      const std::size_t shift = (digits - index - 1U) * 4U;
      encoded[index] = HEX[(value >> shift) & 0x0FU];
    }
    return this->append_(encoded, digits);
  }

  bool finish() {
    return this->flush_() &&
           httpd_resp_send_chunk(this->request_, nullptr, 0) == ESP_OK;
  }

 private:
  bool append_char_(char value) { return this->append_(&value, 1); }

  bool append_(const char *value, std::size_t length) {
    while (length > 0 && this->ok_) {
      if (this->used_ == this->capacity_ && !this->flush_())
        return false;
      const std::size_t available = this->capacity_ - this->used_;
      const std::size_t chunk = std::min(available, length);
      std::memcpy(this->buffer_ + this->used_, value, chunk);
      this->used_ += chunk;
      value += chunk;
      length -= chunk;
    }
    return this->ok_;
  }

  bool vprintf_(const char *format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int written = std::vsnprintf(this->buffer_ + this->used_,
                                 this->capacity_ - this->used_, format, copy);
    va_end(copy);
    if (written < 0) {
      this->ok_ = false;
      return false;
    }
    if (static_cast<std::size_t>(written) < this->capacity_ - this->used_) {
      this->used_ += static_cast<std::size_t>(written);
      return true;
    }
    if (!this->flush_() ||
        static_cast<std::size_t>(written) >= this->capacity_) {
      this->ok_ = false;
      return false;
    }
    written = std::vsnprintf(this->buffer_, this->capacity_, format, args);
    if (written < 0 || static_cast<std::size_t>(written) >= this->capacity_) {
      this->ok_ = false;
      return false;
    }
    this->used_ = static_cast<std::size_t>(written);
    return true;
  }

  bool flush_() {
    if (!this->ok_)
      return false;
    if (this->used_ == 0)
      return true;
    this->ok_ = httpd_resp_send_chunk(this->request_, this->buffer_,
                                      this->used_) == ESP_OK;
    this->used_ = 0;
    return this->ok_;
  }

  httpd_req_t *request_;
  char *buffer_;
  std::size_t capacity_;
  std::size_t used_{0};
  bool ok_{true};
};

void write_participant_assignment(
    PlatformJsonChunkWriter &writer, const char *floorplan_id,
    const char *space_id, const char *group_id, uint32_t geometry_version,
    uint32_t local_pose_version, uint32_t pose_version,
    uint32_t calibration_version, const SpatialTransform2D &transform) {
  writer.write("{\"floorplanId\":");
  writer.json_string(floorplan_id);
  writer.write(",\"spaceId\":");
  writer.json_string(space_id);
  writer.write(",\"groupId\":");
  writer.json_string(group_id);
  writer.write(",\"geometryVersion\":");
  writer.unsigned_number(geometry_version);
  writer.write(",\"localPoseVersion\":");
  writer.unsigned_number(local_pose_version);
  writer.write(",\"poseVersion\":");
  writer.unsigned_number(pose_version);
  writer.write(",\"calibrationVersion\":");
  writer.unsigned_number(calibration_version);
  writer.write(",\"transform\":");
  if (valid_spatial_transform(transform)) {
    writer.write("{\"originXMillimeters\":");
    writer.signed_number(transform.origin_x_mm);
    writer.write(",\"originYMillimeters\":");
    writer.signed_number(transform.origin_y_mm);
    writer.write(",\"rotationCentiDegrees\":");
    writer.signed_number(transform.rotation_cdeg);
    writer.write(",\"residualMillimeters\":");
    writer.unsigned_number(transform.residual_mm);
    writer.write(",\"calibrationSamples\":");
    writer.unsigned_number(transform.sample_count);
    writer.write("}}");
  } else {
    writer.write("null}");
  }
}
#endif

const char *participant_type_string(ParticipantType type) {
  return type == ParticipantType::SATELLITE ? "satellite" : "namo_node";
}

const char *participant_trust_string(ParticipantTrust trust) {
  switch (trust) {
    case ParticipantTrust::UNKNOWN:
      return "unknown";
    case ParticipantTrust::OFFICIAL_VERIFIED:
      return "official_verified";
    case ParticipantTrust::COMMUNITY_UNVERIFIED:
      return "community_unverified";
    case ParticipantTrust::SATELLITE:
      return "satellite";
  }
  return "unknown";
}

void format_ipv4(uint32_t ipv4, char output[16]) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&ipv4);
  std::snprintf(output, 16, "%u.%u.%u.%u", static_cast<unsigned>(bytes[0]),
                static_cast<unsigned>(bytes[1]),
                static_cast<unsigned>(bytes[2]),
                static_cast<unsigned>(bytes[3]));
}

const char *resolve_group_space_id(
    const SiteConfigurationSnapshot &configuration, const char *group_id) {
  if (group_id == nullptr || group_id[0] == '\0')
    return nullptr;
  const char *resolved = nullptr;
  for (const auto &member : configuration.members) {
    if (!member.occupied ||
        member.membership != ParticipantMembership::ACTIVE ||
        !member.spatial_profile.committed ||
        std::strcmp(member.spatial_profile.group_id, group_id) != 0)
      continue;
    if (resolved == nullptr) {
      resolved = member.spatial_profile.space_id;
      continue;
    }
    if (std::strcmp(resolved, member.spatial_profile.space_id) != 0)
      return nullptr;
  }
  return resolved != nullptr && resolved[0] != '\0' ? resolved : nullptr;
}

const char *participant_membership_string(ParticipantMembership membership) {
  switch (membership) {
    case ParticipantMembership::QUARANTINED:
      return "quarantined";
    case ParticipantMembership::ACTIVE:
      return "active";
    case ParticipantMembership::REVOKED:
      return "revoked";
  }
  return "quarantined";
}

bool parse_uint32_arg(AsyncWebServerRequest *request, const char *key, uint32_t *value) {
  if (request == nullptr || value == nullptr || !request->hasArg(key))
    return false;
  const auto text = request->arg(key);
  if (text.length() == 0)
    return false;
  char *end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || parsed > UINT32_MAX)
    return false;
  *value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_hex_uint64_arg(AsyncWebServerRequest *request, const char *key,
                          uint64_t *value) {
  if (request == nullptr || value == nullptr || !request->hasArg(key))
    return false;
  const auto text = request->arg(key);
  if (text.length() != 16)
    return false;
  char *end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 16);
  if (errno != 0 || end == text.c_str() || *end != '\0' || parsed == 0)
    return false;
  *value = static_cast<uint64_t>(parsed);
  return true;
}

bool parse_int32_arg(AsyncWebServerRequest *request, const char *key,
                     int32_t *value) {
  if (request == nullptr || value == nullptr || !request->hasArg(key))
    return false;
  const auto text = request->arg(key);
  if (text.length() == 0)
    return false;
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' ||
      parsed < INT32_MIN || parsed > INT32_MAX)
    return false;
  *value = static_cast<int32_t>(parsed);
  return true;
}

bool parse_bool_arg(AsyncWebServerRequest *request, const char *key, bool *value) {
  if (request == nullptr || value == nullptr || !request->hasArg(key))
    return false;
  const auto text = request->arg(key);
  if (text == "1" || text == "true") {
    *value = true;
    return true;
  }
  if (text == "0" || text == "false") {
    *value = false;
    return true;
  }
  return false;
}

bool parse_optional_bool_arg(AsyncWebServerRequest *request, const char *key,
                             bool *value) {
  if (!request->hasArg(key)) {
    *value = false;
    return true;
  }
  return parse_bool_arg(request, key, value);
}

void copy_arg(char *target, std::size_t capacity, const char *value) {
  std::strncpy(target, value, capacity - 1);
  target[capacity - 1] = '\0';
}

int enrollment_http_status(ParticipantEnrollmentResult result) {
  switch (result) {
    case ParticipantEnrollmentResult::INVALID_ARGUMENT:
      return 400;
    case ParticipantEnrollmentResult::NOT_FOUND:
      return 404;
    case ParticipantEnrollmentResult::PERSISTENCE_FAILED:
      return 503;
    case ParticipantEnrollmentResult::ATTESTATION_REQUIRED:
      return 403;
    case ParticipantEnrollmentResult::NOT_READY:
      return 409;
    case ParticipantEnrollmentResult::OK:
      return 200;
    default:
      return 409;
  }
}

void send_enrollment_error(AsyncWebServerRequest *request,
                           ParticipantEnrollmentResult result) {
  const char *code = participant_enrollment_result_to_string(result);
  http_response::send_error_info(request, enrollment_http_status(result), code, code,
                                 "error", "{}");
}

bool apply_platform_operation_outcome(
    AsyncWebServerRequest *request,
    const PlatformParticipantOperationOutcome &outcome,
    ParticipantEnrollmentResult *result) {
  if (outcome.result == PlatformParticipantOperationResult::OK ||
      outcome.result == PlatformParticipantOperationResult::ENROLLMENT_FAILED) {
    *result = outcome.enrollment_result;
    return true;
  }
  if (outcome.result ==
      PlatformParticipantOperationResult::ROSTER_PERSISTENCE_FAILED) {
    *result = ParticipantEnrollmentResult::PERSISTENCE_FAILED;
    return true;
  }

  const char *code = platform_participant_operation_result_to_string(
      outcome.result);
  int status = 503;
  const char *severity = "error";
  if (outcome.result == PlatformParticipantOperationResult::BUSY) {
    status = 409;
    severity = "warning";
  } else if (outcome.result ==
             PlatformParticipantOperationResult::CONTINUITY_UNAVAILABLE) {
    status = 409;
    severity = "warning";
  }
  http_response::send_error_info(request, status, code, code, severity, "{}");
  return false;
}

}  // namespace

PlatformHandler::~PlatformHandler() {
  if (this->response_workspace_ == nullptr)
    return;
#if defined(USE_ESP32) && defined(USE_PSRAM)
  this->response_workspace_->~PlatformResponseWorkspace();
  heap_caps_free(this->response_workspace_);
#else
  delete this->response_workspace_;
#endif
  this->response_workspace_ = nullptr;
}

void PlatformHandler::setup() {
  if (this->response_workspace_ != nullptr)
    return;
#if defined(USE_ESP32) && defined(USE_PSRAM)
  void *memory = heap_caps_calloc(
      1, sizeof(PlatformResponseWorkspace),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    this->response_workspace_ = new (memory) PlatformResponseWorkspace();
    ESP_LOGI(TAG,
             "Platform response workspace allocated in PSRAM "
             "size=%u",
             static_cast<unsigned>(sizeof(PlatformResponseWorkspace)));
  } else {
    ESP_LOGE(TAG,
             "Platform response workspace PSRAM allocation failed "
             "size=%u; endpoint disabled",
             static_cast<unsigned>(sizeof(PlatformResponseWorkspace)));
  }
#elif !defined(USE_ESP32)
  this->response_workspace_ = new (std::nothrow) PlatformResponseWorkspace();
#else
  ESP_LOGE(TAG,
           "Platform response workspace requires PSRAM; endpoints disabled");
#endif
}

bool PlatformHandler::can_handle(AsyncWebServerRequest *request, StringRef url) const {
  if (request->method() == HTTP_GET && url == "/api/platform/descriptor")
    return true;
  if ((request->method() == HTTP_GET || request->method() == HTTP_POST) &&
      url == "/api/platform/discovery")
    return true;
  if (request->method() == HTTP_GET && url == "/api/platform/groups")
    return true;
  if ((request->method() == HTTP_GET || request->method() == HTTP_POST) &&
      url == "/api/platform/site-configuration")
    return true;
  return (request->method() == HTTP_GET || request->method() == HTTP_POST) &&
         url == "/api/platform/participants";
}

bool PlatformHandler::handle(AsyncWebServerRequest *request, StringRef url) {
  if (url == "/api/platform/descriptor") {
    this->handle_descriptor_(request);
  } else if (url == "/api/platform/discovery") {
    if (request->method() == HTTP_GET)
      this->handle_discovery_get_(request);
    else
      this->handle_discovery_post_(request);
  } else if (url == "/api/platform/groups") {
    this->handle_groups_get_(request);
  } else if (url == "/api/platform/site-configuration") {
    if (request->method() == HTTP_GET)
      this->handle_site_configuration_get_(request);
    else
      this->handle_site_configuration_post_(request);
  } else if (request->method() == HTTP_GET) {
    this->handle_participants_get_(request);
  } else {
    this->handle_participants_post_(request);
  }
  return true;
}

void PlatformHandler::handle_site_configuration_get_(
    AsyncWebServerRequest *request) {
  if (!this->server_->site_configuration_ready()) {
    http_response::send_error_info(request, 503,
                                   "site_configuration_unavailable",
                                   "site_configuration_unavailable", "error",
                                   "{}");
    return;
  }
#ifdef USE_ESP32
  if (this->response_workspace_ == nullptr) {
    http_response::send_error_info(
        request, 503, "site_configuration_response_unavailable",
        "site_configuration_response_unavailable", "error",
        "{\"resource\":\"site_configuration_workspace\"}");
    return;
  }
  const auto &configuration = this->server_->site_configuration_snapshot();
  const uint64_t configuration_digest =
      site_configuration_digest(configuration);
  const bool configuration_consistent =
      this->server_->site_authority_snapshot(millis()).consistent();
  httpd_req_t *raw_request = *request;
  httpd_resp_set_status(raw_request, HTTPD_200);
  httpd_resp_set_type(raw_request, "application/json");
  httpd_resp_set_hdr(raw_request, "Accept-Ranges", "none");
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  PlatformJsonChunkWriter writer(
      raw_request, this->response_workspace_->chunk,
      sizeof(this->response_workspace_->chunk));
  writer.printf(
      "{\"ok\":true,\"schemaVersion\":2,\"siteId\":\"%s\","
      "\"authorityEpoch\":%u,\"generation\":%u,"
      "\"configurationDigest\":\"%08x%08x\","
      "\"configurationConsistent\":%s,\"rosterRevision\":%u,"
      "\"rosterCommitted\":%s,\"configurationOwnerNodeId\":",
      configuration.site_id,
      static_cast<unsigned>(configuration.authority_epoch),
      static_cast<unsigned>(configuration.generation),
      static_cast<unsigned>(configuration_digest >> 32U),
      static_cast<unsigned>(configuration_digest & 0xFFFFFFFFULL),
      configuration_consistent ? "true" : "false",
      static_cast<unsigned>(configuration.roster_revision),
      configuration.roster_committed ? "true" : "false");
  if (configuration.configuration_owner_node_id[0] != '\0')
    writer.printf("\"%s\"", configuration.configuration_owner_node_id);
  else
    writer.write("null");
  writer.printf(
      ",\"localOwner\":%s,\"sharedWritesAllowed\":%s,"
      "\"memberCount\":%u,\"activeMemberCount\":%u,"
      "\"floorplan\":{\"committed\":%s,\"floorplanId\":",
      std::strcmp(configuration.configuration_owner_node_id,
                  this->server_->platform_identity().node_id) == 0
          ? "true"
          : "false",
      this->server_->shared_site_configuration_writable(millis())
          ? "true"
          : "false",
      static_cast<unsigned>(configuration.member_count()),
      static_cast<unsigned>(configuration.active_member_count()),
      configuration.floorplan.committed ? "true" : "false");
  if (configuration.floorplan.committed)
    writer.printf("\"%s\"", configuration.floorplan.floorplan_id);
  else
    writer.write("null");
  writer.printf(
      ",\"geometryVersion\":%u,\"configHash\":%u,\"imageHash\":%u},"
      "\"members\":[",
      static_cast<unsigned>(configuration.floorplan.geometry_version),
      static_cast<unsigned>(configuration.floorplan.config_hash),
      static_cast<unsigned>(configuration.floorplan.image_hash));
  bool first = true;
  for (const auto &member : configuration.members) {
    if (!member.occupied)
      continue;
    if (!first)
      writer.write(",");
    first = false;
    writer.printf(
        "{\"participantId\":\"%s\",\"participantType\":\"%s\","
        "\"trust\":\"%s\",\"membership\":\"%s\","
        "\"leaderEligible\":%s,\"spatialProfile\":",
        member.participant_id, participant_type_string(member.type),
        participant_trust_string(member.trust),
        participant_membership_string(member.membership),
        member.leader_eligible ? "true" : "false");
    if (!member.spatial_profile.committed) {
      writer.write("null");
    } else {
      writer.printf(
          "{\"spaceId\":\"%s\",\"groupId\":\"%s\","
          "\"poseVersion\":%u,\"calibrationVersion\":%u,"
          "\"transform\":{\"originXMillimeters\":%d,"
          "\"originYMillimeters\":%d,\"rotationCentiDegrees\":%d}}",
          member.spatial_profile.space_id,
          member.spatial_profile.group_id,
          static_cast<unsigned>(member.spatial_profile.pose_version),
          static_cast<unsigned>(
              member.spatial_profile.calibration_version),
          static_cast<int>(
              member.spatial_profile.transform.origin_x_mm),
          static_cast<int>(
              member.spatial_profile.transform.origin_y_mm),
          static_cast<int>(
              member.spatial_profile.transform.rotation_cdeg));
    }
    writer.write("}");
  }
  writer.write("],\"replication\":");
  const auto *replication = this->server_->site_replication_diagnostics();
  const auto *replication_source =
      this->server_->site_replication_source_diagnostics();
  if (replication == nullptr) {
    writer.write("null");
  } else {
    writer.printf(
        "{\"supported\":%s,\"socketReady\":%s,\"state\":\"%s\","
        "\"result\":\"%s\",\"validationResult\":\"%s\","
        "\"mode\":\"%s\","
        "\"operation\":\"%s\",\"ownerNodeId\":",
        replication->supported ? "true" : "false",
        replication->socket_ready ? "true" : "false",
        site_replication_state_to_string(replication->state),
        site_replication_result_to_string(replication->result),
        site_configuration_replication_result_to_string(
            replication->validation_result),
        site_configuration_replication_mode_to_string(replication->mode),
        site_replication_target_to_string(replication->target));
    if (replication->owner_node_id[0] != '\0')
      writer.printf("\"%s\"", replication->owner_node_id);
    else
      writer.write("null");
    writer.printf(
        ",\"remoteGeneration\":%u,\"transferredBytes\":%u,"
        "\"totalBytes\":%u,\"requests\":%u,\"responses\":%u,"
        "\"retries\":%u,\"rejected\":%u,\"replayed\":%u,"
        "\"staleRequests\":%u,"
        "\"autoPendingAuthorityEpoch\":%u,"
        "\"autoPendingGeneration\":%u,\"autoStartInMs\":%u,"
        "\"autoRetryCount\":%u,\"directoryAutoActive\":%s,"
        "\"directoryAutoStartInMs\":%u,\"directoryAutoRetryCount\":%u,"
        "\"source\":{\"active\":%s,\"participantId\":",
        static_cast<unsigned>(replication->remote_generation),
        static_cast<unsigned>(replication->transferred_bytes),
        static_cast<unsigned>(replication->total_bytes),
        static_cast<unsigned>(replication->requests),
        static_cast<unsigned>(replication->responses),
        static_cast<unsigned>(replication->retries),
        static_cast<unsigned>(replication->rejected),
        static_cast<unsigned>(replication->replayed),
        static_cast<unsigned>(replication->stale_requests),
        static_cast<unsigned>(
            this->server_->auto_replication_pending_authority_epoch()),
        static_cast<unsigned>(
            this->server_->auto_replication_pending_generation()),
        static_cast<unsigned>(
            this->server_->auto_replication_wait_remaining_ms(millis())),
        static_cast<unsigned>(this->server_->auto_replication_retry_count()),
        this->server_->auto_directory_replication_active() ? "true"
                                                           : "false",
        static_cast<unsigned>(
            this->server_->auto_directory_replication_wait_remaining_ms(
                millis())),
        static_cast<unsigned>(
            this->server_->auto_directory_replication_retry_count()),
        replication_source != nullptr && replication_source->active
            ? "true"
            : "false");
    if (replication_source != nullptr && replication_source->active)
      writer.printf("\"%s\"", replication_source->participant_id);
    else
      writer.write("null");
    writer.printf(
        ",\"leaseRemainingMs\":%u,\"accepted\":%u,\"busy\":%u,"
        "\"completed\":%u,\"expired\":%u}}",
        static_cast<unsigned>(
            this->server_->site_replication_source_lease_remaining_ms(
                millis())),
        static_cast<unsigned>(replication_source != nullptr
                                  ? replication_source->accepted
                                  : 0),
        static_cast<unsigned>(replication_source != nullptr
                                  ? replication_source->busy
                                  : 0),
        static_cast<unsigned>(replication_source != nullptr
                                  ? replication_source->completed
                                  : 0),
        static_cast<unsigned>(replication_source != nullptr
                                  ? replication_source->expired
                                  : 0));
  }
  writer.write(",\"automaticHandover\":{");
  const auto &handover =
      this->server_->automatic_configuration_handover();
  const auto *successor =
      this->server_->automatic_configuration_successor(millis());
  writer.printf("\"state\":\"%s\",\"condition\":\"%s\","
                 "\"successorNodeId\":",
                 automatic_configuration_handover_state_to_string(
                     handover.state()),
                 automatic_configuration_handover_condition_to_string(
                     handover.condition()));
  if (successor != nullptr)
    writer.printf("\"%s\"", successor->participant_id);
  else
    writer.write("null");
  writer.printf(",\"waitRemainingMs\":%u}",
                 static_cast<unsigned>(
                     handover.wait_remaining_ms(millis())));
  writer.write("}");
  if (!writer.finish())
    ESP_LOGW(TAG, "Site configuration response streaming failed");
#else
  http_response::send_error_info(
      request, 503, "site_configuration_response_unavailable",
      "site_configuration_response_unavailable", "error",
      "{\"resource\":\"site_configuration_workspace\"}");
#endif
}

void PlatformHandler::handle_site_configuration_post_(
    AsyncWebServerRequest *request) {
  if (!request->hasArg("action")) {
    http_response::send_error_info(request, 400, "invalid_action",
                                   "invalid_action", "error", "{}");
    return;
  }
  const auto action = request->arg("action");
  if (action == "sync") {
    const char *owner = nullptr;
    std::string owner_arg;
    if (request->hasArg("ownerNodeId")) {
      owner_arg = request->arg("ownerNodeId");
      owner = owner_arg.c_str();
    }
    const auto result = this->server_->start_site_replication(owner);
    if (result != SiteReplicationResult::OK) {
      const char *code = site_replication_result_to_string(result);
      http_response::send_error_info(
          request, result == SiteReplicationResult::BUSY ? 409 : 503, code,
          code, "error", "{}");
      return;
    }
    this->handle_site_configuration_get_(request);
    return;
  }
  if (action == "cancelSync") {
    this->server_->cancel_site_replication();
    this->handle_site_configuration_get_(request);
    return;
  }
  if (!this->server_->site_configuration_persistence_ready() ||
      !this->server_->participant_registry_ready()) {
    http_response::send_error_info(request, 503,
                                   "site_configuration_unavailable",
                                   "site_configuration_unavailable", "error",
                                   "{}");
    return;
  }
  std::string requested_floorplan_id;
  uint32_t requested_geometry_version = 0;
  uint32_t requested_config_hash = 0;
  uint32_t requested_image_hash = 0;
  uint32_t requested_coordinate_width_mm = 0;
  uint32_t requested_coordinate_height_mm = 0;
  if (action == "commitFloorplan") {
    const auto status = this->server_->storage().status();
    if (!request->hasArg("floorplanId") ||
        !parse_uint32_arg(request, "geometryVersion",
                          &requested_geometry_version) ||
        !parse_uint32_arg(request, "coordinateWidthMm",
                          &requested_coordinate_width_mm) ||
        !parse_uint32_arg(request, "coordinateHeightMm",
                          &requested_coordinate_height_mm) ||
        requested_coordinate_width_mm > INT32_MAX ||
        requested_coordinate_height_mm > INT32_MAX ||
        !status.ok || status.header.config_size == 0 ||
        status.header.image_size == 0) {
      http_response::send_error_info(request, 400,
                                     "floorplan_manifest_invalid",
                                     "floorplan_manifest_invalid", "error",
                                     "{}");
      return;
    }
    requested_floorplan_id = request->arg("floorplanId");
    requested_config_hash = status.header.config_hash;
    requested_image_hash = status.header.image_hash;
    if (site_floorplan_manifest_matches(
            this->server_->site_configuration_snapshot(),
            requested_floorplan_id.c_str(), requested_geometry_version,
            requested_config_hash, requested_image_hash)) {
      this->handle_site_configuration_get_(request);
      return;
    }
  }
  if (action != "claimOwnership" && action != "resolveConflict" &&
      std::strcmp(
          this->server_->site_configuration_snapshot()
              .configuration_owner_node_id,
          this->server_->platform_identity().node_id) == 0 &&
      !this->server_->shared_site_configuration_writable(millis())) {
    http_response::send_error_info(
        request, 409, "site_configuration_continuity_unavailable",
        "site_configuration_continuity_unavailable", "warning", "{}");
    return;
  }
  auto &updated = this->server_->site_configuration_update_scratch();
  SiteConfigurationResult build_result =
      SiteConfigurationResult::INVALID_ARGUMENT;
  if (action == "claimOwnership") {
    build_result = claim_local_site_ownership(
        this->server_->platform_identity(),
        this->server_->participant_registry_snapshot().registry,
        this->server_->site_configuration_snapshot(), &updated);
  } else if (action == "resolveConflict") {
    uint64_t expected_digest = 0;
    bool confirmed = false;
    const auto &current = this->server_->site_configuration_snapshot();
    if (!parse_bool_arg(request, "confirm", &confirmed) || !confirmed ||
        !parse_hex_uint64_arg(request, "expectedDigest", &expected_digest) ||
        expected_digest != site_configuration_digest(current) ||
        this->server_->site_authority_snapshot(millis()).consistent()) {
      http_response::send_error_info(
          request, 409, "site_configuration_conflict_changed",
          "site_configuration_conflict_changed", "error", "{}");
      return;
    }
    build_result = prepare_local_configuration_conflict_resolution(
        this->server_->platform_identity(), current, &updated);
  } else if (action == "commitRoster") {
    build_result = commit_local_site_roster(
        this->server_->platform_identity(),
        this->server_->participant_registry_snapshot().registry,
        this->server_->site_configuration_snapshot(), &updated);
  } else if (action == "commitFloorplan") {
    build_result = commit_local_floorplan_manifest(
        this->server_->platform_identity(),
        this->server_->site_configuration_snapshot(),
        requested_floorplan_id.c_str(), requested_geometry_version,
        requested_config_hash, requested_image_hash, &updated,
        static_cast<int32_t>(requested_coordinate_width_mm),
        static_cast<int32_t>(requested_coordinate_height_mm));
  } else {
    http_response::send_error_info(request, 400, "invalid_action",
                                   "invalid_action", "error", "{}");
    return;
  }
  if (build_result == SiteConfigurationResult::NOT_OWNER) {
    http_response::send_error_info(request, 403,
                                   action == "claimOwnership"
                                       ? "site_configuration_owner_resolved"
                                       : "site_configuration_owner_required",
                                   action == "claimOwnership"
                                       ? "site_configuration_owner_resolved"
                                       : "site_configuration_owner_required",
                                   "error", "{}");
    return;
  }
  if (build_result != SiteConfigurationResult::OK) {
    http_response::send_error_info(request, 409,
                                   action == "commitRoster" ||
                                           action == "claimOwnership" ||
                                           action == "resolveConflict"
                                       ? "site_roster_invalid"
                                       : "floorplan_manifest_invalid",
                                   action == "commitRoster" ||
                                           action == "claimOwnership" ||
                                           action == "resolveConflict"
                                       ? "site_roster_invalid"
                                       : "floorplan_manifest_invalid",
                                   "error", "{}");
    return;
  }
  const auto store_result = this->server_->site_configuration_store()->update(
      *this->server_->site_configuration_backend(), updated,
      &this->server_->mutable_site_configuration_snapshot());
  if (store_result != SiteConfigurationStoreResult::UPDATED) {
    http_response::send_error_info(request, 503,
                                   "site_configuration_persistence_failed",
                                   "site_configuration_persistence_failed",
                                   "error", "{}");
    return;
  }
  if (action == "resolveConflict") {
    this->server_->cancel_site_replication();
    ESP_LOGW(TAG,
             "Site configuration conflict resolved from local snapshot "
             "authority_epoch=%u generation=%u",
             static_cast<unsigned>(
                 this->server_->site_configuration_snapshot().authority_epoch),
             static_cast<unsigned>(
                 this->server_->site_configuration_snapshot().generation));
  }
  this->handle_site_configuration_get_(request);
}

void PlatformHandler::handle_descriptor_(AsyncWebServerRequest *request) {
  const std::string descriptor = build_platform_descriptor_json(
      this->server_->platform_identity(), this->server_->platform_identity_ready(),
      dashboard_assets::FIRMWARE_VERSION);
  if (descriptor.empty()) {
    http_response::send_error_info(request, 503, "platform_identity_unavailable",
                                   "platform_identity_unavailable", "error", "{}");
    return;
  }
  http_response::send_json(request, 200, descriptor.c_str());
}

void PlatformHandler::handle_discovery_get_(AsyncWebServerRequest *request) {
  auto &runtime = this->server_->participant_discovery_runtime();
  const auto snapshot = runtime.snapshot();
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      "{\"ok\":true,\"supported\":%s,\"scanning\":%s,"
      "\"generation\":%u,\"result\":\"%s\",\"candidates\":[",
      snapshot.supported ? "true" : "false",
      snapshot.scanning ? "true" : "false",
      static_cast<unsigned>(snapshot.generation),
      participant_discovery_result_to_string(snapshot.result));
  bool first = true;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    ParticipantDiscoveryCandidate candidate{};
    if (!runtime.candidate_at(slot, &candidate))
      continue;
    if (!first)
      stream->print(",");
    first = false;
    stream->printf(
        "{\"participantId\":\"%s\",\"participantType\":\"namo_node\","
        "\"product\":\"namo-spatial-sensor\",\"address\":\"%s\","
        "\"hostname\":\"%s\",\"port\":%u,\"protocolMinimum\":%u,"
        "\"protocolMaximum\":%u}",
        candidate.participant_id, candidate.address, candidate.hostname,
        static_cast<unsigned>(candidate.port),
        static_cast<unsigned>(candidate.protocol_minimum),
        static_cast<unsigned>(candidate.protocol_maximum));
  }
  stream->print("]}");
  request->send(stream);
}

void PlatformHandler::handle_discovery_post_(AsyncWebServerRequest *request) {
  if (!request->hasArg("action") || request->arg("action") != "scan") {
    http_response::send_error_info(request, 400, "invalid_discovery_action",
                                   "invalid_discovery_action", "error", "{}");
    return;
  }
  if (!this->server_->platform_identity_ready()) {
    http_response::send_error_info(request, 503, "platform_identity_unavailable",
                                   "platform_identity_unavailable", "error", "{}");
    return;
  }
  if (!this->server_->participant_discovery_runtime().queue_scan(
          this->server_->platform_identity().node_id)) {
    http_response::send_error_info(request, 409, "discovery_busy",
                                   "discovery_busy", "error", "{}");
    return;
  }
  http_response::send_json(request, 200,
                           "{\"ok\":true,\"accepted\":true}");
}

void PlatformHandler::handle_participants_get_(AsyncWebServerRequest *request) {
  this->send_participants_(request);
}

void PlatformHandler::handle_groups_get_(AsyncWebServerRequest *request) {
  const auto coordinator_lease =
      this->server_->acquire_coordinator_snapshot();
  if (!coordinator_lease.valid()) {
    http_response::send_error_info(request, 503,
                                   "platform_role_snapshot_unavailable",
                                   "platform_role_snapshot_unavailable",
                                   "warning", "{}");
    return;
  }
  const auto &coordinator = *coordinator_lease.snapshot;
  const auto &site = coordinator.site;
  const uint32_t now_ms = millis();
  const bool spatially_consistent =
      this->server_->site_authority_snapshot(now_ms).spatially_consistent();
  const char *site_health =
      !spatially_consistent
          ? "configuration_conflict"
          : site.participants_expected == 1
          ? "standalone"
          : (!site.quorum_available || site.leader_node_id[0] == '\0')
          ? "degraded"
          : (site.participants_online == site.participants_expected
                 ? "healthy"
                 : "degraded");
  const auto &production =
      this->server_->spatial_production_fusion_output();
  const bool production_build_enabled =
      this->server_->production_fusion_build_enabled();
  const char *production_state =
      production_build_enabled && !production.build_enabled
          ? "degraded"
          : spatial_production_fusion_state_to_string(production.state);
  auto *stream = request->beginResponseStream("application/json");
  stream->printf(
      "{\"ok\":true,\"supported\":true,\"shadowOnly\":%s,"
      "\"productionAuthority\":%s,\"generation\":%u,"
      "\"capacity\":{\"maxGroups\":%u,\"maxGroupParticipants\":%u},"
      "\"site\":{\"generation\":%u,"
      "\"term\":%u,\"leaderNodeId\":\"%s\",\"localLeader\":%s,"
      "\"configurationReady\":%s,\"rosterRevision\":%u,"
      "\"leaseRemainingMs\":%u,\"participantsOnline\":%u,"
      "\"participantsExpected\":%u,\"quorumRequired\":%u,"
      "\"quorumAvailable\":%s,\"health\":\"%s\"},\"groups\":[",
      production_build_enabled ? "false" : "true",
      production.authoritative ? "true" : "false",
      static_cast<unsigned>(coordinator.group_generation),
      static_cast<unsigned>(SiteDirectoryLimits::MAX_GROUPS),
      static_cast<unsigned>(SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS),
      static_cast<unsigned>(coordinator.site_generation),
      static_cast<unsigned>(site.term), site.leader_node_id,
      site.local_leader ? "true" : "false",
      site.configuration_ready ? "true" : "false",
      static_cast<unsigned>(site.roster_revision),
      static_cast<unsigned>(
          SiteCoordinator::lease_remaining_ms(site, now_ms)),
      static_cast<unsigned>(site.participants_online),
      static_cast<unsigned>(site.participants_expected),
      static_cast<unsigned>(site.quorum_required),
      site.quorum_available ? "true" : "false", site_health);
  bool first = true;
  for (std::size_t slot = 0; slot < coordinator.group_count; ++slot) {
    const auto *group = &coordinator.groups[slot];
    if (!first)
      stream->print(",");
    first = false;
    const char *health =
        !spatially_consistent
            ? "configuration_conflict"
            : !group->configured
            ? "standalone"
            : (!group->quorum_available ||
               group->leader_node_id[0] == '\0' ||
               !group->canonical_assignment ||
               !group->credentials_complete)
            ? "degraded"
            : (group->participants_online == group->participants_expected
                   ? "healthy"
                   : "degraded");
    stream->print("{\"groupId\":");
    if (group->configured)
      stream->printf("\"%s\"", group->group_id);
    else
      stream->print("null");
    stream->printf(
        ",\"configured\":%s,\"term\":%u,\"leaderNodeId\":\"%s\","
        "\"localLeader\":%s,\"assignmentRevision\":%u,"
        "\"canonicalAssignment\":%s,\"credentialsComplete\":%s,"
        "\"leaseRemainingMs\":%u,"
        "\"participantsOnline\":%u,\"participantsExpected\":%u,"
        "\"quorumRequired\":%u,\"quorumAvailable\":%s,"
        "\"health\":\"%s\"}",
        group->configured ? "true" : "false",
        static_cast<unsigned>(group->term), group->leader_node_id,
        group->local_leader ? "true" : "false",
        static_cast<unsigned>(group->assignment_revision),
        group->canonical_assignment ? "true" : "false",
        group->credentials_complete ? "true" : "false",
        static_cast<unsigned>(
            GroupCoordinator::lease_remaining_ms(*group, now_ms)),
        static_cast<unsigned>(group->participants_online),
        static_cast<unsigned>(group->participants_expected),
        static_cast<unsigned>(group->quorum_required),
        group->quorum_available ? "true" : "false", health);
  }
  const auto &group_roster = this->server_->group_roster_snapshot();
  const auto &site_group_summaries =
      this->server_->site_group_summaries();
  const auto &site_configuration =
      this->server_->site_configuration_snapshot();
  const std::size_t configured_site_groups =
      this->server_->site_directory_ready()
          ? this->server_->site_directory_snapshot().group_count()
          : 0;
  stream->printf(
      "],\"groupRoster\":{\"ready\":%s,\"storeResult\":\"%s\","
      "\"groupId\":",
      this->server_->group_roster_ready() ? "true" : "false",
      group_roster_store_result_to_string(
          this->server_->group_roster_store_result()));
  if (this->server_->group_roster_ready())
    stream->printf("\"%s\"", group_roster.group_id);
  else
    stream->print("null");
  stream->print(",\"leaderNodeId\":");
  if (this->server_->group_roster_ready())
    stream->printf("\"%s\"", group_roster.leader_node_id);
  else
    stream->print("null");
  stream->printf(
      ",\"generation\":%u,\"assignmentRevision\":%u,\"memberCount\":%u},"
      "\"siteGroupSummary\":{\"localAuthority\":%s,"
      "\"capacity\":%u,\"configuredGroups\":%u,\"reportingGroups\":%u,"
      "\"generation\":%u,\"rejected\":%u,\"expired\":%u},"
      "\"siteGroups\":[",
      static_cast<unsigned>(group_roster.generation),
      static_cast<unsigned>(group_roster.assignment_revision),
      static_cast<unsigned>(group_roster.member_count()),
      site.local_leader ? "true" : "false",
      static_cast<unsigned>(SiteGroupSummaryCache::MAX_GROUPS),
      static_cast<unsigned>(configured_site_groups),
      static_cast<unsigned>(site_group_summaries.size()),
      static_cast<unsigned>(site_group_summaries.generation()),
      static_cast<unsigned>(site_group_summaries.rejected()),
      static_cast<unsigned>(site_group_summaries.expired()));
  bool first_site_group = true;
  const auto *site_directory =
      site.local_leader && this->server_->site_directory_ready()
          ? &this->server_->site_directory_snapshot()
          : nullptr;
  for (std::size_t slot = 0; slot < SiteGroupSummaryCache::MAX_GROUPS;
       ++slot) {
    const auto *directory_group =
        site_directory != nullptr ? &site_directory->groups[slot] : nullptr;
    if (directory_group == nullptr || !directory_group->occupied)
      continue;
    const auto *summary =
        site_group_summaries.find(directory_group->group_id);
    const bool summary_matches =
        summary != nullptr &&
        std::strcmp(summary->floorplan_id,
                    directory_group->floorplan_id) == 0 &&
        std::strcmp(summary->leader_node_id,
                    directory_group->leader_node_id) == 0 &&
        summary->assignment_revision ==
            directory_group->assignment_revision &&
        summary->participants_expected == directory_group->member_count;
    if (!first_site_group)
      stream->print(",");
    first_site_group = false;
    const char *health =
        !summary_matches
            ? "unavailable"
            : (summary->participants_online == summary->participants_expected
                   ? "healthy"
                   : "degraded");
    const bool local_route =
        std::strcmp(directory_group->leader_node_id,
                    this->server_->platform_identity().node_id) == 0;
    const auto *route_endpoint =
        local_route
            ? nullptr
            : this->server_->participant_endpoint(
                  directory_group->leader_node_id);
    const char *space_id = resolve_group_space_id(
        site_configuration, directory_group->group_id);
    const bool route_available =
        summary_matches && space_id != nullptr &&
        (local_route ||
         (route_endpoint != nullptr && route_endpoint->ipv4 != 0 &&
          ParticipantEndpointCache::is_online(route_endpoint, now_ms)));
    stream->printf("{\"groupId\":\"%s\",\"spaceId\":",
                   directory_group->group_id);
    if (space_id != nullptr)
      stream->printf("\"%s\"", space_id);
    else
      stream->print("null");
    stream->printf(
        ",\"floorplanId\":\"%s\","
        "\"leaderNodeId\":\"%s\",\"assignmentRevision\":%u,"
        "\"term\":%u,\"leaseRemainingMs\":%u,"
        "\"participantsOnline\":%u,\"participantsExpected\":%u,"
        "\"health\":\"%s\",\"route\":{\"schemaVersion\":1,"
        "\"available\":%s,\"local\":%s,\"address\":",
        directory_group->floorplan_id, directory_group->leader_node_id,
        static_cast<unsigned>(directory_group->assignment_revision),
        static_cast<unsigned>(summary_matches
                                  ? summary->term
                                  : directory_group->assignment_revision),
        static_cast<unsigned>(summary_matches
                                  ? SiteGroupSummaryCache::lease_remaining_ms(
                                        *summary, now_ms)
                                  : 0),
        static_cast<unsigned>(summary_matches
                                  ? summary->participants_online
                                  : 0),
        static_cast<unsigned>(directory_group->member_count), health,
        route_available ? "true" : "false",
        local_route ? "true" : "false");
    if (!local_route && route_available) {
      char address[16]{};
      format_ipv4(route_endpoint->ipv4, address);
      stream->printf("\"%s\"", address);
    } else {
      stream->print("null");
    }
    stream->print(",\"port\":80,\"statePath\":\"/api/state\","
                  "\"lastSeenAgeMs\":");
    if (local_route && summary_matches) {
      stream->print("0");
    } else if (route_endpoint != nullptr && route_endpoint->authenticated) {
      stream->printf(
          "%u",
          static_cast<unsigned>(now_ms - route_endpoint->authenticated_at_ms));
    } else {
      stream->print("null");
    }
    stream->print("}}");
  }
  const auto &site_space_summaries =
      this->server_->site_space_summaries();
  stream->printf(
      "],\"siteSpaceSummary\":{\"shadowOnly\":true,"
      "\"localAuthority\":%s,\"capacity\":%u,\"reportingSpaces\":%u,"
      "\"generation\":%u,\"admitted\":%u,\"rejected\":%u,"
      "\"expired\":%u},\"siteSpaces\":[",
      site.local_leader ? "true" : "false",
      static_cast<unsigned>(SiteSpaceSummaryCache::MAX_SPACES),
      static_cast<unsigned>(site_space_summaries.size()),
      static_cast<unsigned>(site_space_summaries.generation()),
      static_cast<unsigned>(site_space_summaries.admitted()),
      static_cast<unsigned>(site_space_summaries.rejected()),
      static_cast<unsigned>(site_space_summaries.expired()));
  bool first_site_space = true;
  for (std::size_t slot = 0; slot < SiteSpaceSummaryCache::MAX_SPACES;
       ++slot) {
    const auto *summary = site_space_summaries.summary_at(slot);
    if (summary == nullptr)
      continue;
    if (!first_site_space)
      stream->print(",");
    first_site_space = false;
    stream->printf(
        "{\"spaceId\":\"%s\",\"groupId\":\"%s\","
        "\"leaderNodeId\":\"%s\",\"term\":%u,\"sequence\":%u,"
        "\"sourceSequence\":%u,\"ageMs\":%u,\"presence\":%s,"
        "\"motion\":%s,\"targetCount\":%u,"
        "\"health\":\"healthy\"}",
        summary->space_id, summary->group_id, summary->leader_node_id,
        static_cast<unsigned>(summary->group_term),
        static_cast<unsigned>(summary->sequence),
        static_cast<unsigned>(summary->source_sequence),
        static_cast<unsigned>(SiteSpaceSummaryCache::age_ms(*summary, now_ms)),
        summary->presence ? "true" : "false",
        summary->motion ? "true" : "false",
        static_cast<unsigned>(summary->target_count));
  }
  stream->print("],\"shadowFusion\":");
  const auto *runtime =
      this->server_->spatial_shadow_runtime_diagnostics();
  const auto *fusion = this->server_->spatial_shadow_fusion_output();
  if (runtime == nullptr || fusion == nullptr) {
    stream->print(
        "{\"enabled\":false,\"shadowOnly\":true,"
        "\"productionAuthority\":false,\"state\":\"disabled\","
        "\"groupId\":null,\"remoteParticipantId\":null,\"updates\":0,"
        "\"skipped\":0,\"maxCycleUs\":0,\"trackCount\":0,\"tracks\":[]}");
  } else {
    stream->printf(
        "{\"enabled\":true,\"shadowOnly\":true,"
        "\"productionAuthority\":false,\"state\":\"%s\",\"groupId\":",
        spatial_shadow_runtime_state_to_string(runtime->state));
    if (runtime->group_id[0] != '\0')
      stream->printf("\"%s\"", runtime->group_id);
    else
      stream->print("null");
    stream->print(",\"remoteParticipantId\":");
    if (runtime->remote_participant_id[0] != '\0')
      stream->printf("\"%s\"", runtime->remote_participant_id);
    else
      stream->print("null");
    stream->printf(
        ",\"updates\":%u,\"skipped\":%u,\"maxCycleUs\":%u,"
        "\"trackCount\":%u,\"diagnostics\":{\"validInputs\":%u,"
        "\"candidatePairs\":%u,\"pairedMeasurements\":%u,"
        "\"ambiguousPairs\":%u,\"droppedMeasurements\":%u,"
        "\"tentativeTracks\":%u,\"confirmedTracks\":%u,"
        "\"coastingTracks\":%u},\"tracks\":[",
        static_cast<unsigned>(runtime->updates),
        static_cast<unsigned>(runtime->skipped),
        static_cast<unsigned>(runtime->max_cycle_us),
        static_cast<unsigned>(fusion->track_count),
        static_cast<unsigned>(fusion->diagnostics.valid_inputs),
        static_cast<unsigned>(fusion->diagnostics.candidate_pairs),
        static_cast<unsigned>(fusion->diagnostics.paired_measurements),
        static_cast<unsigned>(fusion->diagnostics.ambiguous_pairs),
        static_cast<unsigned>(fusion->diagnostics.dropped_measurements),
        static_cast<unsigned>(fusion->diagnostics.tentative_tracks),
        static_cast<unsigned>(fusion->diagnostics.confirmed_tracks),
        static_cast<unsigned>(fusion->diagnostics.coasting_tracks));
    bool first_track = true;
    for (const auto &track : fusion->tracks) {
      if (!track.valid)
        continue;
      if (!first_track)
        stream->print(",");
      first_track = false;
      stream->printf(
          "{\"id\":%u,\"state\":\"%s\",\"sourceCount\":%u,"
          "\"confidence\":%u,\"correlatedViews\":%s,"
          "\"xMm\":%.1f,\"yMm\":%.1f,\"vxMmS\":%.1f,"
          "\"vyMmS\":%.1f,\"varianceXMillimeters2\":%.1f,"
          "\"varianceYMillimeters2\":%.1f}",
          static_cast<unsigned>(track.fused_track_id),
          spatial_shadow_track_state_to_string(track.state),
          static_cast<unsigned>(track.source_count),
          static_cast<unsigned>(track.confidence),
          track.correlated_views ? "true" : "false",
          static_cast<double>(track.x_mm), static_cast<double>(track.y_mm),
          static_cast<double>(track.vx_mm_s),
          static_cast<double>(track.vy_mm_s),
          static_cast<double>(track.variance_x_mm2),
          static_cast<double>(track.variance_y_mm2));
    }
    stream->print("]}");
  }
  stream->printf(
      ",\"productionFusion\":{\"buildEnabled\":%s,"
      "\"authoritative\":%s,\"state\":\"%s\",\"sourceSequence\":%u,"
      "\"targetCount\":%u,\"tracks\":[",
      production_build_enabled ? "true" : "false",
      production.authoritative ? "true" : "false",
      production_state,
      static_cast<unsigned>(production.source_sequence),
      static_cast<unsigned>(production.target_count));
  bool first_production_track = true;
  for (const auto &target : production.targets) {
    if (!target.valid)
      continue;
    if (!first_production_track)
      stream->print(",");
    first_production_track = false;
    stream->printf(
        "{\"id\":%u,\"state\":\"%s\",\"sourceCount\":%u,"
        "\"confidence\":%u,\"xMm\":%.1f,\"yMm\":%.1f,"
        "\"vxMmS\":%.1f,\"vyMmS\":%.1f}",
        static_cast<unsigned>(target.fused_track_id),
        spatial_shadow_track_state_to_string(target.state),
        static_cast<unsigned>(target.source_count),
        static_cast<unsigned>(target.confidence),
        static_cast<double>(target.x_mm), static_cast<double>(target.y_mm),
        static_cast<double>(target.vx_mm_s),
        static_cast<double>(target.vy_mm_s));
  }
  stream->print("]}");
  stream->print("}");
  this->server_->release_coordinator_snapshot(coordinator_lease);
  request->send(stream);
}

void PlatformHandler::handle_participants_post_(AsyncWebServerRequest *request) {
  if (!this->server_->platform_identity_ready() ||
      !this->server_->participant_registry_persistence_ready()) {
    http_response::send_error_info(request, 503, "participant_registry_unavailable",
                                   "participant_registry_unavailable", "error", "{}");
    return;
  }
  if (!request->hasArg("action")) {
    http_response::send_error_info(request, 400, "action_required", "action_required",
                                   "error", "{}");
    return;
  }

  const auto action = request->arg("action");
  if (action == "open" &&
      !this->server_->standalone_site_reconciliation_ready_for_pairing()) {
    const char *code =
        this->server_->standalone_site_reconciliation_state() ==
                StandaloneSiteReconciliationState::PENDING
            ? "site_reconciliation_pending"
            : "site_reconciliation_failed";
    http_response::send_error_info(request, 503, code, code, "error", "{}");
    return;
  }
  if (action == "set_community_leader") {
    const auto &configuration = this->server_->site_configuration_snapshot();
    if (std::strcmp(configuration.configuration_owner_node_id,
                    this->server_->platform_identity().node_id) != 0) {
      http_response::send_error_info(
          request, 403, "site_configuration_owner_required",
          "site_configuration_owner_required", "error", "{}");
      return;
    }
    if (!request->hasArg("participantId")) {
      http_response::send_error_info(request, 400, "invalid_argument",
                                     "invalid_argument", "error", "{}");
      return;
    }
  }
  ParticipantEnrollmentResult result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
  this->server_->record_httpd_stack_watermark(
      "/api/platform/participants", "pre_mutation");
  if (!this->execute_participant_action_(request, action.c_str(), &result))
    return;
  this->server_->record_httpd_stack_watermark(
      "/api/platform/participants", "post_mutation");

  if (result != ParticipantEnrollmentResult::OK) {
    send_enrollment_error(request, result);
    return;
  }
  this->send_participants_(request, action.c_str());
}

bool PlatformHandler::execute_participant_action_(
    AsyncWebServerRequest *request, const char *action,
    ParticipantEnrollmentResult *result) {
  auto &enrollment = this->server_->participant_enrollment();
  const bool transient_enrollment_action =
      std::strcmp(action, "open") == 0 ||
      std::strcmp(action, "close") == 0 ||
      std::strcmp(action, "stage") == 0;
  if (transient_enrollment_action) {
    auto lease = this->server_->participant_operation().try_acquire();
    if (!lease.acquired()) {
      PlatformParticipantOperationOutcome outcome{};
      outcome.result = PlatformParticipantOperationResult::BUSY;
      return apply_platform_operation_outcome(request, outcome, result);
    }
    if (std::strcmp(action, "open") == 0)
      return this->execute_open_action_(request, result);
    if (std::strcmp(action, "close") == 0) {
      enrollment.close_window();
      *result = ParticipantEnrollmentResult::OK;
      return true;
    }
    return this->execute_stage_action_(request, result);
  }
  if (std::strcmp(action, "approve") == 0) {
    const uint32_t now_ms = millis();
    const ParticipantRecord *candidate = enrollment.pending_candidate(now_ms);
    const ParticipantTrust trust =
        candidate == nullptr
            ? ParticipantTrust::UNKNOWN
            : (candidate->type == ParticipantType::SATELLITE
                   ? ParticipantTrust::SATELLITE
                   : ParticipantTrust::COMMUNITY_UNVERIFIED);
    PlatformParticipantOperationRequest operation{};
    operation.action = PlatformParticipantOperationAction::APPROVE;
    operation.approval_trust = trust;
    operation.now_ms = now_ms;
    return apply_platform_operation_outcome(
        request, this->server_->participant_operation().execute(
                     *this->server_, operation),
        result);
  }
  if (!request->hasArg("participantId")) {
    send_enrollment_error(request, ParticipantEnrollmentResult::INVALID_ARGUMENT);
    return false;
  }
  const auto participant_id = request->arg("participantId");
  if (std::strcmp(action, "configure") == 0 ||
      std::strcmp(action, "reconfigure") == 0)
    return this->execute_configure_action_(
        request, participant_id.c_str(), result,
        std::strcmp(action, "reconfigure") == 0);
  return this->execute_existing_participant_action_(request, action,
                                                    participant_id.c_str(), result);
}

bool PlatformHandler::execute_open_action_(
    AsyncWebServerRequest *request, ParticipantEnrollmentResult *result) {
  uint32_t duration_seconds = DEFAULT_PAIRING_WINDOW_SECONDS;
  if ((request->hasArg("durationSeconds") &&
       !parse_uint32_arg(request, "durationSeconds", &duration_seconds)) ||
      duration_seconds < MIN_PAIRING_WINDOW_SECONDS ||
      duration_seconds > MAX_PAIRING_WINDOW_SECONDS) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }
  *result = this->server_->participant_enrollment().open_window(
      millis(), duration_seconds * 1000U);
  return true;
}

bool PlatformHandler::execute_stage_action_(
    AsyncWebServerRequest *request, ParticipantEnrollmentResult *result) {
  if (!request->hasArg("participantId") ||
      !request->hasArg("participantType") ||
      !request->hasArg("protocolVersion") || !request->hasArg("product")) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }
  uint32_t protocol_version = 0;
  if (!parse_uint32_arg(request, "protocolVersion", &protocol_version) ||
      protocol_version != 1) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }
  const auto participant_type = request->arg("participantType");
  const auto product = request->arg("product");
  ParticipantType type = ParticipantType::NAMO_NODE;
  if (participant_type == "namo_node") {
    if (product != "namo-spatial-sensor") {
      *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
      return true;
    }
  } else if (participant_type == "satellite") {
    type = ParticipantType::SATELLITE;
    if (product.length() == 0 || product.length() > 64) {
      *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
      return true;
    }
  } else {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }
  const auto participant_id = request->arg("participantId");
  if (std::strcmp(participant_id.c_str(),
                  this->server_->platform_identity().node_id) == 0) {
    *result = ParticipantEnrollmentResult::ALREADY_REGISTERED;
    return true;
  }
  *result = this->server_->participant_enrollment().observe_candidate(
      this->server_->participant_registry_snapshot(), participant_id.c_str(), type,
      millis());
  return true;
}

bool PlatformHandler::execute_configure_action_(
    AsyncWebServerRequest *request, const char *participant_id,
    ParticipantEnrollmentResult *result, bool preserve_active) {
  if (this->response_workspace_ == nullptr) {
    http_response::send_error_info(
        request, 503, "platform_response_unavailable",
        "platform_response_unavailable", "error",
        "{\"resource\":\"platform_response_workspace\"}");
    return false;
  }
  const bool readiness_ready =
      this->server_->participant_readiness_persistence_ready();
  if (!readiness_ready || !request->hasArg("pointTracks") ||
      !request->hasArg("presence") || !request->hasArg("motion") ||
      !request->hasArg("maxTargets") || !request->hasArg("floorplanId") ||
      !request->hasArg("spaceId") || !request->hasArg("groupId") ||
      !request->hasArg("geometryVersion") ||
      !request->hasArg("localPoseVersion") ||
      !request->hasArg("poseVersion") ||
      !request->hasArg("calibrationVersion")) {
    const char *code = readiness_ready ? "participant_configuration_required"
                                       : "participant_readiness_unavailable";
    http_response::send_error_info(request, readiness_ready ? 400 : 503, code, code,
                                   "error", "{}");
    return false;
  }

  auto &scratch = this->response_workspace_->participant_configure;
  scratch = PlatformParticipantConfigureScratch{};
  const bool transform_requested =
      request->hasArg("originXMillimeters") ||
      request->hasArg("originYMillimeters") ||
      request->hasArg("rotationCentiDegrees") ||
      request->hasArg("residualMillimeters") ||
      request->hasArg("calibrationSamples");
  if (!parse_bool_arg(request, "pointTracks", &scratch.point_tracks) ||
      !parse_optional_bool_arg(request, "velocity", &scratch.velocity) ||
      !parse_optional_bool_arg(request, "positionUncertainty",
                               &scratch.uncertainty) ||
      !parse_optional_bool_arg(request, "range", &scratch.range) ||
      !parse_bool_arg(request, "presence", &scratch.presence) ||
      !parse_bool_arg(request, "motion", &scratch.motion) ||
      !parse_optional_bool_arg(request, "zonePresence",
                               &scratch.zone_presence) ||
      !parse_optional_bool_arg(request, "bridge", &scratch.bridge) ||
      !parse_optional_bool_arg(request, "automaticGroup",
                               &scratch.automatic_group) ||
      !parse_uint32_arg(request, "maxTargets", &scratch.max_targets) ||
      scratch.max_targets > UINT8_MAX ||
      !parse_uint32_arg(request, "geometryVersion",
                        &scratch.geometry_version) ||
      !parse_uint32_arg(request, "localPoseVersion",
                        &scratch.local_pose_version) ||
      !parse_uint32_arg(request, "poseVersion", &scratch.pose_version) ||
      !parse_uint32_arg(request, "calibrationVersion",
                        &scratch.calibration_version)) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }

  auto assignment_id = request->arg("floorplanId");
  if (assignment_id.empty() ||
      assignment_id.length() >
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }
  copy_arg(scratch.config.floorplan_id, sizeof(scratch.config.floorplan_id),
           assignment_id.c_str());
  assignment_id = request->arg("spaceId");
  if (assignment_id.empty() ||
      assignment_id.length() >
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }
  copy_arg(scratch.config.space_id, sizeof(scratch.config.space_id),
           assignment_id.c_str());
  assignment_id = request->arg("groupId");
  if (assignment_id.empty() ||
      assignment_id.length() >
          ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }
  copy_arg(scratch.config.group_id, sizeof(scratch.config.group_id),
           assignment_id.c_str());

  if (transform_requested &&
      (!scratch.point_tracks || scratch.calibration_version == 0 ||
       !parse_int32_arg(request, "originXMillimeters",
                        &scratch.origin_x_mm) ||
       !parse_int32_arg(request, "originYMillimeters",
                        &scratch.origin_y_mm) ||
       !parse_int32_arg(request, "rotationCentiDegrees",
                        &scratch.rotation_cdeg) ||
       scratch.rotation_cdeg < INT16_MIN ||
       scratch.rotation_cdeg > INT16_MAX ||
       !parse_uint32_arg(request, "residualMillimeters",
                         &scratch.residual_mm) ||
       scratch.residual_mm > UINT16_MAX ||
       !parse_uint32_arg(request, "calibrationSamples",
                         &scratch.calibration_samples) ||
       scratch.calibration_samples > UINT8_MAX)) {
    *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
    return true;
  }

  auto &config = scratch.config;
  config.capability_flags =
      (scratch.point_tracks ? PARTICIPANT_CAP_POINT_TRACKS : 0) |
      (scratch.velocity ? PARTICIPANT_CAP_VELOCITY : 0) |
      (scratch.uncertainty ? PARTICIPANT_CAP_POSITION_UNCERTAINTY : 0) |
      (scratch.range ? PARTICIPANT_CAP_RANGE : 0) |
      (scratch.presence ? PARTICIPANT_CAP_PRESENCE : 0) |
      (scratch.motion ? PARTICIPANT_CAP_MOTION : 0) |
      (scratch.zone_presence ? PARTICIPANT_CAP_ZONE_PRESENCE : 0) |
      (scratch.bridge ? PARTICIPANT_CAP_BRIDGE : 0);
  config.max_targets = static_cast<uint8_t>(scratch.max_targets);
  config.geometry_version = scratch.geometry_version;
  config.local_pose_version = scratch.local_pose_version;
  config.pose_version = scratch.pose_version;
  config.calibration_version = scratch.calibration_version;
  if (transform_requested) {
    config.transform.origin_x_mm = scratch.origin_x_mm;
    config.transform.origin_y_mm = scratch.origin_y_mm;
    config.transform.rotation_cdeg =
        static_cast<int16_t>(scratch.rotation_cdeg);
    config.transform.residual_mm =
        static_cast<uint16_t>(scratch.residual_mm);
    config.transform.sample_count =
        static_cast<uint8_t>(scratch.calibration_samples);
    config.transform.valid = true;
  } else {
    const auto *existing =
        this->server_->participant_readiness_snapshot().registry.find(
            participant_id);
    if (existing != nullptr &&
        existing->config.calibration_version ==
            scratch.calibration_version)
      config.transform = existing->config.transform;
  }

  if (scratch.automatic_group) {
    if (!scratch.point_tracks || !this->server_->site_configuration_ready()) {
      *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
      return true;
    }
    const auto &configuration = this->server_->site_configuration_snapshot();
    const bool local_configuration_owner =
        std::strcmp(configuration.configuration_owner_node_id,
                    this->server_->platform_identity().node_id) == 0;
    if (local_configuration_owner) {
      AutomaticSpatialGroupDecision decision{};
      const auto group_result = resolve_automatic_spatial_group(
          this->server_->platform_identity(), configuration, participant_id,
          config.space_id, config.group_id, &decision);
      if (group_result == AutomaticSpatialGroupResult::GROUP_FULL) {
        http_response::send_error_info(request, 409, "group_full", "group_full",
                                       "warning", "{}");
        return false;
      }
      if (group_result != AutomaticSpatialGroupResult::OK) {
        *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
        return true;
      }
      copy_arg(config.group_id, sizeof(config.group_id), decision.group_id);
    }
  }

  PlatformParticipantOperationRequest operation{};
  operation.action =
      preserve_active ? PlatformParticipantOperationAction::RECONFIGURE
                      : PlatformParticipantOperationAction::CONFIGURE;
  operation.participant_id = participant_id;
  operation.configuration = &config;
  operation.now_ms = millis();
  operation.automatic_group = scratch.automatic_group;
  return apply_platform_operation_outcome(
      request,
      this->server_->participant_operation().execute(*this->server_, operation),
      result);
}

bool PlatformHandler::execute_existing_participant_action_(
    AsyncWebServerRequest *request, const char *action, const char *participant_id,
    ParticipantEnrollmentResult *result) {
  PlatformParticipantOperationRequest operation{};
  operation.participant_id = participant_id;
  operation.now_ms = millis();
  if (std::strcmp(action, "activate") == 0) {
    if (!parse_optional_bool_arg(request, "automaticGroup",
                                 &operation.automatic_group)) {
      *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
      return true;
    }
    if (!this->server_->participant_readiness_persistence_ready()) {
      http_response::send_error_info(request, 503,
                                     "participant_readiness_unavailable",
                                     "participant_readiness_unavailable", "error",
                                     "{}");
      return false;
    }
    const auto *participant =
        this->server_->participant_registry_snapshot().registry.find(
            participant_id);
    if (participant != nullptr &&
        participant->type == ParticipantType::NAMO_NODE) {
      if (!this->server_->site_configuration_ready()) {
        http_response::send_error_info(
            request, 503, "site_configuration_unavailable",
            "site_configuration_unavailable", "error", "{}");
        return false;
      }
      const auto *endpoint =
          this->server_->participant_endpoint(participant_id);
      const bool endpoint_online = ParticipantEndpointCache::is_online(
          endpoint, operation.now_ms);
      const auto authority_gate = evaluate_site_activation_authority_gate(
          this->server_->site_authority_snapshot(operation.now_ms),
          this->server_->site_configuration_snapshot(), endpoint,
          endpoint_online);
      if (authority_gate != SiteActivationAuthorityGateResult::OK) {
        const char *code =
            authority_gate == SiteActivationAuthorityGateResult::CONFLICT
                ? "site_configuration_conflict"
                : "site_configuration_continuity_unavailable";
        http_response::send_error_info(
            request, 409, code, code,
            authority_gate == SiteActivationAuthorityGateResult::CONFLICT
                ? "error"
                : "warning",
            "{}");
        return false;
      }
      const auto admission = validate_namo_node_activation(
          this->server_->site_configuration_snapshot(),
          this->server_->participant_readiness_snapshot().registry.find(
              participant_id),
          participant_id, endpoint_online,
          endpoint != nullptr && endpoint->floorplan_replica_ready,
          endpoint != nullptr ? endpoint->site_configuration_generation : 0,
          endpoint != nullptr ? endpoint->floorplan_geometry_version : 0,
          endpoint != nullptr ? endpoint->floorplan_config_hash : 0,
          endpoint != nullptr ? endpoint->floorplan_image_hash : 0);
      if (admission != SiteActivationAdmissionResult::OK) {
        const char *code =
            site_activation_admission_result_to_string(admission);
        http_response::send_error_info(request, 409, code, code, "error",
                                       "{}");
        return false;
      }
    }
    operation.action = PlatformParticipantOperationAction::ACTIVATE;
  } else if (std::strcmp(action, "quarantine") == 0) {
    operation.action = PlatformParticipantOperationAction::QUARANTINE;
  } else if (std::strcmp(action, "revoke") == 0) {
    operation.action = PlatformParticipantOperationAction::REVOKE;
  } else if (std::strcmp(action, "forget") == 0) {
    if (!this->server_->participant_readiness_persistence_ready()) {
      http_response::send_error_info(request, 503,
                                     "participant_readiness_unavailable",
                                     "participant_readiness_unavailable", "error",
                                     "{}");
      return false;
    }
    operation.action = PlatformParticipantOperationAction::FORGET;
  } else if (std::strcmp(action, "set_community_leader") == 0) {
    bool eligible = false;
    if (!parse_bool_arg(request, "eligible", &eligible)) {
      *result = ParticipantEnrollmentResult::INVALID_ARGUMENT;
      return true;
    }
    operation.action =
        PlatformParticipantOperationAction::SET_COMMUNITY_LEADER;
    operation.community_leader_eligible = eligible;
  } else {
    http_response::send_error_info(request, 400, "invalid_action", "invalid_action",
                                   "error", "{}");
    return false;
  }
  return apply_platform_operation_outcome(
      request,
      this->server_->participant_operation().execute(*this->server_, operation),
      result);
}

void PlatformHandler::send_participants_(AsyncWebServerRequest *request,
                                         const char *action) {
#ifdef USE_ESP32
  if (this->response_workspace_ == nullptr) {
    http_response::send_error_info(
        request, 503, "platform_response_unavailable",
        "platform_response_unavailable", "error",
        "{\"resource\":\"platform_response_workspace\"}");
    return;
  }
  const bool ready = this->server_->participant_registry_persistence_ready();
  const auto &snapshot = this->server_->participant_registry_snapshot();
  const auto &registry = snapshot.registry;
  const bool readiness_ready =
      this->server_->participant_readiness_persistence_ready();
  const auto &readiness_snapshot =
      this->server_->participant_readiness_snapshot();
  const bool credentials_ready =
      this->server_->participant_credential_persistence_ready();
  const auto &credential_snapshot =
      this->server_->participant_credential_snapshot();
  const SiteConfigurationSnapshot *site_configuration =
      this->server_->site_configuration_ready()
          ? &this->server_->site_configuration_snapshot()
          : nullptr;
  const bool management_allowed =
      site_configuration != nullptr &&
      std::strcmp(site_configuration->configuration_owner_node_id,
                  this->server_->platform_identity().node_id) == 0;
  const std::size_t projected_participant_count =
      participant_projection_count(
          this->server_->platform_identity().node_id, site_configuration,
          registry);
  const uint32_t now_ms = millis();
  auto &enrollment = this->server_->participant_enrollment();
  const bool pairing_enabled =
      ready && this->server_->platform_identity_ready() &&
      this->server_->standalone_site_reconciliation_ready_for_pairing() &&
      enrollment.window_open(now_ms);
  const ParticipantRecord *pending = pairing_enabled
                                         ? enrollment.pending_candidate(now_ms)
                                         : nullptr;

  httpd_req_t *raw_request = *request;
  httpd_resp_set_status(raw_request, HTTPD_200);
  httpd_resp_set_type(raw_request, "application/json");
  httpd_resp_set_hdr(raw_request, "Accept-Ranges", "none");
  httpd_resp_set_hdr(raw_request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(raw_request, "Cache-Control", "no-store");
  PlatformJsonChunkWriter writer(raw_request, this->response_workspace_->chunk,
                                 sizeof(this->response_workspace_->chunk));

  writer.write("{\"ok\":true");
  if (action != nullptr) {
    writer.write(",\"action\":");
    writer.json_string(action);
  }
  writer.write(",\"registry\":{\"ready\":");
  writer.boolean(ready);
  writer.write(",\"generation\":");
  writer.unsigned_number(ready ? snapshot.generation : 0U);
  writer.write(",\"remoteCount\":");
  writer.unsigned_number(ready ? registry.size() : 0U);
  writer.write(",\"activeRemoteCount\":");
  writer.unsigned_number(ready ? registry.active_count() : 0U);
  writer.write(",\"maxRemoteParticipants\":");
  writer.unsigned_number(ParticipantRegistry::MAX_REMOTE_PARTICIPANTS);
  writer.write(",\"maxParticipants\":");
  writer.unsigned_number(ParticipantRegistry::MAX_PARTICIPANTS);
  writer.write(",\"projectedRemoteCount\":");
  writer.unsigned_number(projected_participant_count);
  writer.write("},\"managementAllowed\":");
  writer.boolean(management_allowed);
  writer.write(",\"pairing\":{\"supported\":true,\"enabled\":");
  writer.boolean(pairing_enabled);
  writer.write(",\"remainingMs\":");
  writer.unsigned_number(
      pairing_enabled ? enrollment.window_remaining_ms(now_ms) : 0U);
  writer.write(",\"maxDurationMs\":");
  writer.unsigned_number(ParticipantEnrollment::MAX_PAIRING_WINDOW_MS);
  writer.write("},\"readiness\":{\"ready\":");
  writer.boolean(readiness_ready);
  writer.write(",\"generation\":");
  writer.unsigned_number(readiness_ready ? readiness_snapshot.generation : 0U);
  writer.write(",\"configuredCount\":");
  writer.unsigned_number(readiness_ready ? readiness_snapshot.registry.size()
                                         : 0U);
  writer.write("},\"credentials\":{\"ready\":");
  writer.boolean(credentials_ready);
  writer.write(",\"generation\":");
  writer.unsigned_number(credentials_ready ? credential_snapshot.generation
                                            : 0U);
  writer.write(",\"provisionedCount\":");
  writer.unsigned_number(credentials_ready
                             ? credential_snapshot.registry.size()
                             : 0U);
  writer.write("},\"siteReconciliation\":{\"state\":");
  writer.json_string(standalone_site_reconciliation_state_to_string(
      this->server_->standalone_site_reconciliation_state()));
  writer.write(",\"result\":");
  writer.json_string(standalone_site_reconciliation_result_to_string(
      this->server_->standalone_site_reconciliation_result()));
  writer.write("}");

  const auto *transport = this->server_->observation_transport_diagnostics();
  if (transport == nullptr) {
    writer.write(
        ",\"shadowTransport\":{\"supported\":true,\"smokeMode\":false,"
        "\"socketReady\":false,\"port\":0,\"received\":0,"
        "\"authenticated\":0,\"admitted\":0,\"rejected\":0,"
        "\"rateLimited\":0,\"pending\":0,\"maxProcessUs\":0,"
        "\"lastResult\":\"idle\",\"lastAdmission\":\"ok\","
        "\"senderEnabled\":false,\"sent\":0,\"sendSkipped\":0,"
        "\"maxEncodeUs\":0,\"maxSendUs\":0,\"sendTarget\":null,"
        "\"fusionSnapshot\":{\"received\":0,\"admitted\":0,\"sent\":0,"
        "\"rejected\":0,\"expired\":0,\"source\":null,"
        "\"lastAdmission\":\"ok\"},\"siteSpaceSummary\":{"
        "\"received\":0,\"admitted\":0,\"sent\":0,\"rejected\":0,"
        "\"lastAdmission\":\"ok\"},"
        "\"lastSendResult\":\"idle\"}");
  } else {
    writer.write(",\"shadowTransport\":{\"supported\":");
    writer.boolean(transport->supported);
    writer.write(",\"smokeMode\":");
    writer.boolean(transport->smoke_mode);
    writer.write(",\"socketReady\":");
    writer.boolean(transport->socket_ready);
    writer.write(",\"port\":");
    writer.unsigned_number(transport->port);
    writer.write(",\"received\":");
    writer.unsigned_number(transport->received);
    writer.write(",\"authenticated\":");
    writer.unsigned_number(transport->authenticated);
    writer.write(",\"admitted\":");
    writer.unsigned_number(transport->admitted);
    writer.write(",\"rejected\":");
    writer.unsigned_number(transport->rejected);
    writer.write(",\"rateLimited\":");
    writer.unsigned_number(transport->rate_limited);
    writer.write(",\"pending\":");
    writer.unsigned_number(transport->pending);
    writer.write(",\"maxProcessUs\":");
    writer.unsigned_number(transport->max_process_us);
    writer.write(",\"lastResult\":");
    writer.json_string(
        observation_transport_result_to_string(transport->last_result));
    writer.write(",\"lastAdmission\":");
    writer.json_string(
        observation_admission_result_to_string(transport->last_admission));
    writer.write(",\"senderEnabled\":");
    writer.boolean(transport->sender_enabled);
    writer.write(",\"sent\":");
    writer.unsigned_number(transport->sent);
    writer.write(",\"sendSkipped\":");
    writer.unsigned_number(transport->send_skipped);
    writer.write(",\"maxEncodeUs\":");
    writer.unsigned_number(transport->max_encode_us);
    writer.write(",\"maxSendUs\":");
    writer.unsigned_number(transport->max_send_us);
    writer.write(",\"sendTarget\":");
    if (transport->send_target[0] == '\0')
      writer.write("null");
    else
      writer.json_string(transport->send_target);
    writer.write(",\"fusionSnapshot\":{\"received\":");
    writer.unsigned_number(transport->fusion_snapshot_received);
    writer.write(",\"admitted\":");
    writer.unsigned_number(transport->fusion_snapshot_admitted);
    writer.write(",\"sent\":");
    writer.unsigned_number(transport->fusion_snapshot_sent);
    writer.write(",\"rejected\":");
    writer.unsigned_number(transport->fusion_snapshot_rejected);
    writer.write(",\"expired\":");
    writer.unsigned_number(transport->fusion_snapshot_expired);
    writer.write(",\"source\":");
    if (transport->fusion_snapshot_source[0] == '\0')
      writer.write("null");
    else
      writer.json_string(transport->fusion_snapshot_source);
    writer.write(",\"lastAdmission\":");
    writer.json_string(fusion_snapshot_admission_result_to_string(
        transport->last_fusion_snapshot_admission));
    writer.write("},\"siteSpaceSummary\":{\"received\":");
    writer.unsigned_number(transport->site_space_summary_received);
    writer.write(",\"admitted\":");
    writer.unsigned_number(transport->site_space_summary_admitted);
    writer.write(",\"sent\":");
    writer.unsigned_number(transport->site_space_summary_sent);
    writer.write(",\"rejected\":");
    writer.unsigned_number(transport->site_space_summary_rejected);
    writer.write(",\"lastAdmission\":");
    writer.json_string(site_space_summary_admission_result_to_string(
        transport->last_site_space_summary_admission));
    writer.write("},\"lastSendResult\":");
    writer.json_string(
        observation_send_result_to_string(transport->last_send_result));
    writer.write("}");
  }
  const auto *heartbeat = this->server_->site_heartbeat_diagnostics();
  if (heartbeat == nullptr) {
    writer.write(
        ",\"heartbeat\":{\"supported\":true,\"socketReady\":false,"
        "\"port\":0,\"endpoints\":0,\"onlineEndpoints\":0,"
        "\"received\":0,\"authenticated\":0,\"sent\":0,\"rejected\":0,"
        "\"addressChanges\":0,\"lastResult\":\"idle\"}");
  } else {
    writer.write(",\"heartbeat\":{\"supported\":");
    writer.boolean(heartbeat->supported);
    writer.write(",\"socketReady\":");
    writer.boolean(heartbeat->socket_ready);
    writer.write(",\"port\":");
    writer.unsigned_number(heartbeat->port);
    writer.write(",\"endpoints\":");
    writer.unsigned_number(heartbeat->endpoints);
    writer.write(",\"onlineEndpoints\":");
    writer.unsigned_number(heartbeat->online_endpoints);
    writer.write(",\"received\":");
    writer.unsigned_number(heartbeat->received);
    writer.write(",\"authenticated\":");
    writer.unsigned_number(heartbeat->authenticated);
    writer.write(",\"sent\":");
    writer.unsigned_number(heartbeat->sent);
    writer.write(",\"rejected\":");
    writer.unsigned_number(heartbeat->rejected);
    writer.write(",\"addressChanges\":");
    writer.unsigned_number(heartbeat->address_changes);
    writer.write(",\"lastResult\":");
    writer.json_string(site_heartbeat_result_to_string(heartbeat->last_result));
    writer.write("}");
  }
  this->server_->record_httpd_stack_watermark(
      "/api/platform/participants", "diagnostics");

  writer.write(",\"pendingCandidate\":");
  if (pending == nullptr) {
    writer.write("null");
  } else {
    writer.write("{\"participantId\":");
    writer.json_string(pending->participant_id);
    writer.write(",\"participantType\":");
    writer.json_string(participant_type_string(pending->type));
    writer.write("}");
  }
  writer.write(",\"participants\":[");
  bool first = true;
  for (std::size_t index = 0;
       index < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++index) {
      ParticipantProjection projection{};
      if (!participant_projection_at(
              this->server_->platform_identity().node_id,
              site_configuration, registry, index, &projection))
        break;
      const char *participant_id = projection.participant_id();
      if (participant_id == nullptr)
        continue;
      if (!first)
        writer.write(",");
      first = false;
      const ParticipantReadinessRecord *profile =
          readiness_ready
              ? readiness_snapshot.registry.find(participant_id)
              : nullptr;
      const SiteConfigurationMember *canonical =
          projection.canonical_member;
      const bool canonical_profile_ready =
          canonical != nullptr && canonical->spatial_profile.committed;
      const uint8_t missing = canonical_profile_ready
                                  ? PARTICIPANT_MISSING_NONE
                                  : ParticipantReadinessRegistry::
                                        missing_requirements(profile);
      const ParticipantCredentialRecord *credential =
          credentials_ready
              ? credential_snapshot.registry.find(participant_id)
              : nullptr;
      const ParticipantEndpoint *endpoint =
          this->server_->participant_endpoint(participant_id);
      writer.write("{\"participantId\":");
      writer.json_string(participant_id);
      writer.write(",\"participantType\":");
      writer.json_string(participant_type_string(projection.type()));
      writer.write(",\"trust\":");
      writer.json_string(participant_trust_string(projection.trust()));
      writer.write(",\"membership\":");
      writer.json_string(
          participant_membership_string(projection.membership()));
      writer.write(",\"canonicalMembership\":");
      if (const auto *membership = projection.canonical_membership()) {
        writer.json_string(participant_membership_string(*membership));
      } else {
        writer.write("null");
      }
      writer.write(",\"directMembership\":");
      if (const auto *membership = projection.direct_membership()) {
        writer.json_string(participant_membership_string(*membership));
      } else {
        writer.write("null");
      }
      writer.write(",\"leaderEligible\":");
      writer.boolean(projection.leader_eligible());
      writer.write(",\"canonicalMember\":");
      writer.boolean(projection.canonical());
      writer.write(",\"directPair\":");
      writer.boolean(projection.directly_paired());
      writer.write(",\"capabilities\":");
      if (profile == nullptr) {
        writer.write("null");
      } else {
        const uint16_t flags = profile->config.capability_flags;
        writer.write("{\"pointTracks\":");
        writer.boolean((flags & PARTICIPANT_CAP_POINT_TRACKS) != 0);
        writer.write(",\"velocity\":");
        writer.boolean((flags & PARTICIPANT_CAP_VELOCITY) != 0);
        writer.write(",\"positionUncertainty\":");
        writer.boolean((flags & PARTICIPANT_CAP_POSITION_UNCERTAINTY) != 0);
        writer.write(",\"range\":");
        writer.boolean((flags & PARTICIPANT_CAP_RANGE) != 0);
        writer.write(",\"presence\":");
        writer.boolean((flags & PARTICIPANT_CAP_PRESENCE) != 0);
        writer.write(",\"motion\":");
        writer.boolean((flags & PARTICIPANT_CAP_MOTION) != 0);
        writer.write(",\"zonePresence\":");
        writer.boolean((flags & PARTICIPANT_CAP_ZONE_PRESENCE) != 0);
        writer.write(",\"bridge\":");
        writer.boolean((flags & PARTICIPANT_CAP_BRIDGE) != 0);
        writer.write(",\"maxTargets\":");
        writer.unsigned_number(profile->config.max_targets);
        writer.write("}");
      }
      writer.write(",\"assignment\":");
      if (!canonical_profile_ready && profile == nullptr) {
        writer.write("null");
      } else {
        const char *floorplan_id =
            canonical_profile_ready && site_configuration != nullptr
                ? site_configuration->floorplan.floorplan_id
                : profile->config.floorplan_id;
        const char *space_id = canonical_profile_ready
                                   ? canonical->spatial_profile.space_id
                                   : profile->config.space_id;
        const char *group_id = canonical_profile_ready
                                   ? canonical->spatial_profile.group_id
                                   : profile->config.group_id;
        const uint32_t geometry_version =
            canonical_profile_ready && site_configuration != nullptr
                ? site_configuration->floorplan.geometry_version
                : profile->config.geometry_version;
        const uint32_t local_pose_version =
            profile != nullptr
                ? profile->config.local_pose_version
                : (endpoint != nullptr ? endpoint->radar_pose_version : 0U);
        const uint32_t pose_version =
            canonical_profile_ready ? canonical->spatial_profile.pose_version
                                    : profile->config.pose_version;
        const uint32_t calibration_version =
            canonical_profile_ready
                ? canonical->spatial_profile.calibration_version
                : profile->config.calibration_version;
        const SpatialTransform2D &transform =
            canonical_profile_ready ? canonical->spatial_profile.transform
                                    : profile->config.transform;
        write_participant_assignment(
            writer, floorplan_id, space_id, group_id, geometry_version,
            local_pose_version, pose_version, calibration_version, transform);
      }
      writer.write(",\"directAssignment\":");
      if (profile == nullptr) {
        writer.write("null");
      } else {
        write_participant_assignment(
            writer, profile->config.floorplan_id, profile->config.space_id,
            profile->config.group_id, profile->config.geometry_version,
            profile->config.local_pose_version, profile->config.pose_version,
            profile->config.calibration_version, profile->config.transform);
      }
      writer.write(",\"credential\":{\"provisioned\":");
      writer.boolean(credential != nullptr);
      writer.write(",\"enabled\":");
      writer.boolean(credential != nullptr && credential->enabled);
      writer.write(",\"keyVersion\":");
      writer.unsigned_number(credential != nullptr ? credential->key_version
                                                    : 0U);
      writer.write("},\"endpoint\":");
      if (endpoint == nullptr || endpoint->ipv4 == 0) {
        writer.write("null");
      } else {
        char address[16]{};
        format_ipv4(endpoint->ipv4, address);
        const uint32_t last_seen_age =
            endpoint->authenticated
                ? static_cast<uint32_t>(now_ms - endpoint->authenticated_at_ms)
                : 0U;
        const bool online =
            ParticipantEndpointCache::is_online(endpoint, now_ms);
        writer.write("{\"address\":");
        writer.json_string(address);
        writer.write(",\"port\":");
        writer.unsigned_number(endpoint->port);
        writer.write(",\"authenticated\":");
        writer.boolean(endpoint->authenticated);
        writer.write(",\"online\":");
        writer.boolean(online);
        writer.write(",\"lastSeenAgeMs\":");
        writer.unsigned_number(last_seen_age);
        writer.write(",\"siteConfigurationAuthorityEpoch\":");
        writer.unsigned_number(endpoint->site_configuration_authority_epoch);
        writer.write(",\"siteConfigurationGeneration\":");
        writer.unsigned_number(endpoint->site_configuration_generation);
        writer.write(",\"siteConfigurationDigest\":\"");
        writer.fixed_hex(endpoint->site_configuration_digest, 16);
        writer.write("\",\"siteDirectoryRevision\":");
        writer.unsigned_number(endpoint->site_directory_revision);
        writer.write(",\"siteDirectoryDigest\":");
        writer.unsigned_number(endpoint->site_directory_digest);
        writer.write(",\"floorplanGeometryVersion\":");
        writer.unsigned_number(endpoint->floorplan_geometry_version);
        writer.write(",\"floorplanConfigHash\":");
        writer.unsigned_number(endpoint->floorplan_config_hash);
        writer.write(",\"floorplanImageHash\":");
        writer.unsigned_number(endpoint->floorplan_image_hash);
        writer.write(",\"radarPoseVersion\":");
        writer.unsigned_number(endpoint->radar_pose_version);
        writer.write(",\"floorplanReplicaReady\":");
        writer.boolean(endpoint->floorplan_replica_ready);
        writer.write(",\"heartbeatWireVersion\":");
        writer.unsigned_number(endpoint->heartbeat_wire_version);
        writer.write(",\"siteCredentialsComplete\":");
        writer.boolean(endpoint->site_credentials_complete);
        writer.write("}");
      }
      writer.write(",\"readiness\":{\"ready\":");
      writer.boolean(missing == PARTICIPANT_MISSING_NONE);
      writer.write(",\"missing\":[");
      bool first_missing = true;
      const auto append_missing = [&](uint8_t flag, const char *name) {
        if ((missing & flag) == 0)
          return;
        if (!first_missing)
          writer.write(",");
        first_missing = false;
        writer.json_string(name);
      };
      append_missing(PARTICIPANT_MISSING_CAPABILITIES, "capabilities");
      append_missing(PARTICIPANT_MISSING_ASSIGNMENT, "assignment");
      append_missing(PARTICIPANT_MISSING_POSE, "pose");
      append_missing(PARTICIPANT_MISSING_CALIBRATION, "calibration");
      writer.write("]}}");
      this->server_->record_httpd_stack_watermark(
          "/api/platform/participants", "participant");
  }
  writer.write("]}");
  const bool response_ok = writer.finish();
  this->server_->record_httpd_stack_watermark(
      "/api/platform/participants", "flush");
  if (!response_ok)
    ESP_LOGW(TAG, "Participant response streaming failed");
#else
  http_response::send_error_info(
      request, 503, "platform_response_unavailable",
      "platform_response_unavailable", "error",
      "{\"resource\":\"platform_response_workspace\"}");
#endif
}

}  // namespace radar_api_server
}  // namespace esphome
