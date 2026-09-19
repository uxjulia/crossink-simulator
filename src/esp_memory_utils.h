#pragma once

#include <esp_heap_caps.h>

inline bool esp_ptr_external_ram(const void *pointer) {
  std::lock_guard<std::mutex> lock(simulator_heap::mutex);
  const auto found = simulator_heap::live.find(const_cast<void *>(pointer));
  return found != simulator_heap::live.end() && found->second.psram;
}
