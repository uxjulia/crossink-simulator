#include "HalGPIO.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "HalDisplay.h"
#include "SimulatorInput.h"
#include "SimulatorLifecycle.h"

// Defined in HalDisplay.cpp — set here so all SDL event polling lives in one
// place.
extern std::atomic<bool> quitRequested;

// Keyboard mapping:
//   BTN_BACK    (0) → Escape
//   BTN_CONFIRM (1) → Return
//   BTN_LEFT    (2) → Left arrow
//   BTN_RIGHT   (3) → Right arrow
//   BTN_UP      (4) → Up arrow
//   BTN_DOWN    (5) → Down arrow
//   BTN_POWER   (6) → P
//   Simulator sleep shortcut → S

static constexpr int NUM_BUTTONS = 7;
static constexpr SDL_Scancode SIMULATOR_SLEEP_SCANCODE = SDL_SCANCODE_S;

static const SDL_Scancode buttonScancode[NUM_BUTTONS] = {
    SDL_SCANCODE_ESCAPE, // BTN_BACK
    SDL_SCANCODE_RETURN, // BTN_CONFIRM
    SDL_SCANCODE_LEFT,   // BTN_LEFT
    SDL_SCANCODE_RIGHT,  // BTN_RIGHT
    SDL_SCANCODE_UP,     // BTN_UP
    SDL_SCANCODE_DOWN,   // BTN_DOWN
    SDL_SCANCODE_P,      // BTN_POWER
};

static bool pressedThisFrame[NUM_BUTTONS] = {};
static bool releasedThisFrame[NUM_BUTTONS] = {};
static unsigned long buttonPressTime[NUM_BUTTONS] = {};
static bool simulatorSleepRequested = false;

namespace {
constexpr int TOUCH_TAP_SLOP_PX = 28;
constexpr int TOUCH_SWIPE_MIN_PX = 60;
constexpr unsigned long TOUCH_SWIPE_MAX_MS = 700;

struct SimTouchState {
  bool pressed = false;
  bool pressedThisFrame = false;
  bool releasedThisFrame = false;
  bool movedBeyondTapSlop = false;
  float downX = 0.0f;
  float downY = 0.0f;
  float currentX = 0.0f;
  float currentY = 0.0f;
  unsigned long pressedAt = 0;
  unsigned long lastHeldMs = 0;
};

SimTouchState touch;

int touchDistanceX() {
  return static_cast<int>(std::round(std::abs(touch.currentX - touch.downX) *
                                     HalDisplay::DISPLAY_WIDTH));
}

int touchDistanceY() {
  return static_cast<int>(std::round(std::abs(touch.currentY - touch.downY) *
                                     HalDisplay::DISPLAY_HEIGHT));
}

void updateTouchPosition(const int x, const int y) {
  simulatorWindowPointToPanelNormalized(x, y, touch.currentX, touch.currentY);
  if (touchDistanceX() > TOUCH_TAP_SLOP_PX ||
      touchDistanceY() > TOUCH_TAP_SLOP_PX) {
    touch.movedBeyondTapSlop = true;
  }
}
} // namespace

static void clearButtonState() {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
    buttonPressTime[i] = 0;
  }
}

static int scancodeToButton(SDL_Scancode sc) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonScancode[i] == sc)
      return i;
  }
  return -1;
}

void HalGPIO::begin() {
#if defined(SIMULATOR_DEVICE_X3)
  _deviceType = DeviceType::X3;
#else
  _deviceType = DeviceType::X4;
#endif
}

void HalGPIO::beginFrame() {
  // Clear the press/release edge latches once per frame. See update() for why
  // this is deliberately separate from the SDL poll.
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }
  touch.pressedThisFrame = false;
  touch.releasedThisFrame = false;
}

void HalGPIO::update() {
  // Per-frame press/release edges are intentionally NOT cleared here; that
  // happens once per frame in beginFrame(). The firmware calls update() several
  // times within a single frame (e.g. CrossPointWebServerActivity polls input
  // between handleClient() bursts, on top of the top-of-loop gpio.update() in
  // main.cpp). If edges were cleared on every update(), a key press drained by
  // an earlier update() would be wiped before a later update()'s wasPressed()
  // check could observe it — which made Back/Exit require repeated presses.
  // Latching edges for the whole frame keeps wasPressed() stable across all
  // update() calls in that frame, matching the on-device InputManager.

  // HalGPIO owns all SDL event polling so keyboard and quit events are never
  // split between two callers (HalDisplay::presentIfNeeded only renders).
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      quitRequested.store(true);
    } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
      if (e.key.keysym.scancode == SIMULATOR_SLEEP_SCANCODE) {
        simulatorSleepRequested = true;
        continue;
      }
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        pressedThisFrame[btn] = true;
        buttonPressTime[btn] = SDL_GetTicks();
      }
    } else if (e.type == SDL_KEYUP) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        releasedThisFrame[btn] = true;
      }
