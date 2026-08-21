#include "shared_automation_area.h"
#include "platform_runtime_workspace.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using esphome::radar_api_server::FloorplanRadarPose;
using esphome::radar_api_server::PLATFORM_RUNTIME_WORKSPACE_BUDGET;
using esphome::radar_api_server::PlatformCoordinatorSnapshotLease;
using esphome::radar_api_server::PlatformCoordinatorSnapshotStore;
using esphome::radar_api_server::PlatformRuntimeWorkspace;
using esphome::radar_api_server::SharedAutomationAreaCache;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << "\n";
    std::exit(1);
  }
}

void coordinator_publication_never_overwrites_a_pinned_reader() {
  PlatformCoordinatorSnapshotStore store{};
  PlatformCoordinatorSnapshotLease first_reader{};
  require(!store.acquire(&first_reader),
          "an unpublished coordinator snapshot was readable");

  uint8_t first_buffer = 0;
  require(store.prepare_publish(&first_buffer),
          "the initial coordinator publication had no writable buffer");
  store.snapshots[first_buffer].site_generation = 1;
  store.commit_publish(first_buffer);
  require(store.acquire(&first_reader) &&
              first_reader.snapshot->site_generation == 1,
          "the first coordinator publication was not readable");

  uint8_t second_buffer = 0;
  require(store.prepare_publish(&second_buffer) &&
              second_buffer != first_buffer,
          "the second publication selected the pinned active buffer");
  store.snapshots[second_buffer].site_generation = 2;
  store.commit_publish(second_buffer);

  PlatformCoordinatorSnapshotLease second_reader{};
  require(store.acquire(&second_reader) &&
              second_reader.snapshot->site_generation == 2 &&
              first_reader.snapshot->site_generation == 1,
          "overlapping readers did not retain coherent generations");

  uint8_t blocked_buffer = 0;
  require(!store.prepare_publish(&blocked_buffer),
          "publication overwrote a buffer pinned by an older reader");
  store.release(first_reader);
  require(store.prepare_publish(&blocked_buffer) &&
              blocked_buffer == first_buffer,
          "released coordinator buffer did not become writable");
  store.release(second_reader);
}

FloorplanRadarPose pose() {
  FloorplanRadarPose value;
  value.valid = true;
  value.origin_x_px = 100.0f;
  value.origin_y_px = 100.0f;
  value.rotation_deg = 0.0f;
  value.scale_factor = 1.0f;
  value.mm_per_px_x = 10.0f;
  value.mm_per_px_y = 10.0f;
  value.pose_version = 3;
  return value;
}

void receiving_pose_scale_is_applied_to_canonical_points() {
  const std::string document = R"({
    "floorplanId":"fp-0123456789abcdef0123456789abcdef",
    "automationAreas":{"schemaVersion":1,"revision":1,"areas":[
      {"areaId":"desk","name":"Desk","spaceId":"office","enabled":true,
       "pointsPx":[[100,100],[200,100],[200,200],[100,200]]}
    ]}
  })";
  auto receiving_pose = pose();
  receiving_pose.scale_factor = 2.0f;
  SharedAutomationAreaCache cache;
  require(cache.update(document.data(), document.size(),
                       "fp-0123456789abcdef0123456789abcdef", "office",
                       1234, receiving_pose),
          "a scaled receiving pose should parse canonical areas");
  require(cache.area(0).contains(250.0f, -250.0f),
          "canonical points should be divided by the receiving radar scale");
  require(!cache.area(0).contains(750.0f, -250.0f),
          "the sender's unscaled radar coordinates must not leak into the receiver");
}

void missing_receiving_pose_fails_closed() {
  const std::string document = R"({
    "floorplanId":"fp-0123456789abcdef0123456789abcdef",
    "automationAreas":{"schemaVersion":1,"revision":1,"areas":[
      {"areaId":"desk","name":"Desk","spaceId":"office","enabled":true,
       "pointsPx":[[100,100],[200,100],[200,200],[100,200]]}
    ]}
  })";
  auto missing_pose = pose();
  missing_pose.valid = false;
  SharedAutomationAreaCache cache;
  require(!cache.update(document.data(), document.size(),
                        "fp-0123456789abcdef0123456789abcdef", "office",
                        1234, missing_pose),
          "canonical areas must not use a replicated sender pose as fallback");
  require(!cache.configuration_valid(),
          "a missing local pose must keep shared area state unavailable");
}

