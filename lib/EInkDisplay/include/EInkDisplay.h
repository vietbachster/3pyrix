#pragma once

#include <Arduino.h>
#include <SPI.h>

class EInkDisplay {
 public:
  enum RefreshMode {
    FULL_REFRESH,
    HALF_REFRESH,
    FAST_REFRESH,
  };

  static constexpr uint16_t DISPLAY_WIDTH = 792;
  static constexpr uint16_t DISPLAY_HEIGHT = 528;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  EInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy);
  ~EInkDisplay() = default;

  void begin();

  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }

  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;
  void setFramebuffer(const uint8_t* bwBuffer) const;

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
#endif

  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen = false);
  void displayGrayBuffer(bool turnOffScreen = false);
  void refreshDisplay(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  void requestResync(uint8_t settlePasses = 0);
  void grayscaleRevert();
  void deepSleep();

  using IdleCallback = void (*)();
  void setIdleCallback(IdleCallback callback) { idleCallback_ = callback; }

  uint8_t* getFrameBuffer() const { return frameBuffer; }
  void saveFrameBufferAsPBM(const char* filename);

 private:
  struct GrayState {
    bool lastBaseWasPartial = false;
    bool lsbValid = false;
  };

  int8_t _sclk;
  int8_t _mosi;
  int8_t _cs;
  int8_t _dc;
  int8_t _rst;
  int8_t _busy;

  uint8_t frameBuffer0[BUFFER_SIZE];
  uint8_t* frameBuffer = nullptr;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  uint8_t frameBuffer1[BUFFER_SIZE];
  uint8_t* frameBufferActive = nullptr;
#endif

  SPISettings spiSettings;
  bool isScreenOn = false;
  bool inGrayscaleMode = false;
  bool redRamSynced = false;
  GrayState grayState;
  uint8_t initialFullSyncsRemaining = 0;
  bool forceFullSyncNext = false;
  uint8_t forcedConditionPassesNext = 0;
  IdleCallback idleCallback_ = nullptr;

  void resetDisplay();
  void sendCommand(uint8_t command);
  void sendData(uint8_t data);
  void sendData(const uint8_t* data, uint16_t length);
  void sendCommandData(uint8_t command, const uint8_t* data, uint16_t length);
  void sendCommandDataByte(uint8_t command, uint8_t data0, uint8_t data1);
  void waitForRefresh(const char* comment = nullptr);
  void waitWhileBusy(const char* comment = nullptr);
  void initDisplayController();
  void sendMirroredPlane(const uint8_t* plane, bool invertBits);
};
