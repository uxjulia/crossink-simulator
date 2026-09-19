#pragma once
#include "../SimulatorStackCheck.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFF
#define eIncrement 1
#define portTICK_PERIOD_MS 1

using BaseType_t = int;

// ESP-IDF's portMUX_TYPE is a spinlock used with taskENTER_CRITICAL /
// taskEXIT_CRITICAL to guard data shared between tasks (and, on multi-core
// targets, cores). The simulator has no real critical-section primitive, so
// back it with a real mutex to preserve the same mutual-exclusion semantics
// across host threads.
struct SimPortMux {
  std::recursive_mutex mtx;
};
typedef SimPortMux portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED                                           \
  {                                                                            \
  }

inline void taskENTER_CRITICAL(portMUX_TYPE *mux) { mux->mtx.lock(); }
inline void taskEXIT_CRITICAL(portMUX_TYPE *mux) { mux->mtx.unlock(); }
#define portENTER_CRITICAL(mux) taskENTER_CRITICAL(mux)
#define portEXIT_CRITICAL(mux) taskEXIT_CRITICAL(mux)

// TaskHandle wraps a real thread + a notification counter protected by a
// condvar.
struct SimTaskHandle {
  std::thread thread;
  // A detached host task can outlive its public handle. Keep its measurement
  // record alive through the thread capture as well as the handle.
  std::shared_ptr<SimStackUsage> stackUsage = std::make_shared<SimStackUsage>();
  std::mutex mtx;
  std::condition_variable cv;
  uint32_t notifyCount = 0;
  std::thread::id id;
  const char *name = "sim-task";
};
// Static task allocation is an ESP-IDF storage contract. The simulator uses
// std::thread instead, but provides these placeholders so firmware using that
// API builds against the same interface.
typedef uint32_t StackType_t;
struct StaticTask_t {};
typedef SimTaskHandle *TaskHandle_t;
