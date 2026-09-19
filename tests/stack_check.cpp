#include "freertos/task.h"
#include <atomic>
#include <cassert>
#include <cstring>

__attribute__((noinline)) void oversized() {
  volatile char scratch[20 * 1024];
  asm volatile("" : : "r"(scratch) : "memory");
  scratch[0] = 1;
  scratch[sizeof(scratch) - 1] = 2;
}
__attribute__((noinline)) void nested(unsigned depth) {
  volatile char scratch[1024];
  asm volatile("" : : "r"(scratch) : "memory");
  scratch[0] = depth;
  if (depth)
    nested(depth - 1);
  scratch[1] = scratch[0];
}
__attribute__((noinline)) void medium() {
  volatile char scratch[10 * 1024];
  asm volatile("" : : "r"(scratch) : "memory");
  scratch[0] = 1;
}
void task(void *arg) {
  const char *mode = static_cast<const char *>(arg);
  if (std::strcmp(mode, "large") == 0)
    oversized();
  else if (std::strcmp(mode, "medium") == 0)
    medium();
  else if (std::strcmp(mode, "nested") == 0)
    nested(24);
  else
    nested(1);
  const auto free = uxTaskGetStackHighWaterMark(nullptr);
  if (std::strcmp(mode, "small") == 0)
    assert(free > 0 && free < 16384);
}
struct DetachedProbe {
  std::atomic<bool> ready{false};
  std::atomic<bool> proceed{false};
};
void detachedTask(void *arg) {
  auto *probe = static_cast<DetachedProbe *>(arg);
  probe->ready.store(true);
  while (!probe->proceed.load())
    std::this_thread::yield();
  nested(2);
  assert(uxTaskGetStackHighWaterMark(nullptr) > 0);
}
int main(int argc, char **argv) {
  assert(argc == 3 || argc == 4);
  const uint32_t budget = argc == 4 ? 8192 : 16384;
  simStackCheckStartup();
  TaskHandle_t h = nullptr;
  if (std::strcmp(argv[1], "detach") == 0) {
    DetachedProbe probe;
    assert(xTaskCreate(detachedTask, "DetachedProbe", 128 * 1024, &probe, 1,
                       &h) == pdPASS);
    std::weak_ptr<SimStackUsage> lifetime = h->stackUsage;
    while (!probe.ready.load())
      std::this_thread::yield();
    vTaskDelete(h);
    assert(!lifetime.expired());
    probe.proceed.store(true);
    while (!lifetime.expired())
      std::this_thread::yield();
    return 0;
  }
  if (std::strcmp(argv[2], "static") == 0)
    h = xTaskCreateStatic(task, "StackProbe", budget, argv[1], 1, nullptr,
                          nullptr);
  else if (std::strcmp(argv[2], "pinned") == 0)
    assert(xTaskCreatePinnedToCore(task, "StackProbe", budget, argv[1], 1, &h,
                                   1) == pdPASS);
  else
    assert(xTaskCreate(task, "StackProbe", budget, argv[1], 1, &h) == pdPASS);
  assert(h);
  h->thread.join();
  const auto free = uxTaskGetStackHighWaterMark(h);
  if (std::strcmp(argv[1], "small") == 0)
    assert(free > 0 && free < 16384);
  vTaskDelete(h);
}
