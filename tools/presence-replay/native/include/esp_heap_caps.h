#pragma once

#include <cstddef>
#include <cstdlib>

static constexpr unsigned MALLOC_CAP_SPIRAM = 0;
static constexpr unsigned MALLOC_CAP_8BIT = 0;

inline void *heap_caps_calloc(std::size_t count, std::size_t size, unsigned) {
  return std::calloc(count, size);
}

inline void heap_caps_free(void *value) {
  std::free(value);
}
