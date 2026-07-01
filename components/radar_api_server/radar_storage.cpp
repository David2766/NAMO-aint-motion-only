#include "radar_storage.h"

#include "esphome/core/log.h"

#include <algorithm>

namespace esphome {
namespace radar_api_server {

static const char *const STORAGE_TAG = "radar_storage";
static const char *const RADAR_PARTITION_LABEL = "spiffs";

static constexpr uint32_t STORAGE_MAGIC = 0x46504C4E;  // FPLN
static constexpr uint16_t STORAGE_VERSION = 1;
static constexpr uint32_t FLASH_SECTOR_SIZE = 4096;
static constexpr uint32_t HEADER_OFFSET = 0;
static constexpr uint32_t CONFIG_OFFSET = FLASH_SECTOR_SIZE;
static constexpr uint32_t CONFIG_MAX_SIZE = 384 * 1024;
static constexpr uint32_t DEVICE_CONFIG_OFFSET = CONFIG_OFFSET + CONFIG_MAX_SIZE;
static constexpr uint32_t DEVICE_CONFIG_MAX_SIZE = 64 * 1024;
static constexpr uint32_t STATS_OFFSET = DEVICE_CONFIG_OFFSET + DEVICE_CONFIG_MAX_SIZE;
static constexpr uint32_t STATS_MAX_SIZE = 64 * 1024;
static constexpr uint32_t IMAGE_OFFSET = STATS_OFFSET + STATS_MAX_SIZE;
static_assert(IMAGE_OFFSET == FLASH_SECTOR_SIZE + 512 * 1024, "image offset must remain compatible");

static uint32_t round_up_sector(uint32_t size) {
  if (size == 0)
    return FLASH_SECTOR_SIZE;
  return ((size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
}

static uint32_t fnv1a_hash(const uint8_t *data, uint32_t size) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < size; i++) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

static uint32_t upload_id_for_target(RadarPayloadTarget target) {
  if (target == RadarPayloadTarget::CONFIG)
    return 1;
  if (target == RadarPayloadTarget::IMAGE)
    return 2;
  if (target == RadarPayloadTarget::DEVICE_CONFIG)
    return 3;
  if (target == RadarPayloadTarget::STATS)
    return 4;
  return 0;
}

bool RadarStorage::ensure_partition() {
  if (this->partition_ != nullptr)
    return true;

  this->partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                              RADAR_PARTITION_LABEL);
  if (this->partition_ == nullptr)
    ESP_LOGE(STORAGE_TAG, "Radar storage partition not found");
  return this->partition_ != nullptr;
}

RadarStorageStatus RadarStorage::status() {
  const bool storage_ok = this->ensure_partition();
  RadarStorageHeader header{};
  if (!storage_ok || !this->read_header_(&header))
    header = this->default_header_();

  RadarStorageStatus status{};
  status.ok = storage_ok;
  status.partition_label = RADAR_PARTITION_LABEL;
  status.total_bytes = storage_ok ? this->partition_->size : 0;
  status.used_bytes = FLASH_SECTOR_SIZE + header.config_size + header.image_size + header.reserved[0] + header.reserved[1];
  status.header = header;
  return status;
}

bool RadarStorage::read_payload(RadarPayloadTarget target, std::string *out) {
  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    return false;

  const uint32_t size = this->payload_size_(target, header);
  if (size == 0)
    return false;
  return this->read_blob_(this->payload_offset_(target), size, out);
}

bool RadarStorage::write_payload(RadarPayloadTarget target, const uint8_t *data, uint32_t size) {
  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    header = this->default_header_();
  return this->write_blob_(target, data, size, &header);
}

bool RadarStorage::start_upload(RadarPayloadTarget target, uint32_t size) {
  if (size == 0 || size > this->payload_max_size(target))
    return false;

  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    header = this->default_header_();

  if (!this->ensure_partition())
    return false;
  if (esp_partition_erase_range(this->partition_, this->payload_offset_(target), round_up_sector(size)) != ESP_OK)
    return false;

  header.upload_target = upload_id_for_target(target);
  header.upload_size = size;
  header.upload_written = 0;
  return this->write_header_(header);
}

bool RadarStorage::write_upload_chunk(RadarPayloadTarget target, uint32_t offset, const uint8_t *data,
                                          uint32_t size) {
  RadarStorageHeader header{};
  if (!this->read_header_(&header) || header.upload_target != upload_id_for_target(target))
    return false;
  return this->write_chunk_(target, offset, data, size, &header);
}

bool RadarStorage::commit_upload(RadarPayloadTarget target) {
  RadarStorageHeader header{};
  if (!this->read_header_(&header) || header.upload_target != upload_id_for_target(target))
    return false;
  if (header.upload_written < header.upload_size)
    return false;

  std::string data;
  if (!this->read_blob_(this->payload_offset_(target), header.upload_size, &data))
    return false;

  const uint32_t hash = fnv1a_hash(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  this->set_payload_metadata_(target, header.upload_size, hash, &header);
  header.upload_target = 0;
  header.upload_size = 0;
  header.upload_written = 0;
  return this->write_header_(header);
}

bool RadarStorage::delete_floorplan() {
  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    header = this->default_header_();
  if (!this->ensure_partition())
    return false;

  if (header.config_size > 0 &&
      esp_partition_erase_range(this->partition_, CONFIG_OFFSET, round_up_sector(header.config_size)) != ESP_OK)
    return false;
  if (header.image_size > 0 &&
      esp_partition_erase_range(this->partition_, IMAGE_OFFSET, round_up_sector(header.image_size)) != ESP_OK)
    return false;

  header.config_size = 0;
  header.image_size = 0;
  header.config_hash = 0;
  header.image_hash = 0;
  if (header.upload_target == upload_id_for_target(RadarPayloadTarget::CONFIG) ||
      header.upload_target == upload_id_for_target(RadarPayloadTarget::IMAGE)) {
    header.upload_target = 0;
    header.upload_size = 0;
    header.upload_written = 0;
  }
  return this->write_header_(header);
}

bool RadarStorage::delete_all() {
  if (!this->ensure_partition())
    return false;
  if (esp_partition_erase_range(this->partition_, 0, this->partition_->size) != ESP_OK)
    return false;
  return this->write_header_(this->default_header_());
}

bool RadarStorage::parse_target(const std::string &value, RadarPayloadTarget *target) const {
  if (value == "config" || value == "json") {
    *target = RadarPayloadTarget::CONFIG;
    return true;
  }
  if (value == "image") {
    *target = RadarPayloadTarget::IMAGE;
    return true;
  }
  if (value == "device_config") {
    *target = RadarPayloadTarget::DEVICE_CONFIG;
    return true;
  }
  if (value == "stats") {
    *target = RadarPayloadTarget::STATS;
    return true;
  }
  return false;
}

uint32_t RadarStorage::payload_max_size(RadarPayloadTarget target) {
  if (target == RadarPayloadTarget::CONFIG)
    return CONFIG_MAX_SIZE;
  if (target == RadarPayloadTarget::DEVICE_CONFIG)
    return DEVICE_CONFIG_MAX_SIZE;
  if (target == RadarPayloadTarget::STATS)
    return STATS_MAX_SIZE;
  if (!this->ensure_partition() || this->partition_->size <= IMAGE_OFFSET)
    return 0;
  return this->partition_->size - IMAGE_OFFSET;
}

RadarStorageHeader RadarStorage::default_header_() const {
  RadarStorageHeader header{};
  header.magic = STORAGE_MAGIC;
  header.version = STORAGE_VERSION;
  header.header_size = sizeof(RadarStorageHeader);
  return header;
}

bool RadarStorage::read_header_(RadarStorageHeader *header) {
  if (!this->ensure_partition())
    return false;

  if (esp_partition_read(this->partition_, HEADER_OFFSET, header, sizeof(RadarStorageHeader)) != ESP_OK)
    return false;

  if (header->magic != STORAGE_MAGIC || header->version != STORAGE_VERSION ||
      header->header_size != sizeof(RadarStorageHeader)) {
    *header = this->default_header_();
    return false;
  }

  if (header->config_size > CONFIG_MAX_SIZE || header->image_size > this->payload_max_size(RadarPayloadTarget::IMAGE) ||
      header->reserved[0] > DEVICE_CONFIG_MAX_SIZE || header->reserved[1] > STATS_MAX_SIZE)
    return false;

  return true;
}

bool RadarStorage::write_header_(const RadarStorageHeader &header) {
  if (!this->ensure_partition())
    return false;
  if (esp_partition_erase_range(this->partition_, HEADER_OFFSET, FLASH_SECTOR_SIZE) != ESP_OK)
    return false;
  return esp_partition_write(this->partition_, HEADER_OFFSET, &header, sizeof(RadarStorageHeader)) == ESP_OK;
}

bool RadarStorage::read_blob_(uint32_t offset, uint32_t size, std::string *out) {
  if (!this->ensure_partition() || offset + size > this->partition_->size)
    return false;
  out->resize(size);
  return esp_partition_read(this->partition_, offset, out->data(), size) == ESP_OK;
}

bool RadarStorage::write_blob_(RadarPayloadTarget target, const uint8_t *data, uint32_t size,
                                   RadarStorageHeader *header) {
  if (size == 0 || size > this->payload_max_size(target))
    return false;

  const uint32_t offset = this->payload_offset_(target);
  if (!this->ensure_partition())
    return false;
  if (esp_partition_erase_range(this->partition_, offset, round_up_sector(size)) != ESP_OK)
    return false;
  if (esp_partition_write(this->partition_, offset, data, size) != ESP_OK)
    return false;

  this->set_payload_metadata_(target, size, fnv1a_hash(data, size), header);
  header->upload_target = 0;
  header->upload_size = 0;
  header->upload_written = 0;
  return this->write_header_(*header);
}

bool RadarStorage::write_chunk_(RadarPayloadTarget target, uint32_t offset, const uint8_t *data, uint32_t size,
                                    RadarStorageHeader *header) {
  if (!this->ensure_partition() || size == 0)
    return false;
  if (offset + size > header->upload_size || header->upload_size > this->payload_max_size(target))
    return false;
  if (esp_partition_write(this->partition_, this->payload_offset_(target) + offset, data, size) != ESP_OK)
    return false;

  header->upload_written = std::max(header->upload_written, offset + size);
  return this->write_header_(*header);
}

uint32_t RadarStorage::payload_offset_(RadarPayloadTarget target) const {
  if (target == RadarPayloadTarget::CONFIG)
    return CONFIG_OFFSET;
  if (target == RadarPayloadTarget::DEVICE_CONFIG)
    return DEVICE_CONFIG_OFFSET;
  if (target == RadarPayloadTarget::STATS)
    return STATS_OFFSET;
  return IMAGE_OFFSET;
}

uint32_t RadarStorage::payload_size_(RadarPayloadTarget target, const RadarStorageHeader &header) const {
  if (target == RadarPayloadTarget::CONFIG)
    return header.config_size;
  if (target == RadarPayloadTarget::DEVICE_CONFIG)
    return header.reserved[0];
  if (target == RadarPayloadTarget::STATS)
    return header.reserved[1];
  return header.image_size;
}

void RadarStorage::set_payload_metadata_(RadarPayloadTarget target, uint32_t size, uint32_t hash,
                                             RadarStorageHeader *header) const {
  if (target == RadarPayloadTarget::CONFIG) {
    header->config_size = size;
    header->config_hash = hash;
  } else if (target == RadarPayloadTarget::IMAGE) {
    header->image_size = size;
    header->image_hash = hash;
  } else if (target == RadarPayloadTarget::DEVICE_CONFIG) {
    header->reserved[0] = size;
    header->reserved[2] = hash;
  } else if (target == RadarPayloadTarget::STATS) {
    header->reserved[1] = size;
    header->reserved[3] = hash;
  }
}

}  // namespace radar_api_server
}  // namespace esphome