#if defined(SIMULATOR_DEVICE_STICKY)
    } else if (e.type == SDL_MOUSEBUTTONDOWN &&
               e.button.button == SDL_BUTTON_LEFT) {
      touch.pressed = true;
      touch.pressedThisFrame = true;
      touch.movedBeyondTapSlop = false;
      simulatorWindowPointToPanelNormalized(e.button.x, e.button.y, touch.downX,
                                            touch.downY);
      touch.currentX = touch.downX;
      touch.currentY = touch.downY;
      touch.pressedAt = SDL_GetTicks();
    } else if (e.type == SDL_MOUSEMOTION && touch.pressed) {
      updateTouchPosition(e.motion.x, e.motion.y);
    } else if (e.type == SDL_MOUSEBUTTONUP &&
               e.button.button == SDL_BUTTON_LEFT && touch.pressed) {
      updateTouchPosition(e.button.x, e.button.y);
      touch.pressed = false;
      touch.releasedThisFrame = true;
      touch.lastHeldMs = SDL_GetTicks() - touch.pressedAt;
#endif
    }
  }
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  return state[buttonScancode[buttonIndex]];
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  return pressedThisFrame[buttonIndex];
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS)
    return false;
  return releasedThisFrame[buttonIndex];
}

bool HalGPIO::wasAnyPressed() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (pressedThisFrame[i])
      return true;
  }
  return false;
}

bool HalGPIO::wasAnyReleased() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (releasedThisFrame[i])
      return true;
  }
  return false;
}

unsigned long HalGPIO::getHeldTime() const {
  // Return the longest held time among all currently pressed buttons
  unsigned long now = SDL_GetTicks();
  unsigned long maxHeld = 0;
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (state[buttonScancode[i]] && buttonPressTime[i] > 0) {
      unsigned long held = now - buttonPressTime[i];
      if (held > maxHeld)
        maxHeld = held;
    }
  }
  return maxHeld;
}

unsigned long HalGPIO::getPowerButtonHeldTime() const {
  const uint8_t *state = SDL_GetKeyboardState(NULL);
  if (!state[buttonScancode[BTN_POWER]] || buttonPressTime[BTN_POWER] == 0)
    return 0;
  return SDL_GetTicks() - buttonPressTime[BTN_POWER];
}

bool HalGPIO::hasTouch() const {
#if defined(SIMULATOR_DEVICE_STICKY)
  return true;
#else
  return false;
#endif
}

bool HalGPIO::wasTouchTap(float &nx, float &ny) const {
  if (!hasTouch() || !touch.releasedThisFrame || touch.movedBeyondTapSlop)
    return false;
  nx = touch.downX;
  ny = touch.downY;
  return true;
}

bool HalGPIO::wasTouchDown(float &nx, float &ny) const {
  if (!hasTouch() || !touch.pressedThisFrame)
    return false;
  nx = touch.downX;
  ny = touch.downY;
  return true;
}

bool HalGPIO::wasTouchReleased() const {
  return hasTouch() && touch.releasedThisFrame;
}

bool HalGPIO::isTouchTapCandidate(float &nx, float &ny,
                                  unsigned long &heldMs) const {
  if (!hasTouch() || !touch.pressed || touch.movedBeyondTapSlop)
    return false;
  nx = touch.downX;
  ny = touch.downY;
  heldMs = SDL_GetTicks() - touch.pressedAt;
  return true;
}

bool HalGPIO::isTouchHeldAt(float &nx, float &ny) const {
  if (!hasTouch() || !touch.pressed)
    return false;
  nx = touch.currentX;
  ny = touch.currentY;
  return true;
}

unsigned long HalGPIO::lastTouchHeldMs() const {
  return hasTouch() ? touch.lastHeldMs : 0;
}

bool HalGPIO::wasSwipe(float &nxStart, float &nyStart, float &nxEnd,
                       float &nyEnd) const {
  if (!hasTouch() || !touch.releasedThisFrame ||
      touch.lastHeldMs > TOUCH_SWIPE_MAX_MS ||
      (touchDistanceX() < TOUCH_SWIPE_MIN_PX &&
       touchDistanceY() < TOUCH_SWIPE_MIN_PX)) {
    return false;
  }
  nxStart = touch.downX;
  nyStart = touch.downY;
  nxEnd = touch.currentX;
  nyEnd = touch.currentY;
  return true;
}

bool HalGPIO::wasTouchActivity() const {
  return hasTouch() && (touch.pressedThisFrame || touch.releasedThisFrame);
}

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(bool /*enabled*/) {}

bool HalGPIO::consumeSimulatorSleepRequest() {
  const bool requested = simulatorSleepRequested;
  simulatorSleepRequested = false;
  return requested;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  if (SimulatorLifecycle::consumeWakeReason() ==
      SimulatorLifecycle::WakeReason::PowerButton) {
    return WakeupReason::PowerButton;
  }
  return WakeupReason::Other;
}
bool HalGPIO::isUsbConnected() const { return true; }
bool HalGPIO::wasUsbStateChanged() const { return false; }
void HalGPIO::startDeepSleep() {
  clearButtonState();

  while (true) {
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
      if (e.type == SDL_QUIT) {
        quitRequested.store(true);
        return;
      }

      if (e.type == SDL_KEYDOWN && !e.key.repeat &&
          scancodeToButton(e.key.keysym.scancode) >= 0) {
        clearButtonState();
        SimulatorLifecycle::rebootAsPowerWake();
      }
    }

    SDL_Delay(10);
  }
}
bool HalGPIO::verifyPowerButtonWakeup(uint16_t /*requiredDurationMs*/,
                                      bool /*shortPressAllowed*/) {
  return true;
}

HalGPIO gpio;
