#include "EInkDisplay.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace {

constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_GATE_VOLTAGE = 0x03;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint8_t CMD_DATA_START_OLD = 0x10;
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
constexpr uint8_t CMD_DATA_START_NEW = 0x13;
constexpr uint8_t CMD_PLL_CONTROL = 0x30;
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;
constexpr uint8_t CMD_LUT_VCOM = 0x20;
constexpr uint8_t CMD_LUT_WW = 0x21;
constexpr uint8_t CMD_LUT_BW = 0x22;
constexpr uint8_t CMD_LUT_WB = 0x23;
constexpr uint8_t CMD_LUT_BB = 0x24;
constexpr uint8_t CMD_RESOLUTION = 0x61;
constexpr uint8_t CMD_FLASH_MODE = 0x65;
constexpr uint8_t CMD_VCM_DC = 0x82;
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;
constexpr uint8_t CMD_PARTIAL_IN = 0x91;
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;
constexpr uint8_t CMD_OTP_SELECTION = 0xE1;

constexpr uint8_t X3_POWER_SETTING[] = {0x07, 0x17, 0x3F, 0x3F, 0x17};
constexpr uint8_t X3_BOOSTER_SOFT_START[] = {0x25, 0x25, 0x3C, 0x37};
constexpr uint8_t X3_PANEL_SETTING[] = {0x3F, 0x08};
constexpr uint8_t X3_RESOLUTION[] = {0x03, 0x18, 0x02, 0x58};  // 792x600 controller space
constexpr uint8_t X3_FLASH_MODE[] = {0x00, 0x00, 0x00, 0x00};

