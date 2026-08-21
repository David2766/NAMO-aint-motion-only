#include "participant_discovery.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(USE_ESP32) && defined(USE_MDNS)
#include <mdns.h>
#include <esp_netif_ip_addr.h>
#include "esphome/core/log.h"
#endif

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t CURRENT_PROTOCOL_VERSION = 1;
constexpr uint32_t DISCOVERY_TIMEOUT_MS = 3000;
constexpr uint32_t QUERY_TIMEOUT_MS = 1800;
constexpr uint32_t QUERY_START_TIMEOUT_MS = 1000;
constexpr uint32_t QUERY_RETRY_MS = 100;
constexpr uint32_t ADVERTISEMENT_RETRY_MS = 1000;
constexpr std::size_t MAX_MDNS_RESULTS =
    ParticipantRegistry::MAX_REMOTE_PARTICIPANTS;

#if defined(USE_ESP32) && defined(USE_MDNS)
static const char *const TAG = "participant_discovery";
#endif

bool valid_identifier(const char *value) {
  if (value == nullptr || value[0] == '\0')
    return false;
  const std::size_t length = std::strlen(value);
  if (length > ParticipantRecord::MAX_ID_LENGTH)
    return false;
  for (std::size_t index = 0; index < length; ++index) {
    const unsigned char current = static_cast<unsigned char>(value[index]);
    if (!std::isalnum(current) && current != '-' && current != '_')
      return false;
  }
  return true;
}

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

void copy_text(char *target, std::size_t capacity, const char *source) {
  if (target == nullptr || capacity == 0)
    return;
  if (source == nullptr) {
    target[0] = '\0';
    return;
  }
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

#if defined(USE_ESP32) && defined(USE_MDNS)
const char *txt_value(const mdns_result_t *result, const char *key) {
  if (result == nullptr || key == nullptr)
    return nullptr;
  for (std::size_t index = 0; index < result->txt_count; ++index) {
    if (result->txt[index].key != nullptr &&
        std::strcmp(result->txt[index].key, key) == 0)
      return result->txt[index].value;
  }
  return nullptr;
}

bool parse_uint8(const char *value, uint8_t *parsed) {
  if (value == nullptr || parsed == nullptr || value[0] == '\0')
    return false;
  char *end = nullptr;
  const unsigned long number = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || number > UINT8_MAX)
    return false;
  *parsed = static_cast<uint8_t>(number);
  return true;
}

bool result_ipv4(const mdns_result_t *result,
                 char address[ParticipantDiscoveryCandidate::MAX_ADDRESS_LENGTH + 1]) {
  if (result == nullptr)
    return false;
  for (const mdns_ip_addr_t *item = result->addr; item != nullptr;
       item = item->next) {
    if (item->addr.type != ESP_IPADDR_TYPE_V4)
      continue;
    std::snprintf(address,
                  ParticipantDiscoveryCandidate::MAX_ADDRESS_LENGTH + 1,
                  IPSTR, IP2STR(&item->addr.u_addr.ip4));
    return true;
  }
  return false;
}
#endif

}  // namespace

const char *participant_discovery_result_to_string(
    ParticipantDiscoveryResult result) {
  switch (result) {
    case ParticipantDiscoveryResult::IDLE:
      return "idle";
    case ParticipantDiscoveryResult::OK:
      return "ok";
    case ParticipantDiscoveryResult::START_FAILED:
      return "start_failed";
  }
  return "start_failed";
}

void ParticipantDiscoveryResults::reset(const char *local_participant_id) {
  copy_text(this->local_participant_id_, sizeof(this->local_participant_id_),
            local_participant_id);
  for (auto &candidate : this->candidates_)
    candidate = ParticipantDiscoveryCandidate{};
}

