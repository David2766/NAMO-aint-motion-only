#pragma once

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

namespace esphome {
namespace radar_api_server {

inline bool parse_uint32_strict(const std::string &text, uint32_t *value,
                                bool allow_hex = false,
                                bool allow_zero = true) {
  if (value == nullptr || text.empty() || text.front() == '-' ||
      text.front() == '+')
    return false;
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)))
      return false;
  }

  int base = 10;
  if (allow_hex && text.size() > 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X'))
    base = 16;

  errno = 0;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, base);
  if (errno == ERANGE || end == text.c_str() ||
      end != text.c_str() + text.size() ||
      parsed > std::numeric_limits<uint32_t>::max() ||
      (!allow_zero && parsed == 0))
    return false;

  *value = static_cast<uint32_t>(parsed);
  return true;
}

}  // namespace radar_api_server
}  // namespace esphome
