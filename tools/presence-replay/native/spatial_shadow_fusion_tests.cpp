#include "spatial_shadow_fusion.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace esphome::radar_api_server;

namespace {

void require(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "spatial_shadow_fusion_tests: %s\n", message);
  std::exit(1);
}

void require_near(float actual, float expected, float tolerance,
                  const char *message) {
  require(std::fabs(actual - expected) <= tolerance, message);
}

SpatialShadowSource source(uint64_t token, int16_t view_angle_cdeg) {
  SpatialShadowSource value;
  value.valid = true;
  value.participant_token = token;
  value.view_angle_cdeg = view_angle_cdeg;
  return value;
}

void add_track(SpatialShadowSource *source, uint32_t local_track_id, float x_mm,
               float y_mm, float vx_mm_s = 0.0f, float vy_mm_s = 0.0f,
               uint8_t quality = 80, float variance_x_mm2 = 40000.0f,
               float variance_y_mm2 = 40000.0f) {
  require(source != nullptr && source->track_count < source->tracks.size(),
          "test source exceeded its fixed track capacity");
  auto &track = source->tracks[source->track_count++];
  track.valid = true;
  track.velocity_available = true;
  track.uncertainty_available = true;
  track.local_track_id = local_track_id;
  track.x_mm = x_mm;
  track.y_mm = y_mm;
  track.vx_mm_s = vx_mm_s;
  track.vy_mm_s = vy_mm_s;
  track.variance_x_mm2 = variance_x_mm2;
  track.variance_y_mm2 = variance_y_mm2;
  track.quality_score = quality;
}

std::array<SpatialShadowSource, SpatialShadowFusion::MAX_SOURCES> pair(
    const SpatialShadowSource &first, const SpatialShadowSource &second) {
  return {first, second};
}

std::array<SpatialShadowSource, SpatialShadowFusion::MAX_SOURCES> sources(
    std::initializer_list<SpatialShadowSource> values) {
  std::array<SpatialShadowSource, SpatialShadowFusion::MAX_SOURCES> result{};
  std::size_t index = 0;
  for (const auto &value : values) {
    require(index < result.size(),
            "test input exceeded the fixed source capacity");
    result[index++] = value;
  }
  return result;
}

const SpatialShadowTrack *track_with_source(
    const SpatialShadowFusionOutput &output, uint64_t participant_token,
    uint32_t local_track_id) {
  for (const auto &track : output.tracks) {
    if (!track.valid)
      continue;
    for (std::size_t source_index = 0; source_index < track.source_count;
         ++source_index) {
      if (track.participant_tokens[source_index] == participant_token &&
          track.local_track_ids[source_index] == local_track_id)
        return &track;
    }
  }
  return nullptr;
}

void duplicate_person_becomes_one_confirmed_track() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1080.0f, 2020.0f);

  fusion.update(1000, pair(first, second));
  require(fusion.output().track_count == 1,
          "duplicate observations should create one shadow candidate");
  const uint32_t fused_id = fusion.output().tracks[0].fused_track_id;
  require(fusion.output().tracks[0].state ==
              SpatialShadowTrackState::TENTATIVE,
          "first observation should remain tentative");

  first.tracks[0].x_mm = 1030.0f;
  second.tracks[0].x_mm = 1090.0f;
  fusion.update(1100, pair(first, second));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == fused_id &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::CONFIRMED,
          "second consistent observation should confirm the same fused ID");
}

