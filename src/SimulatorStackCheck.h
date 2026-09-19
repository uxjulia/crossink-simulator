#pragma once
#include <cstddef>
#include <cstdint>

// Host measurements only: ESP-IDF task stack depths and watermarks use bytes.
struct SimStackUsage {
  uint32_t budget = 0;
  uint32_t minimumFree = 0;
};

void simStackCheckStartup();
// Device profiles can declare their expected FreeRTOS task size separately
// from the optional host-depth monitor. This checks that declaration whenever
// the simulator creates a task; it never turns a host measurement into an
// ESP32 stack watermark.
void simStackCheckTaskBudget(const char *name, uint32_t requestedBytes);
void simStackBegin(SimStackUsage *usage, const char *name, uintptr_t anchor);
void simStackEnd();
uint32_t simStackMinimumFree(const SimStackUsage *usage);

uint32_t simStackCurrentMinimumFree();
