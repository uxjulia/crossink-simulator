#pragma once

// Logical device heap accounting for firmware-owned capability allocations.
// This deliberately leaves host malloc/new alone: SDL, libc, and std::thread
// allocations are host implementation details, not ESP32 RAM.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

constexpr uint32_t MALLOC_CAP_DEFAULT = 1;
constexpr uint32_t MALLOC_CAP_INTERNAL = 2;
constexpr uint32_t MALLOC_CAP_SPIRAM = 4;
constexpr uint32_t MALLOC_CAP_8BIT = 8;

namespace simulator_heap {
struct Range {
  size_t start;
  size_t size;
};
struct Allocation {
  bool psram;
  size_t start;
  size_t size;
};
struct Pool {
  size_t total = 0;
  size_t free = 0;
  size_t minimumFree = 0;
  size_t largest = 0;
  size_t allocatedBlocks = 0;
  bool failNext = false;
  std::vector<Range> freeRanges;
};

inline std::mutex mutex;
inline std::map<void *, Allocation> live;
inline std::once_flag initializeOnce;
inline size_t defaultPsramThreshold = 0;
inline Pool internal;
inline Pool psram;

inline size_t environmentSize(const std::string &name, const size_t fallback) {
  const char *value = std::getenv(name.c_str());
  if (!value || !*value)
    return fallback;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  return end && *end == '\0' ? static_cast<size_t>(parsed) : fallback;
}

inline bool environmentIs(const std::string &name, const char *expected) {
  const char *value = std::getenv(name.c_str());
  return value && std::strcmp(value, expected) == 0;
}

inline void initializePool(Pool &pool, const char *prefix,
                           const size_t defaultTotal) {
  const bool disabled =
      environmentIs("CROSSINK_SIMULATOR_RESOURCE_MODE", "off");
  const std::string name(prefix);
  const size_t total = environmentSize(name + "_TOTAL", defaultTotal);
  const size_t free =
      disabled ? defaultTotal : environmentSize(name + "_FREE", total);
  const size_t largest =
      disabled ? defaultTotal : environmentSize(name + "_LARGEST", free);
  pool.total = total;
  pool.free = std::min(free, total);
  pool.minimumFree = pool.free;
  pool.largest = std::min(largest, pool.free);
  pool.failNext = environmentIs(
      std::string("CROSSINK_SIMULATOR_FAIL_NEXT_") +
          (std::strcmp(prefix, "CROSSINK_SIMULATOR_PSRAM") == 0 ? "PSRAM"
                                                                : "INTERNAL"),
      "1");
  size_t offset = 0;
  size_t remaining = pool.free;
  const size_t chunk = pool.largest == 0 ? pool.free : pool.largest;
  while (remaining) {
    const size_t size = std::min(chunk, remaining);
    pool.freeRanges.push_back({offset, size});
    offset += size + 1; // A gap models unavailable/fragmented backing storage.
    remaining -= size;
  }
}

inline void initializeOnceBody() {
  const bool resourceEnabled =
      !environmentIs("CROSSINK_SIMULATOR_RESOURCE_MODE", "off");
  initializePool(internal, "CROSSINK_SIMULATOR_INTERNAL", 1024 * 1024);
  initializePool(psram, "CROSSINK_SIMULATOR_PSRAM", 0);
  defaultPsramThreshold =
      environmentSize("CROSSINK_SIMULATOR_DEFAULT_PSRAM_THRESHOLD", 0);
  if (resourceEnabled) {
    const char *mode = std::getenv("CROSSINK_SIMULATOR_RESOURCE_MODE");
    const char *calibrated =
        std::getenv("CROSSINK_SIMULATOR_RESOURCE_CALIBRATED");
    std::fprintf(stderr,
                 "SIMRESOURCE mode=%s calibrated=%s internal=%zu/%zu "
                 "largest=%zu psram=%zu/%zu largest=%zu\n",
                 mode ? mode : "off", calibrated ? calibrated : "0",
                 internal.free, internal.total, internal.largest, psram.free,
                 psram.total, psram.largest);
    std::fflush(stderr);
  }
}

inline void initialize() { std::call_once(initializeOnce, initializeOnceBody); }

inline bool usesDefaultPool(const uint32_t caps) {
  return (caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_SPIRAM)) == 0 &&
         (caps & MALLOC_CAP_DEFAULT) != 0;
}