void single_source_candidate_requires_more_evidence() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  add_track(&first, 11, 1000.0f, 2000.0f);

  fusion.update(1000, pair(first, SpatialShadowSource{}));
  fusion.update(1100, pair(first, SpatialShadowSource{}));
  fusion.update(1200, pair(first, SpatialShadowSource{}));
  fusion.update(1300, pair(first, SpatialShadowSource{}));
  fusion.update(1400, pair(first, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::TENTATIVE,
          "five single-source observations should remain tentative");

  fusion.update(1500, pair(first, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::CONFIRMED,
          "a persistent single-source person should confirm on the sixth observation");
}

void short_single_source_candidate_never_confirms_and_expires() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  add_track(&first, 11, 1000.0f, 2000.0f);

  fusion.update(1000, pair(first, SpatialShadowSource{}));
  fusion.update(1100, pair(first, SpatialShadowSource{}));
  fusion.update(1200, pair(first, SpatialShadowSource{}));
  fusion.update(1300, pair(first, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::TENTATIVE,
          "a four-hit single-source transient must not confirm");

  fusion.update(1801,
                pair(SpatialShadowSource{}, SpatialShadowSource{}));
  require(fusion.output().track_count == 0,
          "a short single-source transient must expire after the tentative hold");
}

void ambiguous_nearby_people_remain_separate() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&first, 12, 1400.0f, 2000.0f);
  add_track(&second, 21, 1150.0f, 2000.0f);
  add_track(&second, 22, 1250.0f, 2000.0f);

  fusion.update(1000, pair(first, second));
  require(fusion.output().track_count == 4,
          "ambiguous close observations must not be forcibly collapsed");
  require(fusion.output().diagnostics.ambiguous_pairs > 0 &&
              fusion.output().diagnostics.paired_measurements == 0,
          "ambiguity should be visible in diagnostics");
}

void local_id_changes_do_not_replace_fused_identity() {
  SpatialShadowFusion fusion;
  SpatialShadowSource first = source(1, 0);
  add_track(&first, 11, 1000.0f, 2000.0f);
  fusion.update(1000, pair(first, SpatialShadowSource{}));
  const uint32_t fused_id = fusion.output().tracks[0].fused_track_id;

  first.tracks[0].local_track_id = 99;
  first.tracks[0].x_mm = 1040.0f;
  fusion.update(1100, pair(first, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == fused_id,
          "nearby observations must retain identity across local ID changes");
}

void source_order_does_not_change_fused_identity() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));
  const uint32_t fused_id = fusion.output().tracks[0].fused_track_id;

  first.tracks[0].x_mm = 1040.0f;
  second.tracks[0].x_mm = 1090.0f;
  fusion.update(1200, pair(second, first));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == fused_id,
          "source array order must not define fused identity");
}

void crossing_tracks_keep_source_continuity() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, -1000.0f, 2000.0f, 1000.0f);
  add_track(&first, 12, 1000.0f, 2000.0f, -1000.0f);
  add_track(&second, 21, -950.0f, 2020.0f, 1000.0f);
  add_track(&second, 22, 1050.0f, 2020.0f, -1000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));

  const auto *left = track_with_source(fusion.output(), 1, 11);
  const auto *right = track_with_source(fusion.output(), 1, 12);
  require(left != nullptr && right != nullptr,
          "both crossing tracks should be present before crossing");
  const uint32_t left_id = left->fused_track_id;
  const uint32_t right_id = right->fused_track_id;

  first.tracks[0].x_mm = 400.0f;
  first.tracks[1].x_mm = -400.0f;
  second.tracks[0].x_mm = 450.0f;
  second.tracks[1].x_mm = -350.0f;
  fusion.update(1200, pair(first, second));
  left = track_with_source(fusion.output(), 1, 11);
  right = track_with_source(fusion.output(), 1, 12);
  require(left != nullptr && right != nullptr &&
              left->fused_track_id == left_id &&
              right->fused_track_id == right_id,
          "source continuity should preserve fused IDs through a crossing");
}

void temporary_loss_coasts_without_replacing_identity() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));
  const uint32_t fused_id = fusion.output().tracks[0].fused_track_id;

  fusion.update(1200, pair(first, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == fused_id &&
              fusion.output().tracks[0].source_count == 1 &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::CONFIRMED,
          "one surviving source should maintain the confirmed identity");

  fusion.update(1300,
                pair(SpatialShadowSource{}, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == fused_id &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::COASTING,
          "complete temporary loss should coast the confirmed track");

  fusion.update(3600,
                pair(SpatialShadowSource{}, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == fused_id &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::COASTING,
          "a corroborated track should retain the confirmed hold");

  fusion.update(3701,
                pair(SpatialShadowSource{}, SpatialShadowSource{}));
  require(fusion.output().track_count == 0,
          "a corroborated track should expire after the confirmed hold");
}

void separated_known_sources_do_not_split_a_confirmed_person() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));
  const uint32_t fused_id = fusion.output().tracks[0].fused_track_id;

  second.tracks[0].x_mm = 4000.0f;
  fusion.update(1200, pair(first, second));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == fused_id,
          "known source tracks that stop pairing must not create a duplicate person");
  require(fusion.output().diagnostics.dropped_measurements == 1,
          "the rejected duplicate source should remain visible in diagnostics");
}

