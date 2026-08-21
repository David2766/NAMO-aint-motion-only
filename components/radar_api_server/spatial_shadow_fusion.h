#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

inline constexpr std::size_t SPATIAL_SHADOW_MAX_SOURCES = 7;

enum class SpatialShadowTrackState : uint8_t {
  EMPTY = 0,
  TENTATIVE,
  CONFIRMED,
  COASTING,
};

const char *spatial_shadow_track_state_to_string(
    SpatialShadowTrackState state);

struct SpatialShadowTrackInput {
  bool valid{false};
  bool velocity_available{false};
  bool uncertainty_available{false};
  uint32_t local_track_id{0};
  uint32_t sample_age_ms{0};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float vx_mm_s{0.0f};
  float vy_mm_s{0.0f};
  float variance_x_mm2{0.0f};
  float variance_y_mm2{0.0f};
  uint8_t quality_score{0};
};

struct SpatialShadowSource {
  static constexpr std::size_t MAX_TRACKS = 3;

  bool valid{false};
  uint64_t participant_token{0};
  int16_t view_angle_cdeg{0};
  uint32_t sample_age_ms{0};
  uint32_t ttl_ms{1500};
  uint8_t track_count{0};
  std::array<SpatialShadowTrackInput, MAX_TRACKS> tracks{};
};

struct SpatialShadowTrack {
  bool valid{false};
  bool correlated_views{false};
  uint32_t fused_track_id{0};
  SpatialShadowTrackState state{SpatialShadowTrackState::EMPTY};
  uint8_t source_count{0};
  uint8_t confidence{0};
  uint16_t hit_count{0};
  uint32_t age_ms{0};
  float x_mm{0.0f};
  float y_mm{0.0f};
  float vx_mm_s{0.0f};
  float vy_mm_s{0.0f};
  float variance_x_mm2{0.0f};
  float variance_y_mm2{0.0f};
  std::array<uint64_t, SPATIAL_SHADOW_MAX_SOURCES> participant_tokens{};
  std::array<uint32_t, SPATIAL_SHADOW_MAX_SOURCES> local_track_ids{};
};

struct SpatialShadowFusionDiagnostics {
  uint8_t valid_inputs{0};
  uint8_t candidate_pairs{0};
  uint8_t paired_measurements{0};
  uint8_t ambiguous_pairs{0};
  uint8_t dropped_measurements{0};
  uint8_t tentative_tracks{0};
  uint8_t confirmed_tracks{0};
  uint8_t coasting_tracks{0};
};

struct SpatialShadowFusionOutput {
  static constexpr bool SHADOW_ONLY = true;
  static constexpr bool PRODUCTION_AUTHORITY = false;
  static constexpr std::size_t MAX_TRACKS = 6;

  uint32_t sequence{0};
  uint8_t track_count{0};
  std::array<SpatialShadowTrack, MAX_TRACKS> tracks{};
  SpatialShadowFusionDiagnostics diagnostics{};
};

struct SpatialShadowFusionPolicy {
  float base_pair_gate_mm{450.0f};
  float uncertainty_gate_multiplier{1.5f};
  float max_pair_gate_mm{1400.0f};
  float pair_release_margin_mm{250.0f};
  float max_velocity_difference_mm_s{1800.0f};
  float ambiguity_margin{0.18f};
  float lifecycle_gate_mm{900.0f};
  float lifecycle_continuity_gate_mm{1800.0f};
  float max_predicted_speed_mm_s{3000.0f};
  float paired_position_alpha{0.68f};
  float single_source_position_alpha{0.50f};
  float source_dropout_position_alpha{0.22f};
  float stationary_position_alpha{0.30f};
  float disagreement_position_alpha{0.24f};
  float velocity_alpha{0.40f};
  uint32_t tentative_hold_ms{500};
  uint32_t confirmed_hold_ms{2500};
  uint8_t confirmation_hits{2};
  uint8_t single_source_confirmation_hits{6};
  uint8_t additional_single_source_confirmation_hits{6};
  uint32_t additional_single_source_guard_ms{60000};
  float secondary_independent_displacement_mm{600.0f};
  uint8_t secondary_independent_hits_required{3};
  int16_t correlated_view_angle_cdeg{2000};
  float default_variance_mm2{62500.0f};
  float minimum_variance_mm2{10000.0f};
  float stationary_speed_mm_s{250.0f};
  float disagreement_spread_mm{600.0f};
  float robust_outlier_gate_mm{900.0f};
  float max_robust_weight_ratio{8.0f};
  float reanchor_consistency_gate_mm{600.0f};
  uint32_t reanchor_timeout_ms{1500};
  uint8_t reanchor_hits_required{3};
};

