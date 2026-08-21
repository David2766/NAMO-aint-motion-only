#include "observation_transport.h"

#include "credential_provisioning.h"
#include "group_coordinator.h"
#include "platform_identity.h"
#include "site_heartbeat.h"
#include "spatial_production_fusion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef USE_ESP32
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <ArduinoJson.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <cerrno>
#include <new>
#endif

namespace esphome {
namespace radar_api_server {

namespace {

uint16_t read_u16_be(const uint8_t *value) {
  return static_cast<uint16_t>((static_cast<uint16_t>(value[0]) << 8U) |
                               static_cast<uint16_t>(value[1]));
}

uint32_t read_u32_be(const uint8_t *value) {
  return (static_cast<uint32_t>(value[0]) << 24U) |
         (static_cast<uint32_t>(value[1]) << 16U) |
         (static_cast<uint32_t>(value[2]) << 8U) |
         static_cast<uint32_t>(value[3]);
}

uint64_t read_u64_be(const uint8_t *value) {
  uint64_t result = 0;
  for (uint8_t byte : std::array<uint8_t, 8>{
           value[0], value[1], value[2], value[3], value[4], value[5],
           value[6], value[7]}) {
    result = (result << 8U) | byte;
  }
  return result;
}

void write_u16_be(uint8_t *target, uint16_t value) {
  target[0] = static_cast<uint8_t>(value >> 8U);
  target[1] = static_cast<uint8_t>(value);
}

void write_u32_be(uint8_t *target, uint32_t value) {
  target[0] = static_cast<uint8_t>(value >> 24U);
  target[1] = static_cast<uint8_t>(value >> 16U);
  target[2] = static_cast<uint8_t>(value >> 8U);
  target[3] = static_cast<uint8_t>(value);
}

void write_u64_be(uint8_t *target, uint64_t value) {
  for (int index = 7; index >= 0; --index) {
    target[index] = static_cast<uint8_t>(value);
    value >>= 8U;
  }
}

class MessagePackWriter {
 public:
  MessagePackWriter(uint8_t *output, std::size_t capacity)
      : output_(output), capacity_(capacity) {}

  bool map(uint8_t size) {
    return size <= 15 && this->byte_(static_cast<uint8_t>(0x80U | size));
  }
  bool array(uint8_t size) {
    return size <= 15 && this->byte_(static_cast<uint8_t>(0x90U | size));
  }
  bool string(const char *value) {
    if (value == nullptr)
      return false;
    const std::size_t size = std::strlen(value);
    if (size <= 31) {
      if (!this->byte_(static_cast<uint8_t>(0xA0U | size)))
        return false;
    } else if (size <= UINT8_MAX) {
      if (!this->byte_(0xD9) || !this->byte_(static_cast<uint8_t>(size)))
        return false;
    } else {
      return false;
    }
    return this->bytes_(reinterpret_cast<const uint8_t *>(value), size);
  }
  bool unsigned_integer(uint32_t value) {
    if (value <= 0x7F)
      return this->byte_(static_cast<uint8_t>(value));
    if (value <= UINT8_MAX)
      return this->byte_(0xCC) && this->byte_(static_cast<uint8_t>(value));
    if (value <= UINT16_MAX) {
      uint8_t encoded[2]{};
      write_u16_be(encoded, static_cast<uint16_t>(value));
      return this->byte_(0xCD) && this->bytes_(encoded, sizeof(encoded));
    }
    uint8_t encoded[4]{};
    write_u32_be(encoded, value);
    return this->byte_(0xCE) && this->bytes_(encoded, sizeof(encoded));
  }
  bool floating(float value) {
    if (!std::isfinite(value))
      return false;
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t encoded[4]{};
    write_u32_be(encoded, bits);
    return this->byte_(0xCA) && this->bytes_(encoded, sizeof(encoded));
  }
  bool boolean(bool value) { return this->byte_(value ? 0xC3 : 0xC2); }
  bool nil() { return this->byte_(0xC0); }
  bool ok() const { return this->ok_; }
  std::size_t size() const { return this->size_; }

 private:
  uint8_t *output_{nullptr};
  std::size_t capacity_{0};
  std::size_t size_{0};
  bool ok_{true};