void established_pair_uses_a_release_margin() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1800.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));

  second.tracks[0].x_mm = 2050.0f;
  fusion.update(1200, pair(first, second));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].source_count == 2,
          "an established pair must remain fused just beyond the admission gate");
}

void paired_position_filter_is_smooth_but_responsive() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1000.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));

  first.tracks[0].x_mm = 1600.0f;
  second.tracks[0].x_mm = 1600.0f;
  fusion.update(1200, pair(first, second));
  require(fusion.output().tracks[0].x_mm > 1150.0f &&
              fusion.output().tracks[0].x_mm < 1250.0f,
          "a zero-velocity coordinate step should be treated as stationary jitter");

  for (uint32_t now_ms = 1300; now_ms <= 1700; now_ms += 100)
    fusion.update(now_ms, pair(first, second));
  require(fusion.output().tracks[0].x_mm > 1500.0f,
          "repeated stable coordinates should still converge without freezing");
}

void corroborated_motion_remains_responsive() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1000.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));

  first.tracks[0].x_mm = 1600.0f;
  first.tracks[0].vx_mm_s = 1000.0f;
  second.tracks[0].x_mm = 1600.0f;
  second.tracks[0].vx_mm_s = 1000.0f;
  fusion.update(1200, pair(first, second));
  require(fusion.output().tracks[0].x_mm > 1350.0f,
          "corroborated moving coordinates must retain responsive smoothing");
}

void three_source_geometry_rejects_one_bounded_outlier() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  auto outlier = source(3, -9000);
  add_track(&first, 11, 1000.0f, 2000.0f, 0.0f, 0.0f, 80,
            160000.0f, 160000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f, 0.0f, 0.0f, 80,
            160000.0f, 160000.0f);
  add_track(&outlier, 31, 2200.0f, 2000.0f, 0.0f, 0.0f, 80,
            160000.0f, 160000.0f);

  fusion.update(1000, sources({first, second, outlier}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].source_count == 3 &&
              fusion.output().tracks[0].x_mm < 1150.0f,
          "one bounded outlier must not pull a three-source coordinate away");
}

void one_source_dropout_does_not_snap_to_one_radar() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1400.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));

  fusion.update(1200, pair(first, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].source_count == 1 &&
              fusion.output().tracks[0].x_mm > 1100.0f,
          "temporary loss of one radar must not snap to the surviving coordinate");
}

void additional_single_source_candidate_uses_longer_confirmation() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));

  add_track(&first, 12, 3500.0f, 2500.0f);
  for (uint32_t now_ms = 1200; now_ms <= 1600; now_ms += 100)
    fusion.update(now_ms, pair(first, second));
  const auto *candidate = track_with_source(fusion.output(), 1, 12);
  require(candidate != nullptr &&
              candidate->state == SpatialShadowTrackState::TENTATIVE,
          "five observations from one source must not yet split an established person");

  for (uint32_t now_ms = 1700; now_ms <= 2000; now_ms += 100) {
    first.tracks[1].x_mm += 500.0f;
    fusion.update(now_ms, pair(first, second));
  }
  candidate = track_with_source(fusion.output(), 1, 12);
  require(candidate != nullptr &&
              candidate->state == SpatialShadowTrackState::CONFIRMED,
          "an independently moving single-source person must still be admitted");
}