bool ParticipantDiscoveryResults::consider(
    const char *participant_id, const char *participant_type,
    const char *product, const char *hostname, const char *address,
    uint16_t port, uint8_t protocol_minimum, uint8_t protocol_maximum,
    bool identity_ready) {
  if (!identity_ready || !valid_identifier(participant_id) ||
      std::strcmp(participant_id, this->local_participant_id_) == 0 ||
      participant_type == nullptr ||
      std::strcmp(participant_type, "namo_node") != 0 || product == nullptr ||
      std::strcmp(product, "namo-spatial-sensor") != 0 || hostname == nullptr ||
      address == nullptr ||
      address[0] == '\0' || port == 0 ||
      protocol_minimum > CURRENT_PROTOCOL_VERSION ||
      protocol_maximum < CURRENT_PROTOCOL_VERSION)
    return false;

  for (auto &candidate : this->candidates_) {
    if (candidate.occupied &&
        std::strcmp(candidate.participant_id, participant_id) == 0) {
      const bool changed = std::strcmp(candidate.hostname, hostname) != 0 ||
                           std::strcmp(candidate.address, address) != 0 ||
                           candidate.port != port ||
                           candidate.protocol_minimum != protocol_minimum ||
                           candidate.protocol_maximum != protocol_maximum;
      if (!changed)
        return false;
      copy_text(candidate.hostname, sizeof(candidate.hostname), hostname);
      copy_text(candidate.address, sizeof(candidate.address), address);
      candidate.port = port;
      candidate.protocol_minimum = protocol_minimum;
      candidate.protocol_maximum = protocol_maximum;
      return true;
    }
  }
  for (auto &candidate : this->candidates_) {
    if (candidate.occupied)
      continue;
    candidate.occupied = true;
    copy_text(candidate.participant_id, sizeof(candidate.participant_id),
              participant_id);
    copy_text(candidate.hostname, sizeof(candidate.hostname), hostname);
    copy_text(candidate.address, sizeof(candidate.address), address);
    candidate.port = port;
    candidate.protocol_minimum = protocol_minimum;
    candidate.protocol_maximum = protocol_maximum;
    return true;
  }
  return false;
}

std::size_t ParticipantDiscoveryResults::size() const {
  std::size_t count = 0;
  for (const auto &candidate : this->candidates_)
    count += candidate.occupied ? 1U : 0U;
  return count;
}

const ParticipantDiscoveryCandidate *ParticipantDiscoveryResults::candidate_at(
    std::size_t slot) const {
  if (slot >= this->candidates_.size() || !this->candidates_[slot].occupied)
    return nullptr;
  return &this->candidates_[slot];
}

bool ParticipantDiscoveryRuntime::configure_advertisement(
    const char *local_participant_id) {
  if (!valid_identifier(local_participant_id))
    return false;
  if (this->advertised_participant_id_[0] != '\0')
    return std::strcmp(this->advertised_participant_id_,
                       local_participant_id) == 0;
  copy_text(this->advertised_participant_id_,
            sizeof(this->advertised_participant_id_), local_participant_id);
  this->results_.reset(local_participant_id);
  return true;
}

bool ParticipantDiscoveryRuntime::advertisement_configured() const {
  return this->advertised_participant_id_[0] != '\0';
}

const char *ParticipantDiscoveryRuntime::advertised_participant_id() const {
  return this->advertised_participant_id_;
}

bool ParticipantDiscoveryRuntime::queue_scan(
    const char *local_participant_id) {
  return this->queue_scan_(local_participant_id, true);
}

bool ParticipantDiscoveryRuntime::queue_refresh(
    const char *local_participant_id) {
  return this->queue_scan_(local_participant_id, false);
}

bool ParticipantDiscoveryRuntime::queue_scan_(
    const char *local_participant_id, bool visible) {
  if (!valid_identifier(local_participant_id))
    return false;
  this->lock_();
  if (std::strcmp(this->advertised_participant_id_, local_participant_id) != 0) {
    this->unlock_();
    return false;
  }
  if (this->scan_pending_ || this->scan_active_) {
    if (visible && !this->scan_visible_) {
      this->scan_visible_ = true;
      this->published_.scanning = true;
      this->published_.result = ParticipantDiscoveryResult::IDLE;
      if (this->scan_active_) {
        const bool had_candidates = this->results_.size() != 0;
        this->results_.reset(this->advertised_participant_id_);
        this->published_.candidate_count = 0;
        if (had_candidates)
          ++this->published_.generation;
      }
    }
    this->unlock_();
    return visible;
  }
  this->scan_pending_ = true;
  this->scan_visible_ = visible;
  if (visible) {
    this->published_.scanning = true;
    this->published_.result = ParticipantDiscoveryResult::IDLE;
  }
  this->unlock_();
  return true;
}

