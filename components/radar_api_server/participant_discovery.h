#pragma once

#include "participant_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef USE_ESP32
#include "esphome/core/defines.h"
#include <freertos/FreeRTOS.h>

struct mdns_search_once_s;
struct mdns_result_s;
#endif

namespace esphome {
namespace radar_api_server {

enum class ParticipantDiscoveryResult : uint8_t {
  IDLE = 0,
  OK,
  START_FAILED,
};

struct ParticipantDiscoveryCandidate {
  static constexpr std::size_t MAX_HOSTNAME_LENGTH = 64;
  static constexpr std::size_t MAX_ADDRESS_LENGTH = 15;

  bool occupied{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char hostname[MAX_HOSTNAME_LENGTH + 1]{};
  char address[MAX_ADDRESS_LENGTH + 1]{};
  uint16_t port{0};
  uint8_t protocol_minimum{0};
  uint8_t protocol_maximum{0};
};

class ParticipantDiscoveryResults {
 public:
  void reset(const char *local_participant_id);
  bool consider(const char *participant_id, const char *participant_type,
                const char *product, const char *hostname, const char *address,
                uint16_t port, uint8_t protocol_minimum,
                uint8_t protocol_maximum, bool identity_ready);
  std::size_t size() const;
  const ParticipantDiscoveryCandidate *candidate_at(std::size_t slot) const;

 private:
  char local_participant_id_[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  std::array<ParticipantDiscoveryCandidate,
             ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      candidates_{};
};

struct ParticipantDiscoverySnapshot {
  bool supported{true};
  bool scanning{false};
  uint32_t generation{0};
  ParticipantDiscoveryResult result{ParticipantDiscoveryResult::IDLE};
  uint8_t candidate_count{0};
};

class ParticipantDiscoveryRuntime {
 public:
  bool configure_advertisement(const char *local_participant_id);
  bool advertisement_configured() const;
  const char *advertised_participant_id() const;
  bool queue_scan(const char *local_participant_id);
  bool queue_refresh(const char *local_participant_id);
  void loop(uint32_t now_ms);
  void shutdown();
  ParticipantDiscoverySnapshot snapshot();
  bool candidate_at(std::size_t slot, ParticipantDiscoveryCandidate *candidate);

 private:
  ParticipantDiscoveryResults results_{};
  ParticipantDiscoverySnapshot published_{};
  char advertised_participant_id_[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t advertisement_next_attempt_ms_{0};
  uint32_t query_next_attempt_ms_{0};
  uint32_t query_start_deadline_ms_{0};
  uint32_t scan_deadline_ms_{0};
  bool advertisement_registered_{false};
  bool advertisement_failure_logged_{false};
  bool query_failure_logged_{false};
  bool scan_pending_{false};
  bool scan_active_{false};
  bool scan_visible_{false};
#ifdef USE_ESP32
  ::mdns_search_once_s *query_{nullptr};
  portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
#endif

  void register_advertisement_(uint32_t now_ms);
  void update_scan_window_(uint32_t now_ms);
  bool queue_scan_(const char *local_participant_id, bool visible);
  void complete_scan_(ParticipantDiscoveryResult result);
  void lock_();
  void unlock_();
#if defined(USE_ESP32) && defined(USE_MDNS)
  bool start_query_(uint32_t now_ms);
  bool poll_query_();
  void ingest_query_results_(const ::mdns_result_s *results);
#endif
};

const char *participant_discovery_result_to_string(
    ParticipantDiscoveryResult result);

static_assert(std::is_trivially_copyable<ParticipantDiscoveryCandidate>::value,
              "discovery candidates must remain fixed-size values");
static_assert(sizeof(ParticipantDiscoveryRuntime) <= 4096,
              "discovery runtime exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
