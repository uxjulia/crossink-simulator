#include "HalDisplay.h"

#include <GfxRenderer.h>
#include <SDL.h>

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>

static SDL_Window *window = nullptr;
static SDL_Renderer *sdl_renderer = nullptr;
static SDL_Texture *texture = nullptr;
// Render the simulator at full panel size. The previous 0.5x window was too
// small. With 1:1 pixel mapping, the simulator can be used for testing fine
// details.
static constexpr int SIMULATOR_WINDOW_SCALE = 1;
// Physical bezels can cover the outermost panel pixels. Keep the framebuffer at
// the real panel size, but crop the visible SDL viewport to mimic that.
static constexpr int SIMULATOR_BEZEL_INSET = 5;
static constexpr int SIMULATOR_VISIBLE_PANEL_WIDTH =
    HalDisplay::DISPLAY_WIDTH - (SIMULATOR_BEZEL_INSET * 2);
static constexpr int SIMULATOR_VISIBLE_PANEL_HEIGHT =
    HalDisplay::DISPLAY_HEIGHT - (SIMULATOR_BEZEL_INSET * 2);
static constexpr int SIMULATOR_VISIBLE_WINDOW_WIDTH =
    SIMULATOR_VISIBLE_PANEL_WIDTH * SIMULATOR_WINDOW_SCALE;
static constexpr int SIMULATOR_VISIBLE_WINDOW_HEIGHT =
    SIMULATOR_VISIBLE_PANEL_HEIGHT * SIMULATOR_WINDOW_SCALE;
static_assert(SIMULATOR_VISIBLE_PANEL_WIDTH > 0,
              "Simulator bezel inset is too large for display width");
static_assert(SIMULATOR_VISIBLE_PANEL_HEIGHT > 0,
              "Simulator bezel inset is too large for display height");

// Pixel buffer written by the render task, read by the main thread for
// SDL_RenderPresent. On macOS, SDL calls must happen on the main thread.
static uint32_t
    pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::atomic<bool> pendingPresent{false};
// Written by HalGPIO::update() (which owns SDL event polling); read by
// shouldQuit().
std::atomic<bool> quitRequested{false};

static int currentWindowWidth = 0;
static int currentWindowHeight = 0;

namespace {

struct GrayscalePreviewState {
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> bwBase{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> lsbPlane{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> msbPlane{};
  bool bwBaseValid = false;
  bool lsbValid = false;
  bool msbValid = false;
};

constexpr uint8_t kGrayWhite = 255;
constexpr uint8_t kGrayLight = 200;
constexpr uint8_t kGrayDark = 96;
constexpr uint8_t kGrayBlack = 0;

GrayscalePreviewState grayscalePreviewState;

uint32_t argbGray(uint8_t level) {
  return 0xFF000000u | (static_cast<uint32_t>(level) << 16) |
         (static_cast<uint32_t>(level) << 8) | level;
}

bool getBit(const uint8_t *buffer, int x, int y) {
  const int byteIdx = (y * HalDisplay::DISPLAY_WIDTH + x) / 8;
  const int bitIdx = 7 - (x % 8);
  return (buffer[byteIdx] & (1 << bitIdx)) != 0;
}

void renderBwPixels(const uint8_t *fb) {
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] =
          getBit(fb, x, y) ? 0xFFFFFFFFu : 0xFF000000u;
    }
  }
  pendingPresent.store(true);
}

void clearGrayscalePlanes() {
  grayscalePreviewState.lsbPlane.fill(0);
  grayscalePreviewState.msbPlane.fill(0);
  grayscalePreviewState.lsbValid = false;
  grayscalePreviewState.msbValid = false;
}

void snapshotBwBase(const uint8_t *fb) {
  memcpy(grayscalePreviewState.bwBase.data(), fb, HalDisplay::BUFFER_SIZE);
  grayscalePreviewState.bwBaseValid = true;
  clearGrayscalePlanes();
}

void copyPlane(std::array<uint8_t, HalDisplay::BUFFER_SIZE> &dst,
               const uint8_t *src, bool &valid) {
  if (!src) {
    valid = false;
    dst.fill(0);
    return;
  }
  memcpy(dst.data(), src, HalDisplay::BUFFER_SIZE);
  valid = true;
}

