#pragma once
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <thread>

#define PROGMEM
#define ICACHE_RODATA_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_NOINIT_ATTR
#define PGM_P const char *
#define PSTR(s) (s)

inline unsigned long millis() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

inline unsigned long micros() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration_cast<microseconds>(steady_clock::now() - start).count();
}

inline void delay(unsigned long ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
inline void yield() { std::this_thread::yield(); }

#include "HardwareSerial.h"
#include "Print.h"
#include "WString.h"
#include "esp_heap_caps.h"

struct ESPMock {
  uint32_t getFreeHeap() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
  void restart();
  uint32_t getHeapSize() {
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
  }
  uint32_t getMinFreeHeap() {
    std::lock_guard<std::mutex> lock(simulator_heap::mutex);
    simulator_heap::initialize();
    return static_cast<uint32_t>(simulator_heap::internal.minimumFree);
  }
  uint32_t getMaxAllocHeap() {
    return static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  uint32_t getFreePsram() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  uint32_t getPsramSize() {
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  }
  uint32_t getMinFreePsram() {
    std::lock_guard<std::mutex> lock(simulator_heap::mutex);
    simulator_heap::initialize();
    return static_cast<uint32_t>(simulator_heap::psram.minimumFree);
  }
  uint32_t getMaxAllocPsram() {
    return static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  }
};
extern ESPMock ESP;

inline long random(long max) { return std::rand() % max; }

template <typename A, typename B>
constexpr auto max(A a, B b) -> decltype(a > b ? a : b) {
  return a > b ? a : b;
}
template <typename A, typename B>
constexpr auto min(A a, B b) -> decltype(a < b ? a : b) {
  return a < b ? a : b;
}
