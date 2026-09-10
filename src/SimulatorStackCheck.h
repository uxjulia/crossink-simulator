#pragma once
#include <cstddef>
#include <cstdint>

// Host measurements only: ESP-IDF task stack depths and watermarks use bytes.
struct SimStackUsage {
  uint32_t budget = 0;
  uint32_t minimumFree = 0;
};

void simStackCheckStartup();
void simStackBegin(SimStackUsage *usage, const char *name, uintptr_t anchor);
void simStackEnd();
uint32_t simStackMinimumFree(const SimStackUsage *usage);

uint32_t simStackCurrentMinimumFree();