  bool byte_(uint8_t value) { return this->bytes_(&value, 1); }
  bool bytes_(const uint8_t *value, std::size_t size) {
    if (!this->ok_ || value == nullptr || this->output_ == nullptr ||
        this->size_ > this->capacity_ ||
        size > this->capacity_ - this->size_) {
      this->ok_ = false;
      return false;
    }
    std::memcpy(this->output_ + this->size_, value, size);
    this->size_ += size;
    return true;
  }
};

bool key(MessagePackWriter &writer, const char *name) {
  return writer.string(name);
}

bool encode_authenticated_transport_prefix(
    AuthenticatedTransportMessageType message_type, uint32_t key_version,
    uint64_t participant_token, std::size_t payload_size, uint8_t *packet,
    std::size_t packet_capacity, std::size_t *signed_size,
    std::size_t *packet_size) {
  if (message_type != AuthenticatedTransportMessageType::OBSERVATION &&
      message_type != AuthenticatedTransportMessageType::FUSION_SNAPSHOT &&
      message_type != AuthenticatedTransportMessageType::SITE_SPACE_SUMMARY)
    return false;
  if (key_version == 0 || participant_token == 0 || payload_size == 0 ||
      payload_size > AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE ||
      packet == nullptr || signed_size == nullptr || packet_size == nullptr ||
      packet_capacity < AuthenticatedObservationEnvelope::PREFIX_SIZE +
                            payload_size +
                            AuthenticatedObservationEnvelope::TAG_SIZE)
    return false;
  std::memcpy(packet, "NAMO", 4);
  packet[4] = 1;
  packet[5] = static_cast<uint8_t>(message_type);
  packet[6] = 0;
  packet[7] = AuthenticatedObservationEnvelope::PREFIX_SIZE;
  write_u32_be(packet + 8, key_version);
  write_u64_be(packet + 12, participant_token);
  write_u16_be(packet + 20, static_cast<uint16_t>(payload_size));
  write_u16_be(packet + 22, 0);
  *signed_size = AuthenticatedObservationEnvelope::PREFIX_SIZE + payload_size;
  *packet_size = *signed_size + AuthenticatedObservationEnvelope::TAG_SIZE;
  return true;
}

}  // namespace

bool parse_authenticated_observation_envelope(
    const uint8_t *packet, std::size_t packet_size,
    AuthenticatedObservationEnvelope *envelope) {
  if (packet == nullptr || envelope == nullptr ||
      packet_size < AuthenticatedObservationEnvelope::PREFIX_SIZE + 1 +
                        AuthenticatedObservationEnvelope::TAG_SIZE ||
      packet_size > AuthenticatedObservationEnvelope::MAX_PACKET_SIZE)
    return false;
  if (std::memcmp(packet, "NAMO", 4) != 0 || packet[4] != 1 ||
      (packet[5] !=
           static_cast<uint8_t>(
               AuthenticatedTransportMessageType::OBSERVATION) &&
       packet[5] !=
           static_cast<uint8_t>(
               AuthenticatedTransportMessageType::FUSION_SNAPSHOT) &&
       packet[5] !=
           static_cast<uint8_t>(
               AuthenticatedTransportMessageType::SITE_SPACE_SUMMARY)) ||
      packet[6] != 0 ||
      packet[7] != AuthenticatedObservationEnvelope::PREFIX_SIZE)
    return false;

  const uint32_t key_version = read_u32_be(packet + 8);
  const uint16_t payload_size = read_u16_be(packet + 20);
  const uint16_t reserved = read_u16_be(packet + 22);
  if (key_version == 0 || reserved != 0 || payload_size == 0 ||
      payload_size > AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE ||
      packet_size != AuthenticatedObservationEnvelope::PREFIX_SIZE +
                         payload_size +
                         AuthenticatedObservationEnvelope::TAG_SIZE)
    return false;

  envelope->message_type =
      static_cast<AuthenticatedTransportMessageType>(packet[5]);
  envelope->key_version = key_version;
  envelope->participant_token = read_u64_be(packet + 12);
  envelope->payload = packet + AuthenticatedObservationEnvelope::PREFIX_SIZE;
  envelope->payload_size = payload_size;
  envelope->tag = envelope->payload + payload_size;
  return true;
}

const char *observation_encode_result_to_string(
    ObservationEncodeResult result) {
  switch (result) {
    case ObservationEncodeResult::OK: return "ok";
    case ObservationEncodeResult::INVALID_ARGUMENT: return "invalid_argument";
    case ObservationEncodeResult::OBSERVATION_EXPIRED:
      return "observation_expired";
    case ObservationEncodeResult::BUFFER_TOO_SMALL: return "buffer_too_small";
  }
  return "invalid_argument";
}

ObservationEncodeResult encode_observation_messagepack(
    const ObservationEncodeContext &context, uint8_t *payload,
    std::size_t payload_capacity, std::size_t *payload_size) {
  if (payload == nullptr || payload_size == nullptr ||
      context.site_id == nullptr || context.site_id[0] == '\0' ||
      context.participant_id == nullptr ||
      context.participant_id[0] == '\0' || context.group_id == nullptr ||
      context.group_id[0] == '\0' || context.boot_id == nullptr ||
      context.boot_id[0] == '\0' || context.sequence == 0 ||
      context.pose_version == 0 || context.calibration_version == 0 ||
      context.max_targets == 0 || context.max_targets > 3 ||
      context.observation == nullptr)
    return ObservationEncodeResult::INVALID_ARGUMENT;

  const auto &observation = *context.observation;
  if (observation.sequence == 0 || observation.ttl_ms < 100 ||
      observation.ttl_ms > 10000)
    return ObservationEncodeResult::INVALID_ARGUMENT;
  const uint32_t sample_age_ms = context.now_ms - observation.observed_at_ms;
  if (sample_age_ms >= observation.ttl_ms)
    return ObservationEncodeResult::OBSERVATION_EXPIRED;

  const bool include_velocity =
      (context.capability_flags & PARTICIPANT_CAP_VELOCITY) != 0;
  const bool include_uncertainty =
      (context.capability_flags & PARTICIPANT_CAP_POSITION_UNCERTAINTY) != 0;
  const bool include_range =
      (context.capability_flags & PARTICIPANT_CAP_RANGE) != 0;
  std::array<const LocalObservationTrack *, LocalObservation::MAX_TRACKS>
      tracks{};
  uint8_t track_count = 0;
  for (const auto &track : observation.tracks) {
    if (!track.valid || track.local_track_id == 0 ||
        track_count >= context.max_targets)
      continue;
    if (!std::isfinite(track.x_mm) || !std::isfinite(track.y_mm) ||
        !std::isfinite(track.vx_mm_s) || !std::isfinite(track.vy_mm_s) ||
        !std::isfinite(track.position_variance_x_mm2) ||
        !std::isfinite(track.position_variance_y_mm2) ||
        track.x_mm < -100000.0f || track.x_mm > 100000.0f ||
        track.y_mm < -100000.0f || track.y_mm > 100000.0f ||
        (include_velocity &&
         (track.vx_mm_s < -20000.0f || track.vx_mm_s > 20000.0f ||
          track.vy_mm_s < -20000.0f || track.vy_mm_s > 20000.0f)) ||
        (include_uncertainty &&
         (track.position_variance_x_mm2 < 0.0f ||
          track.position_variance_x_mm2 > 1.0e10f ||
          track.position_variance_y_mm2 < 0.0f ||
          track.position_variance_y_mm2 > 1.0e10f)))
      return ObservationEncodeResult::INVALID_ARGUMENT;
    tracks[track_count++] = &track;
  }

  // Three covariance-bearing tracks exceed the 768-byte wire budget when
  // every optional per-track age is present. The observation-level age remains
  // authoritative, so only that redundant optional field is omitted.
  const bool include_track_age =
      track_count < LocalObservation::MAX_TRACKS || !include_uncertainty;
  MessagePackWriter writer(payload, payload_capacity);
  writer.map(13);
  key(writer, "protocolVersion") && writer.unsigned_integer(1);
  key(writer, "siteId") && writer.string(context.site_id);
  key(writer, "participantId") && writer.string(context.participant_id);
  key(writer, "participantType") && writer.string("namo_node");
  key(writer, "groupId") && writer.string(context.group_id);
  key(writer, "bootId") && writer.string(context.boot_id);
  key(writer, "sequence") && writer.unsigned_integer(context.sequence);
  key(writer, "observedAtMs") &&
      writer.unsigned_integer(observation.observed_at_ms);
  key(writer, "ttlMs") && writer.unsigned_integer(observation.ttl_ms);

  key(writer, "frame") && writer.map(3);
  key(writer, "id") && writer.string("sensor-local");
  key(writer, "poseVersion") &&
      writer.unsigned_integer(context.pose_version);
  key(writer, "calibrationVersion") &&
      writer.unsigned_integer(context.calibration_version);

  key(writer, "tracks") && writer.array(track_count);
  for (uint8_t index = 0; index < track_count; ++index) {
    const auto &track = *tracks[index];
    const uint8_t field_count =
        static_cast<uint8_t>(4 + (include_track_age ? 1 : 0) +
                             (include_velocity ? 2 : 0) +
                             (include_uncertainty ? 1 : 0));
    writer.map(field_count);
    key(writer, "localTrackId") &&
        writer.unsigned_integer(track.local_track_id);
    key(writer, "xMm") && writer.floating(track.x_mm);
    key(writer, "yMm") && writer.floating(track.y_mm);
    if (include_velocity) {
      key(writer, "vxMmS") && writer.floating(track.vx_mm_s);
      key(writer, "vyMmS") && writer.floating(track.vy_mm_s);
    }
    key(writer, "qualityScore") &&
        writer.unsigned_integer(static_cast<uint32_t>(
            std::max(0, std::min(100, track.track_score))));
    if (include_track_age) {
      const uint32_t available_age =
          observation.ttl_ms - std::min(observation.ttl_ms, sample_age_ms);
      key(writer, "sampleAgeMs") &&
          writer.unsigned_integer(
              sample_age_ms +
              std::min(track.sample_age_ms, available_age));
    }
    if (include_uncertainty) {
      key(writer, "positionVarianceMm2") && writer.array(4);
      writer.floating(std::max(0.0f, track.position_variance_x_mm2));
      writer.floating(0.0f);
      writer.floating(0.0f);
      writer.floating(std::max(0.0f, track.position_variance_y_mm2));
    }
  }

  key(writer, "evidence") && writer.map(4);
  key(writer, "presence") &&
      writer.boolean(
          (context.capability_flags & PARTICIPANT_CAP_PRESENCE) != 0 &&
          observation.evidence.tracker_presence);
  key(writer, "motion") &&
      writer.boolean(
          (context.capability_flags & PARTICIPANT_CAP_MOTION) != 0 &&
          observation.evidence.tracker_motion);
  key(writer, "rangeMm");
  if (include_range && track_count > 0 &&
      std::isfinite(tracks[0]->distance_mm) &&
      tracks[0]->distance_mm >= 0.0f)
    writer.floating(tracks[0]->distance_mm);
  else
    writer.nil();
  key(writer, "zonePresence") && writer.array(0);

  key(writer, "health") && writer.map(2);
  key(writer, "state") && writer.string("healthy");
  key(writer, "sampleAgeMs") && writer.unsigned_integer(sample_age_ms);

  if (!writer.ok())
    return ObservationEncodeResult::BUFFER_TOO_SMALL;
  *payload_size = writer.size();
  return ObservationEncodeResult::OK;
}

bool encode_authenticated_observation_prefix(
    uint32_t key_version, uint64_t participant_token,
    std::size_t payload_size, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size) {
  return encode_authenticated_transport_prefix(
      AuthenticatedTransportMessageType::OBSERVATION, key_version,
      participant_token, payload_size, packet, packet_capacity, signed_size,
      packet_size);
}

bool encode_authenticated_fusion_snapshot_prefix(
    uint32_t key_version, uint64_t participant_token,
    std::size_t payload_size, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size) {
  return encode_authenticated_transport_prefix(
      AuthenticatedTransportMessageType::FUSION_SNAPSHOT, key_version,
      participant_token, payload_size, packet, packet_capacity, signed_size,
      packet_size);
}

bool encode_authenticated_site_space_summary_prefix(
    uint32_t key_version, uint64_t participant_token,
    std::size_t payload_size, uint8_t *packet, std::size_t packet_capacity,
    std::size_t *signed_size, std::size_t *packet_size) {
  return encode_authenticated_transport_prefix(
      AuthenticatedTransportMessageType::SITE_SPACE_SUMMARY, key_version,
      participant_token, payload_size, packet, packet_capacity, signed_size,
      packet_size);
}

ObservationEncodeResult encode_fusion_snapshot_messagepack(
    const FusionSnapshotEncodeContext &context, uint8_t *payload,
    std::size_t payload_capacity, std::size_t *payload_size) {
  if (payload == nullptr || payload_size == nullptr ||
      context.site_id == nullptr || context.site_id[0] == '\0' ||
      context.leader_node_id == nullptr ||
      context.leader_node_id[0] == '\0' || context.group_id == nullptr ||
      context.group_id[0] == '\0' || context.boot_id == nullptr ||
      context.boot_id[0] == '\0' || context.sequence == 0 ||
      context.group_term == 0 ||
      context.ttl_ms < FusionSnapshot::MIN_TTL_MS ||
      context.ttl_ms > FusionSnapshot::MAX_TTL_MS ||
      context.fusion == nullptr || context.fusion->sequence == 0)
    return ObservationEncodeResult::INVALID_ARGUMENT;

  std::array<const SpatialShadowTrack *, FusionSnapshot::MAX_TARGETS> tracks{};
  uint8_t track_count = 0;
  bool motion = false;
  for (const auto &track : context.fusion->tracks) {
    if (!track.valid ||
        (track.state != SpatialShadowTrackState::CONFIRMED &&
         track.state != SpatialShadowTrackState::COASTING) ||
        track_count >= tracks.size())
      continue;
    if (!std::isfinite(track.x_mm) || !std::isfinite(track.y_mm) ||
        !std::isfinite(track.vx_mm_s) || !std::isfinite(track.vy_mm_s) ||
        track.x_mm < -100000.0f || track.x_mm > 100000.0f ||
        track.y_mm < -100000.0f || track.y_mm > 100000.0f ||
        track.vx_mm_s < -20000.0f || track.vx_mm_s > 20000.0f ||
        track.vy_mm_s < -20000.0f || track.vy_mm_s > 20000.0f)
      return ObservationEncodeResult::INVALID_ARGUMENT;
    tracks[track_count++] = &track;
    motion = motion ||
             std::hypot(track.vx_mm_s, track.vy_mm_s) > 100.0f;
  }

  MessagePackWriter writer(payload, payload_capacity);
  writer.map(12);
  key(writer, "protocolVersion") && writer.unsigned_integer(1);
  key(writer, "siteId") && writer.string(context.site_id);
  key(writer, "leaderNodeId") && writer.string(context.leader_node_id);
  key(writer, "groupId") && writer.string(context.group_id);
  key(writer, "bootId") && writer.string(context.boot_id);
  key(writer, "sequence") && writer.unsigned_integer(context.sequence);
  key(writer, "groupTerm") && writer.unsigned_integer(context.group_term);
  key(writer, "sourceSequence") &&
      writer.unsigned_integer(context.fusion->sequence);
  key(writer, "ttlMs") && writer.unsigned_integer(context.ttl_ms);
  key(writer, "presence") && writer.boolean(track_count > 0);
  key(writer, "motion") && writer.boolean(motion);
  key(writer, "targets") && writer.array(track_count);
  for (uint8_t index = 0; index < track_count; ++index) {
    const auto &track = *tracks[index];
    writer.map(8);
    key(writer, "id") && writer.unsigned_integer(track.fused_track_id);
    key(writer, "state") &&
        writer.unsigned_integer(static_cast<uint8_t>(track.state));
    key(writer, "sourceCount") &&
        writer.unsigned_integer(track.source_count);
    key(writer, "confidence") &&
        writer.unsigned_integer(track.confidence);
    key(writer, "xMm") && writer.floating(track.x_mm);
    key(writer, "yMm") && writer.floating(track.y_mm);
    key(writer, "vxMmS") && writer.floating(track.vx_mm_s);
    key(writer, "vyMmS") && writer.floating(track.vy_mm_s);
  }
  if (!writer.ok())
    return ObservationEncodeResult::BUFFER_TOO_SMALL;
  *payload_size = writer.size();
  return ObservationEncodeResult::OK;
}

ObservationEncodeResult encode_site_space_summary_messagepack(
    const SiteSpaceSummary &summary, uint8_t *payload,
    std::size_t payload_capacity, std::size_t *payload_size) {
  if (!summary.occupied || payload == nullptr || payload_size == nullptr ||
      summary.site_id[0] == '\0' || summary.space_id[0] == '\0' ||
      summary.group_id[0] == '\0' || summary.leader_node_id[0] == '\0' ||
      summary.boot_id[0] == '\0' || summary.sequence == 0 ||
      summary.group_term == 0 || summary.source_sequence == 0 ||
      summary.ttl_ms < SiteSpaceSummary::MIN_TTL_MS ||
      summary.ttl_ms > SiteSpaceSummary::MAX_TTL_MS ||
      summary.target_count > SpatialProductionFusionOutput::MAX_TARGETS)
    return ObservationEncodeResult::INVALID_ARGUMENT;

  MessagePackWriter writer(payload, payload_capacity);
  writer.map(13);
  key(writer, "protocolVersion") && writer.unsigned_integer(1);
  key(writer, "siteId") && writer.string(summary.site_id);
  key(writer, "spaceId") && writer.string(summary.space_id);
  key(writer, "groupId") && writer.string(summary.group_id);
  key(writer, "leaderNodeId") && writer.string(summary.leader_node_id);
  key(writer, "bootId") && writer.string(summary.boot_id);
  key(writer, "sequence") && writer.unsigned_integer(summary.sequence);
  key(writer, "groupTerm") && writer.unsigned_integer(summary.group_term);
  key(writer, "sourceSequence") &&
      writer.unsigned_integer(summary.source_sequence);
  key(writer, "ttlMs") && writer.unsigned_integer(summary.ttl_ms);
  key(writer, "presence") && writer.boolean(summary.presence);
  key(writer, "motion") && writer.boolean(summary.motion);
  key(writer, "targetCount") && writer.unsigned_integer(summary.target_count);
  if (!writer.ok())
    return ObservationEncodeResult::BUFFER_TOO_SMALL;
  *payload_size = writer.size();
  return ObservationEncodeResult::OK;
}

void ObservationTransportRateLimiter::clear() {
  for (auto &entry : this->entries_)
    entry = ObservationTransportRateState{};
}

bool ObservationTransportRateLimiter::allow(uint64_t participant_token,
                                            uint32_t now_ms) {
  if (participant_token == 0)
    return false;
  ObservationTransportRateState *state = nullptr;
  for (auto &entry : this->entries_) {
    if (entry.occupied && entry.participant_token == participant_token) {
      state = &entry;
      break;
    }
    if (!entry.occupied && state == nullptr)
      state = &entry;
  }
  if (state == nullptr)
    return false;
  if (!state->occupied) {
    state->occupied = true;
    state->participant_token = participant_token;
  }
  if (state->accepted_in_window != 0 &&
      now_ms < state->rate_window_started_ms)
    return false;
  if (state->accepted_in_window == 0 ||
      now_ms - state->rate_window_started_ms >= RATE_WINDOW_MS) {
    state->rate_window_started_ms = now_ms;
    state->accepted_in_window = 1;
    return true;
  }
  if (state->accepted_in_window >= MAX_PACKETS_PER_WINDOW)
    return false;
  state->accepted_in_window++;
  return true;
}

const char *observation_transport_result_to_string(
    ObservationTransportResult result) {
  switch (result) {
    case ObservationTransportResult::IDLE: return "idle";
    case ObservationTransportResult::OK: return "ok";
    case ObservationTransportResult::PACKET_TOO_LARGE: return "packet_too_large";
    case ObservationTransportResult::INVALID_ENVELOPE: return "invalid_envelope";
    case ObservationTransportResult::UNKNOWN_KEY: return "unknown_key";
    case ObservationTransportResult::AUTHENTICATION_FAILED:
      return "authentication_failed";
    case ObservationTransportResult::RATE_LIMITED: return "rate_limited";
    case ObservationTransportResult::INVALID_MESSAGEPACK:
      return "invalid_messagepack";
    case ObservationTransportResult::INVALID_OBSERVATION:
      return "invalid_observation";
    case ObservationTransportResult::IDENTITY_MISMATCH:
      return "identity_mismatch";
    case ObservationTransportResult::ADMISSION_REJECTED:
      return "admission_rejected";
    case ObservationTransportResult::SOCKET_ERROR: return "socket_error";
  }
  return "unknown";
}

const char *observation_send_result_to_string(ObservationSendResult result) {
  switch (result) {
    case ObservationSendResult::IDLE: return "idle";
    case ObservationSendResult::OK: return "ok";
    case ObservationSendResult::GROUP_UNAVAILABLE: return "group_unavailable";
    case ObservationSendResult::LOCAL_LEADER: return "local_leader";
    case ObservationSendResult::PARTICIPANT_INACTIVE:
      return "participant_inactive";
    case ObservationSendResult::READINESS_UNAVAILABLE:
      return "readiness_unavailable";
    case ObservationSendResult::POSE_MISMATCH: return "pose_mismatch";
    case ObservationSendResult::CREDENTIAL_UNAVAILABLE:
      return "credential_unavailable";
    case ObservationSendResult::ENDPOINT_OFFLINE: return "endpoint_offline";
    case ObservationSendResult::OBSERVATION_UNAVAILABLE:
      return "observation_unavailable";
    case ObservationSendResult::ENCODE_FAILED: return "encode_failed";
    case ObservationSendResult::SOCKET_ERROR: return "socket_error";
  }
  return "idle";
}

ObservationSendResult evaluate_observation_send(
    const char *local_participant_id,
    const GroupCoordinatorState *selected_group, bool multiple_groups,
    const ParticipantRecord *leader,
    const ParticipantReadinessRecord *leader_readiness,
    const ParticipantCredentialRecord *leader_credential,
    const ParticipantEndpoint *leader_endpoint, uint32_t local_pose_version,
    uint32_t now_ms) {
  if (local_participant_id == nullptr || local_participant_id[0] == '\0' ||
      multiple_groups || selected_group == nullptr ||
      !selected_group->configured ||
      selected_group->leader_node_id[0] == '\0' ||
      (selected_group->assignment_revision != 0 &&
       (!selected_group->canonical_assignment ||
        !selected_group->credentials_complete)))
    return ObservationSendResult::GROUP_UNAVAILABLE;
  if (selected_group->local_leader ||
      std::strcmp(selected_group->leader_node_id, local_participant_id) == 0)
    return ObservationSendResult::LOCAL_LEADER;
  if (leader == nullptr ||
      leader->membership != ParticipantMembership::ACTIVE)
    return ObservationSendResult::PARTICIPANT_INACTIVE;
  if (!ParticipantReadinessRegistry::ready(leader_readiness) ||
      (leader_readiness->config.capability_flags &
       PARTICIPANT_CAP_POINT_TRACKS) == 0)
    return ObservationSendResult::READINESS_UNAVAILABLE;
  if (local_pose_version == 0 ||
      leader_readiness->config.local_pose_version != local_pose_version ||
      leader_readiness->config.calibration_version == 0)
    return ObservationSendResult::POSE_MISMATCH;
  if (leader_credential == nullptr || !leader_credential->enabled)
    return ObservationSendResult::CREDENTIAL_UNAVAILABLE;
  if (!ParticipantEndpointCache::is_online(leader_endpoint, now_ms) ||
      leader_endpoint->ipv4 == 0)
    return ObservationSendResult::ENDPOINT_OFFLINE;
  return ObservationSendResult::OK;
}

bool ObservationTransportInbox::store(
    const ObservationTransportSample &sample) {
  if (!sample.occupied || sample.participant_id[0] == '\0' ||
      sample.participant_token == 0 || sample.sequence == 0 ||
      sample.ttl_ms < 100 || sample.ttl_ms > 10000 ||
      sample.track_count > sample.tracks.size())
    return false;

  ObservationTransportSample *target = nullptr;
  for (auto &entry : this->samples_) {
    if (entry.occupied &&
        std::strcmp(entry.participant_id, sample.participant_id) == 0) {
      target = &entry;
      break;
    }
    if (!entry.occupied && target == nullptr)
      target = &entry;
  }
  if (target == nullptr)
    return false;
  *target = sample;
  return true;
}

bool ObservationTransportInbox::copy_latest(
    const char *participant_id, uint32_t now_ms,
    ObservationTransportSample *sample) const {
  if (participant_id == nullptr || participant_id[0] == '\0' ||
      sample == nullptr)
    return false;
  for (const auto &entry : this->samples_) {
    if (!entry.occupied ||
        std::strcmp(entry.participant_id, participant_id) != 0)
      continue;
    if (static_cast<int32_t>(now_ms - entry.expires_at_ms) >= 0)
      return false;
    *sample = entry;
    const uint32_t elapsed_ms = now_ms - entry.received_at_ms;
    sample->sample_age_ms =
        std::min<uint32_t>(UINT32_MAX - entry.sample_age_ms, elapsed_ms) +
        entry.sample_age_ms;
    for (std::size_t index = 0; index < sample->track_count; ++index) {
      auto &track = sample->tracks[index];
      track.sample_age_ms =
          std::min<uint32_t>(UINT32_MAX - track.sample_age_ms, elapsed_ms) +
          track.sample_age_ms;
    }
    return sample->sample_age_ms <= sample->ttl_ms;
  }
  return false;
}

void ObservationTransportInbox::expire(uint32_t now_ms) {
  for (auto &entry : this->samples_) {
    if (entry.occupied &&
        static_cast<int32_t>(now_ms - entry.expires_at_ms) >= 0)
      entry = ObservationTransportSample{};
  }
}

void ObservationTransportInbox::clear() {
  this->samples_ = {};
}

uint8_t ObservationTransportInbox::size() const {
  uint8_t count = 0;
  for (const auto &entry : this->samples_) {
    if (entry.occupied)
      ++count;
  }
  return count;
}

#ifdef USE_ESP32

namespace {

static const char *const TAG = "observation_transport";

enum class RuntimeAllocationLogLevel : uint8_t {
  INFO = 0,
  WARN,
  ERROR,
};

void log_transport_allocation(const char *component, const char *region,
                              std::size_t size,
                              RuntimeAllocationLogLevel level) {
  const auto internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const auto internal_largest = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const auto psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const auto psram_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (level == RuntimeAllocationLogLevel::ERROR) {
    ESP_LOGE(TAG,
             "Runtime allocation component=%s region=%s size=%u "
             "internal_free=%u internal_largest=%u psram_free=%u "
             "psram_largest=%u",
             component, region, static_cast<unsigned>(size),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest));
  } else if (level == RuntimeAllocationLogLevel::WARN) {
    ESP_LOGW(TAG,
             "Runtime allocation component=%s region=%s size=%u "
             "internal_free=%u internal_largest=%u psram_free=%u "
             "psram_largest=%u",
             component, region, static_cast<unsigned>(size),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest));
  } else {
    ESP_LOGI(TAG,
             "Runtime allocation component=%s region=%s size=%u "
             "internal_free=%u internal_largest=%u psram_free=%u "
             "psram_largest=%u",
             component, region, static_cast<unsigned>(size),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest));
  }
}

