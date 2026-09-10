#include "SimulatorStackCheck.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

#define NO_INSTRUMENT __attribute__((no_instrument_function))

#ifdef CROSSPOINT_SIM_STACK_CHECK
namespace {
// POD TLS avoids instrumented C++ initialization or library calls in the hook.
thread_local SimStackUsage *currentUsage;
thread_local const char *currentName;
thread_local uintptr_t stackAnchor;
thread_local bool reporting;
thread_local bool warned;
thread_local bool failOnOverflow;
} // namespace
#endif

NO_INSTRUMENT void simStackCheckStartup() {
#ifdef CROSSPOINT_SIM_STACK_CHECK
  const char *mode = std::getenv("CROSSPOINT_SIM_STACK_CHECK");
  std::fprintf(stderr,
               "[SIM STACK] Instrumented host build, mode=%s; "
               "task budgets are bytes. Main/SDL stack is not checked.\n",
               mode ? mode : "fail");
#else
  const char *mode = std::getenv("CROSSPOINT_SIM_STACK_CHECK");
  if (mode && std::strcmp(mode, "off") != 0) {
    std::fprintf(stderr,
                 "[SIM STACK] Requested checks require a rebuild with "
                 "CROSSPOINT_SIM_STACK_CHECK and -finstrument-functions\n");
    _exit(86);
  }
#endif
}

NO_INSTRUMENT void simStackBegin(SimStackUsage *usage, const char *name,
                                 uintptr_t anchor) {
#ifdef CROSSPOINT_SIM_STACK_CHECK
  const char *mode = std::getenv("CROSSPOINT_SIM_STACK_CHECK");
  if (mode && std::strcmp(mode, "off") == 0)
    return;
  if (mode && std::strcmp(mode, "warn") != 0 &&
      std::strcmp(mode, "fail") != 0) {
    std::fprintf(
        stderr, "[SIM STACK] Invalid mode '%s'; use fail, warn or off\n", mode);
    _exit(86);
  }
  failOnOverflow = !mode || std::strcmp(mode, "warn") != 0;
  stackAnchor = anchor;
  currentName = name;
  warned = false;
  __atomic_store_n(&usage->minimumFree, usage->budget, __ATOMIC_RELAXED);
  currentUsage = usage;
#else
  (void)usage;
  (void)name;
  (void)anchor;
#endif
}

NO_INSTRUMENT void simStackEnd() {
#ifdef CROSSPOINT_SIM_STACK_CHECK
  currentUsage = nullptr;
#endif
}

NO_INSTRUMENT uint32_t simStackMinimumFree(const SimStackUsage *usage) {
  return __atomic_load_n(&usage->minimumFree, __ATOMIC_RELAXED);
}

NO_INSTRUMENT uint32_t simStackCurrentMinimumFree() {
#ifdef CROSSPOINT_SIM_STACK_CHECK
  return currentUsage ? simStackMinimumFree(currentUsage) : 0;
#else
  return 0;
#endif
}

#ifdef CROSSPOINT_SIM_STACK_CHECK
extern "C" NO_INSTRUMENT void __cyg_profile_func_enter(void *function,
                                                       void *caller) {
  if (!currentUsage || reporting)
    return;
  // The compiler calls us after the function prologue, including its fixed
  // local array allocation. No polling timer can miss a short-lived frame.
  const uintptr_t here =
      reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  const uintptr_t used =
      here < stackAnchor ? stackAnchor - here : here - stackAnchor;
  const uint32_t budget = currentUsage->budget;
  const uint32_t remaining = used >= budget ? 0 : budget - used;
  if (remaining < __atomic_load_n(&currentUsage->minimumFree, __ATOMIC_RELAXED))
    __atomic_store_n(&currentUsage->minimumFree, remaining, __ATOMIC_RELAXED);
  if (used <= budget || warned)
    return;
  reporting = true;
  warned = true;
  std::fprintf(stderr,
               "[SIM STACK] %s: task=%s host-used=%zu budget=%u bytes "
               "function=%p caller=%p (host estimate, not ESP32 measurement)\n",
               failOnOverflow ? "FAIL" : "WARNING", currentName,
               static_cast<size_t>(used), budget, function, caller);
  Dl_info location{};
  if (dladdr(function, &location) && location.dli_fbase) {
    const uintptr_t offset = reinterpret_cast<uintptr_t>(function) -
                             reinterpret_cast<uintptr_t>(location.dli_fbase);
    std::fprintf(stderr, "[SIM STACK] image=%s image-offset=0x%zx symbol=%s\n",
                 location.dli_fname ? location.dli_fname : "?",
                 static_cast<size_t>(offset),
                 location.dli_sname ? location.dli_sname : "?");
  }
  std::fflush(stderr);
  if (failOnOverflow)
    _exit(86);
  reporting = false;
}

extern "C" NO_INSTRUMENT void __cyg_profile_func_exit(void *, void *) {}
#endif