inline bool psramAvailable() {
  initialize();
  return psram.total != 0;
}

inline Pool &capabilityPoolFor(const uint32_t caps) {
  initialize();
  // ESP-IDF capability-default allocations prefer PSRAM whenever present.
  // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is a libc malloc policy, not a
  // heap_caps_malloc(MALLOC_CAP_DEFAULT) policy.
  return ((caps & MALLOC_CAP_SPIRAM) ||
          (usesDefaultPool(caps) && psramAvailable()))
             ? psram
             : internal;
}

inline Pool &defaultMallocPoolFor(const size_t bytes) {
  initialize();
  return psramAvailable() && bytes > defaultPsramThreshold ? psram : internal;
}

inline void reportFinal() {
  initialize();
  std::lock_guard<std::mutex> lock(mutex);
  std::fprintf(
      stderr,
      "SIMRESOURCE final internal_current=%zu internal_minimum=%zu "
      "internal_largest=%zu internal_live=%zu "
      "psram_current=%zu psram_minimum=%zu psram_largest=%zu psram_live=%zu\n",
      internal.free, internal.minimumFree, internal.largest,
      internal.allocatedBlocks, psram.free, psram.minimumFree, psram.largest,
      psram.allocatedBlocks);
  std::fflush(stderr);
}

inline void recomputeLargest(Pool &pool) {
  pool.largest = 0;
  for (const auto &range : pool.freeRanges)
    pool.largest = std::max(pool.largest, range.size);
}

inline void freeRange(Pool &pool, Range freed) {
  auto position = pool.freeRanges.begin();
  while (position != pool.freeRanges.end() && position->start < freed.start)
    ++position;
  if (position != pool.freeRanges.begin()) {
    auto previous = position - 1;
    if (previous->start + previous->size == freed.start) {
      freed.start = previous->start;
      freed.size += previous->size;
      position = pool.freeRanges.erase(previous);
    }
  }
  if (position != pool.freeRanges.end() &&
      freed.start + freed.size == position->start) {
    freed.size += position->size;
    position = pool.freeRanges.erase(position);
  }
  pool.freeRanges.insert(position, freed);
  recomputeLargest(pool);
}

inline void *allocateFromPool(Pool &pool, const bool poolIsPsram,
                              const size_t bytes, const size_t alignment) {
  if (pool.failNext) {
    pool.failNext = false;
    return nullptr;
  }
  for (auto it = pool.freeRanges.begin(); it != pool.freeRanges.end(); ++it) {
    const size_t alignedStart = (it->start + alignment - 1) & ~(alignment - 1);
    if (alignedStart < it->start || alignedStart - it->start > it->size ||
        bytes > it->size - (alignedStart - it->start))
      continue;
    void *pointer = nullptr;
    if (alignment <= alignof(std::max_align_t))
      pointer = std::malloc(bytes);
    else if (posix_memalign(&pointer, alignment, bytes) != 0)
      pointer = nullptr;
    if (!pointer)
      return nullptr;
    const Range original = *it;
    const size_t prefix = alignedStart - original.start;
    const size_t suffixStart = alignedStart + bytes;
    const size_t suffix = original.size - prefix - bytes;
    const size_t index = static_cast<size_t>(it - pool.freeRanges.begin());
    pool.freeRanges.erase(it);
    auto position =
        pool.freeRanges.begin() + static_cast<std::ptrdiff_t>(index);
    if (prefix)
      position = pool.freeRanges.insert(position, {original.start, prefix}) + 1;
    if (suffix)
      pool.freeRanges.insert(position, {suffixStart, suffix});
    pool.free -= bytes;
    pool.minimumFree = std::min(pool.minimumFree, pool.free);
    ++pool.allocatedBlocks;
    live.emplace(pointer, Allocation{poolIsPsram, alignedStart, bytes});
    recomputeLargest(pool);
    return pointer;
  }
  return nullptr;
}