void canonical_areas_are_bounded_and_converted_to_local_radar_space() {
  if (sizeof(PlatformRuntimeWorkspace) >
      PLATFORM_RUNTIME_WORKSPACE_BUDGET) {
    std::cerr << "PlatformRuntimeWorkspace size: "
              << sizeof(PlatformRuntimeWorkspace) << " bytes\n";
  }
  require(sizeof(PlatformRuntimeWorkspace) <=
              PLATFORM_RUNTIME_WORKSPACE_BUDGET,
          "the complete runtime workspace must remain within its PSRAM budget");
  const std::string document = R"({
    "version":2,
    "floorplanId":"fp-0123456789abcdef0123456789abcdef",
    "automationAreas":{"schemaVersion":1,"revision":4,"areas":[
      {"areaId":"desk","name":"Desk","spaceId":"office","enabled":true,
       "pointsPx":[[100,100],[200,100],[200,200],[100,200]]},
      {"areaId":"hall","name":"Hall","spaceId":"hallway","enabled":true,
       "pointsPx":[[0,0],[10,0],[10,10]]}
    ]}
  })";
  SharedAutomationAreaCache cache;
  require(cache.update(document.data(), document.size(),
                       "fp-0123456789abcdef0123456789abcdef", "office",
                       1234, pose()),
          "valid canonical areas should parse");
  require(cache.configuration_valid() && cache.revision() == 4 &&
              cache.size() == 1,
          "only the current Space should remain in the bounded cache");
  require(cache.area(0).contains(500.0f, -500.0f),
          "Floorplan pixels should convert to the local radar frame");
  require(!cache.area(0).contains(1500.0f, -500.0f),
          "points outside the canonical polygon should remain outside");
}

void legacy_floorplans_are_valid_and_empty() {
  const std::string document =
      R"({"version":2,"floorplanId":"fp-0123456789abcdef0123456789abcdef"})";
  SharedAutomationAreaCache cache;
  require(cache.update(document.data(), document.size(),
                       "fp-0123456789abcdef0123456789abcdef", "office",
                       1234, pose()),
          "a legacy Floorplan without automationAreas should remain valid");
  require(cache.configuration_valid() && cache.revision() == 0 &&
              cache.size() == 0,
          "legacy Floorplans should expose no canonical areas");
}

void malformed_or_over_capacity_areas_fail_closed() {
  std::string areas;
  for (int index = 0; index < 7; ++index) {
    if (!areas.empty())
      areas += ',';
    areas += "{\"areaId\":\"area-" + std::to_string(index) +
             "\",\"name\":\"Area\",\"spaceId\":\"office\","
             "\"enabled\":true,\"pointsPx\":[[0,0],[10,0],[10,10]]}";
  }
  const std::string document =
      "{\"floorplanId\":\"fp-0123456789abcdef0123456789abcdef\","
      "\"automationAreas\":{\"schemaVersion\":1,\"revision\":1,"
      "\"areas\":[" + areas + "]}}";
  SharedAutomationAreaCache cache;
  require(!cache.update(document.data(), document.size(),
                        "fp-0123456789abcdef0123456789abcdef", "office",
                        1234, pose()),
          "a seventh enabled area in one Space must be rejected");
  require(!cache.configuration_valid() && cache.size() == 6,
          "a rejected collection must never become authoritative");
}

void malformed_json_arrays_fail_closed() {
  const std::string document =
      R"({"floorplanId":"fp-0123456789abcdef0123456789abcdef","automationAreas":{"schemaVersion":1,"revision":1,"areas":[{"areaId":"desk","name":"Desk","spaceId":"office","enabled":true,"pointsPx":[[10,20] [30,20],[30,40]]}]}})";
  SharedAutomationAreaCache cache;
  require(!cache.update(document.data(), document.size(),
                        "fp-0123456789abcdef0123456789abcdef", "office",
                        1234, pose()),
          "JSON arrays without separators must be rejected");
  require(!cache.configuration_valid(),
          "malformed JSON must never become authoritative");
}

}  // namespace

void run_shared_automation_area_tests() {
  coordinator_publication_never_overwrites_a_pinned_reader();
  canonical_areas_are_bounded_and_converted_to_local_radar_space();
  receiving_pose_scale_is_applied_to_canonical_points();
  missing_receiving_pose_fails_closed();
  legacy_floorplans_are_valid_and_empty();
  malformed_or_over_capacity_areas_fail_closed();
  malformed_json_arrays_fail_closed();
}
