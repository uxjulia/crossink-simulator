#pragma once

#include <Arduino.h>

#define FREEINK_LOG_TRANSPORT_SERIAL 0
#define FREEINK_LOG_TRANSPORT_USB_CDC_WRITE 1
#define FREEINK_LOG_TRANSPORT_ROM_PRINTF 2
#define FREEINK_LOG_TRANSPORT FREEINK_LOG_TRANSPORT_SERIAL
#define FREEINK_SERIAL_HAS_TX_TIMEOUT 1

namespace BoardConfig {

inline auto &serialTransport() {
  static HWCDC transport;
  return transport;
}

inline bool hasTouch() {
#if defined(SIMULATOR_DEVICE_STICKY)
  return true;
#else
  return false;
#endif
}

inline bool isSticky() { return hasTouch(); }

inline void holdPowerRails() {}

} // namespace BoardConfig
