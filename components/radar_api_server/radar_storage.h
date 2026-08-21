#pragma once

#include "esp_partition.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

enum class RadarPayloadTarget : uint8_t {
  CONFIG = 0,
  IMAGE = 1,
  DEVICE_CONFIG = 2,
  STATS = 3,
};

enum class RadarUploadResult : uint8_t {
  OK = 0,
  INVALID_REQUEST,
  SESSION_MISMATCH,
  OFFSET_MISMATCH,
  PAYLOAD_TOO_LARGE,
  INCOMPLETE,
  STORAGE_FAILED,
  READ_FAILED,
};

enum class RadarPlatformBlob : uint8_t {
  PARTICIPANT_REGISTRY_SLOT_0 = 0,
  PARTICIPANT_REGISTRY_SLOT_1 = 1,
  PARTICIPANT_REGISTRY_COMMIT = 2,
  PARTICIPANT_READINESS_SLOT_0 = 3,
  PARTICIPANT_READINESS_SLOT_1 = 4,
  PARTICIPANT_READINESS_COMMIT = 5,
  PARTICIPANT_CREDENTIAL_SLOT_0 = 6,
  PARTICIPANT_CREDENTIAL_SLOT_1 = 7,
  PARTICIPANT_CREDENTIAL_COMMIT = 8,
  SITE_CONFIGURATION_SLOT_0 = 9,
  SITE_CONFIGURATION_SLOT_1 = 10,
  SITE_CONFIGURATION_COMMIT = 11,
  SITE_DIRECTORY_SLOT_0 = 12,
  SITE_DIRECTORY_SLOT_1 = 13,
  SITE_DIRECTORY_COMMIT = 14,
  GROUP_ROSTER_SLOT_0 = 15,
  GROUP_ROSTER_SLOT_1 = 16,
  GROUP_ROSTER_COMMIT = 17,
};

enum class RadarPlatformBlobWriteResult : uint8_t {
  IDLE = 0,
  OK,
  INVALID_ARGUMENT,
  PARTITION_UNAVAILABLE,
  LAYOUT_INVALID,
  ERASE_FAILED,
  WRITE_FAILED,
};

struct RadarPlatformBlobWriteDiagnostic {
  RadarPlatformBlobWriteResult result{RadarPlatformBlobWriteResult::IDLE};
  RadarPlatformBlob target{RadarPlatformBlob::PARTICIPANT_REGISTRY_SLOT_0};
  uint32_t offset{0};
  int32_t error{0};
};

const char *radar_platform_blob_write_result_to_string(
    RadarPlatformBlobWriteResult result);

struct RadarStorageHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t config_size;
  uint32_t image_size;
  uint32_t config_hash;
  uint32_t image_hash;
  uint32_t upload_target;
  uint32_t upload_size;
  uint32_t upload_written;
  uint32_t reserved[7];
  uint32_t active_config_slot;
  uint32_t active_image_slot;
  uint32_t upload_slot;
};

static_assert(std::is_trivially_copyable<RadarStorageHeader>::value,
              "radar storage header must remain a fixed flash record");
static_assert(sizeof(RadarStorageHeader) == 76,
              "radar storage header layout changed");
static_assert(offsetof(RadarStorageHeader, active_config_slot) == 64,
              "radar storage header slot fields moved");

struct RadarStorageStatus {
  bool ok;
  const char *partition_label;
  uint32_t total_bytes;
  uint32_t used_bytes;
  RadarStorageHeader header;
};

class RadarStorage {
 public:
  static constexpr uint32_t PLATFORM_SECTOR_SIZE = 4096;
  static constexpr uint32_t PLATFORM_IO_CHUNK_SIZE = 256;
  static constexpr uint32_t LEGACY_PLATFORM_SECTOR_COUNT = 6;
  static constexpr uint32_t TAIL_PLATFORM_SECTOR_COUNT = 15;
  static constexpr uint32_t PLATFORM_SECTOR_COUNT = 18;
  static constexpr uint32_t GROUP_ROSTER_STORAGE_OFFSET = 0x0007E000;

  static constexpr uint32_t platform_io_chunk_size(uint32_t remaining) {
    return remaining < PLATFORM_IO_CHUNK_SIZE ? remaining
                                              : PLATFORM_IO_CHUNK_SIZE;
  }

