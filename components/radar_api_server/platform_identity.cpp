#include "platform_identity.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t STORAGE_MAGIC[] = {'N', 'A', 'M', 'O'};
constexpr uint8_t NODE_STORAGE_MAGIC[] = {'N', 'A', 'M', 'N'};
constexpr std::size_t HEADER_SIZE = 10;
constexpr std::size_t NODE_HEADER_SIZE = 8;
constexpr std::size_t CHECKSUM_SIZE = 4;

uint16_t read_u16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t read_u32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) |
         (static_cast<uint32_t>(data[3]) << 24U);
}

void write_u16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFFU);
  data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void write_u32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFFU);
  data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  data[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  data[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

bool valid_identifier(const char *value, std::size_t *length_out = nullptr) {
  if (value == nullptr || value[0] == '\0')
    return false;

  std::size_t length = 0;
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    if (++length > PlatformIdentity::MAX_ID_LENGTH)
      return false;
    const char character = *cursor;
    const bool alpha_numeric = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z') ||
                               (character >= '0' && character <= '9');
    if (!alpha_numeric &&
        (cursor == value || (character != '.' && character != '_' &&
                             character != ':' && character != '-')))
      return false;
  }
  if (length_out != nullptr)
    *length_out = length;
  return true;
}

bool all_bytes_are(const uint8_t *data, std::size_t size, uint8_t expected) {
  for (std::size_t index = 0; index < size; ++index) {
    if (data[index] != expected)
      return false;
  }
  return true;
}

}  // namespace