const uint8_t lut_x3_vcom_full[] PROGMEM = {
    0x00, 0x06, 0x02, 0x06, 0x06, 0x01, 0x00, 0x05, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_ww_full[] PROGMEM = {
    0x20, 0x06, 0x02, 0x06, 0x06, 0x01, 0x00, 0x05, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_bw_full[] PROGMEM = {
    0xAA, 0x06, 0x02, 0x06, 0x06, 0x01, 0x80, 0x05, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_wb_full[] PROGMEM = {
    0x55, 0x06, 0x02, 0x06, 0x06, 0x01, 0x40, 0x05, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_bb_full[] PROGMEM = {
    0x10, 0x06, 0x02, 0x06, 0x06, 0x01, 0x00, 0x05, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t lut_x3_vcom_gray[] PROGMEM = {
    0x00, 0x03, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_ww_gray[] PROGMEM = {
    0x20, 0x03, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_bw_gray[] PROGMEM = {
    0x80, 0x03, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_wb_gray[] PROGMEM = {
    0x00, 0x03, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_bb_gray[] PROGMEM = {
    0x00, 0x03, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t lut_x3_vcom_img[] PROGMEM = {
    0x00, 0x08, 0x0B, 0x02, 0x03, 0x01, 0x00, 0x0C, 0x02, 0x07, 0x02, 0x01, 0x00, 0x01,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_ww_img[] PROGMEM = {
    0xA8, 0x08, 0x0B, 0x02, 0x03, 0x01, 0x44, 0x0C, 0x02, 0x07, 0x02, 0x01, 0x04, 0x01,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_bw_img[] PROGMEM = {
    0x80, 0x08, 0x0B, 0x02, 0x03, 0x01, 0x62, 0x0C, 0x02, 0x07, 0x02, 0x01, 0x00, 0x01,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_wb_img[] PROGMEM = {
    0x88, 0x08, 0x0B, 0x02, 0x03, 0x01, 0x60, 0x0C, 0x02, 0x07, 0x02, 0x01, 0x00, 0x01,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t lut_x3_bb_img[] PROGMEM = {
    0x00, 0x08, 0x0B, 0x02, 0x03, 0x01, 0x4A, 0x0C, 0x02, 0x07, 0x02, 0x01, 0x88, 0x01,
    0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

}  // namespace

EInkDisplay::EInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy)
    : _sclk(sclk), _mosi(mosi), _cs(cs), _dc(dc), _rst(rst), _busy(busy) {}

void EInkDisplay::begin() {
  frameBuffer = frameBuffer0;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  frameBufferActive = frameBuffer1;
  memset(frameBuffer1, 0xFF, BUFFER_SIZE);
#endif
  memset(frameBuffer0, 0xFF, BUFFER_SIZE);

  redRamSynced = false;
  initialFullSyncsRemaining = 1;
  forceFullSyncNext = false;
  forcedConditionPassesNext = 0;
  grayState = {};
  inGrayscaleMode = false;
  isScreenOn = false;

  SPI.begin(_sclk, -1, _mosi, _cs);
  spiSettings = SPISettings(10000000, MSBFIRST, SPI_MODE0);

  pinMode(_cs, OUTPUT);
  pinMode(_dc, OUTPUT);
  pinMode(_rst, OUTPUT);
  pinMode(_busy, INPUT);
  digitalWrite(_cs, HIGH);
  digitalWrite(_dc, HIGH);

  resetDisplay();
  initDisplayController();
}

void EInkDisplay::resetDisplay() {
  digitalWrite(_rst, HIGH);
  delay(20);
  digitalWrite(_rst, LOW);
  delay(2);
  digitalWrite(_rst, HIGH);
  delay(20);
  delay(50);
}

void EInkDisplay::sendCommand(uint8_t command) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, LOW);
  digitalWrite(_cs, LOW);
  SPI.transfer(command);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::sendData(uint8_t data) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  SPI.transfer(data);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::sendData(const uint8_t* data, uint16_t length) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  SPI.writeBytes(data, length);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::sendCommandData(uint8_t command, const uint8_t* data, uint16_t length) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, LOW);
  SPI.transfer(command);
  if (length > 0 && data != nullptr) {
    digitalWrite(_dc, HIGH);
    SPI.writeBytes(data, length);
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void EInkDisplay::sendCommandDataByte(uint8_t command, uint8_t data0, uint8_t data1) {
  const uint8_t payload[2] = {data0, data1};
  sendCommandData(command, payload, sizeof(payload));
}

void EInkDisplay::waitForRefresh(const char* comment) {
  unsigned long start = millis();
  bool sawLow = false;
  while (digitalRead(_busy) == HIGH) {
    delay(1);
    if (millis() - start > 1000) break;
  }
  if (digitalRead(_busy) == LOW) {
    sawLow = true;
    int idleTick = 0;
    while (digitalRead(_busy) == LOW) {
      delay(1);
      if (millis() - start > 30000) break;
      if (idleCallback_ && (++idleTick % 10 == 0)) idleCallback_();
    }
  }
  if (comment != nullptr && sawLow && Serial) {
    Serial.printf("[%lu]   Wait complete: %s (%lu ms)\n", millis(), comment, millis() - start);
  }
}

void EInkDisplay::waitWhileBusy(const char* comment) { waitForRefresh(comment); }

void EInkDisplay::initDisplayController() {
  sendCommandData(CMD_PANEL_SETTING, X3_PANEL_SETTING, sizeof(X3_PANEL_SETTING));
  sendCommandData(CMD_RESOLUTION, X3_RESOLUTION, sizeof(X3_RESOLUTION));
  sendCommandData(CMD_FLASH_MODE, X3_FLASH_MODE, sizeof(X3_FLASH_MODE));
  sendCommand(CMD_GATE_VOLTAGE);
  sendData(0x1D);
  sendCommandData(CMD_POWER_SETTING, X3_POWER_SETTING, sizeof(X3_POWER_SETTING));
  sendCommand(CMD_VCM_DC);
  sendData(0x1D);
  sendCommandData(CMD_BOOSTER_SOFT_START, X3_BOOSTER_SOFT_START, sizeof(X3_BOOSTER_SOFT_START));
  sendCommand(CMD_PLL_CONTROL);
  sendData(0x09);
  sendCommand(CMD_OTP_SELECTION);
  sendData(0x02);

  sendCommandData(CMD_LUT_VCOM, lut_x3_vcom_full, 42);
  sendCommandData(CMD_LUT_WW, lut_x3_ww_full, 42);
  sendCommandData(CMD_LUT_BW, lut_x3_bw_full, 42);
  sendCommandData(CMD_LUT_WB, lut_x3_wb_full, 42);
  sendCommandData(CMD_LUT_BB, lut_x3_bb_full, 42);
}

void EInkDisplay::sendMirroredPlane(const uint8_t* plane, bool invertBits) {
  uint8_t row[DISPLAY_WIDTH_BYTES];
  for (uint16_t y = 0; y < DISPLAY_HEIGHT; y++) {
    const uint16_t srcY = static_cast<uint16_t>(DISPLAY_HEIGHT - 1 - y);
    const uint8_t* src = plane + static_cast<uint32_t>(srcY) * DISPLAY_WIDTH_BYTES;
    for (uint16_t x = 0; x < DISPLAY_WIDTH_BYTES; x++) {
      row[x] = invertBits ? static_cast<uint8_t>(~src[x]) : src[x];
    }
    sendData(row, DISPLAY_WIDTH_BYTES);
  }
}

void EInkDisplay::clearScreen(uint8_t color) const { memset(frameBuffer, color, BUFFER_SIZE); }

void EInkDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem) const {
  if (!frameBuffer) return;

  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) break;

    const uint32_t destOffset = static_cast<uint32_t>(destY) * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES) break;
      frameBuffer[destOffset + col] = fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
    }
  }
}

void EInkDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                       bool fromProgmem) const {
  if (!frameBuffer) return;

  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) break;

    const uint32_t destOffset = static_cast<uint32_t>(destY) * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES) break;
      const uint8_t srcByte = fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
      frameBuffer[destOffset + col] &= srcByte;
    }
  }
}

void EInkDisplay::setFramebuffer(const uint8_t* bwBuffer) const { memcpy(frameBuffer, bwBuffer, BUFFER_SIZE); }

void EInkDisplay::requestResync(uint8_t settlePasses) {
  forceFullSyncNext = true;
  forcedConditionPassesNext = settlePasses;
}

void EInkDisplay::grayscaleRevert() {
  inGrayscaleMode = false;
  grayState.lsbValid = false;
  redRamSynced = false;
  forceFullSyncNext = true;
  forcedConditionPassesNext = 0;
}

void EInkDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  if (!lsbBuffer) {
    grayState.lsbValid = false;
    return;
  }

  sendCommand(CMD_DATA_START_OLD);
  sendMirroredPlane(lsbBuffer, false);
  grayState.lsbValid = true;
}

void EInkDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  if (!msbBuffer || !grayState.lsbValid) return;
  sendCommand(CMD_DATA_START_NEW);
  sendMirroredPlane(msbBuffer, false);
}

void EInkDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}

#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
void EInkDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  if (!bwBuffer) return;

  sendCommand(CMD_DATA_START_NEW);
  sendMirroredPlane(bwBuffer, false);
  sendCommand(CMD_DATA_START_OLD);
  sendMirroredPlane(bwBuffer, false);

  redRamSynced = true;
  grayState.lsbValid = false;
  forceFullSyncNext = false;
  forcedConditionPassesNext = 0;
  inGrayscaleMode = false;
}
#endif

void EInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  if (inGrayscaleMode) grayscaleRevert();

  const bool fastMode = (mode != FULL_REFRESH);
  const bool forcedFullSync = forceFullSyncNext;
  const bool doFullSync = !fastMode || !redRamSynced || initialFullSyncsRemaining > 0 || forcedFullSync;

  grayState.lastBaseWasPartial = !doFullSync;

  if (doFullSync) {
    sendCommandData(CMD_LUT_VCOM, lut_x3_vcom_img, 42);
    sendCommandData(CMD_LUT_WW, lut_x3_ww_img, 42);
    sendCommandData(CMD_LUT_BW, lut_x3_bw_img, 42);
    sendCommandData(CMD_LUT_WB, lut_x3_wb_img, 42);
    sendCommandData(CMD_LUT_BB, lut_x3_bb_img, 42);

    sendCommand(CMD_DATA_START_NEW);
    sendMirroredPlane(frameBuffer, true);
    sendCommand(CMD_DATA_START_OLD);
    sendMirroredPlane(frameBuffer, true);

    sendCommandDataByte(CMD_VCOM_DATA_INTERVAL, 0xA9, 0x07);
  } else {
    sendCommandData(CMD_LUT_VCOM, lut_x3_vcom_full, 42);
    sendCommandData(CMD_LUT_WW, lut_x3_ww_full, 42);
    sendCommandData(CMD_LUT_BW, lut_x3_bw_full, 42);
    sendCommandData(CMD_LUT_WB, lut_x3_wb_full, 42);
    sendCommandData(CMD_LUT_BB, lut_x3_bb_full, 42);

    sendCommand(CMD_DATA_START_NEW);
    sendMirroredPlane(frameBuffer, false);

    sendCommandDataByte(CMD_VCOM_DATA_INTERVAL, 0x29, 0x07);
  }

  if (!isScreenOn || doFullSync) {
    sendCommand(CMD_POWER_ON);
    waitForRefresh("power on");
    isScreenOn = true;
  }

  sendCommand(CMD_DISPLAY_REFRESH);
  waitForRefresh("display refresh");

  if (turnOffScreen) {
    sendCommand(CMD_POWER_OFF);
    waitForRefresh("power off");
    isScreenOn = false;
  }

  if (!fastMode) delay(200);

  uint8_t postConditionPasses = 0;
  if (doFullSync) {
    if (forcedFullSync) {
      postConditionPasses = forcedConditionPassesNext;
    } else if (initialFullSyncsRemaining == 1) {
      postConditionPasses = 1;
    }
  }

  if (postConditionPasses > 0) {
    const uint16_t xStart = 0;
    const uint16_t xEnd = DISPLAY_WIDTH - 1;
    const uint16_t yStart = 0;
    const uint16_t yEnd = DISPLAY_HEIGHT - 1;
    const uint8_t partialWindow[9] = {
        static_cast<uint8_t>(xStart >> 8), static_cast<uint8_t>(xStart & 0xFF), static_cast<uint8_t>(xEnd >> 8),
        static_cast<uint8_t>(xEnd & 0xFF), static_cast<uint8_t>(yStart >> 8), static_cast<uint8_t>(yStart & 0xFF),
        static_cast<uint8_t>(yEnd >> 8), static_cast<uint8_t>(yEnd & 0xFF), 0x01};

    sendCommandData(CMD_LUT_VCOM, lut_x3_vcom_full, 42);
    sendCommandData(CMD_LUT_WW, lut_x3_ww_full, 42);
    sendCommandData(CMD_LUT_BW, lut_x3_bw_full, 42);
    sendCommandData(CMD_LUT_WB, lut_x3_wb_full, 42);
    sendCommandData(CMD_LUT_BB, lut_x3_bb_full, 42);
    sendCommandDataByte(CMD_VCOM_DATA_INTERVAL, 0x29, 0x07);

    for (uint8_t i = 0; i < postConditionPasses; i++) {
      sendCommand(CMD_PARTIAL_IN);
      sendCommandData(CMD_PARTIAL_WINDOW, partialWindow, sizeof(partialWindow));
      sendCommand(CMD_DATA_START_NEW);
      sendMirroredPlane(frameBuffer, false);
      sendCommand(CMD_PARTIAL_OUT);
      if (!isScreenOn) {
        sendCommand(CMD_POWER_ON);
        waitForRefresh("power on");
        isScreenOn = true;
      }
      sendCommand(CMD_DISPLAY_REFRESH);
      waitForRefresh("settle refresh");
    }
  }

  sendCommand(CMD_DATA_START_OLD);
  sendMirroredPlane(frameBuffer, false);
  redRamSynced = true;

  if (doFullSync && initialFullSyncsRemaining > 0) initialFullSyncsRemaining--;
  forceFullSyncNext = false;
  forcedConditionPassesNext = 0;
}

void EInkDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  if (x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) return;
  if (x % 8 != 0 || w % 8 != 0) return;
  if (!frameBuffer) return;
  if (inGrayscaleMode) grayscaleRevert();

  const uint16_t windowWidthBytes = w / 8;
  std::vector<uint8_t> windowBuffer(static_cast<size_t>(windowWidthBytes) * h);
  for (uint16_t row = 0; row < h; row++) {
    const uint32_t srcOffset = static_cast<uint32_t>(y + row) * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint32_t dstOffset = static_cast<uint32_t>(row) * windowWidthBytes;
    memcpy(&windowBuffer[dstOffset], &frameBuffer[srcOffset], windowWidthBytes);
  }

  const uint16_t xEnd = static_cast<uint16_t>(x + w - 1);
  const uint16_t yEnd = static_cast<uint16_t>(y + h - 1);
  const uint8_t partialWindow[9] = {
      static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x & 0xFF), static_cast<uint8_t>(xEnd >> 8),
      static_cast<uint8_t>(xEnd & 0xFF), static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y & 0xFF),
      static_cast<uint8_t>(yEnd >> 8), static_cast<uint8_t>(yEnd & 0xFF), 0x01};

  auto sendMirroredWindow = [&](bool invertBits) {
    uint8_t row[DISPLAY_WIDTH_BYTES];
    for (uint16_t rowIndex = 0; rowIndex < h; rowIndex++) {
      const uint16_t srcY = static_cast<uint16_t>(h - 1 - rowIndex);
      const uint8_t* src = windowBuffer.data() + static_cast<uint32_t>(srcY) * windowWidthBytes;
      for (uint16_t col = 0; col < windowWidthBytes; col++) {
        row[col] = invertBits ? static_cast<uint8_t>(~src[col]) : src[col];
      }
      sendData(row, windowWidthBytes);
    }
  };

  sendCommandData(CMD_LUT_VCOM, lut_x3_vcom_full, 42);
  sendCommandData(CMD_LUT_WW, lut_x3_ww_full, 42);
  sendCommandData(CMD_LUT_BW, lut_x3_bw_full, 42);
  sendCommandData(CMD_LUT_WB, lut_x3_wb_full, 42);
  sendCommandData(CMD_LUT_BB, lut_x3_bb_full, 42);
  sendCommandDataByte(CMD_VCOM_DATA_INTERVAL, 0x29, 0x07);

  sendCommand(CMD_PARTIAL_IN);
  sendCommandData(CMD_PARTIAL_WINDOW, partialWindow, sizeof(partialWindow));
  sendCommand(CMD_DATA_START_NEW);
  sendMirroredWindow(false);
  sendCommand(CMD_PARTIAL_OUT);

  if (!isScreenOn) {
    sendCommand(CMD_POWER_ON);
    waitForRefresh("power on");
    isScreenOn = true;
  }

  sendCommand(CMD_DISPLAY_REFRESH);
  waitForRefresh("window refresh");

  if (turnOffScreen) {
    sendCommand(CMD_POWER_OFF);
    waitForRefresh("power off");
    isScreenOn = false;
  }

  sendCommand(CMD_PARTIAL_IN);
  sendCommandData(CMD_PARTIAL_WINDOW, partialWindow, sizeof(partialWindow));
  sendCommand(CMD_DATA_START_OLD);
  sendMirroredWindow(false);
  sendCommand(CMD_PARTIAL_OUT);

  redRamSynced = true;
  forceFullSyncNext = false;
  forcedConditionPassesNext = 0;
}