void composeGrayscalePreview() {
  const uint8_t *bwBase = grayscalePreviewState.bwBaseValid
                              ? grayscalePreviewState.bwBase.data()
                              : display.getFrameBuffer();
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      const bool baseWhite = getBit(bwBase, x, y);
      const bool lsbActive =
          grayscalePreviewState.lsbValid &&
          getBit(grayscalePreviewState.lsbPlane.data(), x, y);
      const bool msbActive =
          grayscalePreviewState.msbValid &&
          getBit(grayscalePreviewState.msbPlane.data(), x, y);

      uint8_t level = kGrayWhite;
      if (!baseWhite) {
        if (msbActive) {
          level = lsbActive ? kGrayDark : kGrayLight;
        } else if (lsbActive) {
          level = kGrayDark;
        } else {
          level = kGrayBlack;
        }
      }

      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] = argbGray(level);
    }
  }
  pendingPresent.store(true);
}

} // namespace

static bool isPortraitOrientation(GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::Portrait ||
         orientation == GfxRenderer::PortraitInverted;
}

static void getLogicalWindowSize(GfxRenderer::Orientation orientation,
                                 int *width, int *height) {
  const bool isPortrait = isPortraitOrientation(orientation);
  *width = isPortrait ? SIMULATOR_VISIBLE_WINDOW_HEIGHT
                      : SIMULATOR_VISIBLE_WINDOW_WIDTH;
  *height = isPortrait ? SIMULATOR_VISIBLE_WINDOW_WIDTH
                       : SIMULATOR_VISIBLE_WINDOW_HEIGHT;
}

static void applyWindowGeometryIfNeeded(GfxRenderer::Orientation orientation) {
  if (!window || !sdl_renderer)
    return;

  int winW = 0;
  int winH = 0;
  getLogicalWindowSize(orientation, &winW, &winH);
  if (winW == currentWindowWidth && winH == currentWindowHeight)
    return;

  SDL_SetWindowSize(window, winW, winH);
  SDL_RenderSetLogicalSize(sdl_renderer, winW, winH);
  currentWindowWidth = winW;
  currentWindowHeight = winH;
}

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

#if defined(SIMULATOR_DEVICE_X3)
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X3";
#else
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X4";
#endif

void HalDisplay::begin() {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError()
              << std::endl;
    return;
  }

  int winW = 0;
  int winH = 0;
  extern GfxRenderer renderer;
  getLogicalWindowSize(renderer.getOrientation(), &winW, &winH);

  // SDL_WINDOW_ALLOW_HIGHDPI lets the renderer use full Retina/HiDPI pixels on
  // macOS so we get crisp 1:1 rendering instead of a blurry upscale.
  window = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, winW, winH,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
  sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  // Keep all rendering logic in logical (winW×winH) coordinates; SDL maps to
  // drawable pixels.
  SDL_RenderSetLogicalSize(sdl_renderer, winW, winH);
  currentWindowWidth = winW;
  currentWindowHeight = winH;

  // Linear filtering: Bayer-dithered pixels average to correct gray at scaled
  // sizes rather than showing harsh black/white patterns.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH,
                              DISPLAY_HEIGHT);
}

void HalDisplay::begin(bool /*seamless*/) { begin(); }

void HalDisplay::clearScreen(uint8_t color) const {
  memset(getFrameBuffer(), color, BUFFER_SIZE);
}

void HalDisplay::drawImage(const uint8_t *imageData, uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h, bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] = imageData[srcOffset + col];
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t *imageData, uint16_t x,
                                      uint16_t y, uint16_t w, uint16_t h,
                                      bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] &= imageData[srcOffset + col];
    }
  }
}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  refreshDisplay(mode, turnOffScreen);
}

void HalDisplay::displayWindow(int, int, int, int) {
  refreshDisplay(RefreshMode::FAST_REFRESH, false);
}

// Called from the render task (background thread): convert framebuffer to
// pixels and flag for present.
void HalDisplay::refreshDisplay(RefreshMode /*mode*/, bool /*turnOffScreen*/) {
  const uint8_t *fb = getFrameBuffer();
  snapshotBwBase(fb);
  renderBwPixels(fb);
}