PlatformIdentityCodecResult create_initial_platform_identity(
    const uint8_t mac[6], const uint8_t site_random[16], PlatformIdentity *identity) {
  if (mac == nullptr || site_random == nullptr || identity == nullptr)
    return PlatformIdentityCodecResult::INVALID_ARGUMENT;

  const bool mac_all_zero = all_bytes_are(mac, 6, 0x00);
  const bool mac_all_ff = all_bytes_are(mac, 6, 0xFF);
  const bool random_all_zero = all_bytes_are(site_random, 16, 0x00);
  const bool random_all_ff = all_bytes_are(site_random, 16, 0xFF);
  if (mac_all_zero || mac_all_ff || (mac[0] & 0x01U) != 0 ||
      random_all_zero || random_all_ff)
    return PlatformIdentityCodecResult::INVALID_SEED;

  PlatformIdentity generated{};
  std::snprintf(generated.node_id, sizeof(generated.node_id),
                "namo-node-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);

  static constexpr char HEX[] = "0123456789abcdef";
  std::memcpy(generated.site_id, "site-", 5);
  for (std::size_t index = 0; index < 16; ++index) {
    generated.site_id[5 + (index * 2)] = HEX[(site_random[index] >> 4U) & 0x0FU];
    generated.site_id[6 + (index * 2)] = HEX[site_random[index] & 0x0FU];
  }
  generated.site_id[37] = '\0';

  *identity = generated;
  return PlatformIdentityCodecResult::OK;
}

PlatformIdentityCodecResult PlatformIdentityCodec::encode(
    const PlatformIdentity &identity, uint8_t *output,
    std::size_t output_capacity, std::size_t *output_size) {
  if (output == nullptr || output_size == nullptr)
    return PlatformIdentityCodecResult::INVALID_ARGUMENT;

  std::size_t site_length = 0;
  std::size_t node_length = 0;
  if (!valid_identifier(identity.site_id, &site_length) ||
      !valid_identifier(identity.node_id, &node_length))
    return PlatformIdentityCodecResult::INVALID_ID;

  const std::size_t payload_size = 2 + site_length + node_length;
  const std::size_t encoded_size = HEADER_SIZE + site_length + node_length + CHECKSUM_SIZE;
  if (output_capacity < encoded_size)
    return PlatformIdentityCodecResult::INVALID_ARGUMENT;

  std::memset(output, 0xFF, output_capacity);
  std::memcpy(output, STORAGE_MAGIC, sizeof(STORAGE_MAGIC));
  write_u16(output + 4, STORAGE_VERSION);
  write_u16(output + 6, static_cast<uint16_t>(payload_size));
  output[8] = static_cast<uint8_t>(site_length);
  output[9] = static_cast<uint8_t>(node_length);
  std::memcpy(output + HEADER_SIZE, identity.site_id, site_length);
  std::memcpy(output + HEADER_SIZE + site_length, identity.node_id, node_length);
  write_u32(output + encoded_size - CHECKSUM_SIZE,
            fnv1a(output, encoded_size - CHECKSUM_SIZE));
  *output_size = encoded_size;
  return PlatformIdentityCodecResult::OK;
}

PlatformIdentityCodecResult PlatformIdentityCodec::decode(
    const uint8_t *data, std::size_t size, PlatformIdentity *identity) {
  if (data == nullptr || identity == nullptr)
    return PlatformIdentityCodecResult::INVALID_ARGUMENT;
  if (size == 0 || all_bytes_are(data, size, 0xFF))
    return PlatformIdentityCodecResult::EMPTY;
  if (size < HEADER_SIZE + CHECKSUM_SIZE ||
      std::memcmp(data, STORAGE_MAGIC, sizeof(STORAGE_MAGIC)) != 0)
    return PlatformIdentityCodecResult::CORRUPT;
  if (read_u16(data + 4) != STORAGE_VERSION)
    return PlatformIdentityCodecResult::UNSUPPORTED_VERSION;

  const std::size_t payload_size = read_u16(data + 6);
  const std::size_t site_length = data[8];
  const std::size_t node_length = data[9];
  const std::size_t encoded_size = HEADER_SIZE + site_length + node_length + CHECKSUM_SIZE;
  if (site_length == 0 || node_length == 0 ||
      site_length > PlatformIdentity::MAX_ID_LENGTH ||
      node_length > PlatformIdentity::MAX_ID_LENGTH ||
      payload_size != 2 + site_length + node_length || encoded_size > size)
    return PlatformIdentityCodecResult::CORRUPT;
  if (read_u32(data + encoded_size - CHECKSUM_SIZE) !=
      fnv1a(data, encoded_size - CHECKSUM_SIZE))
    return PlatformIdentityCodecResult::CORRUPT;
  if (size > encoded_size &&
      !all_bytes_are(data + encoded_size, size - encoded_size, 0xFF))
    return PlatformIdentityCodecResult::CORRUPT;

  PlatformIdentity decoded{};
  std::memcpy(decoded.site_id, data + HEADER_SIZE, site_length);
  std::memcpy(decoded.node_id, data + HEADER_SIZE + site_length, node_length);
  if (!valid_identifier(decoded.site_id) || !valid_identifier(decoded.node_id))
    return PlatformIdentityCodecResult::CORRUPT;

  *identity = decoded;
  return PlatformIdentityCodecResult::OK;
}

PlatformIdentityCodecResult NodeIdentityCodec::encode(
    const NodeIdentity &identity, uint8_t *output, std::size_t output_capacity,
    std::size_t *output_size) {
  if (output == nullptr || output_size == nullptr)
    return PlatformIdentityCodecResult::INVALID_ARGUMENT;

  std::size_t node_length = 0;
  if (!valid_identifier(identity.node_id, &node_length))
    return PlatformIdentityCodecResult::INVALID_ID;

  const std::size_t encoded_size = NODE_HEADER_SIZE + node_length + CHECKSUM_SIZE;
  if (output_capacity < encoded_size)
    return PlatformIdentityCodecResult::INVALID_ARGUMENT;

  std::memset(output, 0xFF, output_capacity);
  std::memcpy(output, NODE_STORAGE_MAGIC, sizeof(NODE_STORAGE_MAGIC));
  write_u16(output + 4, STORAGE_VERSION);
  output[6] = static_cast<uint8_t>(node_length);
  output[7] = 0;
  std::memcpy(output + NODE_HEADER_SIZE, identity.node_id, node_length);
  write_u32(output + encoded_size - CHECKSUM_SIZE,
            fnv1a(output, encoded_size - CHECKSUM_SIZE));
  *output_size = encoded_size;
  return PlatformIdentityCodecResult::OK;
}

PlatformIdentityCodecResult NodeIdentityCodec::decode(
    const uint8_t *data, std::size_t size, NodeIdentity *identity) {
  if (data == nullptr || identity == nullptr)
    return PlatformIdentityCodecResult::INVALID_ARGUMENT;
  if (size == 0 || all_bytes_are(data, size, 0xFF))
    return PlatformIdentityCodecResult::EMPTY;
  if (size < NODE_HEADER_SIZE + CHECKSUM_SIZE ||
      std::memcmp(data, NODE_STORAGE_MAGIC, sizeof(NODE_STORAGE_MAGIC)) != 0)
    return PlatformIdentityCodecResult::CORRUPT;
  if (read_u16(data + 4) != STORAGE_VERSION)
    return PlatformIdentityCodecResult::UNSUPPORTED_VERSION;

  const std::size_t node_length = data[6];
  const std::size_t encoded_size = NODE_HEADER_SIZE + node_length + CHECKSUM_SIZE;
  if (data[7] != 0 || node_length == 0 || node_length > NodeIdentity::MAX_ID_LENGTH ||
      encoded_size > size)
    return PlatformIdentityCodecResult::CORRUPT;
  if (read_u32(data + encoded_size - CHECKSUM_SIZE) !=
      fnv1a(data, encoded_size - CHECKSUM_SIZE))
    return PlatformIdentityCodecResult::CORRUPT;
  if (size > encoded_size &&
      !all_bytes_are(data + encoded_size, size - encoded_size, 0xFF))
    return PlatformIdentityCodecResult::CORRUPT;

  NodeIdentity decoded{};
  std::memcpy(decoded.node_id, data + NODE_HEADER_SIZE, node_length);
  if (!valid_identifier(decoded.node_id))
    return PlatformIdentityCodecResult::CORRUPT;

  *identity = decoded;
  return PlatformIdentityCodecResult::OK;
}

}  // namespace radar_api_server
}  // namespace esphome