// Fixed-memory, seven-source fusion core. It consumes already admitted
// world-coordinate observations and cannot affect production presence or Zones.
class SpatialShadowFusion {
 public:
  static constexpr std::size_t MAX_SOURCES =
      SPATIAL_SHADOW_MAX_SOURCES;

  explicit SpatialShadowFusion(
      const SpatialShadowFusionPolicy &policy = SpatialShadowFusionPolicy{});

  void update(
      uint32_t now_ms,
      const std::array<SpatialShadowSource, MAX_SOURCES> &sources);
  void clear();

  const SpatialShadowFusionOutput &output() const { return this->output_; }

 private:
  struct Measurement {
    bool valid{false};
    bool correlated_views{false};
    bool velocity_available{false};
    bool source_had_multiple_tracks{false};
    uint8_t source_count{0};
    uint8_t confidence{0};
    float x_mm{0.0f};
    float y_mm{0.0f};
    float vx_mm_s{0.0f};
    float vy_mm_s{0.0f};
    float variance_x_mm2{0.0f};
    float variance_y_mm2{0.0f};
    float geometry_spread_mm{0.0f};
    std::array<uint64_t, MAX_SOURCES> participant_tokens{};
    std::array<uint32_t, MAX_SOURCES> local_track_ids{};
    std::array<int16_t, MAX_SOURCES> view_angles_cdeg{};
    std::array<float, MAX_SOURCES> source_x_mm{};
    std::array<float, MAX_SOURCES> source_y_mm{};
    std::array<float, MAX_SOURCES> source_vx_mm_s{};
    std::array<float, MAX_SOURCES> source_vy_mm_s{};
    std::array<float, MAX_SOURCES> source_variance_x_mm2{};
    std::array<float, MAX_SOURCES> source_variance_y_mm2{};
    std::array<bool, MAX_SOURCES> source_velocity_available{};
  };

  struct TrackSlot {
    bool occupied{false};
    uint32_t fused_track_id{0};
    SpatialShadowTrackState state{SpatialShadowTrackState::EMPTY};
    uint16_t hit_count{0};
    uint8_t confirmation_hits_required{0};
    uint8_t identity_count{0};
    bool secondary_guarded{false};
    bool secondary_relative_initialized{false};
    bool secondary_independent{false};
    uint32_t secondary_parent_track_id{0};
    float secondary_initial_relative_x_mm{0.0f};
    float secondary_initial_relative_y_mm{0.0f};
    uint8_t secondary_independent_hits{0};
    uint32_t first_seen_ms{0};
    uint32_t last_seen_ms{0};
    uint32_t last_update_ms{0};
    bool reanchor_pending{false};
    uint8_t reanchor_hits{0};
    uint32_t reanchor_last_ms{0};
    float reanchor_x_mm{0.0f};
    float reanchor_y_mm{0.0f};
    Measurement value{};
    std::array<uint64_t, MAX_SOURCES> identity_participant_tokens{};
    std::array<uint32_t, MAX_SOURCES> identity_local_track_ids{};
  };

  struct PairCandidate {
    bool valid{false};
    uint8_t first{0};
    uint8_t second{0};
    float cost{0.0f};
  };

  struct LifecycleCandidate {
    bool valid{false};
    uint8_t track{0};
    uint8_t measurement{0};
    float cost{0.0f};
  };