void captured_static_cross_source_split_stays_tentative() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, -350.0f, 1650.0f);
  add_track(&second, 21, -1376.0f, 3729.0f);

  for (uint32_t now_ms = 1000; now_ms <= 59000; now_ms += 500)
    fusion.update(now_ms, pair(first, second));

  require(fusion.output().diagnostics.confirmed_tracks == 1 &&
              fusion.output().diagnostics.tentative_tracks == 1,
          "the captured 59-second one-person cross-source split must not publish a second person");
}

void known_single_target_source_id_churn_does_not_create_a_person() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));

  second.tracks[0].local_track_id = 99;
  second.tracks[0].x_mm = 3900.0f;
  second.tracks[0].y_mm = 3600.0f;
  fusion.update(1200, pair(first, second));

  require(fusion.output().track_count == 1 &&
              fusion.output().diagnostics.dropped_measurements == 1,
          "one known source changing its only local ID must not create a duplicate fused person");
}

void coordinate_jump_does_not_reanchor_confirmed_track() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));
  const uint32_t stable_id = fusion.output().tracks[0].fused_track_id;

  first.tracks[0].x_mm = 5000.0f;
  second.tracks[0].x_mm = 5050.0f;
  fusion.update(1200, pair(first, second));
  require(fusion.output().track_count == 1,
          "a large jump from known sources must not split a confirmed person");
  const auto *stable = static_cast<const SpatialShadowTrack *>(nullptr);
  for (const auto &track : fusion.output().tracks) {
    if (track.valid && track.fused_track_id == stable_id)
      stable = &track;
  }
  require(stable != nullptr &&
              stable->state == SpatialShadowTrackState::COASTING &&
              stable->x_mm < 2000.0f,
          "a coordinate jump must not move the confirmed identity");
  require(fusion.output().diagnostics.dropped_measurements == 1,
          "the rejected known-source jump should remain diagnosable");
}

void repeated_corroborated_jump_reanchors_the_existing_identity() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f);
  add_track(&second, 21, 1050.0f, 2000.0f);
  fusion.update(1000, pair(first, second));
  fusion.update(1100, pair(first, second));
  const uint32_t stable_id = fusion.output().tracks[0].fused_track_id;

  first.tracks[0].x_mm = 5000.0f;
  second.tracks[0].x_mm = 5050.0f;
  fusion.update(1200, pair(first, second));
  fusion.update(1300, pair(first, second));
  fusion.update(1400, pair(first, second));

  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].fused_track_id == stable_id &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::CONFIRMED &&
              fusion.output().tracks[0].x_mm > 4500.0f,
          "three corroborated jump samples must reanchor the existing ID");
}

void view_geometry_controls_confidence_and_variance() {
  SpatialShadowFusion correlated_fusion;
  auto first = source(1, 0);
  auto correlated = source(2, 1000);
  add_track(&first, 11, 1000.0f, 2000.0f, 0.0f, 0.0f, 80, 40000.0f,
            40000.0f);
  add_track(&correlated, 21, 1100.0f, 2000.0f, 0.0f, 0.0f, 60, 90000.0f,
            90000.0f);
  correlated_fusion.update(1000, pair(first, correlated));
  const auto &correlated_track = correlated_fusion.output().tracks[0];
  require(correlated_track.correlated_views &&
              correlated_track.confidence == 80,
          "similar views must not double confidence");
  require_near(correlated_track.variance_x_mm2, 40000.0f, 1.0f,
               "similar views must not claim independent variance reduction");

  SpatialShadowFusion diverse_fusion;
  correlated.view_angle_cdeg = 9000;
  diverse_fusion.update(1000, pair(first, correlated));
  const auto &diverse_track = diverse_fusion.output().tracks[0];
  require(!diverse_track.correlated_views &&
              diverse_track.confidence > correlated_track.confidence,
          "different views may add bounded confidence");
  require(diverse_track.variance_x_mm2 <
              correlated_track.variance_x_mm2,
          "different views should reduce independent geometric uncertainty");
  require_near(diverse_track.x_mm, 1030.77f, 1.0f,
               "different views should use inverse-variance geometry");
}