void EInkDisplay::displayGrayBuffer(bool turnOffScreen) {
  if (!grayState.lsbValid) return;

  inGrayscaleMode = false;
  sendCommandData(CMD_LUT_VCOM, lut_x3_vcom_gray, 42);
  sendCommandData(CMD_LUT_WW, lut_x3_ww_gray, 42);
  sendCommandData(CMD_LUT_BW, lut_x3_bw_gray, 42);
  sendCommandData(CMD_LUT_WB, lut_x3_wb_gray, 42);
  sendCommandData(CMD_LUT_BB, lut_x3_bb_gray, 42);
  sendCommandDataByte(CMD_VCOM_DATA_INTERVAL, 0x29, 0x07);

  if (!isScreenOn) {
    sendCommand(CMD_POWER_ON);
    waitForRefresh("power on");
    isScreenOn = true;
  }

  sendCommand(CMD_DISPLAY_REFRESH);
  waitForRefresh("grayscale refresh");

  if (turnOffScreen) {
    sendCommand(CMD_POWER_OFF);
    waitForRefresh("power off");
    isScreenOn = false;
  }

  redRamSynced = false;
  grayState.lsbValid = false;
  forceFullSyncNext = false;
  forcedConditionPassesNext = 0;
}

void EInkDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) { displayBuffer(mode, turnOffScreen); }

void EInkDisplay::deepSleep() {
  if (isScreenOn) {
    sendCommand(CMD_POWER_OFF);
    waitForRefresh("power off");
    isScreenOn = false;
  }
  sendCommand(CMD_DEEP_SLEEP);
  sendData(0xA5);
}

void EInkDisplay::saveFrameBufferAsPBM(const char* filename) {
#ifndef ARDUINO
  const uint8_t* buffer = getFrameBuffer();
  std::ofstream file(filename, std::ios::binary);
  if (!file) return;

  file << "P4\n";
  file << DISPLAY_HEIGHT << " " << DISPLAY_WIDTH << "\n";

  std::vector<uint8_t> rotatedBuffer((DISPLAY_HEIGHT / 8) * DISPLAY_WIDTH, 0);
  for (int outY = 0; outY < DISPLAY_WIDTH; outY++) {
    for (int outX = 0; outX < DISPLAY_HEIGHT; outX++) {
      const int inX = outY;
      const int inY = DISPLAY_HEIGHT - 1 - outX;
      const int inByteIndex = inY * DISPLAY_WIDTH_BYTES + (inX / 8);
      const int inBitPosition = 7 - (inX % 8);
      const bool isWhite = (buffer[inByteIndex] >> inBitPosition) & 1;

      const int outByteIndex = outY * (DISPLAY_HEIGHT / 8) + (outX / 8);
      const int outBitPosition = 7 - (outX % 8);
      if (!isWhite) rotatedBuffer[outByteIndex] |= (1 << outBitPosition);
    }
  }

  file.write(reinterpret_cast<const char*>(rotatedBuffer.data()), rotatedBuffer.size());
  file.close();
#else
  (void)filename;
#endif
}
