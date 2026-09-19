#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "FreeRTOS.h"

// Thread-local pointer so ulTaskNotifyTake can find the current task's handle.
inline thread_local SimTaskHandle *tl_currentTaskHandle = nullptr;

inline SimTaskHandle *simMainTaskHandle() {
  static SimTaskHandle mainTask;
  static std::once_flag initFlag;
  std::call_once(initFlag, [] {
    mainTask.name = "main";
    mainTask.id = std::this_thread::get_id();
  });
  return &mainTask;
}

inline TaskHandle_t xTaskGetCurrentTaskHandle() {
  return tl_currentTaskHandle ? tl_currentTaskHandle : simMainTaskHandle();
}

// Create a real OS thread. The FreeRTOS task function signature is
// void(*)(void*).
inline void simulatorReportStackBudget(const char *name,
                                       const uint32_t stackDepth) {
  // Host frames and target frames have different ABIs. This records the real
  // requested FreeRTOS budget and can flag a declared logical-budget breach,
  // but never pretends to measure ESP32 high-water on macOS/Linux.
  const char *budgets = std::getenv("CROSSINK_SIMULATOR_STACK_BUDGETS");
  uint32_t budget = 0;
  if (budgets) {
    const char *entry = budgets;
    const size_t nameLength = std::strlen(name);
    while (*entry) {
      if (std::strncmp(entry, name, nameLength) == 0 &&
          entry[nameLength] == '=') {
        budget = static_cast<uint32_t>(
            std::strtoul(entry + nameLength + 1, nullptr, 10));
        break;
      }
      entry = std::strchr(entry, ',');
      if (!entry)
        break;
      ++entry;
    }
  }
  std::fprintf(stderr,
               "SIMSTACK task=%s requested=%u host-depth=unavailable%s%s\n",
               name, stackDepth, budget ? " budget=" : "",
               budget ? std::to_string(budget).c_str() : "");
  std::fflush(stderr);
  if (budget && stackDepth > budget)
    std::fprintf(stderr,
                 "SIMSTACK BUDGET_BREACH task=%s requested=%u budget=%u\n",
                 name, stackDepth, budget);
}

inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                              uint32_t stackDepth, void *param,
                              BaseType_t /*priority*/, TaskHandle_t *handle) {
  auto *h = new SimTaskHandle();
  h->name = name ? name : "sim-task";
  h->requestedStackBytes = stackDepth;
  simulatorReportStackBudget(h->name, stackDepth);
  h->thread = std::thread([fn, param, h]() {
    tl_currentTaskHandle = h;
    h->id = std::this_thread::get_id();
    fn(param);
  });
  if (handle)
    *handle = h;
  return 1; // pdPASS
}

// The host simulator has no FreeRTOS stack allocator. Keep the static-task
// API compatible while delegating thread creation to the normal simulator
// task path; the provided storage is only meaningful on firmware.
inline TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name,
                                      uint32_t stackDepth, void *param,
                                      BaseType_t priority,
                                      StackType_t * /*stack*/,
                                      StaticTask_t * /*taskStorage*/) {
  TaskHandle_t handle = nullptr;
  return xTaskCreate(fn, name, stackDepth, param, priority, &handle) == pdPASS
             ? handle
             : nullptr;
}

// Core pinning has no meaning on the host; delegate to xTaskCreate and
// ignore the core ID.
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                          uint32_t stackDepth, void *param,
                                          BaseType_t priority,
                                          TaskHandle_t *handle,
                                          BaseType_t /*coreId*/) {
  return xTaskCreate(fn, name, stackDepth, param, priority, handle);
}

// Block until notified (simulates ulTaskNotifyTake with clear-on-exit).
inline uint32_t ulTaskNotifyTake(int /*clearOnExit*/,
                                 uint32_t /*ticksToWait*/) {
  auto *h = xTaskGetCurrentTaskHandle();
  std::unique_lock<std::mutex> lk(h->mtx);
  h->cv.wait(lk, [h] { return h->notifyCount > 0; });
  h->notifyCount--;
  return 1;
}

// Wake a task by incrementing its notification counter and signalling its
// condvar.
inline void xTaskNotify(TaskHandle_t handle, uint32_t /*value*/,
                        int /*action*/) {
  if (!handle)
    return;
  {
    std::lock_guard<std::mutex> lk(handle->mtx);
    handle->notifyCount++;
  }
  handle->cv.notify_one();
}

inline const char *pcTaskGetName(TaskHandle_t h) {
  if (!h)
    h = xTaskGetCurrentTaskHandle();
  return h ? h->name : "main";
}
inline void vTaskDelete(TaskHandle_t h) {
  if (h) {
    if (h->thread.joinable())
      h->thread.detach();
    delete h;
  }
}
inline unsigned int uxTaskGetStackHighWaterMark(TaskHandle_t task) {
  // This is a compatibility value only; no host-thread depth is reported as a
  // device stack measurement. Target -fstack-usage and hardware telemetry are
  // the authoritative stack checks.
  if (!task)
    task = xTaskGetCurrentTaskHandle();
  return task ? task->requestedStackBytes / sizeof(StackType_t) : 0;
}
inline void vTaskList(char *) {}
inline void vTaskDelay(int) {}