void ParticipantDiscoveryRuntime::loop(uint32_t now_ms) {
#if defined(USE_ESP32) && defined(USE_MDNS)
  this->register_advertisement_(now_ms);
#endif
  this->update_scan_window_(now_ms);
}

void ParticipantDiscoveryRuntime::register_advertisement_(uint32_t now_ms) {
#if defined(USE_ESP32) && defined(USE_MDNS)
  if (this->advertisement_registered_ ||
      !this->advertisement_configured() ||
      (this->advertisement_next_attempt_ms_ != 0 &&
       static_cast<int32_t>(now_ms - this->advertisement_next_attempt_ms_) < 0))
    return;

  mdns_txt_item_t txt_records[] = {
      {"protocol_min", "1"},
      {"protocol_max", "1"},
      {"participant_id", this->advertised_participant_id_},
      {"participant_type", "namo_node"},
      {"product", "namo-spatial-sensor"},
      {"descriptor_path", "/api/platform/descriptor"},
      {"pairing", "manual"},
      {"identity_ready", "true"},
  };
  const esp_err_t result =
      mdns_service_add(nullptr, "_namo", "_tcp", 80, txt_records,
                       sizeof(txt_records) / sizeof(txt_records[0]));
  if (result == ESP_OK) {
    this->advertisement_registered_ = true;
    ESP_LOGI(TAG, "Registered _namo._tcp as %s",
             this->advertised_participant_id_);
    return;
  }

  this->advertisement_next_attempt_ms_ = now_ms + ADVERTISEMENT_RETRY_MS;
  if (!this->advertisement_failure_logged_) {
    ESP_LOGW(TAG, "Could not register _namo._tcp: %s",
             esp_err_to_name(result));
    this->advertisement_failure_logged_ = true;
  }
#else
  (void) now_ms;
#endif
}

void ParticipantDiscoveryRuntime::update_scan_window_(uint32_t now_ms) {
  bool begin_scan = false;
  this->lock_();
  if (this->scan_pending_) {
    this->scan_pending_ = false;
    this->scan_active_ = true;
    begin_scan = true;
    this->query_next_attempt_ms_ = now_ms;
    this->query_start_deadline_ms_ = now_ms + QUERY_START_TIMEOUT_MS;
    this->scan_deadline_ms_ = now_ms + DISCOVERY_TIMEOUT_MS;
  }
  const bool scanning = this->scan_active_;
  const bool visible = this->scan_visible_;
  this->unlock_();

  if (begin_scan && visible) {
    this->lock_();
    const bool had_candidates = this->results_.size() != 0;
    this->results_.reset(this->advertised_participant_id_);
    this->published_.candidate_count = 0;
    if (had_candidates)
      ++this->published_.generation;
    this->unlock_();
  }

  if (!scanning)
    return;

#if defined(USE_ESP32) && defined(USE_MDNS)
  if (this->poll_query_())
    return;

  if (this->query_ == nullptr &&
      !time_reached(now_ms, this->query_start_deadline_ms_) &&
      time_reached(now_ms, this->query_next_attempt_ms_))
    this->start_query_(now_ms);

  if (this->poll_query_())
    return;

  if (this->query_ == nullptr &&
      time_reached(now_ms, this->query_start_deadline_ms_)) {
    this->complete_scan_(ParticipantDiscoveryResult::START_FAILED);
    return;
  }

  if (time_reached(now_ms, this->scan_deadline_ms_))
    this->complete_scan_(ParticipantDiscoveryResult::START_FAILED);
#else
  if (time_reached(now_ms, this->scan_deadline_ms_))
    this->complete_scan_(ParticipantDiscoveryResult::START_FAILED);
#endif
}

#if defined(USE_ESP32) && defined(USE_MDNS)
bool ParticipantDiscoveryRuntime::start_query_(uint32_t now_ms) {
  this->query_ = mdns_query_async_new(
      nullptr, "_namo", "_tcp", MDNS_TYPE_PTR, QUERY_TIMEOUT_MS,
      MAX_MDNS_RESULTS, nullptr);
  if (this->query_ != nullptr) {
    this->query_failure_logged_ = false;
    return true;
  }

  this->query_next_attempt_ms_ = now_ms + QUERY_RETRY_MS;
  if (!this->query_failure_logged_) {
    ESP_LOGW(TAG, "Could not start _namo._tcp discovery query; retrying");
    this->query_failure_logged_ = true;
  }
  return false;
}

