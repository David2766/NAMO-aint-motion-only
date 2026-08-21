#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct PresenceFusionOutput;
struct PresenceTrackerOutput;

enum class LocalObservationRoomState : uint8_t {
  UNKNOWN = 0,
  INSIDE,
  OUTSIDE,
};

struct LocalObservationTrack {
  bool valid{false};
  uint32_t local_track_id{0};
  bool moving{false};
  bool coasting{false};
  uint32_t sample_age_ms{0};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float vx_mm_s{0.0f};
  float vy_mm_s{0.0f};
  float position_variance_x_mm2{0.0f};
  float position_variance_y_mm2{0.0f};
  float distance_mm{0.0f};
  int track_score{0};
  int exit_zone_mask{0};
  int exit_age_ms{-1};
  bool exit_recent{false};
  LocalObservationRoomState room_state{LocalObservationRoomState::UNKNOWN};
};

struct LocalObservationEvidence {
  bool pir_motion{false};
  bool filter_blocked{false};
  bool tracker_presence{false};
  bool tracker_motion{false};
  bool static_radar_available{false};
  bool static_radar_presence{false};
  bool static_radar_moving{false};
  bool static_radar_still{false};
  float static_radar_distance_mm{0.0f};
  bool static_assist_armed{false};
  bool static_assist_active{false};
  bool static_assist_exit_veto{false};
};

// Internal shadow state only. Wire codecs must serialize selected fields
// explicitly instead of copying this compiler-dependent C++ layout.
struct LocalObservation {
  static constexpr uint8_t VERSION = 1;
  static constexpr uint32_t DEFAULT_TTL_MS = 1500;
  static constexpr std::size_t MAX_TRACKS = 3;

  uint8_t version{VERSION};
  uint32_t sequence{0};
  uint32_t observed_at_ms{0};
  uint32_t ttl_ms{DEFAULT_TTL_MS};
  uint8_t track_count{0};
  std::array<LocalObservationTrack, MAX_TRACKS> tracks{};
  LocalObservationEvidence evidence{};
};

static_assert(LocalObservation::MAX_TRACKS == 3,
              "LD2450 local observations must remain bounded to three tracks");
static_assert(std::is_trivially_copyable<LocalObservation>::value,
              "local observations must not own heap-backed state");
static_assert(sizeof(LocalObservation) <= 256,
              "local observation memory exceeded the embedded budget");

class LocalObservationAdapter {
 public:
  void update(uint32_t now_ms, bool pir_motion, bool filter_blocked,
              const PresenceTrackerOutput &tracker, const PresenceFusionOutput &fusion);

  const LocalObservation &output() const { return this->output_; }

 private:
  LocalObservation output_{};
  uint32_t next_sequence_{1};

  static LocalObservationRoomState room_state_(const char *value);
  uint32_t allocate_sequence_();
};

}  // namespace radar_api_server
}  // namespace esphome
