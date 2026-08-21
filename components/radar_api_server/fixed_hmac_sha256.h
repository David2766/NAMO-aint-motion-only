#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef USE_ESP32
#include <mbedtls/sha256.h>
#endif

namespace esphome {
namespace radar_api_server {

#ifdef USE_ESP32

class FixedHmacSha256 {
 public:
  static constexpr std::size_t KEY_SIZE = 32;
  static constexpr std::size_t DIGEST_SIZE = 32;
  static constexpr std::size_t BLOCK_SIZE = 64;

  FixedHmacSha256();
  ~FixedHmacSha256();

  FixedHmacSha256(const FixedHmacSha256 &) = delete;
  FixedHmacSha256 &operator=(const FixedHmacSha256 &) = delete;

  bool calculate(const std::array<uint8_t, KEY_SIZE> &key,
                 const uint8_t *data, std::size_t size, uint8_t *output);

 private:
  mbedtls_sha256_context context_{};
  std::array<uint8_t, BLOCK_SIZE> inner_pad_{};
  std::array<uint8_t, BLOCK_SIZE> outer_pad_{};
  std::array<uint8_t, DIGEST_SIZE> inner_digest_{};

  void clear_();
};

static_assert(sizeof(FixedHmacSha256) <= 384,
              "fixed HMAC workspace exceeded its embedded budget");

#endif

}  // namespace radar_api_server
}  // namespace esphome