bool ParticipantDiscoveryRuntime::poll_query_() {
  if (this->query_ == nullptr)
    return false;

  mdns_result_t *results = nullptr;
  if (!mdns_query_async_get_results(this->query_, 0, &results, nullptr))
    return false;

  auto *completed_query = this->query_;
  this->query_ = nullptr;
  this->ingest_query_results_(results);
  const esp_err_t delete_result = mdns_query_async_delete(completed_query);
  if (delete_result != ESP_OK) {
    ESP_LOGW(TAG, "Could not release completed discovery query: %s",
             esp_err_to_name(delete_result));
  } else if (results != nullptr) {
    mdns_query_results_free(results);
  }
  this->complete_scan_(ParticipantDiscoveryResult::OK);
  return true;
}

void ParticipantDiscoveryRuntime::ingest_query_results_(
    const mdns_result_s *results) {
  bool changed = false;
  this->lock_();
  for (const mdns_result_t *result = results; result != nullptr;
       result = result->next) {
    uint8_t protocol_minimum = 0;
    uint8_t protocol_maximum = 0;
    char address[ParticipantDiscoveryCandidate::MAX_ADDRESS_LENGTH + 1]{};
    const char *identity_ready = txt_value(result, "identity_ready");
    if (!parse_uint8(txt_value(result, "protocol_min"), &protocol_minimum) ||
        !parse_uint8(txt_value(result, "protocol_max"), &protocol_maximum) ||
        !result_ipv4(result, address))
      continue;

    changed = this->results_.consider(
                  txt_value(result, "participant_id"),
                  txt_value(result, "participant_type"),
                  txt_value(result, "product"), result->hostname, address,
                  result->port, protocol_minimum, protocol_maximum,
                  identity_ready != nullptr &&
                      std::strcmp(identity_ready, "true") == 0) ||
              changed;
  }
  this->published_.candidate_count =
      static_cast<uint8_t>(this->results_.size());
  if (changed) {
    ++this->published_.generation;
  }
  this->unlock_();
}
#endif

void ParticipantDiscoveryRuntime::complete_scan_(
    ParticipantDiscoveryResult result) {
  this->lock_();
  this->scan_active_ = false;
  if (this->scan_visible_) {
    this->published_.scanning = false;
    this->published_.result = result;
  }
  this->scan_visible_ = false;
  this->scan_deadline_ms_ = 0;
  this->published_.candidate_count =
      static_cast<uint8_t>(this->results_.size());
  ++this->published_.generation;
  this->unlock_();
}

void ParticipantDiscoveryRuntime::shutdown() {
#if defined(USE_ESP32) && defined(USE_MDNS)
  if (this->query_ != nullptr) {
    mdns_result_t *results = nullptr;
    if (mdns_query_async_get_results(this->query_, 0, &results, nullptr)) {
      mdns_query_async_delete(this->query_);
      if (results != nullptr)
        mdns_query_results_free(results);
    }
    this->query_ = nullptr;
  }
#endif
  this->scan_pending_ = false;
  this->scan_active_ = false;
  this->scan_visible_ = false;
  this->published_.scanning = false;
}

ParticipantDiscoverySnapshot ParticipantDiscoveryRuntime::snapshot() {
  this->lock_();
  const auto snapshot = this->published_;
  this->unlock_();
  return snapshot;
}

bool ParticipantDiscoveryRuntime::candidate_at(
    std::size_t slot, ParticipantDiscoveryCandidate *candidate) {
  if (candidate == nullptr)
    return false;
  this->lock_();
  const auto *stored = this->results_.candidate_at(slot);
  if (stored != nullptr)
    *candidate = *stored;
  this->unlock_();
  return stored != nullptr;
}

void ParticipantDiscoveryRuntime::lock_() {
#ifdef USE_ESP32
  portENTER_CRITICAL(&this->mutex_);
#endif
}

void ParticipantDiscoveryRuntime::unlock_() {
#ifdef USE_ESP32
  portEXIT_CRITICAL(&this->mutex_);
#endif
}

}  // namespace radar_api_server
}  // namespace esphome
