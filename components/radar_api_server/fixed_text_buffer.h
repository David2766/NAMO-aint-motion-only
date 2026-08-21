#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace esphome {
namespace radar_api_server {

class FixedTextBuffer {
 public:
  FixedTextBuffer(char *data, std::size_t capacity)
      : data_(data), capacity_(capacity) {
    this->clear();
  }

  void clear() {
    this->size_ = 0;
    this->ok_ = this->data_ != nullptr && this->capacity_ > 0;
    if (this->ok_)
      this->data_[0] = '\0';
  }

  FixedTextBuffer &operator+=(const char *value) {
    if (value != nullptr)
      this->append_(value, std::strlen(value));
    else
      this->ok_ = false;
    return *this;
  }

  FixedTextBuffer &operator+=(const std::string &value) {
    this->append_(value.data(), value.size());
    return *this;
  }

  void push_back(char value) { this->append_(&value, 1); }

  const char *data() const { return this->data_; }
  std::size_t size() const { return this->size_; }
  std::size_t capacity() const { return this->capacity_; }
  bool ok() const { return this->ok_; }

 private:
  void append_(const char *value, std::size_t length) {
    if (!this->ok_ || value == nullptr)
      return;
    if (length > this->capacity_ - this->size_ - 1) {
      this->ok_ = false;
      return;
    }
    std::memcpy(this->data_ + this->size_, value, length);
    this->size_ += length;
    this->data_[this->size_] = '\0';
  }

  char *data_{nullptr};
  std::size_t capacity_{0};
  std::size_t size_{0};
  bool ok_{false};
};

}  // namespace radar_api_server
}  // namespace esphome