constexpr uint64_t SMOKE_PARTICIPANT_TOKEN = 0x9CB0888507208122ULL;
constexpr std::array<uint8_t, 32> SMOKE_TRANSPORT_KEY = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

bool valid_identifier(const char *value, std::size_t maximum) {
  if (value == nullptr)
    return false;
  const std::size_t length = std::strlen(value);
  if (length == 0 || length > maximum)
    return false;
  const auto valid_first = [](char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  const auto valid_next = [&](char character) {
    return valid_first(character) || character == '.' || character == '_' ||
           character == ':' || character == '-';
  };
  if (!valid_first(value[0]))
    return false;
  for (std::size_t index = 1; index < length; ++index) {
    if (!valid_next(value[index]))
      return false;
  }
  return true;
}

bool copy_identifier(JsonObjectConst object, const char *key, char *target,
                     std::size_t capacity) {
  const JsonVariantConst value = object[key];
  if (!value.is<const char *>())
    return false;
  const char *text = value.as<const char *>();
  if (!valid_identifier(text, capacity - 1))
    return false;
  std::strncpy(target, text, capacity - 1);
  target[capacity - 1] = '\0';
  return true;
}

bool required_u32(JsonObjectConst object, const char *key, uint32_t minimum,
                  uint32_t maximum, uint32_t *output) {
  const JsonVariantConst value = object[key];
  if (value.is<bool>() || !value.is<uint32_t>())
    return false;
  const uint32_t number = value.as<uint32_t>();
  if (number < minimum || number > maximum)
    return false;
  *output = number;
  return true;
}

bool required_bool(JsonObjectConst object, const char *key, bool *output) {
  const JsonVariantConst value = object[key];
  if (!value.is<bool>())
    return false;
  *output = value.as<bool>();
  return true;
}

bool finite_number(JsonVariantConst value, double minimum, double maximum) {
  if (value.is<bool>() || !value.is<double>())
    return false;
  const double number = value.as<double>();
  return std::isfinite(number) && number >= minimum && number <= maximum;
}

bool has_member(JsonObjectConst object, const char *key) {
  return object[key].is<JsonVariantConst>();
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                         std::size_t size) {
  uint8_t difference = 0;
  for (std::size_t index = 0; index < size; ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

bool verify_hmac(FixedHmacSha256 *hmac, const uint8_t *signed_data,
                 std::size_t signed_size, const std::array<uint8_t, 32> &key,
                 const uint8_t *tag) {
  if (hmac == nullptr)
    return false;
  std::array<uint8_t, 32> expected{};
  if (!hmac->calculate(key, signed_data, signed_size, expected.data()))
    return false;
  return constant_time_equal(expected.data(), tag, expected.size());
}

bool sign_hmac(FixedHmacSha256 *hmac, const uint8_t *signed_data,
               std::size_t signed_size, const std::array<uint8_t, 32> &key,
               uint8_t *tag) {
  return hmac != nullptr &&
         hmac->calculate(key, signed_data, signed_size, tag);
}

bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

struct ParsedObservation {
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char site_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char boot_id[49]{};
  ParticipantType participant_type{ParticipantType::SATELLITE};
  uint32_t sequence{0};
  uint32_t sample_age_ms{0};
  uint32_t ttl_ms{0};
  uint32_t pose_version{0};
  uint32_t calibration_version{0};
  uint8_t track_count{0};
  bool presence{false};
  bool motion{false};
  bool has_velocity{false};
  bool has_position_uncertainty{false};
  bool has_range{false};
  bool has_zone_presence{false};
  std::array<ObservationTransportTrack,
             ObservationTransportSample::MAX_TRACKS>
      tracks{};

  ParticipantObservationMetadata metadata() const {
    ParticipantObservationMetadata result;
    result.participant_id = this->participant_id;
    result.participant_type = this->participant_type;
    result.site_id = this->site_id;
    result.group_id = this->group_id;
    result.boot_id = this->boot_id;
    result.sequence = this->sequence;
    result.sample_age_ms = this->sample_age_ms;
    result.ttl_ms = this->ttl_ms;
    result.pose_version = this->pose_version;
    result.calibration_version = this->calibration_version;
    result.track_count = this->track_count;
    result.presence = this->presence;
    result.motion = this->motion;
    result.has_velocity = this->has_velocity;
    result.has_position_uncertainty = this->has_position_uncertainty;
    result.has_range = this->has_range;
    result.has_zone_presence = this->has_zone_presence;
    return result;
  }
};

bool validate_covariance(JsonVariantConst value, float *variance_x_mm2,
                         float *variance_y_mm2) {
  if (!value.is<JsonArrayConst>())
    return false;
  const JsonArrayConst entries = value.as<JsonArrayConst>();
  if (entries.size() != 4)
    return false;
  double covariance[4]{};
  std::size_t index = 0;
  for (JsonVariantConst entry : entries) {
    if (!finite_number(entry, -1.0e10, 1.0e10))
      return false;
    covariance[index++] = entry.as<double>();
  }
  const bool valid =
      covariance[0] >= 0 && covariance[3] >= 0 &&
      std::fabs(covariance[1] - covariance[2]) <= 0.001 &&
      covariance[0] * covariance[3] >=
          covariance[1] * covariance[2] - 0.001;
  if (valid) {
    *variance_x_mm2 = static_cast<float>(covariance[0]);
    *variance_y_mm2 = static_cast<float>(covariance[3]);
  }
  return valid;
}

bool validate_tracks(JsonArrayConst tracks, uint32_t ttl_ms,
                     ParsedObservation *output) {
  if (tracks.size() > 3)
    return false;
  std::array<uint32_t, 3> track_ids{};
  std::size_t track_index = 0;
  bool expected_velocity = false;
  bool expected_uncertainty = false;
  for (JsonVariantConst item : tracks) {
    if (!item.is<JsonObjectConst>())
      return false;
    const JsonObjectConst track = item.as<JsonObjectConst>();
    if (has_member(track, "roomState") || has_member(track, "exitZoneMask") ||
        has_member(track, "exitAgeMs"))
      return false;
    uint32_t track_id = 0;
    uint32_t quality = 0;
    if (!required_u32(track, "localTrackId", 1, UINT32_MAX, &track_id) ||
        !finite_number(track["xMm"], -100000.0, 100000.0) ||
        !finite_number(track["yMm"], -100000.0, 100000.0) ||
        !required_u32(track, "qualityScore", 0, 100, &quality))
      return false;
    for (std::size_t previous = 0; previous < track_index; ++previous) {
      if (track_ids[previous] == track_id)
        return false;
    }
    track_ids[track_index] = track_id;
    auto &parsed_track = output->tracks[track_index];
    parsed_track.valid = true;
    parsed_track.local_track_id = track_id;
    parsed_track.x_mm = track["xMm"].as<float>();
    parsed_track.y_mm = track["yMm"].as<float>();
    parsed_track.quality_score = static_cast<uint8_t>(quality);

    const bool velocity_x = has_member(track, "vxMmS");
    const bool velocity_y = has_member(track, "vyMmS");
    if (velocity_x != velocity_y ||
        (velocity_x &&
         (!finite_number(track["vxMmS"], -20000.0, 20000.0) ||
          !finite_number(track["vyMmS"], -20000.0, 20000.0))))
      return false;
    parsed_track.velocity_available = velocity_x;
    if (velocity_x) {
      parsed_track.vx_mm_s = track["vxMmS"].as<float>();
      parsed_track.vy_mm_s = track["vyMmS"].as<float>();
    }
    const bool uncertainty = has_member(track, "positionVarianceMm2");
    if (uncertainty &&
        !validate_covariance(track["positionVarianceMm2"],
                             &parsed_track.variance_x_mm2,
                             &parsed_track.variance_y_mm2))
      return false;
    parsed_track.uncertainty_available = uncertainty;
    if (track_index == 0) {
      expected_velocity = velocity_x;
      expected_uncertainty = uncertainty;
    } else if (expected_velocity != velocity_x ||
               expected_uncertainty != uncertainty) {
      return false;
    }
    if (has_member(track, "sampleAgeMs")) {
      uint32_t track_age = 0;
      if (!required_u32(track, "sampleAgeMs", 0, ttl_ms, &track_age))
        return false;
      parsed_track.sample_age_ms = track_age;
    }
    track_index++;
  }
  output->track_count = static_cast<uint8_t>(track_index);
  output->has_velocity = track_index > 0 && expected_velocity;
  output->has_position_uncertainty = track_index > 0 && expected_uncertainty;
  return true;
}

}  // namespace

class ObservationTransportParser {
 public:
#ifdef USE_PSRAM
  ObservationTransportParser() : document_(&this->allocator_) {}
#else
  ObservationTransportParser() = default;
#endif

  ObservationTransportResult parse(const uint8_t *payload,
                                   std::size_t payload_size,
                                   ParsedObservation *output) {
    this->document_.clear();
    const auto error = deserializeMsgPack(
        this->document_, payload, payload_size,
        DeserializationOption::NestingLimit(8));
    if (error || this->document_.overflowed() ||
        !this->document_.is<JsonObjectConst>())
      return ObservationTransportResult::INVALID_MESSAGEPACK;

    const JsonObjectConst root = this->document_.as<JsonObjectConst>();
    ParsedObservation parsed;
    uint32_t protocol_version = 0;
    uint32_t observed_at_ms = 0;
    if (!required_u32(root, "protocolVersion", 1, 1, &protocol_version) ||
        !copy_identifier(root, "siteId", parsed.site_id,
                         sizeof(parsed.site_id)) ||
        !copy_identifier(root, "participantId", parsed.participant_id,
                         sizeof(parsed.participant_id)) ||
        !copy_identifier(root, "groupId", parsed.group_id,
                         sizeof(parsed.group_id)) ||
        !copy_identifier(root, "bootId", parsed.boot_id,
                         sizeof(parsed.boot_id)) ||
        !required_u32(root, "sequence", 1, UINT32_MAX, &parsed.sequence) ||
        !required_u32(root, "observedAtMs", 0, UINT32_MAX,
                      &observed_at_ms) ||
        !required_u32(root, "ttlMs", 100, 10000, &parsed.ttl_ms))
      return ObservationTransportResult::INVALID_OBSERVATION;

    const JsonVariantConst participant_type = root["participantType"];
    if (!participant_type.is<const char *>())
      return ObservationTransportResult::INVALID_OBSERVATION;
    const char *participant_type_text = participant_type.as<const char *>();
    if (std::strcmp(participant_type_text, "namo_node") == 0)
      parsed.participant_type = ParticipantType::NAMO_NODE;
    else if (std::strcmp(participant_type_text, "satellite") == 0)
      parsed.participant_type = ParticipantType::SATELLITE;
    else
      return ObservationTransportResult::INVALID_OBSERVATION;

    const JsonVariantConst frame_value = root["frame"];
    if (!frame_value.is<JsonObjectConst>())
      return ObservationTransportResult::INVALID_OBSERVATION;
    const JsonObjectConst frame = frame_value.as<JsonObjectConst>();
    const JsonVariantConst frame_id = frame["id"];
    if (!frame_id.is<const char *>() ||
        std::strcmp(frame_id.as<const char *>(), "sensor-local") != 0 ||
        !required_u32(frame, "poseVersion", 1, UINT32_MAX,
                      &parsed.pose_version) ||
        !required_u32(frame, "calibrationVersion", 1, UINT32_MAX,
                      &parsed.calibration_version))
      return ObservationTransportResult::INVALID_OBSERVATION;

    const JsonVariantConst tracks_value = root["tracks"];
    if (!tracks_value.is<JsonArrayConst>() ||
        !validate_tracks(tracks_value.as<JsonArrayConst>(), parsed.ttl_ms,
                         &parsed))
      return ObservationTransportResult::INVALID_OBSERVATION;

    const JsonVariantConst evidence_value = root["evidence"];
    if (!evidence_value.is<JsonObjectConst>())
      return ObservationTransportResult::INVALID_OBSERVATION;
    const JsonObjectConst evidence = evidence_value.as<JsonObjectConst>();
    if (has_member(evidence, "staticAssistArmed") ||
        has_member(evidence, "staticAssistActive") ||
        has_member(evidence, "staticAssistExitVeto") ||
        !required_bool(evidence, "presence", &parsed.presence) ||
        !required_bool(evidence, "motion", &parsed.motion) ||
        !has_member(evidence, "rangeMm") ||
        !has_member(evidence, "zonePresence"))
      return ObservationTransportResult::INVALID_OBSERVATION;
    const JsonVariantConst range = evidence["rangeMm"];
    if (!range.isNull()) {
      if (!finite_number(range, 0.0, 100000.0))
        return ObservationTransportResult::INVALID_OBSERVATION;
      parsed.has_range = true;
    }
    const JsonVariantConst zones_value = evidence["zonePresence"];
    if (!zones_value.is<JsonArrayConst>())
      return ObservationTransportResult::INVALID_OBSERVATION;
    const JsonArrayConst zones = zones_value.as<JsonArrayConst>();
    if (zones.size() > 6)
      return ObservationTransportResult::INVALID_OBSERVATION;
    for (JsonVariantConst zone : zones) {
      if (!zone.is<const char *>() ||
          !valid_identifier(zone.as<const char *>(),
                            ParticipantRecord::MAX_ID_LENGTH))
        return ObservationTransportResult::INVALID_OBSERVATION;
    }
    parsed.has_zone_presence = !zones.isNull() && zones.size() > 0;

    const JsonVariantConst health_value = root["health"];
    if (!health_value.is<JsonObjectConst>())
      return ObservationTransportResult::INVALID_OBSERVATION;
    const JsonObjectConst health = health_value.as<JsonObjectConst>();
    const JsonVariantConst health_state = health["state"];
    if (!health_state.is<const char *>() ||
        (std::strcmp(health_state.as<const char *>(), "healthy") != 0 &&
         std::strcmp(health_state.as<const char *>(), "degraded") != 0 &&
         std::strcmp(health_state.as<const char *>(), "unavailable") != 0) ||
        !required_u32(health, "sampleAgeMs", 0, parsed.ttl_ms,
                      &parsed.sample_age_ms))
      return ObservationTransportResult::INVALID_OBSERVATION;

    *output = parsed;
    return ObservationTransportResult::OK;
  }

  ObservationTransportResult parse_fusion_snapshot(
      const uint8_t *payload, std::size_t payload_size,
      FusionSnapshot *output) {
    this->document_.clear();
    const auto error = deserializeMsgPack(
        this->document_, payload, payload_size,
        DeserializationOption::NestingLimit(6));
    if (error || this->document_.overflowed() ||
        !this->document_.is<JsonObjectConst>())
      return ObservationTransportResult::INVALID_MESSAGEPACK;

    const JsonObjectConst root = this->document_.as<JsonObjectConst>();
    FusionSnapshot parsed;
    uint32_t protocol_version = 0;
    if (!required_u32(root, "protocolVersion", 1, 1, &protocol_version) ||
        !copy_identifier(root, "siteId", parsed.site_id,
                         sizeof(parsed.site_id)) ||
        !copy_identifier(root, "leaderNodeId", parsed.leader_node_id,
                         sizeof(parsed.leader_node_id)) ||
        !copy_identifier(root, "groupId", parsed.group_id,
                         sizeof(parsed.group_id)) ||
        !copy_identifier(root, "bootId", parsed.boot_id,
                         sizeof(parsed.boot_id)) ||
        !required_u32(root, "sequence", 1, UINT32_MAX,
                      &parsed.sequence) ||
        !required_u32(root, "groupTerm", 1, UINT32_MAX,
                      &parsed.group_term) ||
        !required_u32(root, "sourceSequence", 1, UINT32_MAX,
                      &parsed.source_sequence) ||
        !required_u32(root, "ttlMs", FusionSnapshot::MIN_TTL_MS,
                      FusionSnapshot::MAX_TTL_MS, &parsed.ttl_ms) ||
        !required_bool(root, "presence", &parsed.presence) ||
        !required_bool(root, "motion", &parsed.motion))
      return ObservationTransportResult::INVALID_OBSERVATION;

    const JsonVariantConst targets_value = root["targets"];
    if (!targets_value.is<JsonArrayConst>())
      return ObservationTransportResult::INVALID_OBSERVATION;
    const JsonArrayConst targets = targets_value.as<JsonArrayConst>();
    if (targets.size() > parsed.targets.size())
      return ObservationTransportResult::INVALID_OBSERVATION;
    for (JsonVariantConst item : targets) {
      if (!item.is<JsonObjectConst>())
        return ObservationTransportResult::INVALID_OBSERVATION;
      const JsonObjectConst target = item.as<JsonObjectConst>();
      uint32_t id = 0;
      uint32_t state = 0;
      uint32_t source_count = 0;
      uint32_t confidence = 0;
      if (!required_u32(target, "id", 1, UINT32_MAX, &id) ||
          !required_u32(
              target, "state",
              static_cast<uint32_t>(SpatialShadowTrackState::CONFIRMED),
              static_cast<uint32_t>(SpatialShadowTrackState::COASTING),
              &state) ||
          !required_u32(target, "sourceCount", 1, UINT8_MAX,
                        &source_count) ||
          !required_u32(target, "confidence", 0, 100, &confidence) ||
          !finite_number(target["xMm"], -100000.0, 100000.0) ||
          !finite_number(target["yMm"], -100000.0, 100000.0) ||
          !finite_number(target["vxMmS"], -20000.0, 20000.0) ||
          !finite_number(target["vyMmS"], -20000.0, 20000.0))
        return ObservationTransportResult::INVALID_OBSERVATION;
      auto &parsed_target = parsed.targets[parsed.target_count++];
      parsed_target.valid = true;
      parsed_target.fused_track_id = id;
      parsed_target.state = static_cast<SpatialShadowTrackState>(state);
      parsed_target.source_count = static_cast<uint8_t>(source_count);
      parsed_target.confidence = static_cast<uint8_t>(confidence);
      parsed_target.x_mm = target["xMm"].as<float>();
      parsed_target.y_mm = target["yMm"].as<float>();
      parsed_target.vx_mm_s = target["vxMmS"].as<float>();
      parsed_target.vy_mm_s = target["vyMmS"].as<float>();
    }
    if (parsed.presence != (parsed.target_count > 0))
      return ObservationTransportResult::INVALID_OBSERVATION;
    parsed.occupied = true;
    *output = parsed;
    return ObservationTransportResult::OK;
  }

  ObservationTransportResult parse_site_space_summary(
      const uint8_t *payload, std::size_t payload_size,
      SiteSpaceSummary *output) {
    this->document_.clear();
    const auto error = deserializeMsgPack(
        this->document_, payload, payload_size,
        DeserializationOption::NestingLimit(5));
    if (error || this->document_.overflowed() ||
        !this->document_.is<JsonObjectConst>())
      return ObservationTransportResult::INVALID_MESSAGEPACK;

    const JsonObjectConst root = this->document_.as<JsonObjectConst>();
    SiteSpaceSummary parsed{};
    uint32_t protocol_version = 0;
    uint32_t target_count = 0;
    if (!required_u32(root, "protocolVersion", 1, 1, &protocol_version) ||
        !copy_identifier(root, "siteId", parsed.site_id,
                         sizeof(parsed.site_id)) ||
        !copy_identifier(root, "spaceId", parsed.space_id,
                         sizeof(parsed.space_id)) ||
        !copy_identifier(root, "groupId", parsed.group_id,
                         sizeof(parsed.group_id)) ||
        !copy_identifier(root, "leaderNodeId", parsed.leader_node_id,
                         sizeof(parsed.leader_node_id)) ||
        !copy_identifier(root, "bootId", parsed.boot_id,
                         sizeof(parsed.boot_id)) ||
        !required_u32(root, "sequence", 1, UINT32_MAX, &parsed.sequence) ||
        !required_u32(root, "groupTerm", 1, UINT32_MAX,
                      &parsed.group_term) ||
        !required_u32(root, "sourceSequence", 1, UINT32_MAX,
                      &parsed.source_sequence) ||
        !required_u32(root, "ttlMs", SiteSpaceSummary::MIN_TTL_MS,
                      SiteSpaceSummary::MAX_TTL_MS, &parsed.ttl_ms) ||
        !required_bool(root, "presence", &parsed.presence) ||
        !required_bool(root, "motion", &parsed.motion) ||
        !required_u32(root, "targetCount", 0,
                      SpatialProductionFusionOutput::MAX_TARGETS,
                      &target_count))
      return ObservationTransportResult::INVALID_OBSERVATION;
    parsed.target_count = static_cast<uint8_t>(target_count);

    if (parsed.presence != (parsed.target_count > 0))
      return ObservationTransportResult::INVALID_OBSERVATION;
    parsed.occupied = true;
    *output = parsed;
    return ObservationTransportResult::OK;
  }

 private:
#ifdef USE_PSRAM
  json::SpiRamAllocator allocator_{};
  JsonDocument document_;
#else
  JsonDocument document_{};
#endif
};

ObservationTransport::ObservationTransport() = default;

ObservationTransport::~ObservationTransport() {
  this->destroy_socket_();
  this->destroy_parser_();
  this->destroy_inbox_();
  this->destroy_fusion_snapshot_cache_();
}

bool ObservationTransport::setup(
    ParticipantCredentialRegistry *credentials) {
  this->diagnostics_.smoke_mode = false;
  return this->setup_runtime_(credentials);
}

bool ObservationTransport::setup_smoke(
    ParticipantCredentialRegistry *credentials) {
  this->diagnostics_.smoke_mode = true;
  this->smoke_credential_ = ParticipantCredentialRecord{};
  this->smoke_credential_.occupied = true;
  this->smoke_credential_.enabled = true;
  std::strncpy(this->smoke_credential_.participant_id, SMOKE_PARTICIPANT_ID,
               ParticipantRecord::MAX_ID_LENGTH);
  this->smoke_credential_.participant_token = SMOKE_PARTICIPANT_TOKEN;
  this->smoke_credential_.key_version = 1;
  this->smoke_credential_.key = SMOKE_TRANSPORT_KEY;

  this->smoke_participant_ = ParticipantRecord{};
  this->smoke_participant_.occupied = true;
  std::strncpy(this->smoke_participant_.participant_id, SMOKE_PARTICIPANT_ID,
               ParticipantRecord::MAX_ID_LENGTH);
  this->smoke_participant_.type = ParticipantType::SATELLITE;
  this->smoke_participant_.trust = ParticipantTrust::SATELLITE;
  this->smoke_participant_.membership = ParticipantMembership::ACTIVE;

  this->smoke_readiness_ = ParticipantReadinessRecord{};
  this->smoke_readiness_.occupied = true;
  std::strncpy(this->smoke_readiness_.participant_id, SMOKE_PARTICIPANT_ID,
               ParticipantRecord::MAX_ID_LENGTH);
  auto &smoke_config = this->smoke_readiness_.config;
  smoke_config.capability_flags =
      PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_VELOCITY |
      PARTICIPANT_CAP_RANGE | PARTICIPANT_CAP_PRESENCE |
      PARTICIPANT_CAP_MOTION;
  smoke_config.max_targets = 3;
  std::strncpy(smoke_config.floorplan_id, "smoke-floor",
               ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH);
  std::strncpy(smoke_config.space_id, "smoke-room",
               ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH);
  std::strncpy(smoke_config.group_id, "smoke-group",
               ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH);
  smoke_config.geometry_version = 1;
  smoke_config.local_pose_version = 1;
  smoke_config.pose_version = 1;
  smoke_config.calibration_version = 1;
  if (!ParticipantReadinessRegistry::ready(&this->smoke_readiness_)) {
    this->diagnostics_.last_result =
        ObservationTransportResult::ADMISSION_REJECTED;
    return false;
  }

  return this->setup_runtime_(credentials);
}

bool ObservationTransport::setup_runtime_(
    ParticipantCredentialRegistry *credentials) {
  this->diagnostics_.port = PORT;
  this->credentials_ = credentials;
  if (this->credentials_ == nullptr) {
    ESP_LOGE(TAG,
             "Observation transport setup failed stage=credentials "
             "reason=null_registry");
    this->diagnostics_.last_result =
        ObservationTransportResult::UNKNOWN_KEY;
    return false;
  }
  if (!this->allocate_parser_() || !this->allocate_inbox_() ||
      !this->allocate_fusion_snapshot_cache_()) {
    ESP_LOGE(TAG,
             "Observation transport setup failed stage=workspace "
             "parser=%s inbox=%s snapshot_cache=%s",
             this->parser_ != nullptr ? "ready" : "missing",
             this->inbox_ != nullptr ? "ready" : "missing",
             this->fusion_snapshot_cache_ != nullptr ? "ready" : "missing");
    this->diagnostics_.last_result =
        ObservationTransportResult::INVALID_MESSAGEPACK;
    return false;
  }

  uint32_t random_values[2]{};
  if (!esphome::random_bytes(reinterpret_cast<uint8_t *>(random_values),
                             sizeof(random_values))) {
    ESP_LOGE(TAG,
             "Observation transport setup failed stage=boot_id "
             "reason=random_source");
    return false;
  }
  std::snprintf(this->boot_id_, sizeof(this->boot_id_), "%08x%08x",
                static_cast<unsigned>(random_values[0]),
                static_cast<unsigned>(random_values[1]));
  this->diagnostics_.sender_enabled = true;
  return true;
}

void ObservationTransport::loop(
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const GroupCoordinator &groups,
    const SiteConfigurationSnapshot *configuration,
    const SiteCoordinatorState &site,
    SiteSpaceSummaryCache *site_space_summaries,
    const char *local_participant_id, const char *site_id, uint32_t now_ms) {
  this->loop(participants, readiness, groups, configuration, nullptr, site,
             site_space_summaries, local_participant_id, site_id, now_ms);
}

void ObservationTransport::loop(
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const GroupCoordinator &groups,
    const SiteConfigurationSnapshot *configuration,
    const GroupRosterSnapshot *group_roster,
    const SiteCoordinatorState &site,
    SiteSpaceSummaryCache *site_space_summaries,
    const char *local_participant_id, const char *site_id, uint32_t now_ms) {
  this->expire_(now_ms);
  if (site_space_summaries != nullptr) {
    if (site.local_leader)
      site_space_summaries->expire(now_ms);
    else
      site_space_summaries->clear();
  }
  if (this->parser_ == nullptr || !network::is_connected() ||
      !this->ensure_socket_(now_ms) ||
      (!this->drain_pending_ && !this->socket_->ready()))
    return;

  this->drain_pending_ = false;
  for (uint8_t packet_index = 0; packet_index < MAX_PACKETS_PER_LOOP;
       ++packet_index) {
    this->ram_checkpoint_.record("observation_receive", "entry");
    sockaddr_storage sender{};
    socklen_t sender_size = sizeof(sender);
    const ssize_t received = this->socket_->recvfrom(
        this->receive_buffer_.data(), this->receive_buffer_.size(),
        reinterpret_cast<sockaddr *>(&sender), &sender_size);
    this->ram_checkpoint_.record("observation_receive", "socket");
    if (received < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        this->diagnostics_.last_result =
            ObservationTransportResult::SOCKET_ERROR;
      }
      return;
    }

    const int64_t started_us = esp_timer_get_time();
    const auto result = this->process_packet_(
        this->receive_buffer_.data(), static_cast<std::size_t>(received),
        participants, readiness, groups, configuration, group_roster, site,
        site_space_summaries, local_participant_id, site_id, now_ms);
    this->ram_checkpoint_.record("observation_receive", "complete");
    const uint32_t elapsed_us = static_cast<uint32_t>(
        std::max<int64_t>(0, esp_timer_get_time() - started_us));
    this->diagnostics_.max_process_us =
        std::max(this->diagnostics_.max_process_us, elapsed_us);
    this->diagnostics_.last_result = result;
    if (result != ObservationTransportResult::OK) {
      this->diagnostics_.rejected++;
      if (result == ObservationTransportResult::RATE_LIMITED)
        this->diagnostics_.rate_limited++;
    }
    if (packet_index + 1 == MAX_PACKETS_PER_LOOP)
      this->drain_pending_ = true;
  }
}

void ObservationTransport::send(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const ParticipantEndpointCache &endpoints, const GroupCoordinator &groups,
    const SiteConfigurationSnapshot *configuration,
    const LocalObservation &observation, uint32_t local_pose_version,
    const char *site_id, uint32_t now_ms) {
  this->send(identity, participants, readiness, endpoints, groups,
             configuration, nullptr, observation, local_pose_version, site_id,
             now_ms);
}

void ObservationTransport::send(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const ParticipantEndpointCache &endpoints, const GroupCoordinator &groups,
    const SiteConfigurationSnapshot *configuration,
    const GroupRosterSnapshot *group_roster,
    const LocalObservation &observation, uint32_t local_pose_version,
    const char *site_id, uint32_t now_ms) {
  if (this->next_send_ms_ != 0 &&
      !time_reached(now_ms, this->next_send_ms_))
    return;
  this->next_send_ms_ = now_ms + 500;

  const GroupCoordinatorState *selected_group =
      groups.local_group(configuration, identity.node_id);
  const GroupMembershipView *membership = groups.local_membership();

  const char *leader_id =
      selected_group != nullptr ? selected_group->leader_node_id : nullptr;
  const auto *direct_participant =
      leader_id != nullptr ? participants.find(leader_id) : nullptr;
  const auto *direct_profile =
      leader_id != nullptr ? readiness.find(leader_id) : nullptr;
  const ParticipantRecord *participant = direct_participant;
  const ParticipantReadinessRecord *profile = direct_profile;
  ParticipantRecord canonical_participant{};
  ParticipantReadinessRecord canonical_readiness{};
  if (leader_id != nullptr &&
      (direct_participant == nullptr ||
       direct_participant->membership != ParticipantMembership::REVOKED) &&
      ((group_roster != nullptr &&
        build_group_roster_runtime_records(
            group_roster, identity.node_id, leader_id,
            &canonical_participant, &canonical_readiness)) ||
       build_canonical_group_runtime_records(
           configuration, identity.node_id, leader_id,
           &canonical_participant, &canonical_readiness))) {
    participant = &canonical_participant;
    profile = &canonical_readiness;
  }
  const auto *credential =
      this->credentials_ != nullptr && leader_id != nullptr
          ? select_site_sender_credential(identity, configuration,
                                           *this->credentials_, leader_id)
          : nullptr;
  const auto *endpoint =
      leader_id != nullptr ? endpoints.find(leader_id) : nullptr;
  if (membership != nullptr &&
      (!membership->local_admitted ||
       !membership->contains(identity.node_id) ||
       (leader_id != nullptr && !membership->contains(leader_id)))) {
    this->skip_send_(ObservationSendResult::GROUP_UNAVAILABLE, leader_id);
    return;
  }
  const auto eligibility = evaluate_observation_send(
      identity.node_id, selected_group, false, participant, profile,
      credential, endpoint, local_pose_version, now_ms);
  if (eligibility != ObservationSendResult::OK) {
    this->skip_send_(eligibility, leader_id);
    return;
  }
  if (!network::is_connected() || !this->ensure_socket_(now_ms)) {
    this->skip_send_(ObservationSendResult::SOCKET_ERROR, leader_id);
    return;
  }

  uint32_t sequence = ++this->send_sequence_;
  if (sequence == 0)
    sequence = ++this->send_sequence_;
  ObservationEncodeContext context{};
  context.site_id = site_id;
  context.participant_id = identity.node_id;
  context.group_id = selected_group->group_id;
  context.boot_id = this->boot_id_;
  context.sequence = sequence;
  context.pose_version = profile->config.local_pose_version;
  const auto *local_member =
      group_roster != nullptr ? group_roster->find(identity.node_id) : nullptr;
  if (local_member == nullptr && configuration != nullptr)
    local_member = configuration->find(identity.node_id);
  context.calibration_version =
      local_member != nullptr && local_member->spatial_profile.committed
          ? local_member->spatial_profile.calibration_version
          : profile->config.calibration_version;
  context.capability_flags = profile->config.capability_flags;
  context.max_targets = profile->config.max_targets;
  context.now_ms = now_ms;
  context.observation = &observation;

  const int64_t encode_started_us = esp_timer_get_time();
  this->ram_checkpoint_.record("observation_send", "entry");
  std::size_t payload_size = 0;
  const auto encode_result = encode_observation_messagepack(
      context,
      this->send_buffer_.data() +
          AuthenticatedObservationEnvelope::PREFIX_SIZE,
      AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE, &payload_size);
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  const bool prefix_ready =
      encode_result == ObservationEncodeResult::OK &&
      encode_authenticated_observation_prefix(
          credential->key_version, credential->participant_token,
          payload_size, this->send_buffer_.data(), this->send_buffer_.size(),
          &signed_size, &packet_size);
  this->ram_checkpoint_.record("observation_send", "encoded");
  const bool envelope_ready =
      prefix_ready &&
      sign_hmac(&this->hmac_, this->send_buffer_.data(), signed_size,
                credential->key, this->send_buffer_.data() + signed_size);
  this->ram_checkpoint_.record("observation_send", "hmac");
  const uint32_t encode_us = static_cast<uint32_t>(std::max<int64_t>(
      0, esp_timer_get_time() - encode_started_us));
  this->diagnostics_.max_encode_us =
      std::max(this->diagnostics_.max_encode_us, encode_us);
  if (!envelope_ready) {
    this->skip_send_(
        encode_result == ObservationEncodeResult::OBSERVATION_EXPIRED
            ? ObservationSendResult::OBSERVATION_UNAVAILABLE
            : ObservationSendResult::ENCODE_FAILED,
        leader_id);
    return;
  }

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(PORT);
  target.sin_addr.s_addr = endpoint->ipv4;
  const int64_t send_started_us = esp_timer_get_time();
  const ssize_t sent = this->socket_->sendto(
      this->send_buffer_.data(), packet_size, 0,
      reinterpret_cast<sockaddr *>(&target), sizeof(target));
  this->ram_checkpoint_.record("observation_send", "socket");
  const uint32_t send_us = static_cast<uint32_t>(std::max<int64_t>(
      0, esp_timer_get_time() - send_started_us));
  this->diagnostics_.max_send_us =
      std::max(this->diagnostics_.max_send_us, send_us);
  if (sent != static_cast<ssize_t>(packet_size)) {
    this->skip_send_(ObservationSendResult::SOCKET_ERROR, leader_id);
    return;
  }

  std::strncpy(this->diagnostics_.send_target, leader_id,
               sizeof(this->diagnostics_.send_target) - 1);
  this->diagnostics_.send_target[
      sizeof(this->diagnostics_.send_target) - 1] = '\0';
  this->diagnostics_.sent++;
  this->diagnostics_.last_send_result = ObservationSendResult::OK;
}

void ObservationTransport::send_fusion_snapshot(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const ParticipantEndpointCache &endpoints,
    const GroupCoordinator &groups,
    const SiteConfigurationSnapshot *configuration,
    const SpatialShadowFusionOutput &fusion,
    const SpatialProductionFusionOutput &production,
    const char *site_id, uint32_t now_ms) {
  this->send_fusion_snapshot(identity, participants, readiness, endpoints,
                             groups, configuration, nullptr, fusion,
                             production, site_id, now_ms);
}

void ObservationTransport::send_fusion_snapshot(
    const PlatformIdentity &identity,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const ParticipantEndpointCache &endpoints,
    const GroupCoordinator &groups,
    const SiteConfigurationSnapshot *configuration,
    const GroupRosterSnapshot *group_roster,
    const SpatialShadowFusionOutput &fusion,
    const SpatialProductionFusionOutput &production,
    const char *site_id, uint32_t now_ms) {
  if (!production.build_enabled || !production.authoritative ||
      production.state != SpatialProductionFusionState::ACTIVE ||
      fusion.sequence == 0 || site_id == nullptr || site_id[0] == '\0' ||
      this->credentials_ == nullptr ||
      (this->next_fusion_snapshot_send_ms_ != 0 &&
       !time_reached(now_ms, this->next_fusion_snapshot_send_ms_)))
    return;
  this->next_fusion_snapshot_send_ms_ = now_ms + 50;

  const GroupCoordinatorState *selected_group =
      groups.local_group(configuration, identity.node_id);
  const GroupMembershipView *membership = groups.local_membership();
  if (selected_group == nullptr || !selected_group->local_leader ||
      selected_group->term == 0 ||
      (selected_group->assignment_revision != 0 &&
       (!selected_group->canonical_assignment ||
        !selected_group->credentials_complete)) ||
      std::strcmp(selected_group->leader_node_id, identity.node_id) != 0 ||
      !network::is_connected() || !this->ensure_socket_(now_ms))
    return;

  const bool canonical_membership =
      membership != nullptr && membership->valid &&
      membership->local_admitted;
  const std::size_t candidate_count =
      canonical_membership ? membership->members.size()
                           : ParticipantRegistry::MAX_REMOTE_PARTICIPANTS;
  for (std::size_t attempt = 0; attempt < candidate_count; ++attempt) {
    const std::size_t slot =
        (this->fusion_snapshot_send_cursor_ + attempt) %
        candidate_count;
    const char *participant_id = nullptr;
    const ParticipantRecord *participant = nullptr;
    const ParticipantReadinessRecord *profile = nullptr;
    ParticipantRecord canonical_participant{};
    ParticipantReadinessRecord canonical_readiness{};
    if (canonical_membership) {
      const auto &member = membership->members[slot];
      if (!member.occupied ||
          std::strcmp(member.participant_id, identity.node_id) == 0)
        continue;
      participant_id = member.participant_id;
      const auto *direct = participants.find(participant_id);
      if (direct != nullptr &&
          direct->membership == ParticipantMembership::REVOKED)
        continue;
      if (!((group_roster != nullptr &&
             build_group_roster_runtime_records(
                 group_roster, identity.node_id, participant_id,
                 &canonical_participant, &canonical_readiness)) ||
            build_canonical_group_runtime_records(
                configuration, identity.node_id, participant_id,
                &canonical_participant, &canonical_readiness)))
        continue;
      participant = &canonical_participant;
      profile = &canonical_readiness;
    } else {
      participant = participants.record_at(slot);
      if (participant == nullptr ||
          participant->membership != ParticipantMembership::ACTIVE)
        continue;
      participant_id = participant->participant_id;
      profile = readiness.find(participant_id);
    }
    const auto *target_credential =
        this->credentials_->find(participant_id);
    const auto *sender_credential = select_site_sender_credential(
        identity, configuration, *this->credentials_,
        participant_id);
    const auto *endpoint = endpoints.find(participant_id);
    if (!ParticipantReadinessRegistry::ready(profile) ||
        std::strcmp(profile->config.group_id, selected_group->group_id) != 0 ||
        (profile->config.capability_flags &
         PARTICIPANT_CAP_POINT_TRACKS) == 0 ||
        target_credential == nullptr || !target_credential->enabled ||
        sender_credential == nullptr || !sender_credential->enabled ||
        !ParticipantEndpointCache::is_online(endpoint, now_ms) ||
        endpoint->ipv4 == 0)
      continue;
    if (this->fusion_snapshot_sent_at_ms_[slot] != 0 &&
        now_ms - this->fusion_snapshot_sent_at_ms_[slot] < 500)
      continue;

    uint32_t sequence = ++this->fusion_snapshot_sequence_;
    if (sequence == 0)
      sequence = ++this->fusion_snapshot_sequence_;
    FusionSnapshotEncodeContext context{};
    context.site_id = site_id;
    context.leader_node_id = identity.node_id;
    context.group_id = selected_group->group_id;
    context.boot_id = this->boot_id_;
    context.sequence = sequence;
    context.group_term = selected_group->term;
    context.fusion = &fusion;

    const int64_t encode_started_us = esp_timer_get_time();
    this->ram_checkpoint_.record("fusion_snapshot_send", "entry");
    std::size_t payload_size = 0;
    const auto encode_result = encode_fusion_snapshot_messagepack(
        context,
        this->send_buffer_.data() +
            AuthenticatedObservationEnvelope::PREFIX_SIZE,
        AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE, &payload_size);
    std::size_t signed_size = 0;
    std::size_t packet_size = 0;
    const bool prefix_ready =
        encode_result == ObservationEncodeResult::OK &&
        encode_authenticated_fusion_snapshot_prefix(
            sender_credential->key_version,
            sender_credential->participant_token,
            payload_size, this->send_buffer_.data(),
            this->send_buffer_.size(), &signed_size, &packet_size);
    this->ram_checkpoint_.record("fusion_snapshot_send", "encoded");
    const bool envelope_ready =
        prefix_ready &&
        sign_hmac(&this->hmac_, this->send_buffer_.data(), signed_size,
                  sender_credential->key,
                  this->send_buffer_.data() + signed_size);
    this->ram_checkpoint_.record("fusion_snapshot_send", "hmac");
    const uint32_t encode_us = static_cast<uint32_t>(std::max<int64_t>(
        0, esp_timer_get_time() - encode_started_us));
    this->diagnostics_.max_encode_us =
        std::max(this->diagnostics_.max_encode_us, encode_us);
    if (!envelope_ready) {
      this->diagnostics_.fusion_snapshot_rejected++;
      return;
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(PORT);
    target.sin_addr.s_addr = endpoint->ipv4;
    const int64_t send_started_us = esp_timer_get_time();
    const ssize_t sent = this->socket_->sendto(
        this->send_buffer_.data(), packet_size, 0,
        reinterpret_cast<sockaddr *>(&target), sizeof(target));
    this->ram_checkpoint_.record("fusion_snapshot_send", "socket");
    const uint32_t send_us = static_cast<uint32_t>(std::max<int64_t>(
        0, esp_timer_get_time() - send_started_us));
    this->diagnostics_.max_send_us =
        std::max(this->diagnostics_.max_send_us, send_us);
    if (sent != static_cast<ssize_t>(packet_size)) {
      this->diagnostics_.fusion_snapshot_rejected++;
      return;
    }
    this->fusion_snapshot_sent_at_ms_[slot] = now_ms;
    this->fusion_snapshot_send_cursor_ = static_cast<uint8_t>(
        (slot + 1) % candidate_count);
    this->diagnostics_.fusion_snapshot_sent++;
    return;
  }
}

void ObservationTransport::send_site_space_summary(
    const PlatformIdentity &identity,
    const ParticipantEndpointCache &endpoints,
    const GroupCoordinator &groups, const SiteCoordinatorState &site,
    const SiteConfigurationSnapshot *configuration,
    const SpatialProductionFusionOutput &production,
    const char *site_id, SiteSpaceSummaryCache *site_space_summaries,
    uint32_t now_ms) {
  if (!production.build_enabled || !production.authoritative ||
      production.state != SpatialProductionFusionState::ACTIVE ||
      production.source_sequence == 0 || configuration == nullptr ||
      site_id == nullptr || site_id[0] == '\0' ||
      this->credentials_ == nullptr ||
      (this->next_site_space_summary_send_ms_ != 0 &&
       !time_reached(now_ms, this->next_site_space_summary_send_ms_)))
    return;

  const auto *selected_group =
      groups.local_group(configuration, identity.node_id);
  const auto *local_member = configuration->find(identity.node_id);
  if (selected_group == nullptr || !selected_group->configured ||
      !selected_group->local_leader || selected_group->term == 0 ||
      (selected_group->assignment_revision != 0 &&
       (!selected_group->canonical_assignment ||
        !selected_group->credentials_complete)) ||
      std::strcmp(selected_group->leader_node_id, identity.node_id) != 0 ||
      local_member == nullptr || !local_member->spatial_profile.committed ||
      local_member->spatial_profile.space_id[0] == '\0' ||
      local_member->spatial_profile.group_id[0] == '\0' ||
      std::strcmp(local_member->spatial_profile.group_id,
                  selected_group->group_id) != 0)
    return;

  this->next_site_space_summary_send_ms_ = now_ms + 2000;
  uint32_t sequence = ++this->site_space_summary_sequence_;
  if (sequence == 0)
    sequence = ++this->site_space_summary_sequence_;
  SiteSpaceSummary summary{};
  if (!build_site_space_summary(
          site_id, local_member->spatial_profile.space_id,
          selected_group->group_id, identity.node_id, this->boot_id_,
          sequence, selected_group->term, production,
          &summary)) {
    this->diagnostics_.site_space_summary_rejected++;
    return;
  }

  if (site.local_leader &&
      std::strcmp(site.leader_node_id, identity.node_id) == 0) {
    const auto admission = evaluate_site_space_summary_admission(
        summary, identity.node_id, identity.node_id, site_id, site,
        configuration, selected_group);
    this->diagnostics_.last_site_space_summary_admission = admission;
    if (admission != SiteSpaceSummaryAdmissionResult::OK ||
        site_space_summaries == nullptr ||
        site_space_summaries->store(summary, now_ms) !=
            SiteSpaceSummaryAdmissionResult::OK) {
      this->diagnostics_.site_space_summary_rejected++;
      return;
    }
    this->diagnostics_.site_space_summary_admitted++;
    return;
  }

  if (site.leader_node_id[0] == '\0' ||
      std::strcmp(site.leader_node_id, identity.node_id) == 0 ||
      !network::is_connected() || !this->ensure_socket_(now_ms))
    return;
  const auto *site_leader = configuration->find(site.leader_node_id);
  const auto *target_credential =
      this->credentials_->find(site.leader_node_id);
  const auto *sender_credential = select_site_sender_credential(
      identity, configuration, *this->credentials_, site.leader_node_id);
  const auto *endpoint = endpoints.find(site.leader_node_id);
  if (site_leader == nullptr ||
      site_leader->membership != ParticipantMembership::ACTIVE ||
      target_credential == nullptr || !target_credential->enabled ||
      sender_credential == nullptr || !sender_credential->enabled ||
      !ParticipantEndpointCache::is_online(endpoint, now_ms) ||
      endpoint->ipv4 == 0)
    return;

  std::size_t payload_size = 0;
  this->ram_checkpoint_.record("site_summary_send", "entry");
  const auto encode_result = encode_site_space_summary_messagepack(
      summary,
      this->send_buffer_.data() +
          AuthenticatedObservationEnvelope::PREFIX_SIZE,
      AuthenticatedObservationEnvelope::MAX_PAYLOAD_SIZE, &payload_size);
  std::size_t signed_size = 0;
  std::size_t packet_size = 0;
  const bool prefix_ready =
      encode_result == ObservationEncodeResult::OK &&
      encode_authenticated_site_space_summary_prefix(
          sender_credential->key_version,
          sender_credential->participant_token,
          payload_size, this->send_buffer_.data(), this->send_buffer_.size(),
          &signed_size, &packet_size);
  this->ram_checkpoint_.record("site_summary_send", "encoded");
  const bool envelope_ready =
      prefix_ready &&
      sign_hmac(&this->hmac_, this->send_buffer_.data(), signed_size,
                sender_credential->key,
                this->send_buffer_.data() + signed_size);
  this->ram_checkpoint_.record("site_summary_send", "hmac");
  if (!envelope_ready) {
    this->diagnostics_.site_space_summary_rejected++;
    return;
  }

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(PORT);
  target.sin_addr.s_addr = endpoint->ipv4;
  const ssize_t sent = this->socket_->sendto(
      this->send_buffer_.data(), packet_size, 0,
      reinterpret_cast<sockaddr *>(&target), sizeof(target));
  this->ram_checkpoint_.record("site_summary_send", "socket");
  if (sent != static_cast<ssize_t>(packet_size)) {
    this->diagnostics_.site_space_summary_rejected++;
    return;
  }
  this->diagnostics_.site_space_summary_sent++;
}

bool ObservationTransport::ensure_socket_(uint32_t now_ms) {
  if (this->socket_ != nullptr)
    return true;
  if (this->next_socket_attempt_ms_ != 0 &&
      static_cast<int32_t>(now_ms - this->next_socket_attempt_ms_) < 0)
    return false;
  this->next_socket_attempt_ms_ = now_ms + SOCKET_RETRY_MS;

  this->socket_ =
      socket::socket_ip_loop_monitored(SOCK_DGRAM, IPPROTO_UDP).release();
  if (this->socket_ == nullptr || this->socket_->setblocking(false) != 0) {
    ESP_LOGE(TAG,
             "Observation socket failed stage=create_or_nonblocking errno=%d",
             errno);
    this->diagnostics_.last_result = ObservationTransportResult::SOCKET_ERROR;
    this->destroy_socket_();
    return false;
  }
  sockaddr_storage server{};
  const socklen_t server_size = socket::set_sockaddr_any(
      reinterpret_cast<sockaddr *>(&server), sizeof(server), PORT);
  if (server_size == 0 ||
      this->socket_->bind(reinterpret_cast<sockaddr *>(&server), server_size) !=
          0) {
    ESP_LOGE(TAG, "Observation socket failed stage=bind port=%u errno=%d",
             static_cast<unsigned>(PORT), errno);
    this->diagnostics_.last_result = ObservationTransportResult::SOCKET_ERROR;
    this->destroy_socket_();
    return false;
  }
  this->next_socket_attempt_ms_ = 0;
  this->diagnostics_.socket_ready = true;
  if (this->diagnostics_.smoke_mode) {
    ESP_LOGW(TAG,
             "Development shadow transport enabled on UDP %u; this build must "
             "not be released",
             static_cast<unsigned>(PORT));
  } else {
    ESP_LOGI(TAG, "Authenticated observation transport listening on UDP %u",
             static_cast<unsigned>(PORT));
  }
  return true;
}

void ObservationTransport::destroy_socket_() {
  delete this->socket_;
  this->socket_ = nullptr;
  this->diagnostics_.socket_ready = false;
}

ObservationTransportResult ObservationTransport::process_packet_(
    const uint8_t *packet, std::size_t packet_size,
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness,
    const GroupCoordinator &groups,
    const SiteConfigurationSnapshot *configuration,
    const GroupRosterSnapshot *group_roster,
    const SiteCoordinatorState &site,
    SiteSpaceSummaryCache *site_space_summaries,
    const char *local_participant_id, const char *site_id, uint32_t now_ms) {
  this->diagnostics_.received++;
  this->ram_checkpoint_.record("observation_packet", "entry");
  if (packet_size > AuthenticatedObservationEnvelope::MAX_PACKET_SIZE)
    return ObservationTransportResult::PACKET_TOO_LARGE;

  AuthenticatedObservationEnvelope envelope;
  if (!parse_authenticated_observation_envelope(packet, packet_size,
                                                &envelope))
    return ObservationTransportResult::INVALID_ENVELOPE;
  this->ram_checkpoint_.record("observation_packet", "envelope");
  if (this->credentials_ == nullptr)
    return ObservationTransportResult::UNKNOWN_KEY;
  const auto *credential = this->credentials_->resolve(
      envelope.participant_token, envelope.key_version);
  if (credential == nullptr && this->diagnostics_.smoke_mode &&
      this->smoke_credential_.enabled &&
      this->smoke_credential_.participant_token == envelope.participant_token &&
      this->smoke_credential_.key_version == envelope.key_version)
    credential = &this->smoke_credential_;
  if (credential == nullptr)
    return ObservationTransportResult::UNKNOWN_KEY;
  const std::size_t signed_size =
      AuthenticatedObservationEnvelope::PREFIX_SIZE + envelope.payload_size;
  const bool authenticated = verify_hmac(
      &this->hmac_, packet, signed_size, credential->key, envelope.tag);
  this->ram_checkpoint_.record("observation_packet", "hmac");
  if (!authenticated)
    return ObservationTransportResult::AUTHENTICATION_FAILED;
  this->diagnostics_.authenticated++;
  if (!this->rate_limiter_.allow(envelope.participant_token, now_ms))
    return ObservationTransportResult::RATE_LIMITED;

  if (envelope.message_type ==
      AuthenticatedTransportMessageType::SITE_SPACE_SUMMARY) {
    this->diagnostics_.site_space_summary_received++;
    SiteSpaceSummary summary{};
    const auto parse_result = this->parser_->parse_site_space_summary(
        envelope.payload, envelope.payload_size, &summary);
    this->ram_checkpoint_.record("observation_packet", "summary_parse");
    if (parse_result != ObservationTransportResult::OK) {
      this->diagnostics_.site_space_summary_rejected++;
      return parse_result;
    }
    const GroupCoordinatorState *expected_group = nullptr;
    for (std::size_t slot = 0; slot < GroupCoordinator::MAX_GROUPS; ++slot) {
      const auto *candidate = groups.group_at(slot);
      if (candidate != nullptr && candidate->configured &&
          std::strcmp(candidate->group_id, summary.group_id) == 0) {
        expected_group = candidate;
        break;
      }
    }
    const auto admission_result = evaluate_site_space_summary_admission(
        summary, credential->participant_id, local_participant_id, site_id,
        site, configuration, expected_group);
    this->diagnostics_.last_site_space_summary_admission = admission_result;
    if (admission_result != SiteSpaceSummaryAdmissionResult::OK ||
        site_space_summaries == nullptr) {
      this->diagnostics_.site_space_summary_rejected++;
      return ObservationTransportResult::ADMISSION_REJECTED;
    }
    const auto store_result = site_space_summaries->store(summary, now_ms);
    this->diagnostics_.last_site_space_summary_admission = store_result;
    if (store_result != SiteSpaceSummaryAdmissionResult::OK) {
      this->diagnostics_.site_space_summary_rejected++;
      return ObservationTransportResult::ADMISSION_REJECTED;
    }
    this->diagnostics_.site_space_summary_admitted++;
    return ObservationTransportResult::OK;
  }

  if (envelope.message_type ==
      AuthenticatedTransportMessageType::FUSION_SNAPSHOT) {
    this->diagnostics_.fusion_snapshot_received++;
    FusionSnapshot snapshot;
    const auto parse_result = this->parser_->parse_fusion_snapshot(
        envelope.payload, envelope.payload_size, &snapshot);
    this->ram_checkpoint_.record("observation_packet", "snapshot_parse");
    if (parse_result != ObservationTransportResult::OK) {
      this->diagnostics_.fusion_snapshot_rejected++;
      return parse_result;
    }
    const GroupCoordinatorState *selected_group =
        groups.local_group(configuration, local_participant_id);
    const GroupMembershipView *membership = groups.local_membership();
    if (membership != nullptr &&
        (!membership->local_admitted ||
         !membership->contains(credential->participant_id))) {
      this->diagnostics_.fusion_snapshot_rejected++;
      return ObservationTransportResult::ADMISSION_REJECTED;
    }
    const ParticipantRecord *participant =
        participants.find(credential->participant_id);
    const ParticipantReadinessRecord *profile =
        readiness.find(credential->participant_id);
    ParticipantRecord canonical_participant{};
    ParticipantReadinessRecord canonical_readiness{};
    if ((participant == nullptr ||
         participant->membership != ParticipantMembership::REVOKED) &&
        ((group_roster != nullptr &&
          build_group_roster_runtime_records(
              group_roster, local_participant_id,
              credential->participant_id, &canonical_participant,
              &canonical_readiness)) ||
         build_canonical_group_runtime_records(
             configuration, local_participant_id,
             credential->participant_id, &canonical_participant,
             &canonical_readiness))) {
      participant = &canonical_participant;
      profile = &canonical_readiness;
    }
    const auto admission_result = evaluate_fusion_snapshot_admission(
        snapshot, credential->participant_id, site_id, participant,
        profile, selected_group);
    this->diagnostics_.last_fusion_snapshot_admission = admission_result;
    if (admission_result != FusionSnapshotAdmissionResult::OK) {
      this->diagnostics_.fusion_snapshot_rejected++;
      return ObservationTransportResult::ADMISSION_REJECTED;
    }
    snapshot.received_at_ms = now_ms;
    snapshot.expires_at_ms = now_ms + snapshot.ttl_ms;
    if (this->fusion_snapshot_cache_ == nullptr ||
        !this->fusion_snapshot_cache_->store(snapshot)) {
      this->diagnostics_.fusion_snapshot_rejected++;
      return ObservationTransportResult::INVALID_OBSERVATION;
    }
    std::strncpy(this->diagnostics_.fusion_snapshot_source,
                 snapshot.leader_node_id,
                 sizeof(this->diagnostics_.fusion_snapshot_source) - 1);
    this->diagnostics_.fusion_snapshot_source[
        sizeof(this->diagnostics_.fusion_snapshot_source) - 1] = '\0';
    this->diagnostics_.fusion_snapshot_admitted++;
    return ObservationTransportResult::OK;
  }

  ParsedObservation parsed;
  const auto parse_result =
      this->parser_->parse(envelope.payload, envelope.payload_size, &parsed);
  this->ram_checkpoint_.record("observation_packet", "observation_parse");
  if (parse_result != ObservationTransportResult::OK)
    return parse_result;
  if (std::strcmp(parsed.participant_id, credential->participant_id) != 0)
    return ObservationTransportResult::IDENTITY_MISMATCH;
  const GroupMembershipView *membership = groups.local_membership();
  if (membership != nullptr &&
      (!membership->local_admitted ||
       !membership->contains(parsed.participant_id) ||
       std::strcmp(membership->group_id, parsed.group_id) != 0))
    return ObservationTransportResult::ADMISSION_REJECTED;

  const bool use_smoke_fixture =
      this->diagnostics_.smoke_mode && credential == &this->smoke_credential_;
  const SiteConfigurationMember *canonical_member =
      group_roster != nullptr
          ? group_roster->find(parsed.participant_id)
          : nullptr;
  if (canonical_member == nullptr && configuration != nullptr)
    canonical_member = configuration->find(parsed.participant_id);
  const char *expected_group_id =
      canonical_member != nullptr &&
              canonical_member->spatial_profile.committed &&
              canonical_member->spatial_profile.group_id[0] != '\0'
          ? canonical_member->spatial_profile.group_id
          : nullptr;
  const ParticipantRecord *participant =
      participants.find(parsed.participant_id);
  const ParticipantReadinessRecord *profile =
      readiness.find(parsed.participant_id);
  ParticipantRecord canonical_participant{};
  ParticipantReadinessRecord canonical_readiness{};
  if (!use_smoke_fixture &&
      (participant == nullptr ||
       participant->membership != ParticipantMembership::REVOKED) &&
      ((group_roster != nullptr &&
        build_group_roster_runtime_records(
            group_roster, local_participant_id, parsed.participant_id,
            &canonical_participant, &canonical_readiness)) ||
       build_canonical_group_runtime_records(
           configuration, local_participant_id, parsed.participant_id,
           &canonical_participant, &canonical_readiness))) {
    participant = &canonical_participant;
    profile = &canonical_readiness;
  }
  const auto admission_result = this->admission_.admit(
      use_smoke_fixture ? &this->smoke_participant_ : participant,
      use_smoke_fixture ? &this->smoke_readiness_ : profile, site_id,
      parsed.metadata(), expected_group_id);
  this->diagnostics_.last_admission = admission_result;
  if (admission_result != ObservationAdmissionResult::OK)
    return ObservationTransportResult::ADMISSION_REJECTED;

  const uint32_t remaining_ms = parsed.ttl_ms - parsed.sample_age_ms;
  ObservationTransportSample sample;
  sample.occupied = true;
  std::strncpy(sample.participant_id, parsed.participant_id,
               ParticipantRecord::MAX_ID_LENGTH);
  sample.participant_id[ParticipantRecord::MAX_ID_LENGTH] = '\0';
  sample.participant_token = envelope.participant_token;
  sample.sequence = parsed.sequence;
  sample.received_at_ms = now_ms;
  sample.expires_at_ms = now_ms + remaining_ms;
  sample.sample_age_ms = parsed.sample_age_ms;
  sample.ttl_ms = parsed.ttl_ms;
  sample.pose_version = parsed.pose_version;
  sample.calibration_version = parsed.calibration_version;
  sample.track_count = parsed.track_count;
  sample.presence = parsed.presence;
  sample.motion = parsed.motion;
  sample.tracks = parsed.tracks;
  this->queue_(sample);
  this->ram_checkpoint_.record("observation_packet", "stored");
  this->diagnostics_.admitted++;
  return ObservationTransportResult::OK;
}

void ObservationTransport::queue_(const ObservationTransportSample &sample) {
  if (this->inbox_ == nullptr || !this->inbox_->store(sample))
    return;
  this->update_pending_count_();
}

void ObservationTransport::expire_(uint32_t now_ms) {
  const bool snapshot_expired =
      this->fusion_snapshot_cache_ != nullptr &&
      this->fusion_snapshot_cache_->expire(now_ms);
  if (snapshot_expired) {
    this->diagnostics_.fusion_snapshot_expired++;
    this->diagnostics_.fusion_snapshot_source[0] = '\0';
  }
  if (this->inbox_ == nullptr)
    return;
  const uint8_t inbox_size_before = this->inbox_->size();
  this->inbox_->expire(now_ms);
  if (inbox_size_before != this->inbox_->size())
    this->update_pending_count_();
}

void ObservationTransport::update_pending_count_() {
  this->diagnostics_.pending =
      this->inbox_ != nullptr ? this->inbox_->size() : 0;
}

bool ObservationTransport::allocate_parser_() {
  if (this->parser_ != nullptr)
    return true;
#ifdef USE_PSRAM
  void *memory =
      heap_caps_calloc(1, sizeof(ObservationTransportParser),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    this->parser_ = new (memory) ObservationTransportParser();
    this->parser_in_psram_ = true;
    log_transport_allocation("observation_parser", "psram",
                             sizeof(ObservationTransportParser),
                             RuntimeAllocationLogLevel::INFO);
    return true;
  }
  log_transport_allocation("observation_parser", "psram_failed",
                           sizeof(ObservationTransportParser),
                           RuntimeAllocationLogLevel::WARN);
#endif
  this->parser_ = new (std::nothrow) ObservationTransportParser();
  this->parser_in_psram_ = false;
  if (this->parser_ != nullptr) {
    log_transport_allocation("observation_parser",
#ifdef USE_PSRAM
                             "internal_fallback",
#else
                             "internal",
#endif
                             sizeof(ObservationTransportParser),
#ifdef USE_PSRAM
                             RuntimeAllocationLogLevel::WARN
#else
                             RuntimeAllocationLogLevel::INFO
#endif
    );
  } else {
    log_transport_allocation("observation_parser", "allocation_failed",
                             sizeof(ObservationTransportParser),
                             RuntimeAllocationLogLevel::ERROR);
  }
  return this->parser_ != nullptr;
}

void ObservationTransport::destroy_parser_() {
  if (this->parser_ == nullptr)
    return;
#ifdef USE_PSRAM
  if (this->parser_in_psram_) {
    this->parser_->~ObservationTransportParser();
    heap_caps_free(this->parser_);
    this->parser_ = nullptr;
    this->parser_in_psram_ = false;
    return;
  }
#endif
  delete this->parser_;
  this->parser_ = nullptr;
  this->parser_in_psram_ = false;
}

bool ObservationTransport::allocate_inbox_() {
  if (this->inbox_ != nullptr)
    return true;
#ifdef USE_PSRAM
  void *memory =
      heap_caps_calloc(1, sizeof(ObservationTransportInbox),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    this->inbox_ = new (memory) ObservationTransportInbox();
    this->inbox_in_psram_ = true;
    log_transport_allocation("observation_inbox", "psram",
                             sizeof(ObservationTransportInbox),
                             RuntimeAllocationLogLevel::INFO);
    return true;
  }
  log_transport_allocation("observation_inbox", "psram_failed",
                           sizeof(ObservationTransportInbox),
                           RuntimeAllocationLogLevel::WARN);
#endif
  this->inbox_ = new (std::nothrow) ObservationTransportInbox();
  this->inbox_in_psram_ = false;
  if (this->inbox_ != nullptr) {
    log_transport_allocation("observation_inbox",
#ifdef USE_PSRAM
                             "internal_fallback",
#else
                             "internal",
#endif
                             sizeof(ObservationTransportInbox),
#ifdef USE_PSRAM
                             RuntimeAllocationLogLevel::WARN
#else
                             RuntimeAllocationLogLevel::INFO
#endif
    );
  } else {
    log_transport_allocation("observation_inbox", "allocation_failed",
                             sizeof(ObservationTransportInbox),
                             RuntimeAllocationLogLevel::ERROR);
  }
  return this->inbox_ != nullptr;
}

void ObservationTransport::destroy_inbox_() {
  if (this->inbox_ == nullptr)
    return;
#ifdef USE_PSRAM
  if (this->inbox_in_psram_) {
    this->inbox_->~ObservationTransportInbox();
    heap_caps_free(this->inbox_);
    this->inbox_ = nullptr;
    this->inbox_in_psram_ = false;
    return;
  }
#endif
  delete this->inbox_;
  this->inbox_ = nullptr;
}

bool ObservationTransport::allocate_fusion_snapshot_cache_() {
  if (this->fusion_snapshot_cache_ != nullptr)
    return true;
#ifdef USE_PSRAM
  void *memory =
      heap_caps_calloc(1, sizeof(FusionSnapshotCache),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    this->fusion_snapshot_cache_ = new (memory) FusionSnapshotCache();
    this->fusion_snapshot_cache_in_psram_ = true;
    log_transport_allocation("fusion_snapshot_cache", "psram",
                             sizeof(FusionSnapshotCache),
                             RuntimeAllocationLogLevel::INFO);
    return true;
  }
  log_transport_allocation("fusion_snapshot_cache", "psram_failed",
                           sizeof(FusionSnapshotCache),
                           RuntimeAllocationLogLevel::WARN);
#endif
  this->fusion_snapshot_cache_ =
      new (std::nothrow) FusionSnapshotCache();
  this->fusion_snapshot_cache_in_psram_ = false;
  if (this->fusion_snapshot_cache_ != nullptr) {
    log_transport_allocation("fusion_snapshot_cache",
#ifdef USE_PSRAM
                             "internal_fallback",
#else
                             "internal",
#endif
                             sizeof(FusionSnapshotCache),
#ifdef USE_PSRAM
                             RuntimeAllocationLogLevel::WARN
#else
                             RuntimeAllocationLogLevel::INFO
#endif
    );
  } else {
    log_transport_allocation("fusion_snapshot_cache", "allocation_failed",
                             sizeof(FusionSnapshotCache),
                             RuntimeAllocationLogLevel::ERROR);
  }
  return this->fusion_snapshot_cache_ != nullptr;
}

void ObservationTransport::destroy_fusion_snapshot_cache_() {
  if (this->fusion_snapshot_cache_ == nullptr)
    return;
#ifdef USE_PSRAM
  if (this->fusion_snapshot_cache_in_psram_) {
    this->fusion_snapshot_cache_->~FusionSnapshotCache();
    heap_caps_free(this->fusion_snapshot_cache_);
    this->fusion_snapshot_cache_ = nullptr;
    this->fusion_snapshot_cache_in_psram_ = false;
    return;
  }
#endif
  delete this->fusion_snapshot_cache_;
  this->fusion_snapshot_cache_ = nullptr;
}

void ObservationTransport::skip_send_(ObservationSendResult result,
                                      const char *target) {
  this->diagnostics_.send_skipped++;
  this->diagnostics_.last_send_result = result;
  if (target == nullptr) {
    this->diagnostics_.send_target[0] = '\0';
    return;
  }
  std::strncpy(this->diagnostics_.send_target, target,
               sizeof(this->diagnostics_.send_target) - 1);
  this->diagnostics_.send_target[
      sizeof(this->diagnostics_.send_target) - 1] = '\0';
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