inline void *allocateWithFallback(Pool &preferred, const bool mayFallback,
                                  const size_t bytes, const size_t alignment) {
  if (void *pointer =
          allocateFromPool(preferred, &preferred == &psram, bytes, alignment))
    return pointer;
  if (mayFallback) {
    Pool &alternate = &preferred == &psram ? internal : psram;
    return allocateFromPool(alternate, &alternate == &psram, bytes, alignment);
  }
  return nullptr;
}

inline void *allocate(const size_t bytes, const uint32_t caps,
                      const size_t alignment = alignof(std::max_align_t)) {
  if (bytes == 0)
    return nullptr;
  initialize();
  std::lock_guard<std::mutex> lock(mutex);
  Pool &preferred = capabilityPoolFor(caps);
  // Only MALLOC_CAP_DEFAULT has no physical-pool requirement; explicit
  // INTERNAL/SPIRAM requests remain strict.
  return allocateWithFallback(
      preferred, usesDefaultPool(caps) && psramAvailable(), bytes, alignment);
}

// These simulator-only helpers model libc malloc/posix_memalign, whose
// size-based split is controlled by CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL.
inline void *allocateDefaultMalloc(const size_t bytes, const size_t alignment) {
  if (bytes == 0)
    return nullptr;
  initialize();
  std::lock_guard<std::mutex> lock(mutex);
  Pool &preferred = defaultMallocPoolFor(bytes);
  return allocateWithFallback(preferred, psramAvailable(), bytes, alignment);
}

inline void *defaultMalloc(const size_t bytes) {
  return allocateDefaultMalloc(bytes, alignof(std::max_align_t));
}

inline void *defaultAlignedAlloc(const size_t alignment, const size_t bytes) {
  return allocateDefaultMalloc(bytes, alignment);
}

inline void release(void *pointer) {
  if (!pointer)
    return;
  initialize();
  std::lock_guard<std::mutex> lock(mutex);
  const auto found = live.find(pointer);
  if (found == live.end()) {
    std::free(pointer);
    return;
  }
  Pool &pool = found->second.psram ? psram : internal;
  const size_t bytes = found->second.size;
  freeRange(pool, {found->second.start, bytes});
  pool.free += bytes;
  --pool.allocatedBlocks;
  live.erase(found);
  std::free(pointer);
}
} // namespace simulator_heap

inline size_t heap_caps_get_total_size(const uint32_t caps) {
  simulator_heap::initialize();
  std::lock_guard<std::mutex> lock(simulator_heap::mutex);
  return simulator_heap::capabilityPoolFor(caps).total;
}
inline size_t heap_caps_get_free_size(const uint32_t caps) {
  simulator_heap::initialize();
  std::lock_guard<std::mutex> lock(simulator_heap::mutex);
  return simulator_heap::capabilityPoolFor(caps).free;
}
inline size_t heap_caps_get_largest_free_block(const uint32_t caps) {
  simulator_heap::initialize();
  std::lock_guard<std::mutex> lock(simulator_heap::mutex);
  return simulator_heap::capabilityPoolFor(caps).largest;
}
inline void *heap_caps_malloc(const size_t bytes, const uint32_t caps) {
  return simulator_heap::allocate(bytes, caps);
}
inline void *heap_caps_aligned_alloc(const size_t alignment, const size_t bytes,
                                     const uint32_t caps) {
  return simulator_heap::allocate(bytes, caps, alignment);
}
inline void heap_caps_free(void *pointer) { simulator_heap::release(pointer); }

struct multi_heap_info_t {
  size_t free_blocks = 0;
  size_t allocated_blocks = 0;
};
inline void heap_caps_get_info(multi_heap_info_t *info, const uint32_t caps) {
  simulator_heap::initialize();
  std::lock_guard<std::mutex> lock(simulator_heap::mutex);
  const auto &pool = simulator_heap::capabilityPoolFor(caps);
  *info = {pool.freeRanges.size(), pool.allocatedBlocks};
}
