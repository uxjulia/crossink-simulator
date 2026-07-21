#pragma once

#include <cstdarg>
#include <cstdio>

inline int esp_rom_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int written = std::vfprintf(stderr, format, args);
  va_end(args);
  return written;
}
