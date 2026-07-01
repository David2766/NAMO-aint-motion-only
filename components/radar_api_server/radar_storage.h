#pragma once

#include "esp_partition.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace radar_api_server {

enum class RadarPayloadTarget : uint8_t {
  CONFIG = 0,
  IMAGE = 1,
  DEVICE_CONFIG = 2,
  STATS = 3,
};

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
};

struct RadarStorageStatus {
  bool ok;
  const char *partition_label;
  uint32_t total_bytes;
  uint32_t used_bytes;
  RadarStorageHeader header;
};

class RadarStorage {
 public:
  bool ensure_partition();
  RadarStorageStatus status();

  bool read_payload(RadarPayloadTarget target, std::string *out);
  bool write_payload(RadarPayloadTarget target, const uint8_t *data, uint32_t size);
  bool start_upload(RadarPayloadTarget target, uint32_t size);
  bool write_upload_chunk(RadarPayloadTarget target, uint32_t offset, const uint8_t *data, uint32_t size);
  bool commit_upload(RadarPayloadTarget target);
  bool delete_floorplan();
  bool delete_all();

  bool parse_target(const std::string &value, RadarPayloadTarget *target) const;
  uint32_t payload_max_size(RadarPayloadTarget target);

 private:
  const esp_partition_t *partition_{nullptr};

  RadarStorageHeader default_header_() const;
  bool read_header_(RadarStorageHeader *header);
  bool write_header_(const RadarStorageHeader &header);
  bool read_blob_(uint32_t offset, uint32_t size, std::string *out);
  bool write_blob_(RadarPayloadTarget target, const uint8_t *data, uint32_t size,
                   RadarStorageHeader *header);
  bool write_chunk_(RadarPayloadTarget target, uint32_t offset, const uint8_t *data, uint32_t size,
                    RadarStorageHeader *header);
  uint32_t payload_offset_(RadarPayloadTarget target) const;
  uint32_t payload_size_(RadarPayloadTarget target, const RadarStorageHeader &header) const;
  void set_payload_metadata_(RadarPayloadTarget target, uint32_t size, uint32_t hash,
                             RadarStorageHeader *header) const;
};

}  // namespace radar_api_server
}  // namespace esphome