void asymmetric_quality_prefers_precise_observation() {
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 1000.0f, 2000.0f, 0.0f, 0.0f, 95, 10000.0f,
            10000.0f);
  add_track(&second, 21, 1300.0f, 2000.0f, 0.0f, 0.0f, 40, 160000.0f,
            160000.0f);
  fusion.update(1000, pair(first, second));
  require(fusion.output().track_count == 1,
          "compatible asymmetric observations should still pair");
  require(fusion.output().tracks[0].x_mm < 1050.0f,
          "weighted geometry should stay near the precise source");
}

void capacity_and_shadow_authority_are_fixed() {
  require(SpatialShadowFusionOutput::SHADOW_ONLY &&
              !SpatialShadowFusionOutput::PRODUCTION_AUTHORITY,
          "the fusion core must remain shadow-only");
  SpatialShadowFusion fusion;
  auto first = source(1, 0);
  auto second = source(2, 9000);
  add_track(&first, 11, 0.0f, 1000.0f);
  add_track(&first, 12, 3000.0f, 1000.0f);
  add_track(&first, 13, 6000.0f, 1000.0f);
  add_track(&second, 21, 10000.0f, 1000.0f);
  add_track(&second, 22, 13000.0f, 1000.0f);
  add_track(&second, 23, 16000.0f, 1000.0f);
  fusion.update(1000, pair(first, second));
  require(fusion.output().track_count ==
              SpatialShadowFusionOutput::MAX_TRACKS &&
              fusion.output().diagnostics.dropped_measurements == 0,
          "two three-track sources must fit the fixed six-track budget");
}

void four_sources_keep_two_people_separate() {
  SpatialShadowFusion fusion;
  std::array<SpatialShadowSource, 4> input_sources{};
  for (std::size_t index = 0; index < input_sources.size(); ++index) {
    input_sources[index] =
        source(static_cast<uint64_t>(index + 1),
               static_cast<int16_t>(index * 4500));
    add_track(&input_sources[index], static_cast<uint32_t>(10 + index),
              1000.0f + static_cast<float>(index * 20), 2000.0f);
    add_track(&input_sources[index], static_cast<uint32_t>(20 + index),
              5000.0f + static_cast<float>(index * 20), 2000.0f);
  }

  fusion.update(1000, sources({input_sources[0], input_sources[1],
                               input_sources[2], input_sources[3]}));
  require(fusion.output().track_count == 2,
          "four sources observing two separated people must yield two tracks");
  for (const auto &track : fusion.output().tracks) {
    if (track.valid)
      require(track.source_count == 4,
              "each four-source person must retain all source identities");
  }
}

void six_sources_collapse_one_person_without_heap_growth() {
  SpatialShadowFusion fusion;
  std::array<SpatialShadowSource, SpatialShadowFusion::MAX_SOURCES>
      input_sources{};
  for (std::size_t index = 0; index < 6; ++index) {
    input_sources[index] =
        source(static_cast<uint64_t>(index + 1),
               static_cast<int16_t>(index * 3000));
    add_track(&input_sources[index], static_cast<uint32_t>(100 + index),
              2000.0f + static_cast<float>(index * 15), 3000.0f);
  }

  fusion.update(1000, input_sources);
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].source_count == 6,
          "six compatible source tracks must collapse into one bounded track");
  require(fusion.output().diagnostics.valid_inputs == 6 &&
              fusion.output().diagnostics.paired_measurements == 5,
          "six-source diagnostics must account for every incremental merge");
}

void seven_sources_collapse_one_person_without_heap_growth() {
  SpatialShadowFusion fusion;
  std::array<SpatialShadowSource, SpatialShadowFusion::MAX_SOURCES>
      input_sources{};
  for (std::size_t index = 0; index < input_sources.size(); ++index) {
    input_sources[index] =
        source(static_cast<uint64_t>(index + 1),
               static_cast<int16_t>(index * 3000));
    add_track(&input_sources[index], static_cast<uint32_t>(100 + index),
              2000.0f + static_cast<float>(index * 15), 3000.0f);
  }

  fusion.update(1000, input_sources);
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].source_count ==
                  SpatialShadowFusion::MAX_SOURCES,
          "seven compatible source tracks must collapse into one bounded track");
  require(fusion.output().diagnostics.valid_inputs == 7 &&
              fusion.output().diagnostics.paired_measurements == 6,
          "seven-source diagnostics must account for every incremental merge");
}

