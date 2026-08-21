#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

struct PlatformIdentity {
  static constexpr std::size_t MAX_ID_LENGTH = 64;

  char site_id[MAX_ID_LENGTH + 1]{};
  char node_id[MAX_ID_LENGTH + 1]{};
};

struct NodeIdentity {
  static constexpr std::size_t MAX_ID_LENGTH = PlatformIdentity::MAX_ID_LENGTH;

  char node_id[MAX_ID_LENGTH + 1]{};
};

enum class PlatformIdentityCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_ID,
  INVALID_SEED,
  EMPTY,
  CORRUPT,
  UNSUPPORTED_VERSION,
};

class PlatformIdentityCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 1;
  static constexpr std::size_t MAX_ENCODED_SIZE = 144;

  static PlatformIdentityCodecResult encode(const PlatformIdentity &identity, uint8_t *output,
                                            std::size_t output_capacity,
                                            std::size_t *output_size);
  static PlatformIdentityCodecResult decode(const uint8_t *data, std::size_t size,
                                            PlatformIdentity *identity);
};

class NodeIdentityCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 1;
  static constexpr std::size_t MAX_ENCODED_SIZE = 80;

  static PlatformIdentityCodecResult encode(const NodeIdentity &identity, uint8_t *output,
                                            std::size_t output_capacity,
                                            std::size_t *output_size);
  static PlatformIdentityCodecResult decode(const uint8_t *data, std::size_t size,
                                            NodeIdentity *identity);
};

PlatformIdentityCodecResult create_initial_platform_identity(
    const uint8_t mac[6], const uint8_t site_random[16], PlatformIdentity *identity);

static_assert(std::is_trivially_copyable<PlatformIdentity>::value,
              "platform identity must not own heap-backed state");
static_assert(std::is_trivially_copyable<NodeIdentity>::value,
              "node identity must not own heap-backed state");
static_assert(sizeof(PlatformIdentity) == 130,
              "platform identity layout changed unexpectedly");
static_assert(PlatformIdentityCodec::MAX_ENCODED_SIZE >=
                  10 + (2 * PlatformIdentity::MAX_ID_LENGTH) + 4,
              "platform identity storage cannot hold maximum length ids");
static_assert(NodeIdentityCodec::MAX_ENCODED_SIZE >=
                  8 + NodeIdentity::MAX_ID_LENGTH + 4,
              "node identity storage cannot hold a maximum length id");

}  // namespace radar_api_server
}  // namespace esphome