  static constexpr uint32_t platform_blob_offset_for_partition(
      uint32_t partition_size, RadarPlatformBlob target) {
    const uint32_t index = static_cast<uint32_t>(target);
    return index < LEGACY_PLATFORM_SECTOR_COUNT
               ? partition_size -
                     PLATFORM_SECTOR_SIZE * LEGACY_PLATFORM_SECTOR_COUNT +
                     index * PLATFORM_SECTOR_SIZE
               : index < 9
                     ? partition_size - PLATFORM_SECTOR_SIZE * 9 +
                           (index - LEGACY_PLATFORM_SECTOR_COUNT) *
                               PLATFORM_SECTOR_SIZE
                     : index < 12
                           ? partition_size -
                                 PLATFORM_SECTOR_SIZE * 12 +
                                 (index - 9) * PLATFORM_SECTOR_SIZE
                           : index < 15
                                 ? partition_size -
                                       PLATFORM_SECTOR_SIZE * 15 +
                                       (index - 12) * PLATFORM_SECTOR_SIZE
                                 : GROUP_ROSTER_STORAGE_OFFSET +
                                       (index - 15) * PLATFORM_SECTOR_SIZE;
  }

  bool ensure_partition();
  RadarStorageStatus status();

  bool read_payload(RadarPayloadTarget target, std::string *out);
  bool write_payload(RadarPayloadTarget target, const uint8_t *data, uint32_t size);
  bool start_upload(RadarPayloadTarget target, uint32_t size, uint32_t session_id);
  bool write_upload_chunk(RadarPayloadTarget target, uint32_t offset, const uint8_t *data, uint32_t size,
                          uint32_t session_id);
  bool commit_upload(RadarPayloadTarget target, uint32_t session_id);
  bool start_floorplan_replica(uint32_t config_size, uint32_t image_size,
                               uint32_t session_id);
  bool write_floorplan_replica_chunk(RadarPayloadTarget target,
                                     uint32_t offset, const uint8_t *data,
                                     uint32_t size, uint32_t session_id);
  bool commit_floorplan_replica(uint32_t session_id,
                                uint32_t expected_config_hash,
                                uint32_t expected_image_hash);
  void cancel_floorplan_replica();
  RadarUploadResult last_upload_result() const { return this->last_upload_result_; }
  bool delete_payload(RadarPayloadTarget target);
  bool delete_floorplan();
  bool delete_all();

  bool parse_target(const std::string &value, RadarPayloadTarget *target) const;
  uint32_t payload_max_size(RadarPayloadTarget target);
  bool payload_info(RadarPayloadTarget target, uint32_t *offset, uint32_t *size);
  bool read_storage_range(uint32_t offset, uint8_t *out, uint32_t size);
  bool read_platform_blob(RadarPlatformBlob target, uint8_t *out, uint32_t size);
  bool read_platform_blob_raw(RadarPlatformBlob target, uint8_t *out,
                              uint32_t size, bool *contains_data = nullptr);
  bool write_platform_blob(RadarPlatformBlob target, const uint8_t *data, uint32_t size);
  bool prepare_group_roster_storage();
  void reset_platform_blob_write_diagnostic() {
    this->last_platform_blob_write_diagnostic_ = {};
  }
  const RadarPlatformBlobWriteDiagnostic &
  last_platform_blob_write_diagnostic() const {
    return this->last_platform_blob_write_diagnostic_;
  }

 private:
  const esp_partition_t *partition_{nullptr};
  RadarUploadResult last_upload_result_{RadarUploadResult::OK};
  RadarPlatformBlobWriteDiagnostic last_platform_blob_write_diagnostic_{};
  struct FloorplanReplicaUpload {
    uint32_t session_id{0};
    uint32_t config_size{0};
    uint32_t image_size{0};
    uint32_t config_written{0};
    uint32_t image_written{0};
    uint32_t config_slot{0};
    uint32_t image_slot{0};
  } replica_upload_{};

  RadarStorageHeader default_header_() const;
  bool read_header_(RadarStorageHeader *header);
  bool write_header_(const RadarStorageHeader &header);
  bool read_blob_(uint32_t offset, uint32_t size, std::string *out);
  bool hash_blob_(uint32_t offset, uint32_t size, uint32_t *hash);
  bool write_blob_(RadarPayloadTarget target, const uint8_t *data, uint32_t size,
                   RadarStorageHeader *header);
  bool write_chunk_(RadarPayloadTarget target, uint32_t offset, const uint8_t *data, uint32_t size,
                    RadarStorageHeader *header);
  bool migrate_v1_header_(RadarStorageHeader *header);
  uint32_t payload_offset_(RadarPayloadTarget target,
                           const RadarStorageHeader &header) const;
  uint32_t upload_offset_(RadarPayloadTarget target,
                          const RadarStorageHeader &header) const;
  uint32_t payload_size_(RadarPayloadTarget target, const RadarStorageHeader &header) const;
  uint32_t platform_blob_offset_(RadarPlatformBlob target) const;
  void set_payload_metadata_(RadarPayloadTarget target, uint32_t size, uint32_t hash,
                             RadarStorageHeader *header) const;
};

}  // namespace radar_api_server
}  // namespace esphome
