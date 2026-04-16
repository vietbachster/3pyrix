#include "BootSleepViews.h"

#include <EInkDisplay.h>

namespace ui {

void render(const GfxRenderer& r, const Theme& t, const BootView& v) {
  const auto pageWidth = r.getScreenWidth();
  const auto pageHeight = r.getScreenHeight();
  (void)pageWidth;

  r.clearScreen(t.backgroundColor);

  r.drawCenteredText(t.uiFontId, pageHeight / 2 - 10, "Papyrix", t.primaryTextBlack);
  r.drawCenteredText(t.smallFontId, pageHeight / 2 + 30, v.status, t.primaryTextBlack);
  r.drawCenteredText(t.smallFontId, pageHeight - 30, v.version, t.primaryTextBlack);

  r.displayBuffer();
}

void render(const GfxRenderer& r, const Theme& t, const SleepView& v) {
  const auto pageWidth = r.getScreenWidth();
  const auto pageHeight = r.getScreenHeight();
  (void)pageWidth;

  // Always start with background color (light)
  r.clearScreen(t.backgroundColor);

  if (v.mode == SleepView::Mode::Text) {
    r.drawCenteredText(t.uiFontId, pageHeight / 2 - 10, "Papyrix", t.primaryTextBlack);
    r.drawCenteredText(t.smallFontId, pageHeight / 2 + 30, "SLEEPING", t.primaryTextBlack);
    if (v.darkMode) {
      r.invertScreen();
    }
  } else if (v.mode == SleepView::Mode::Black) {
    r.clearScreen(0x00);
  } else if (v.mode == SleepView::Mode::BookCover || v.mode == SleepView::Mode::Custom) {
    // Image modes: center the image
    if (v.imageData != nullptr) {
      const int imageX = (pageWidth - v.imageWidth) / 2;
      const int imageY = (pageHeight - v.imageHeight) / 2;
      r.drawImage(v.imageData, imageX, imageY, v.imageWidth, v.imageHeight);
    }
  }

  // Use HALF_REFRESH for sleep (matches old SleepActivity)
  r.displayBuffer(EInkDisplay::HALF_REFRESH);
}

}  // namespace ui
