#include "SharedSpiLock.h"

#include <freertos/task.h>

namespace papyrix::spi {

namespace {

SemaphoreHandle_t gBusSem = nullptr;
portMUX_TYPE gBusMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t gBusOwner = nullptr;
uint32_t gBusDepth = 0;

void ensureInit() {
  if (gBusSem) return;

  portENTER_CRITICAL(&gBusMux);
  if (!gBusSem) {
    gBusSem = xSemaphoreCreateBinary();
    if (gBusSem) {
      xSemaphoreGive(gBusSem);
    }
  }
  portEXIT_CRITICAL(&gBusMux);
}

bool busTake(const TickType_t timeout) {
  ensureInit();
  if (!gBusSem) return false;

  TaskHandle_t self = xTaskGetCurrentTaskHandle();

  portENTER_CRITICAL(&gBusMux);
  if (gBusOwner == self) {
    ++gBusDepth;
    portEXIT_CRITICAL(&gBusMux);
    return true;
  }
  portEXIT_CRITICAL(&gBusMux);

  if (xSemaphoreTake(gBusSem, timeout) != pdTRUE) {
    return false;
  }

  portENTER_CRITICAL(&gBusMux);
  gBusOwner = self;
  gBusDepth = 1;
  portEXIT_CRITICAL(&gBusMux);
  return true;
}

void busGive() {
  portENTER_CRITICAL(&gBusMux);
  if (gBusDepth > 1) {
    --gBusDepth;
    portEXIT_CRITICAL(&gBusMux);
    return;
  }

  gBusOwner = nullptr;
  gBusDepth = 0;
  portEXIT_CRITICAL(&gBusMux);
  xSemaphoreGive(gBusSem);
}

}  // namespace

SemaphoreHandle_t sharedBusMutex() {
  ensureInit();
  return gBusSem;
}

SharedBusLock::SharedBusLock(const TickType_t timeout) { acquired_ = busTake(timeout); }

SharedBusLock::~SharedBusLock() {
  if (acquired_) {
    busGive();
  }
}

SharedBusLock::SharedBusLock(SharedBusLock&& other) noexcept : acquired_(other.acquired_) { other.acquired_ = false; }

}  // namespace papyrix::spi