  static constexpr std::size_t MAX_MEASUREMENTS =
      SpatialShadowFusionOutput::MAX_TRACKS;
  static constexpr std::size_t MAX_PAIR_CANDIDATES =
      MAX_MEASUREMENTS * SpatialShadowSource::MAX_TRACKS;
  static constexpr std::size_t MAX_LIFECYCLE_CANDIDATES =
      SpatialShadowFusionOutput::MAX_TRACKS * MAX_MEASUREMENTS;

  SpatialShadowFusionPolicy policy_{};
  std::array<TrackSlot, SpatialShadowFusionOutput::MAX_TRACKS> tracks_{};
  std::array<Measurement, MAX_MEASUREMENTS> measurements_{};
  std::array<PairCandidate, MAX_PAIR_CANDIDATES> pair_candidates_{};
  std::array<LifecycleCandidate, MAX_LIFECYCLE_CANDIDATES>
      lifecycle_candidates_{};
  SpatialShadowFusionOutput output_{};
  uint32_t next_track_id_{1};
  uint32_t next_sequence_{1};

  uint8_t build_measurements_(
      const std::array<SpatialShadowSource, MAX_SOURCES> &sources,
      SpatialShadowFusionDiagnostics *diagnostics);
  void associate_measurements_(uint32_t now_ms, uint8_t measurement_count,
                               SpatialShadowFusionDiagnostics *diagnostics);
  void publish_(uint32_t now_ms,
                const SpatialShadowFusionDiagnostics &diagnostics);
  TrackSlot *allocate_track_();
  uint32_t allocate_track_id_();
  static bool pair_candidate_less_(const PairCandidate &first,
                                   const PairCandidate &second);
  static bool lifecycle_candidate_less_(const LifecycleCandidate &first,
                                        const LifecycleCandidate &second);
  static bool track_identity_matches_(const TrackSlot &track,
                                      const Measurement &measurement);
  static bool track_source_matches_(const TrackSlot &track,
                                    const Measurement &measurement);
  bool established_measurement_matches_(
      const Measurement &measurement, uint64_t participant_token,
      uint32_t local_track_id) const;
  void seed_measurement_(Measurement *measurement,
                         const SpatialShadowSource &source,
                         const SpatialShadowTrackInput &track) const;
  void merge_measurement_(Measurement *measurement,
                          const SpatialShadowSource &source,
                          const SpatialShadowTrackInput &track) const;
  void recompute_robust_geometry_(Measurement *measurement) const;
  void smooth_track_measurement_(TrackSlot *track,
                                 const Measurement &measurement,
                                 uint32_t now_ms) const;
  bool consider_reanchor_(TrackSlot *track, const Measurement &measurement,
                          uint32_t now_ms) const;
  static void remember_measurement_identity_(TrackSlot *track,
                                             const Measurement &measurement);
  uint8_t confirmation_hits_for_(const Measurement &measurement,
                                 bool established_track_exists) const;
  const TrackSlot *find_track_by_id_(uint32_t fused_track_id) const;
  const TrackSlot *nearest_reference_track_(const Measurement &measurement) const;
  void update_secondary_evidence_(TrackSlot *track);
  bool should_confirm_track_(const TrackSlot &track, uint32_t now_ms) const;
};

static_assert(SpatialShadowSource::MAX_TRACKS == 3,
              "each spatial source must remain bounded to three tracks");
static_assert(SpatialShadowFusion::MAX_SOURCES == 7,
              "one Fusion Group must remain bounded to seven sources");
static_assert(SpatialShadowFusionOutput::MAX_TRACKS == 6,
              "shadow fusion output must remain bounded to six tracks");
static_assert(std::is_trivially_copyable<SpatialShadowFusionOutput>::value,
              "shadow fusion output must not own heap-backed state");
static_assert(std::is_trivially_copyable<SpatialShadowFusion>::value,
              "shadow fusion must not own heap-backed state");
static_assert(sizeof(SpatialShadowFusion) <= 6144,
              "shadow fusion memory exceeded the embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