void seven_source_capacity_drops_excess_measurements_deterministically() {
  SpatialShadowFusion fusion;
  std::array<SpatialShadowSource, SpatialShadowFusion::MAX_SOURCES>
      input_sources{};
  for (std::size_t source_index = 0; source_index < input_sources.size();
       ++source_index) {
    input_sources[source_index] =
        source(static_cast<uint64_t>(source_index + 1), 0);
    for (std::size_t track_index = 0;
         track_index < SpatialShadowSource::MAX_TRACKS; ++track_index) {
      add_track(&input_sources[source_index],
                static_cast<uint32_t>(source_index * 10 + track_index + 1),
                static_cast<float>(source_index * 10000 +
                                   track_index * 2500),
                1000.0f);
    }
  }

  fusion.update(1000, input_sources);
  require(fusion.output().track_count ==
              SpatialShadowFusionOutput::MAX_TRACKS &&
              fusion.output().diagnostics.dropped_measurements == 15,
          "twenty-one independent inputs must remain bounded to six outputs");
}

void stale_input_is_ignored_and_confirmed_track_expires() {
  SpatialShadowFusion fusion;
  SpatialShadowSource first = source(1, 0);
  add_track(&first, 11, 1000.0f, 2000.0f);
  fusion.update(1000, pair(first, SpatialShadowSource{}));
  fusion.update(1100, pair(first, SpatialShadowSource{}));
  fusion.update(1200, pair(first, SpatialShadowSource{}));
  fusion.update(1300, pair(first, SpatialShadowSource{}));
  fusion.update(1400, pair(first, SpatialShadowSource{}));
  fusion.update(1500, pair(first, SpatialShadowSource{}));
  require(fusion.output().tracks[0].state ==
              SpatialShadowTrackState::CONFIRMED,
          "fixture should establish a confirmed track");

  first.tracks[0].sample_age_ms = first.ttl_ms + 1;
  fusion.update(1600, pair(first, SpatialShadowSource{}));
  require(fusion.output().track_count == 1 &&
              fusion.output().tracks[0].state ==
                  SpatialShadowTrackState::COASTING,
          "expired input must be ignored instead of refreshing a track");
  fusion.update(2001,
                pair(SpatialShadowSource{}, SpatialShadowSource{}));
  require(fusion.output().track_count == 0,
          "an uncorroborated confirmed track must expire after the short hold");
}

}  // namespace

void run_spatial_shadow_fusion_tests() {
  duplicate_person_becomes_one_confirmed_track();
  single_source_candidate_requires_more_evidence();
  short_single_source_candidate_never_confirms_and_expires();
  ambiguous_nearby_people_remain_separate();
  local_id_changes_do_not_replace_fused_identity();
  source_order_does_not_change_fused_identity();
  crossing_tracks_keep_source_continuity();
  temporary_loss_coasts_without_replacing_identity();
  separated_known_sources_do_not_split_a_confirmed_person();
  established_pair_uses_a_release_margin();
  paired_position_filter_is_smooth_but_responsive();
  corroborated_motion_remains_responsive();
  three_source_geometry_rejects_one_bounded_outlier();
  one_source_dropout_does_not_snap_to_one_radar();
  additional_single_source_candidate_uses_longer_confirmation();
  captured_static_cross_source_split_stays_tentative();
  known_single_target_source_id_churn_does_not_create_a_person();
  coordinate_jump_does_not_reanchor_confirmed_track();
  repeated_corroborated_jump_reanchors_the_existing_identity();
  view_geometry_controls_confidence_and_variance();
  asymmetric_quality_prefers_precise_observation();
  capacity_and_shadow_authority_are_fixed();
  four_sources_keep_two_people_separate();
  six_sources_collapse_one_person_without_heap_growth();
  seven_sources_collapse_one_person_without_heap_growth();
  seven_source_capacity_drops_excess_measurements_deterministically();
  stale_input_is_ignored_and_confirmed_track_expires();
}
