#pragma once

#include <RenderConfig.h>

#include <cstdint>

#include "Result.h"

struct Theme;

namespace papyrix {

namespace drivers {
class Storage;
}

struct Settings {
  // Sleep screen display modes
  enum SleepScreenMode : uint8_t { SleepDark = 0, SleepLight = 1, SleepCustom = 2, SleepCover = 3 };

  // Screen orientation
  enum Orientation : uint8_t {
    Portrait = 0,      // 528x792 logical coordinates (current default)
    LandscapeCW = 1,   // 792x528 logical coordinates, rotated 180deg (swap top/bottom)
    Inverted = 2,      // 528x792 logical coordinates, inverted
    LandscapeCCW = 3,  // 792x528 logical coordinates, native panel orientation
  };

  // Reader font size. Numeric values are kept for compatibility with previously saved settings.
  enum FontSize : uint8_t { FontSmall = 1, FontLarge = 3 };

  // Side button layout
  enum SideButtonLayout : uint8_t { PrevNext = 0, NextPrev = 1 };

  // Front button layout
  enum FrontButtonLayout : uint8_t { FrontBCLR = 0, FrontLRBC = 1 };

  // Auto-sleep timeout (in minutes)
  enum AutoSleepTimeout : uint8_t { Sleep5Min = 0, Sleep10Min = 1, Sleep15Min = 2, Sleep30Min = 3, SleepNever = 4 };

  // Pages per full refresh (to clear ghosting)
  enum PagesPerRefresh : uint8_t { PPR1 = 0, PPR5 = 1, PPR10 = 2, PPR15 = 3, PPR30 = 4 };

  // Paragraph alignment (values match TextBlock::BLOCK_STYLE)
  enum ParagraphAlignment : uint8_t { AlignJustified = 0, AlignLeft = 1, AlignCenter = 2, AlignRight = 3 };

  // Text layout presets
  enum TextLayout : uint8_t { LayoutCompact = 0, LayoutStandard = 1, LayoutLarge = 2 };

  // Line spacing presets
  enum LineSpacing : uint8_t { SpacingCompact = 0, SpacingNormal = 1, SpacingRelaxed = 2, SpacingLarge = 3 };

  // Short power button press actions
  enum PowerButtonAction : uint8_t { PowerIgnore = 0, PowerSleep = 1, PowerPageTurn = 2 };

  // Startup behavior
  enum StartupBehavior : uint8_t { StartupLastDocument = 0, StartupHome = 1 };

  // Auto page turn mode
  enum AutoPageTurnMode : uint8_t { AutoPageOff = 0, AutoPageOn = 1 };

  // Auto page turn WPM presets (used when auto page turn is ON)
  enum AutoPageWpm : uint8_t { WpmSlow = 0, WpmNormal = 1, WpmFast = 2, WpmVeryFast = 3, WpmExpert = 4 };

  // Settings fields (same order as CrossPointSettings for binary compatibility)
  uint8_t sleepScreen = SleepDark;
  uint8_t _reserved2 = 0;  // was statusBar, kept for serialization compatibility
  uint8_t textLayout = LayoutStandard;
  uint8_t shortPwrBtn = PowerIgnore;
  uint8_t orientation = Portrait;
  uint8_t fontSize = FontSmall;
  uint8_t pagesPerRefresh = PPR15;
  uint8_t sideButtonLayout = PrevNext;
  uint8_t autoSleepMinutes = Sleep10Min;
  uint8_t paragraphAlignment = AlignJustified;
  uint8_t hyphenation = 1;
  uint8_t textAntiAliasing = 0;
  uint8_t showImages = 1;
  uint8_t startupBehavior = StartupLastDocument;
  uint8_t _reserved = 0;  // was coverDithering, kept for serialization compatibility
  uint8_t lineSpacing = SpacingNormal;
  char themeName[32] = "light";
  char lastBookPath[256] = "";          // Path to last opened book
  uint8_t pendingTransition = 0;        // 0=none, 1=UI, 2=Reader
  uint8_t transitionReturnTo = 0;       // ReturnTo enum value (0=HOME, 1=FILE_MANAGER)
  uint8_t sunlightFadingFix = 0;        // Power down display after refresh (SSD1677 UV protection)
  char fileListDir[256] = "/";          // FileListState: last directory
  char fileListSelectedName[128] = "";  // FileListState: last selected filename
  uint16_t fileListSelectedIndex = 0;   // FileListState: last selected index
  uint8_t frontButtonLayout = FrontBCLR;
  uint8_t autoPageTurnMode = AutoPageOff;   // OFF/ON (ON uses adaptive timing)
  uint8_t autoPageTurnWpm = WpmNormal;      // WPM preset for auto page turn when ON
  uint8_t tiltPageTurn = 0;                 // X3 tilt-based page turn via QMI8658

  // Persistence (using drivers::Storage wrapper)
  Result<void> load(drivers::Storage& storage);
  Result<void> save(drivers::Storage& storage) const;

  // Legacy persistence (uses SdMan directly - for early init before Core)
  bool loadFromFile();
  bool saveToFile() const;

  // Computed values
  uint16_t getPowerButtonDuration() const { return (shortPwrBtn == PowerSleep) ? 10 : 400; }

  uint32_t getAutoSleepTimeoutMs() const {
    switch (autoSleepMinutes) {
      case Sleep5Min:
        return 5 * 60 * 1000;
      case Sleep15Min:
        return 15 * 60 * 1000;
      case Sleep30Min:
        return 30 * 60 * 1000;
      case SleepNever:
        return 0;
      default:
        return 10 * 60 * 1000;
    }
  }

  // Returns auto page turn delay in milliseconds.
  // wordCount: number of words on current page (-1 = not available, auto page turn unsupported).
  uint32_t getAutoPageTurnDelayMs(int wordCount) const {
    if (autoPageTurnMode == AutoPageOff || wordCount < 0) {
      return 0;
    }
    // ON mode: delay based on word count and WPM preset
    static constexpr uint16_t kWpmTable[] = {450, 490, 495, 500, 505};
    const uint16_t wpm = kWpmTable[autoPageTurnWpm < 5 ? autoPageTurnWpm : 1];
    const uint32_t delay = (static_cast<uint32_t>(wordCount) * 60000u) / wpm;
    // Clamp: minimum 5 seconds, maximum 10 minutes
    if (delay < 5000u) return 5000u;
    if (delay > 600000u) return 600000u;
    return delay;
  }

  int getReaderFontId(const Theme& theme) const;
  bool hasExternalReaderFont(const Theme& theme) const;

  int getPagesPerRefreshValue() const {
    constexpr int values[] = {1, 5, 10, 15, 30};
    return values[pagesPerRefresh];
  }

  uint8_t getIndentLevel() const { return 2; }

  uint8_t getSpacingLevel() const { return 0; }

  float getLineCompression() const { return 1.08f; }

  RenderConfig getRenderConfig(const Theme& theme, uint16_t viewportWidth, uint16_t viewportHeight) const;
};

}  // namespace papyrix