// Called from the main thread (simulator_main.cpp) to push pixels to SDL.
void HalDisplay::presentIfNeeded() {
  if (!pendingPresent.load())
    return;
  pendingPresent.store(false);

  if (!texture || !sdl_renderer)
    return;

  extern GfxRenderer renderer;
  const GfxRenderer::Orientation orientation = renderer.getOrientation();
  applyWindowGeometryIfNeeded(orientation);

  SDL_UpdateTexture(texture, nullptr, pixelBuf,
                    DISPLAY_WIDTH * sizeof(uint32_t));
  SDL_RenderClear(sdl_renderer);

  const SDL_Rect src = {SIMULATOR_BEZEL_INSET, SIMULATOR_BEZEL_INSET,
                        SIMULATOR_VISIBLE_PANEL_WIDTH,
                        SIMULATOR_VISIBLE_PANEL_HEIGHT};
  const SDL_Rect landscapeDst = {0, 0, SIMULATOR_VISIBLE_WINDOW_WIDTH,
                                 SIMULATOR_VISIBLE_WINDOW_HEIGHT};

  // For portrait modes the landscape panel texture must be rotated to fill the
  // portrait window. SDL_RenderCopyEx rotates around the centre of dst, so dst
  // must stay landscape-oriented and be offset so its centre coincides with the
  // window centre. After rotation the result fills the portrait window.
  //
  // Portrait rotateCoordinates stores content rotated 90° CCW in the physical
  // buffer, so we rotate +90° CW here to undo it. PortraitInverted stores
  // content rotated 90° CW → undo with -90°.
  switch (orientation) {
  case GfxRenderer::Portrait: {
    // dst centre = window centre, landscape-sized panel texture.
    SDL_Rect dst = {
        (SIMULATOR_VISIBLE_WINDOW_HEIGHT - SIMULATOR_VISIBLE_WINDOW_WIDTH) / 2,
        (SIMULATOR_VISIBLE_WINDOW_WIDTH - SIMULATOR_VISIBLE_WINDOW_HEIGHT) / 2,
        SIMULATOR_VISIBLE_WINDOW_WIDTH, SIMULATOR_VISIBLE_WINDOW_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, &src, &dst, 90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::PortraitInverted: {
    SDL_Rect dst = {
        (SIMULATOR_VISIBLE_WINDOW_HEIGHT - SIMULATOR_VISIBLE_WINDOW_WIDTH) / 2,
        (SIMULATOR_VISIBLE_WINDOW_WIDTH - SIMULATOR_VISIBLE_WINDOW_HEIGHT) / 2,
        SIMULATOR_VISIBLE_WINDOW_WIDTH, SIMULATOR_VISIBLE_WINDOW_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, &src, &dst, -90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::LandscapeClockwise: {
    SDL_RenderCopyEx(sdl_renderer, texture, &src, &landscapeDst, 180.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  default: {
    SDL_RenderCopy(sdl_renderer, texture, &src, &landscapeDst);
    break;
  }
  }

  SDL_RenderPresent(sdl_renderer);
}

bool HalDisplay::shouldQuit() const { return quitRequested.load(); }

void HalDisplay::deepSleep() { presentIfNeeded(); }

uint8_t *HalDisplay::getFrameBuffer() const {
  static uint8_t buffer[HalDisplay::BUFFER_SIZE];
  return buffer;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t *lsbBuffer,
                                      const uint8_t *msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}
void HalDisplay::displayGrayscaleBase(RefreshMode fallback,
                                      bool turnOffScreen) {
  displayBuffer(fallback, turnOffScreen);
}
void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {
}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t *lsbBuffer) {
  copyPlane(grayscalePreviewState.lsbPlane, lsbBuffer,
            grayscalePreviewState.lsbValid);
}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t *msbBuffer) {
  copyPlane(grayscalePreviewState.msbPlane, msbBuffer,
            grayscalePreviewState.msbValid);
}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t *bwBuffer) {
  if (bwBuffer) {
    snapshotBwBase(bwBuffer);
  } else {
    grayscalePreviewState.bwBaseValid = false;
    grayscalePreviewState.bwBase.fill(0);
    clearGrayscalePlanes();
  }
}
void HalDisplay::displayGrayBuffer(bool, const unsigned char *, bool) {
  composeGrayscalePreview();
}
void HalDisplay::displayFactoryGrayBuffer(bool) { composeGrayscalePreview(); }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t *rows,
                                          uint16_t yStart, uint16_t numRows) {
  if (!rows || numRows == 0 || yStart >= DISPLAY_HEIGHT) {
    return;
  }

  const uint16_t rowsToCopy =
      (yStart + numRows > DISPLAY_HEIGHT) ? (DISPLAY_HEIGHT - yStart) : numRows;
  const size_t offset = static_cast<size_t>(yStart) * DISPLAY_WIDTH_BYTES;
  const size_t byteCount =
      static_cast<size_t>(rowsToCopy) * DISPLAY_WIDTH_BYTES;
  auto &plane = lsbPlane ? grayscalePreviewState.lsbPlane
                         : grayscalePreviewState.msbPlane;
  memcpy(plane.data() + offset, rows, byteCount);
  if (lsbPlane) {
    grayscalePreviewState.lsbValid = true;
  } else {
    grayscalePreviewState.msbValid = true;
  }
}
bool HalDisplay::supportsStripGrayscale() const { return true; }

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const {
  return DISPLAY_WIDTH_BYTES;
}
uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

HalDisplay display;
