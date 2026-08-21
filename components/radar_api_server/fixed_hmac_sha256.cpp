#include "fixed_hmac_sha256.h"

#ifdef USE_ESP32

namespace esphome {
namespace radar_api_server {

namespace {

void secure_clear(uint8_t *data, std::size_t size) {
  volatile uint8_t *cursor = data;
  while (size-- != 0)
    *cursor++ = 0;
}

}  // namespace

FixedHmacSha256::FixedHmacSha256() {
  mbedtls_sha256_init(&this->context_);
}

FixedHmacSha256::~FixedHmacSha256() {
  this->clear_();
  mbedtls_sha256_free(&this->context_);
}

bool FixedHmacSha256::calculate(
    const std::array<uint8_t, KEY_SIZE> &key, const uint8_t *data,
    std::size_t size, uint8_t *output) {
  if ((data == nullptr && size != 0) || output == nullptr)
    return false;

  this->inner_pad_.fill(0x36U);
  this->outer_pad_.fill(0x5CU);
  for (std::size_t index = 0; index < key.size(); ++index) {
    this->inner_pad_[index] ^= key[index];
    this->outer_pad_[index] ^= key[index];
  }

  const bool ok =
      mbedtls_sha256_starts(&this->context_, 0) == 0 &&
      mbedtls_sha256_update(&this->context_, this->inner_pad_.data(),
                            this->inner_pad_.size()) == 0 &&
      (size == 0 ||
       mbedtls_sha256_update(&this->context_, data, size) == 0) &&
      mbedtls_sha256_finish(&this->context_, this->inner_digest_.data()) == 0 &&
      mbedtls_sha256_starts(&this->context_, 0) == 0 &&
      mbedtls_sha256_update(&this->context_, this->outer_pad_.data(),
                            this->outer_pad_.size()) == 0 &&
      mbedtls_sha256_update(&this->context_, this->inner_digest_.data(),
                            this->inner_digest_.size()) == 0 &&
      mbedtls_sha256_finish(&this->context_, output) == 0;
  this->clear_();
  return ok;
}

void FixedHmacSha256::clear_() {
  secure_clear(this->inner_pad_.data(), this->inner_pad_.size());
  secure_clear(this->outer_pad_.data(), this->outer_pad_.size());
  secure_clear(this->inner_digest_.data(), this->inner_digest_.size());
}

}  // namespace radar_api_server
}  // namespace esphome

#endif
