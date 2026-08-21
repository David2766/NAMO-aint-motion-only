#include "radar_storage.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <esp_err.h>
#include <memory>
#include <new>

namespace esphome {
namespace radar_api_server {

static const char *const STORAGE_TAG = "radar_storage";
static const char *const RADAR_PARTITION_LABEL = "spiffs";

static constexpr uint32_t STORAGE_MAGIC = 0x46504C4E;  // FPLN
static constexpr uint16_t STORAGE_VERSION = 2;
static constexpr uint32_t FLASH_SECTOR_SIZE = 4096;
static constexpr uint32_t HEADER_OFFSET = 0;
static constexpr uint32_t CONFIG_OFFSET = FLASH_SECTOR_SIZE;
static constexpr uint32_t CONFIG_MAX_SIZE = 384 * 1024;
static constexpr uint32_t DEVICE_CONFIG_OFFSET = CONFIG_OFFSET + CONFIG_MAX_SIZE;
static constexpr uint32_t DEVICE_CONFIG_MAX_SIZE = 64 * 1024;
static constexpr uint32_t STATS_OFFSET = DEVICE_CONFIG_OFFSET + DEVICE_CONFIG_MAX_SIZE;
static constexpr uint32_t LEGACY_STATS_MAX_SIZE = 64 * 1024;
static constexpr uint32_t GROUP_ROSTER_STORAGE_SIZE = 3 * FLASH_SECTOR_SIZE;
static constexpr uint32_t STATS_MAX_SIZE =
    LEGACY_STATS_MAX_SIZE - GROUP_ROSTER_STORAGE_SIZE;
static constexpr uint32_t GROUP_ROSTER_STORAGE_OFFSET =
    STATS_OFFSET + STATS_MAX_SIZE;
static constexpr uint32_t LEGACY_IMAGE_OFFSET =
    STATS_OFFSET + LEGACY_STATS_MAX_SIZE;
static constexpr uint32_t CONFIG_SLOT_0_OFFSET = CONFIG_OFFSET;
static constexpr uint32_t CONFIG_SLOT_1_OFFSET = LEGACY_IMAGE_OFFSET;
static constexpr uint32_t IMAGE_SLOT_MAX_SIZE = 512 * 1024;
static constexpr uint32_t IMAGE_SLOT_0_OFFSET =
    CONFIG_SLOT_1_OFFSET + CONFIG_MAX_SIZE;
static constexpr uint32_t IMAGE_SLOT_1_OFFSET =
    IMAGE_SLOT_0_OFFSET + IMAGE_SLOT_MAX_SIZE;
static constexpr uint32_t PAYLOAD_STORAGE_END =
    IMAGE_SLOT_1_OFFSET + IMAGE_SLOT_MAX_SIZE;
static constexpr uint32_t HASH_STREAM_CHUNK_SIZE = 512;
static constexpr uint32_t PLATFORM_STORAGE_SECTOR_COUNT =
    RadarStorage::PLATFORM_SECTOR_COUNT;
static constexpr uint32_t TAIL_PLATFORM_STORAGE_SIZE =
    FLASH_SECTOR_SIZE * RadarStorage::TAIL_PLATFORM_SECTOR_COUNT;
static_assert(FLASH_SECTOR_SIZE == RadarStorage::PLATFORM_SECTOR_SIZE,
              "platform storage sector size must match flash erase size");
static_assert(LEGACY_IMAGE_OFFSET == FLASH_SECTOR_SIZE + 512 * 1024,
              "legacy image offset must remain compatible");
static_assert(GROUP_ROSTER_STORAGE_OFFSET ==
                  RadarStorage::GROUP_ROSTER_STORAGE_OFFSET,
              "Group roster storage offset changed");
static_assert(STATS_MAX_SIZE % FLASH_SECTOR_SIZE == 0 &&
                  GROUP_ROSTER_STORAGE_OFFSET % FLASH_SECTOR_SIZE == 0,
              "Statistics and Group roster boundaries must be sector aligned");
static_assert(GROUP_ROSTER_STORAGE_OFFSET + GROUP_ROSTER_STORAGE_SIZE ==
                  LEGACY_IMAGE_OFFSET,
              "Group roster storage must fit the reserved statistics tail");

static bool platform_storage_layout_valid(uint32_t partition_size) {
  return partition_size >= PAYLOAD_STORAGE_END + TAIL_PLATFORM_STORAGE_SIZE &&
         GROUP_ROSTER_STORAGE_OFFSET >= STATS_OFFSET + STATS_MAX_SIZE &&
         GROUP_ROSTER_STORAGE_OFFSET + GROUP_ROSTER_STORAGE_SIZE <=
             LEGACY_IMAGE_OFFSET;
}

struct RadarStorageHeaderV1 {
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

static_assert(sizeof(RadarStorageHeaderV1) == 64,
              "legacy storage header layout changed");

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

static uint32_t &upload_session_id(RadarStorageHeader *header) { return header->reserved[4]; }
static uint32_t &last_upload_session_id(RadarStorageHeader *header) { return header->reserved[5]; }
static uint32_t &last_upload_target(RadarStorageHeader *header) { return header->reserved[6]; }

static uint32_t upload_session_id(const RadarStorageHeader &header) { return header.reserved[4]; }
static uint32_t last_upload_session_id(const RadarStorageHeader &header) { return header.reserved[5]; }
static uint32_t last_upload_target(const RadarStorageHeader &header) { return header.reserved[6]; }

const char *radar_platform_blob_write_result_to_string(
    RadarPlatformBlobWriteResult result) {
  switch (result) {
    case RadarPlatformBlobWriteResult::IDLE: return "idle";
    case RadarPlatformBlobWriteResult::OK: return "ok";
    case RadarPlatformBlobWriteResult::INVALID_ARGUMENT:
      return "invalid_argument";
    case RadarPlatformBlobWriteResult::PARTITION_UNAVAILABLE:
      return "partition_unavailable";
    case RadarPlatformBlobWriteResult::LAYOUT_INVALID:
      return "layout_invalid";
    case RadarPlatformBlobWriteResult::ERASE_FAILED: return "erase_failed";
    case RadarPlatformBlobWriteResult::WRITE_FAILED: return "write_failed";
  }
  return "unknown";
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
  return this->read_blob_(this->payload_offset_(target, header), size, out);
}

bool RadarStorage::payload_info(RadarPayloadTarget target, uint32_t *offset, uint32_t *size) {
  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    return false;

  const uint32_t payload_size = this->payload_size_(target, header);
  if (payload_size == 0)
    return false;

  if (offset != nullptr)
    *offset = this->payload_offset_(target, header);
  if (size != nullptr)
    *size = payload_size;
  return true;
}

bool RadarStorage::read_storage_range(uint32_t offset, uint8_t *out, uint32_t size) {
  if (out == nullptr || size == 0)
    return false;
  if (!this->ensure_partition() || offset > this->partition_->size || size > this->partition_->size - offset)
    return false;
  return esp_partition_read(this->partition_, offset, out, size) == ESP_OK;
}

bool RadarStorage::read_platform_blob(RadarPlatformBlob target, uint8_t *out,
                                      uint32_t size) {
  bool contains_data = false;
  return this->read_platform_blob_raw(target, out, size, &contains_data) &&
         contains_data;
}

bool RadarStorage::read_platform_blob_raw(RadarPlatformBlob target,
                                          uint8_t *out, uint32_t size,
                                          bool *contains_data) {
  if (out == nullptr || size == 0 || size > FLASH_SECTOR_SIZE ||
      static_cast<uint32_t>(target) >= PLATFORM_STORAGE_SECTOR_COUNT ||
      !this->ensure_partition() ||
      !platform_storage_layout_valid(this->partition_->size))
    return false;
  const uint32_t offset = this->platform_blob_offset_(target);
  alignas(4) std::array<uint8_t, PLATFORM_IO_CHUNK_SIZE> staging{};
  bool present = false;
  uint32_t cursor = 0;
  while (cursor < size) {
    const uint32_t chunk = platform_io_chunk_size(size - cursor);
    const esp_err_t error = esp_partition_read(
        this->partition_, offset + cursor, staging.data(), chunk);
    if (error != ESP_OK) {
      ESP_LOGE(
          STORAGE_TAG,
          "Platform blob read failed target=%u offset=%u size=%u error=%s",
          static_cast<unsigned>(target),
          static_cast<unsigned>(offset + cursor), static_cast<unsigned>(chunk),
          esp_err_to_name(error));
      return false;
    }
    std::memcpy(out + cursor, staging.data(), chunk);
    for (uint32_t index = 0; index < chunk; ++index)
      present = present || staging[index] != 0xFF;
    cursor += chunk;
  }
  if (contains_data != nullptr)
    *contains_data = present;
  return true;
}

bool RadarStorage::write_platform_blob(RadarPlatformBlob target,
                                       const uint8_t *data, uint32_t size) {
  this->last_platform_blob_write_diagnostic_ = {
      RadarPlatformBlobWriteResult::INVALID_ARGUMENT, target, 0,
      ESP_ERR_INVALID_ARG};
  if (data == nullptr || size == 0 || size > FLASH_SECTOR_SIZE ||
      static_cast<uint32_t>(target) >= PLATFORM_STORAGE_SECTOR_COUNT) {
    ESP_LOGE(STORAGE_TAG,
             "Platform blob write rejected target=%u size=%u reason=invalid_argument",
             static_cast<unsigned>(target), static_cast<unsigned>(size));
    return false;
  }
  if (!this->ensure_partition()) {
    this->last_platform_blob_write_diagnostic_.result =
        RadarPlatformBlobWriteResult::PARTITION_UNAVAILABLE;
    this->last_platform_blob_write_diagnostic_.error = ESP_ERR_NOT_FOUND;
    return false;
  }
  if (!platform_storage_layout_valid(this->partition_->size)) {
    this->last_platform_blob_write_diagnostic_.result =
        RadarPlatformBlobWriteResult::LAYOUT_INVALID;
    this->last_platform_blob_write_diagnostic_.error = ESP_ERR_INVALID_SIZE;
    ESP_LOGE(STORAGE_TAG,
             "Platform blob write rejected target=%u partition=%u required=%u "
             "reason=layout_invalid",
             static_cast<unsigned>(target),
             static_cast<unsigned>(this->partition_->size),
             static_cast<unsigned>(PAYLOAD_STORAGE_END +
                                   TAIL_PLATFORM_STORAGE_SIZE));
    return false;
  }
  const uint32_t offset = this->platform_blob_offset_(target);
  this->last_platform_blob_write_diagnostic_.offset = offset;
  esp_err_t error =
      esp_partition_erase_range(this->partition_, offset, FLASH_SECTOR_SIZE);
  if (error != ESP_OK) {
    this->last_platform_blob_write_diagnostic_.result =
        RadarPlatformBlobWriteResult::ERASE_FAILED;
    this->last_platform_blob_write_diagnostic_.error = error;
    ESP_LOGE(STORAGE_TAG,
             "Platform blob erase failed target=%u offset=%u error=%s",
             static_cast<unsigned>(target), static_cast<unsigned>(offset),
             esp_err_to_name(error));
    return false;
  }
  alignas(4) std::array<uint8_t, PLATFORM_IO_CHUNK_SIZE> staging{};
  uint32_t cursor = 0;
  while (cursor < size) {
    const uint32_t chunk = platform_io_chunk_size(size - cursor);
    std::memcpy(staging.data(), data + cursor, chunk);
    error = esp_partition_write(this->partition_, offset + cursor,
                                staging.data(), chunk);
    if (error != ESP_OK) {
      this->last_platform_blob_write_diagnostic_.result =
          RadarPlatformBlobWriteResult::WRITE_FAILED;
      this->last_platform_blob_write_diagnostic_.offset = offset + cursor;
      this->last_platform_blob_write_diagnostic_.error = error;
      ESP_LOGE(
          STORAGE_TAG,
          "Platform blob write failed target=%u offset=%u size=%u error=%s",
          static_cast<unsigned>(target),
          static_cast<unsigned>(offset + cursor), static_cast<unsigned>(chunk),
          esp_err_to_name(error));
      return false;
    }
    cursor += chunk;
  }
  this->last_platform_blob_write_diagnostic_.result =
      RadarPlatformBlobWriteResult::OK;
  this->last_platform_blob_write_diagnostic_.error = ESP_OK;
  return true;
}

bool RadarStorage::prepare_group_roster_storage() {
  if (!this->ensure_partition() ||
      !platform_storage_layout_valid(this->partition_->size))
    return false;

  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    return true;
  if (header.reserved[1] <= STATS_MAX_SIZE)
    return true;
  if (header.reserved[1] > LEGACY_STATS_MAX_SIZE)
    return false;

  ESP_LOGW(STORAGE_TAG,
           "Legacy statistics exceed the reserved Group roster boundary; "
           "discarding statistics size=%u limit=%u",
           static_cast<unsigned>(header.reserved[1]),
           static_cast<unsigned>(STATS_MAX_SIZE));
  if (esp_partition_erase_range(this->partition_, STATS_OFFSET,
                                LEGACY_STATS_MAX_SIZE) != ESP_OK)
    return false;
  header.reserved[1] = 0;
  header.reserved[3] = 0;
  if (header.upload_target == upload_id_for_target(RadarPayloadTarget::STATS)) {
    header.upload_target = 0;
    header.upload_size = 0;
    header.upload_written = 0;
    upload_session_id(&header) = 0;
    header.upload_slot = 0;
  }
  return this->write_header_(header);
}

bool RadarStorage::write_payload(RadarPayloadTarget target, const uint8_t *data, uint32_t size) {
  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    header = this->default_header_();
  return this->write_blob_(target, data, size, &header);
}

bool RadarStorage::start_upload(RadarPayloadTarget target, uint32_t size, uint32_t session_id) {
  this->last_upload_result_ = RadarUploadResult::OK;
  const uint32_t max_size = this->payload_max_size(target);
  if (session_id == 0 || size == 0) {
    this->last_upload_result_ = RadarUploadResult::INVALID_REQUEST;
    return false;
  }
  if (size > max_size) {
    this->last_upload_result_ = RadarUploadResult::PAYLOAD_TOO_LARGE;
    return false;
  }

  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    header = this->default_header_();

  const uint32_t expected_upload_target = upload_id_for_target(target);
  if (header.upload_target != 0) {
    if (header.upload_target != expected_upload_target) {
      ESP_LOGW(STORAGE_TAG, "upload start rejected target=%u active_target=%u session=%08x active_session=%08x",
               static_cast<unsigned>(target), static_cast<unsigned>(header.upload_target),
               static_cast<unsigned>(session_id), static_cast<unsigned>(upload_session_id(header)));
      this->last_upload_result_ = RadarUploadResult::SESSION_MISMATCH;
      return false;
    }
    if (upload_session_id(header) == session_id && header.upload_size == size) {
      ESP_LOGI(STORAGE_TAG, "upload start reused target=%u session=%08x size=%u",
               static_cast<unsigned>(target), static_cast<unsigned>(session_id), static_cast<unsigned>(size));
      return true;
    }
  }

  if (!this->ensure_partition()) {
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }
  if (target == RadarPayloadTarget::CONFIG ||
      target == RadarPayloadTarget::IMAGE) {
    header.upload_slot = target == RadarPayloadTarget::CONFIG
                             ? (header.active_config_slot == 0 ? 1 : 0)
                             : (header.active_image_slot == 0 ? 1 : 0);
  } else {
    header.upload_slot = 0;
  }
  if (esp_partition_erase_range(this->partition_,
                                this->upload_offset_(target, header),
                                round_up_sector(size)) != ESP_OK) {
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }

  header.upload_target = expected_upload_target;
  header.upload_size = size;
  header.upload_written = 0;
  upload_session_id(&header) = session_id;
  ESP_LOGW(STORAGE_TAG, "upload start target=%u upload_id=%u session=%08x size=%u offset=%u",
           static_cast<unsigned>(target), static_cast<unsigned>(header.upload_target),
           static_cast<unsigned>(session_id), static_cast<unsigned>(size),
           static_cast<unsigned>(this->upload_offset_(target, header)));
  if (!this->write_header_(header)) {
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }
  return true;
}

bool RadarStorage::write_upload_chunk(RadarPayloadTarget target, uint32_t offset, const uint8_t *data,
                                          uint32_t size, uint32_t session_id) {
  this->last_upload_result_ = RadarUploadResult::OK;
  RadarStorageHeader header{};
  if (!this->read_header_(&header)) {
    ESP_LOGE(STORAGE_TAG, "upload chunk read_header failed target=%u offset=%u size=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(offset), static_cast<unsigned>(size));
    this->last_upload_result_ = RadarUploadResult::READ_FAILED;
    return false;
  }
  const uint32_t expected_upload_target = upload_id_for_target(target);
  if (session_id == 0 || header.upload_target != expected_upload_target || upload_session_id(header) != session_id) {
    ESP_LOGE(STORAGE_TAG,
             "upload chunk session mismatch target=%u expected=%u header_target=%u session=%08x header_session=%08x "
             "upload_size=%u upload_written=%u offset=%u size=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(expected_upload_target),
             static_cast<unsigned>(header.upload_target), static_cast<unsigned>(session_id),
             static_cast<unsigned>(upload_session_id(header)), static_cast<unsigned>(header.upload_size),
             static_cast<unsigned>(header.upload_written), static_cast<unsigned>(offset), static_cast<unsigned>(size));
    this->last_upload_result_ = RadarUploadResult::SESSION_MISMATCH;
    return false;
  }
  ESP_LOGD(STORAGE_TAG, "upload chunk target=%u offset=%u size=%u upload_size=%u upload_written=%u",
           static_cast<unsigned>(target), static_cast<unsigned>(offset), static_cast<unsigned>(size),
           static_cast<unsigned>(header.upload_size), static_cast<unsigned>(header.upload_written));
  return this->write_chunk_(target, offset, data, size, &header);
}

bool RadarStorage::commit_upload(RadarPayloadTarget target, uint32_t session_id) {
  this->last_upload_result_ = RadarUploadResult::OK;
  RadarStorageHeader header{};
  if (!this->read_header_(&header)) {
    ESP_LOGE(STORAGE_TAG, "upload commit read_header failed target=%u", static_cast<unsigned>(target));
    this->last_upload_result_ = RadarUploadResult::READ_FAILED;
    return false;
  }
  const uint32_t expected_upload_target = upload_id_for_target(target);
  if (session_id != 0 && header.upload_target == 0 && last_upload_target(header) == expected_upload_target &&
      last_upload_session_id(header) == session_id) {
    ESP_LOGI(STORAGE_TAG, "upload commit duplicate target=%u session=%08x", static_cast<unsigned>(target),
             static_cast<unsigned>(session_id));
    return true;
  }
  if (session_id == 0 || header.upload_target != expected_upload_target || upload_session_id(header) != session_id) {
    ESP_LOGE(STORAGE_TAG,
             "upload commit session mismatch target=%u expected=%u header_target=%u session=%08x header_session=%08x "
             "upload_size=%u upload_written=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(expected_upload_target),
             static_cast<unsigned>(header.upload_target), static_cast<unsigned>(session_id),
             static_cast<unsigned>(upload_session_id(header)), static_cast<unsigned>(header.upload_size),
             static_cast<unsigned>(header.upload_written));
    this->last_upload_result_ = RadarUploadResult::SESSION_MISMATCH;
    return false;
  }
  if (header.upload_written < header.upload_size) {
    ESP_LOGE(STORAGE_TAG, "upload commit incomplete target=%u upload_size=%u upload_written=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(header.upload_size),
             static_cast<unsigned>(header.upload_written));
    this->last_upload_result_ = RadarUploadResult::INCOMPLETE;
    return false;
  }

  uint32_t hash = 0;
  if (!this->hash_blob_(this->upload_offset_(target, header),
                        header.upload_size, &hash)) {
    ESP_LOGE(STORAGE_TAG, "upload commit hash_blob failed target=%u offset=%u size=%u",
             static_cast<unsigned>(target),
             static_cast<unsigned>(this->upload_offset_(target, header)),
             static_cast<unsigned>(header.upload_size));
    this->last_upload_result_ = RadarUploadResult::READ_FAILED;
    return false;
  }

  const uint32_t committed_size = header.upload_size;
  this->set_payload_metadata_(target, committed_size, hash, &header);
  if (target == RadarPayloadTarget::CONFIG)
    header.active_config_slot = header.upload_slot;
  else if (target == RadarPayloadTarget::IMAGE)
    header.active_image_slot = header.upload_slot;
  header.upload_target = 0;
  header.upload_size = 0;
  header.upload_written = 0;
  last_upload_session_id(&header) = session_id;
  last_upload_target(&header) = expected_upload_target;
  upload_session_id(&header) = 0;
  header.upload_slot = 0;
  ESP_LOGW(STORAGE_TAG, "upload commit target=%u session=%08x size=%u hash=%08x", static_cast<unsigned>(target),
           static_cast<unsigned>(session_id), static_cast<unsigned>(committed_size), static_cast<unsigned>(hash));
  if (!this->write_header_(header)) {
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }
  return true;
}

bool RadarStorage::start_floorplan_replica(uint32_t config_size,
                                           uint32_t image_size,
                                           uint32_t session_id) {
  this->last_upload_result_ = RadarUploadResult::OK;
  if (session_id == 0 || config_size == 0) {
    this->last_upload_result_ = RadarUploadResult::INVALID_REQUEST;
    return false;
  }
  if (config_size > this->payload_max_size(RadarPayloadTarget::CONFIG) ||
      image_size > this->payload_max_size(RadarPayloadTarget::IMAGE)) {
    this->last_upload_result_ = RadarUploadResult::PAYLOAD_TOO_LARGE;
    return false;
  }
  if (this->replica_upload_.session_id != 0) {
    if (this->replica_upload_.session_id == session_id &&
        this->replica_upload_.config_size == config_size &&
        this->replica_upload_.image_size == image_size)
      return true;
    this->last_upload_result_ = RadarUploadResult::SESSION_MISMATCH;
    return false;
  }

  RadarStorageHeader header{};
  if (!this->read_header_(&header)) {
    this->last_upload_result_ = RadarUploadResult::READ_FAILED;
    return false;
  }
  if (header.upload_target != 0 || !this->ensure_partition()) {
    this->last_upload_result_ = header.upload_target != 0
                                    ? RadarUploadResult::SESSION_MISMATCH
                                    : RadarUploadResult::STORAGE_FAILED;
    return false;
  }

  FloorplanReplicaUpload upload{};
  upload.session_id = session_id;
  upload.config_size = config_size;
  upload.image_size = image_size;
  upload.config_slot = header.active_config_slot == 0 ? 1 : 0;
  upload.image_slot = header.active_image_slot == 0 ? 1 : 0;
  RadarStorageHeader staging = header;
  staging.upload_slot = upload.config_slot;
  if (esp_partition_erase_range(
          this->partition_,
          this->upload_offset_(RadarPayloadTarget::CONFIG, staging),
          round_up_sector(config_size)) != ESP_OK) {
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }
  if (image_size > 0) {
    staging.upload_slot = upload.image_slot;
    if (esp_partition_erase_range(
            this->partition_,
            this->upload_offset_(RadarPayloadTarget::IMAGE, staging),
            round_up_sector(image_size)) != ESP_OK) {
      this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
      return false;
    }
  }
  this->replica_upload_ = upload;
  return true;
}

bool RadarStorage::write_floorplan_replica_chunk(
    RadarPayloadTarget target, uint32_t offset, const uint8_t *data,
    uint32_t size, uint32_t session_id) {
  this->last_upload_result_ = RadarUploadResult::OK;
  if ((target != RadarPayloadTarget::CONFIG &&
       target != RadarPayloadTarget::IMAGE) ||
      data == nullptr || size == 0 || session_id == 0 ||
      this->replica_upload_.session_id != session_id) {
    this->last_upload_result_ = RadarUploadResult::SESSION_MISMATCH;
    return false;
  }
  uint32_t *written = target == RadarPayloadTarget::CONFIG
                          ? &this->replica_upload_.config_written
                          : &this->replica_upload_.image_written;
  const uint32_t total = target == RadarPayloadTarget::CONFIG
                             ? this->replica_upload_.config_size
                             : this->replica_upload_.image_size;
  if (offset != *written || size > total - *written) {
    this->last_upload_result_ = RadarUploadResult::OFFSET_MISMATCH;
    return false;
  }
  RadarStorageHeader staging{};
  staging.upload_slot = target == RadarPayloadTarget::CONFIG
                            ? this->replica_upload_.config_slot
                            : this->replica_upload_.image_slot;
  const uint32_t write_offset = this->upload_offset_(target, staging) + offset;
  if (!this->ensure_partition() ||
      esp_partition_write(this->partition_, write_offset, data, size) != ESP_OK) {
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }
  *written += size;
  return true;
}

bool RadarStorage::commit_floorplan_replica(
    uint32_t session_id, uint32_t expected_config_hash,
    uint32_t expected_image_hash) {
  this->last_upload_result_ = RadarUploadResult::OK;
  if (session_id == 0 || this->replica_upload_.session_id != session_id) {
    this->last_upload_result_ = RadarUploadResult::SESSION_MISMATCH;
    return false;
  }
  if (this->replica_upload_.config_written !=
          this->replica_upload_.config_size ||
      this->replica_upload_.image_written !=
          this->replica_upload_.image_size) {
    this->last_upload_result_ = RadarUploadResult::INCOMPLETE;
    return false;
  }

  RadarStorageHeader staging{};
  staging.upload_slot = this->replica_upload_.config_slot;
  uint32_t config_hash = 0;
  if (!this->hash_blob_(
          this->upload_offset_(RadarPayloadTarget::CONFIG, staging),
          this->replica_upload_.config_size, &config_hash) ||
      config_hash != expected_config_hash) {
    this->last_upload_result_ = RadarUploadResult::READ_FAILED;
    return false;
  }
  uint32_t image_hash = 0;
  if (this->replica_upload_.image_size > 0) {
    staging.upload_slot = this->replica_upload_.image_slot;
    if (!this->hash_blob_(
            this->upload_offset_(RadarPayloadTarget::IMAGE, staging),
            this->replica_upload_.image_size, &image_hash) ||
        image_hash != expected_image_hash) {
      this->last_upload_result_ = RadarUploadResult::READ_FAILED;
      return false;
    }
  } else if (expected_image_hash != 0) {
    this->last_upload_result_ = RadarUploadResult::READ_FAILED;
    return false;
  }

  RadarStorageHeader header{};
  if (!this->read_header_(&header)) {
    this->last_upload_result_ = RadarUploadResult::READ_FAILED;
    return false;
  }
  header.config_size = this->replica_upload_.config_size;
  header.config_hash = config_hash;
  header.image_size = this->replica_upload_.image_size;
  header.image_hash = image_hash;
  header.active_config_slot = this->replica_upload_.config_slot;
  header.active_image_slot = this->replica_upload_.image_slot;
  if (!this->write_header_(header)) {
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }
  this->replica_upload_ = {};
  return true;
}

void RadarStorage::cancel_floorplan_replica() {
  this->replica_upload_ = {};
}

bool RadarStorage::delete_payload(RadarPayloadTarget target) {
  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    header = this->default_header_();
  if (!this->ensure_partition())
    return false;

  const uint32_t size = this->payload_size_(target, header);
  if (size > 0 &&
      esp_partition_erase_range(this->partition_,
                                this->payload_offset_(target, header),
                                round_up_sector(size)) != ESP_OK)
    return false;

  this->set_payload_metadata_(target, 0, 0, &header);
  if (header.upload_target == upload_id_for_target(target)) {
    header.upload_target = 0;
    header.upload_size = 0;
    header.upload_written = 0;
    upload_session_id(&header) = 0;
    header.upload_slot = 0;
  }
  return this->write_header_(header);
}

bool RadarStorage::delete_floorplan() {
  RadarStorageHeader header{};
  if (!this->read_header_(&header))
    header = this->default_header_();
  if (!this->ensure_partition())
    return false;

  if (header.config_size > 0 &&
      esp_partition_erase_range(
          this->partition_,
          this->payload_offset_(RadarPayloadTarget::CONFIG, header),
          round_up_sector(header.config_size)) != ESP_OK)
    return false;
  if (header.image_size > 0 &&
      esp_partition_erase_range(
          this->partition_,
          this->payload_offset_(RadarPayloadTarget::IMAGE, header),
          round_up_sector(header.image_size)) != ESP_OK)
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
    upload_session_id(&header) = 0;
    header.upload_slot = 0;
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
  if (!this->ensure_partition() ||
      !platform_storage_layout_valid(this->partition_->size))
    return 0;
  return IMAGE_SLOT_MAX_SIZE;
}

RadarStorageHeader RadarStorage::default_header_() const {
  RadarStorageHeader header{};
  header.magic = STORAGE_MAGIC;
  header.version = STORAGE_VERSION;
  header.header_size = sizeof(RadarStorageHeader);
  return header;
}

bool RadarStorage::read_header_(RadarStorageHeader *header) {
  if (header == nullptr || !this->ensure_partition())
    return false;

  RadarStorageHeaderV1 prefix{};
  if (esp_partition_read(this->partition_, HEADER_OFFSET, &prefix,
                         sizeof(prefix)) != ESP_OK)
    return false;
  if (prefix.magic == STORAGE_MAGIC && prefix.version == 1 &&
      prefix.header_size == sizeof(RadarStorageHeaderV1))
    return this->migrate_v1_header_(header);
  if (esp_partition_read(this->partition_, HEADER_OFFSET, header,
                         sizeof(RadarStorageHeader)) != ESP_OK)
    return false;

  if (header->magic != STORAGE_MAGIC || header->version != STORAGE_VERSION ||
      header->header_size != sizeof(RadarStorageHeader) ||
      header->active_config_slot > 1 || header->active_image_slot > 1 ||
      header->upload_slot > 1) {
    *header = this->default_header_();
    return false;
  }

  if (header->config_size > CONFIG_MAX_SIZE || header->image_size > this->payload_max_size(RadarPayloadTarget::IMAGE) ||
      header->reserved[0] > DEVICE_CONFIG_MAX_SIZE ||
      header->reserved[1] > LEGACY_STATS_MAX_SIZE)
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

bool RadarStorage::migrate_v1_header_(RadarStorageHeader *header) {
  if (header == nullptr || !this->ensure_partition() ||
      !platform_storage_layout_valid(this->partition_->size))
    return false;

  RadarStorageHeaderV1 legacy{};
  if (esp_partition_read(this->partition_, HEADER_OFFSET, &legacy,
                         sizeof(legacy)) != ESP_OK ||
      legacy.magic != STORAGE_MAGIC || legacy.version != 1 ||
      legacy.header_size != sizeof(RadarStorageHeaderV1) ||
      legacy.config_size > CONFIG_MAX_SIZE ||
      legacy.image_size > IMAGE_SLOT_MAX_SIZE ||
      legacy.reserved[0] > DEVICE_CONFIG_MAX_SIZE ||
      legacy.reserved[1] > LEGACY_STATS_MAX_SIZE)
    return false;

  if (legacy.image_size > 0) {
    std::unique_ptr<uint8_t[]> buffer(
        new (std::nothrow) uint8_t[HASH_STREAM_CHUNK_SIZE]);
    if (!buffer ||
        esp_partition_erase_range(this->partition_, IMAGE_SLOT_1_OFFSET,
                                  round_up_sector(legacy.image_size)) != ESP_OK)
      return false;
    uint32_t copied = 0;
    while (copied < legacy.image_size) {
      const uint32_t chunk = std::min<uint32_t>(
          HASH_STREAM_CHUNK_SIZE, legacy.image_size - copied);
      if (esp_partition_read(this->partition_, LEGACY_IMAGE_OFFSET + copied,
                             buffer.get(), chunk) != ESP_OK ||
          esp_partition_write(this->partition_, IMAGE_SLOT_1_OFFSET + copied,
                              buffer.get(), chunk) != ESP_OK)
        return false;
      copied += chunk;
    }
  }

  RadarStorageHeader migrated = this->default_header_();
  migrated.config_size = legacy.config_size;
  migrated.image_size = legacy.image_size;
  migrated.config_hash = legacy.config_hash;
  migrated.image_hash = legacy.image_hash;
  migrated.reserved[0] = legacy.reserved[0];
  migrated.reserved[1] = legacy.reserved[1];
  migrated.reserved[2] = legacy.reserved[2];
  migrated.reserved[3] = legacy.reserved[3];
  migrated.active_image_slot = legacy.image_size > 0 ? 1 : 0;
  if (!this->write_header_(migrated))
    return false;
  *header = migrated;
  ESP_LOGI(STORAGE_TAG,
           "Migrated floorplan storage to transactional payload slots");
  return true;
}

bool RadarStorage::read_blob_(uint32_t offset, uint32_t size, std::string *out) {
  if (out == nullptr || !this->ensure_partition() || offset > this->partition_->size ||
      size > this->partition_->size - offset)
    return false;
  out->resize(size);
  return esp_partition_read(this->partition_, offset, out->data(), size) == ESP_OK;
}

bool RadarStorage::hash_blob_(uint32_t offset, uint32_t size, uint32_t *hash) {
  if (hash == nullptr || !this->ensure_partition() || offset > this->partition_->size ||
      size > this->partition_->size - offset)
    return false;

  uint32_t value = 2166136261u;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[HASH_STREAM_CHUNK_SIZE]);
  if (!buffer) {
    ESP_LOGE(STORAGE_TAG, "hash buffer allocation failed size=%u", static_cast<unsigned>(HASH_STREAM_CHUNK_SIZE));
    return false;
  }
  uint32_t read = 0;
  while (read < size) {
    const uint32_t chunk = std::min<uint32_t>(HASH_STREAM_CHUNK_SIZE, size - read);
    if (esp_partition_read(this->partition_, offset + read, buffer.get(), chunk) != ESP_OK)
      return false;
    for (uint32_t i = 0; i < chunk; i++) {
      value ^= buffer[i];
      value *= 16777619u;
    }
    read += chunk;
  }
  *hash = value;
  return true;
}

bool RadarStorage::write_blob_(RadarPayloadTarget target, const uint8_t *data, uint32_t size,
                                   RadarStorageHeader *header) {
  if (size == 0 || size > this->payload_max_size(target))
    return false;

  if (target == RadarPayloadTarget::CONFIG ||
      target == RadarPayloadTarget::IMAGE) {
    header->upload_slot = target == RadarPayloadTarget::CONFIG
                              ? (header->active_config_slot == 0 ? 1 : 0)
                              : (header->active_image_slot == 0 ? 1 : 0);
  } else {
    header->upload_slot = 0;
  }
  const uint32_t offset = this->upload_offset_(target, *header);
  if (!this->ensure_partition())
    return false;
  if (esp_partition_erase_range(this->partition_, offset, round_up_sector(size)) != ESP_OK)
    return false;
  if (esp_partition_write(this->partition_, offset, data, size) != ESP_OK)
    return false;

  this->set_payload_metadata_(target, size, fnv1a_hash(data, size), header);
  if (target == RadarPayloadTarget::CONFIG)
    header->active_config_slot = header->upload_slot;
  else if (target == RadarPayloadTarget::IMAGE)
    header->active_image_slot = header->upload_slot;
  header->upload_target = 0;
  header->upload_size = 0;
  header->upload_written = 0;
  upload_session_id(header) = 0;
  last_upload_session_id(header) = 0;
  last_upload_target(header) = 0;
  header->upload_slot = 0;
  return this->write_header_(*header);
}

bool RadarStorage::write_chunk_(RadarPayloadTarget target, uint32_t offset, const uint8_t *data, uint32_t size,
                                    RadarStorageHeader *header) {
  if (!this->ensure_partition() || size == 0) {
    ESP_LOGE(STORAGE_TAG, "upload chunk invalid partition/size target=%u offset=%u size=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(offset), static_cast<unsigned>(size));
    this->last_upload_result_ = size == 0 ? RadarUploadResult::INVALID_REQUEST : RadarUploadResult::STORAGE_FAILED;
    return false;
  }
  const uint32_t max_size = this->payload_max_size(target);
  if (offset + size > header->upload_size || header->upload_size > max_size) {
    ESP_LOGE(STORAGE_TAG,
             "upload chunk bounds failed target=%u offset=%u size=%u end=%u upload_size=%u max_size=%u "
             "upload_written=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(offset), static_cast<unsigned>(size),
             static_cast<unsigned>(offset + size), static_cast<unsigned>(header->upload_size),
             static_cast<unsigned>(max_size), static_cast<unsigned>(header->upload_written));
    this->last_upload_result_ =
        header->upload_size > max_size ? RadarUploadResult::PAYLOAD_TOO_LARGE : RadarUploadResult::OFFSET_MISMATCH;
    return false;
  }
  const uint32_t write_offset =
      this->upload_offset_(target, *header) + offset;
  const esp_err_t write_err = esp_partition_write(this->partition_, write_offset, data, size);
  if (write_err != ESP_OK) {
    ESP_LOGE(STORAGE_TAG, "upload chunk partition_write failed target=%u write_offset=%u offset=%u size=%u err=%d",
             static_cast<unsigned>(target), static_cast<unsigned>(write_offset), static_cast<unsigned>(offset),
             static_cast<unsigned>(size), static_cast<int>(write_err));
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
    return false;
  }

  header->upload_written = std::max(header->upload_written, offset + size);
  const bool header_ok = this->write_header_(*header);
  if (!header_ok) {
    ESP_LOGE(STORAGE_TAG, "upload chunk write_header failed target=%u upload_size=%u upload_written=%u",
             static_cast<unsigned>(target), static_cast<unsigned>(header->upload_size),
             static_cast<unsigned>(header->upload_written));
    this->last_upload_result_ = RadarUploadResult::STORAGE_FAILED;
  }
  return header_ok;
}

uint32_t RadarStorage::payload_offset_(
    RadarPayloadTarget target, const RadarStorageHeader &header) const {
  if (target == RadarPayloadTarget::CONFIG)
    return header.active_config_slot == 0 ? CONFIG_SLOT_0_OFFSET
                                          : CONFIG_SLOT_1_OFFSET;
  if (target == RadarPayloadTarget::DEVICE_CONFIG)
    return DEVICE_CONFIG_OFFSET;
  if (target == RadarPayloadTarget::STATS)
    return STATS_OFFSET;
  return header.active_image_slot == 0 ? IMAGE_SLOT_0_OFFSET
                                        : IMAGE_SLOT_1_OFFSET;
}

uint32_t RadarStorage::upload_offset_(
    RadarPayloadTarget target, const RadarStorageHeader &header) const {
  if (target == RadarPayloadTarget::CONFIG)
    return header.upload_slot == 0 ? CONFIG_SLOT_0_OFFSET
                                   : CONFIG_SLOT_1_OFFSET;
  if (target == RadarPayloadTarget::IMAGE)
    return header.upload_slot == 0 ? IMAGE_SLOT_0_OFFSET
                                   : IMAGE_SLOT_1_OFFSET;
  return this->payload_offset_(target, header);
}

uint32_t RadarStorage::platform_blob_offset_(RadarPlatformBlob target) const {
  // Keep every previously shipped platform sector at its original absolute
  // offset. New transactional stores grow toward lower addresses.
  return platform_blob_offset_for_partition(this->partition_->size, target);
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
