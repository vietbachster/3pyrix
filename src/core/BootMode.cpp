#include "BootMode.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SDCardManager.h>

#include "../ThemeManager.h"
#include "Core.h"
#include "PapyrixSettings.h"

// Access global renderer from main.cpp
extern GfxRenderer renderer;

#define TAG "BOOT"

namespace papyrix {

// Access global core from main.cpp
extern Core core;

// Cached transition for current boot
static ModeTransition cachedTransition = {};
static bool transitionCached = false;
static RTC_DATA_ATTR SleepResumeTransition rtcSleepResumeTransition = {};
static SleepResumeTransition cachedSleepResumeTransition = {};
static bool sleepResumeCached = false;

BootMode detectBootMode() {
  LOG_DBG(TAG, "Checking boot mode...");

  if (rtcSleepResumeTransition.isValid() && rtcSleepResumeTransition.bookPath[0] != '\0' &&
      SdMan.exists(rtcSleepResumeTransition.bookPath)) {
    LOG_INF(TAG, "Sleep-resume transition: path=%s, returnTo=%d", rtcSleepResumeTransition.bookPath,
            static_cast<int>(rtcSleepResumeTransition.returnTo));

    cachedTransition.magic = ModeTransition::MAGIC;
    cachedTransition.mode = BootMode::READER;
    cachedTransition.returnTo = rtcSleepResumeTransition.returnTo;
    strncpy(cachedTransition.bookPath, rtcSleepResumeTransition.bookPath, sizeof(cachedTransition.bookPath) - 1);
    cachedTransition.bookPath[sizeof(cachedTransition.bookPath) - 1] = '\0';
    transitionCached = true;

    cachedSleepResumeTransition = rtcSleepResumeTransition;
    sleepResumeCached = true;

    // Reader sleep writes a one-shot pending reader transition to settings so
    // early boot can skip wake verification before RTC state is inspected.
    // Once RTC resume data has been consumed, clear the persistent flag to
    // avoid reopening the last book on a later unrelated reset.
    if (core.settings.pendingTransition != 0) {
      clearTransition();
    }
    clearSleepResumeTransition();

    return BootMode::READER;
  }

  // Check settings for pending UI transition (1=UI mode)
  if (core.settings.pendingTransition == 1) {
    LOG_INF(TAG, "Pending UI transition, returnTo=%d", core.settings.transitionReturnTo);

    // Cache transition info before clearing (so initUIMode can detect mode transition)
    cachedTransition.magic = ModeTransition::MAGIC;
    cachedTransition.mode = BootMode::UI;
    cachedTransition.returnTo = static_cast<ReturnTo>(core.settings.transitionReturnTo);
    cachedTransition.bookPath[0] = '\0';
    transitionCached = true;

    clearTransition();
    return BootMode::UI;
  }

  // Check settings for pending Reader transition (2=Reader mode)
  if (core.settings.pendingTransition == 2 && core.settings.lastBookPath[0] != '\0' &&
      SdMan.exists(core.settings.lastBookPath)) {
    LOG_INF(TAG, "Pending Reader transition: path=%s, returnTo=%d", core.settings.lastBookPath,
            core.settings.transitionReturnTo);

    // Set up cached transition for reader mode
    cachedTransition.magic = ModeTransition::MAGIC;
    cachedTransition.mode = BootMode::READER;
    cachedTransition.returnTo = static_cast<ReturnTo>(core.settings.transitionReturnTo);
    strncpy(cachedTransition.bookPath, core.settings.lastBookPath, sizeof(cachedTransition.bookPath) - 1);
    cachedTransition.bookPath[sizeof(cachedTransition.bookPath) - 1] = '\0';
    transitionCached = true;

    // Clear the pending flag to prevent boot loop
    clearTransition();

    return BootMode::READER;
  }

  // No pending transition - check "Last Document" startup behavior setting
  if (core.settings.startupBehavior == Settings::StartupLastDocument && core.settings.lastBookPath[0] != '\0' &&
      SdMan.exists(core.settings.lastBookPath)) {
    LOG_INF(TAG, "'Last Document' startup: %s", core.settings.lastBookPath);

    // Set up cached transition for reader mode
    cachedTransition.magic = ModeTransition::MAGIC;
    cachedTransition.mode = BootMode::READER;
    cachedTransition.returnTo = ReturnTo::HOME;
    strncpy(cachedTransition.bookPath, core.settings.lastBookPath, sizeof(cachedTransition.bookPath) - 1);
    cachedTransition.bookPath[sizeof(cachedTransition.bookPath) - 1] = '\0';
    transitionCached = true;

    // Clear lastBookPath to prevent boot loop if reader fails
    // ReaderState will re-save it after successful open
    core.settings.lastBookPath[0] = '\0';
    core.settings.saveToFile();

    return BootMode::READER;
  }

  LOG_DBG(TAG, "No transition pending, using default UI mode");
  return BootMode::UI;
}

const ModeTransition& getTransition() { return cachedTransition; }

const SleepResumeTransition& getSleepResumeTransition() { return cachedSleepResumeTransition; }

bool hasSleepResumeTransition() { return sleepResumeCached && cachedSleepResumeTransition.isValid(); }

void saveTransition(BootMode mode, const char* bookPath, ReturnTo returnTo) {
  // Only set lastBookPath when transitioning to Reader mode
  // For UI transitions, keep existing lastBookPath for "Continue reading"
  if (mode == BootMode::READER && bookPath && bookPath[0] != '\0') {
    strncpy(core.settings.lastBookPath, bookPath, sizeof(core.settings.lastBookPath) - 1);
    core.settings.lastBookPath[sizeof(core.settings.lastBookPath) - 1] = '\0';
  }

  // Store mode: 1=UI, 2=Reader
  core.settings.pendingTransition = (mode == BootMode::UI) ? 1 : 2;
  core.settings.transitionReturnTo = static_cast<uint8_t>(returnTo);
  core.settings.saveToFile();

  LOG_INF(TAG, "Saved transition to settings: mode=%d, returnTo=%d, path=%s", static_cast<int>(mode),
          static_cast<int>(returnTo), core.settings.lastBookPath);
}

void saveSleepResumeTransition(const char* bookPath, ReturnTo returnTo, const ProgressManager::Progress& progress) {
  rtcSleepResumeTransition.magic = SleepResumeTransition::MAGIC;
  rtcSleepResumeTransition.returnTo = returnTo;
  rtcSleepResumeTransition.progress = progress;
  rtcSleepResumeTransition.bookPath[0] = '\0';

  if (bookPath && bookPath[0] != '\0') {
    strncpy(rtcSleepResumeTransition.bookPath, bookPath, sizeof(rtcSleepResumeTransition.bookPath) - 1);
    rtcSleepResumeTransition.bookPath[sizeof(rtcSleepResumeTransition.bookPath) - 1] = '\0';
  }

  LOG_INF(TAG, "Saved sleep-resume transition: path=%s spine=%d page=%d flat=%u",
          rtcSleepResumeTransition.bookPath, progress.spineIndex, progress.sectionPage, progress.flatPage);
}

void clearSleepResumeTransition() {
  rtcSleepResumeTransition.invalidate();
  rtcSleepResumeTransition.bookPath[0] = '\0';
}

void clearTransition() {
  core.settings.pendingTransition = 0;
  core.settings.transitionReturnTo = 0;
  core.settings.saveToFile();
  LOG_DBG(TAG, "Cleared pending transition");
}

void clearCachedTransition() {
  cachedTransition.invalidate();
  cachedTransition.bookPath[0] = '\0';
  transitionCached = false;
  LOG_DBG(TAG, "Cleared cached transition");
}

void showTransitionNotification(const char* message) {
  const Theme& theme = THEME_MANAGER.current();

  renderer.clearScreen(theme.backgroundColor);

  // Draw centered message
  const int screenHeight = renderer.getScreenHeight();
  const int y = screenHeight / 2 - 20;

  renderer.drawCenteredText(theme.uiFontId, y, message, theme.primaryTextBlack, REGULAR);

  // Display immediately (partial refresh for speed)
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);

  LOG_DBG(TAG, "Displayed notification: %s", message);
}

}  // namespace papyrix
