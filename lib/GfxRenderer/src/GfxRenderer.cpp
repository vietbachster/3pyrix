#include "GfxRenderer.h"

#include <ExternalFont.h>
#include <Logging.h>
#include <StreamingEpdFont.h>
#include <Utf8.h>

#include <cassert>

#define TAG "GFX"

namespace {

bool isCjkCodepoint(const uint32_t cp) {
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;
  if (cp >= 0x20000 && cp <= 0x2A6DF) return true;
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  return false;
}

}  // namespace

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) { fontMap.insert({fontId, font}); }

void GfxRenderer::removeFont(const int fontId) {
  fontMap.erase(fontId);
  _streamingFonts.erase(fontId);
  clearWidthCache();
}

bool GfxRenderer::tryResolveExternalFont() const {
  if (_externalFont) return true;
  if (!_externalFontResolver) return false;
  _externalFontResolver(_externalFontResolverCtx);
  // Resolver cleared after use — only triggers once
  _externalFontResolver = nullptr;
  _externalFontResolverCtx = nullptr;
  return _externalFont != nullptr;
}

static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y,
                                     int* rotatedX, int* rotatedY) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *rotatedX = y;
      *rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *rotatedX = EInkDisplay::DISPLAY_WIDTH - 1 - x;
      *rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *rotatedX = EInkDisplay::DISPLAY_WIDTH - 1 - y;
      *rotatedY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *rotatedX = x;
      *rotatedY = y;
      break;
    }
  }
}

void GfxRenderer::begin() {
  frameBuffer = einkDisplay.getFrameBuffer();
  assert(frameBuffer && "GfxRenderer::begin() called before display.begin()");
}

void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY);

  // Bounds checking against physical panel dimensions
  if (rotatedX < 0 || rotatedX >= EInkDisplay::DISPLAY_WIDTH || rotatedY < 0 ||
      rotatedY >= EInkDisplay::DISPLAY_HEIGHT) {
    LOG_ERR(TAG, "!! Outside range (%d, %d) -> (%d, %d)", x, y, rotatedX, rotatedY);
    return;
  }

  // Calculate byte position and bit position
  const uint16_t byteIndex = rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
  const uint8_t bitPosition = 7 - (rotatedX % 8);  // MSB first

  if (state) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (!text || !*text) return 0;

  if (fontMap.count(fontId) == 0) {
    LOG_ERR(TAG, "Font %d not found", fontId);
    return 0;
  }

  // Trigger lazy loading of deferred font variant (e.g., bold custom font)
  if (style != EpdFontFamily::REGULAR) {
    getStreamingFont(fontId, style);
  }

  // Check cache first (significant speedup during EPUB section creation)
  const uint64_t key = makeWidthCacheKey(fontId, text, style);
  size_t slot = key % MAX_WIDTH_CACHE_SIZE;
  for (size_t i = 0; i < MAX_WIDTH_CACHE_SIZE; i++) {
    if (widthCacheKeys_[slot] == key) {
      return widthCacheValues_[slot];
    }
    if (widthCacheKeys_[slot] == 0) break;  // Empty slot — not in cache
    slot = (slot + 1) % MAX_WIDTH_CACHE_SIZE;
  }

  int w = 0;
  if (isExternalFontAllowed(fontId) && ((_externalFont && _externalFont->isLoaded()) || tryResolveExternalFont())) {
    // Character-by-character: try external font first, then builtin fallback
    const auto& font = fontMap.at(fontId);
    const char* ptr = text;
    uint32_t cp;
    while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
      const int extWidth = getExternalGlyphWidth(cp);
      if (extWidth > 0) {
        w += extWidth;
      } else {
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        if (glyph) {
          w += glyph->advanceX;
        } else {
          const EpdGlyph* fallback = font.getGlyph('?', style);
          if (fallback) {
            w += fallback->advanceX;
          }
        }
      }
    }
  } else {
    // Advance-based measurement: sum glyph->advanceX per character.
    // This matches how drawText advances the cursor, avoiding the bounding-box
    // underestimate that causes justified lines to overflow the right margin.
    const auto& font = fontMap.at(fontId);
    const char* ptr = text;
    uint32_t cp;
    while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
      if (utf8IsCombiningMark(cp)) continue;
      const EpdGlyph* glyph = font.getGlyph(cp, style);
      if (glyph) {
        w += glyph->advanceX;
      } else {
        const EpdGlyph* fallback = font.getGlyph('?', style);
        if (fallback) w += fallback->advanceX;
      }
    }
  }

  // Insert into flat hash table; clear if full
  if (widthCacheCount_ >= MAX_WIDTH_CACHE_SIZE) {
    clearWidthCache();
  }

  slot = key % MAX_WIDTH_CACHE_SIZE;
  for (size_t i = 0; i < MAX_WIDTH_CACHE_SIZE; i++) {
    if (widthCacheKeys_[slot] == 0) {
      widthCacheKeys_[slot] = key;
      widthCacheValues_[slot] = static_cast<int16_t>(w);
      widthCacheCount_++;
      break;
    }
    slot = (slot + 1) % MAX_WIDTH_CACHE_SIZE;
  }

  return w;
}

void GfxRenderer::getTextBounds(const int fontId, const char* text, int* minX, int* minY, int* maxX, int* maxY,
                                const EpdFontFamily::Style style) const {
  if (minX == nullptr || minY == nullptr || maxX == nullptr || maxY == nullptr) {
    return;
  }

  if (text == nullptr || *text == '\0') {
    *minX = 0;
    *minY = 0;
    *maxX = 0;
    *maxY = 0;
    return;
  }

  if (fontMap.count(fontId) == 0) {
    LOG_ERR(TAG, "Font %d not found", fontId);
    *minX = 0;
    *minY = 0;
    *maxX = 0;
    *maxY = 0;
    return;
  }

  if (style != EpdFontFamily::REGULAR) {
    getStreamingFont(fontId, style);
  }

  const auto& font = fontMap.at(fontId);
  const int baselineY = getFontAscenderSize(fontId);
  *minX = 0;
  *minY = 0x7FFF;  // Sentinel: updated to actual glyph bottom (math Y-up)
  *maxX = 0;
  *maxY = 0;

  int cursorX = 0;
  int lastBaseX = 0;
  int lastBaseAdvance = 0;
  const char* ptr = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) {
      glyph = font.getGlyph('?', style);
    }
    if (!glyph) {
      continue;
    }

    int glyphX = cursorX + glyph->left;
    if (utf8IsCombiningMark(cp)) {
      glyphX = lastBaseX + lastBaseAdvance / 2 - glyph->width / 2 + glyph->left;
    } else {
      lastBaseX = cursorX;
      lastBaseAdvance = glyph->advanceX;
      cursorX += glyph->advanceX;
    }

    const int glyphMinY = baselineY + glyph->top - glyph->height;
    const int glyphMaxY = baselineY + glyph->top;
    if (glyphX < *minX) *minX = glyphX;
    if (glyphMinY < *minY) *minY = glyphMinY;
    if (glyphX + glyph->width > *maxX) *maxX = glyphX + glyph->width;
    if (glyphMaxY > *maxY) *maxY = glyphMaxY;
  }
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontMap.count(fontId) == 0) {
    LOG_ERR(TAG, "Font %d not found", fontId);
    return;
  }

  // Trigger lazy loading of deferred font variant (e.g., bold custom font)
  if (style != EpdFontFamily::REGULAR) {
    getStreamingFont(fontId, style);
  }

  const auto font = fontMap.at(fontId);

  // no printable characters
  if (!font.hasPrintableChars(text, style)) {
    return;
  }

  const int yPos = y + getFontAscenderSize(fontId);
  int xpos = x;
  int lastBaseX = x;
  int lastBaseAdvance = 0;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* glyph = font.getGlyph(cp, style);
      if (glyph) {
        int combX = lastBaseX + lastBaseAdvance / 2 - glyph->width / 2;
        int combY = yPos - 1;
        renderChar(font, cp, &combX, &combY, black, style, fontId);
      }
    } else {
      lastBaseX = xpos;
      renderChar(font, cp, &xpos, &yPos, black, style, fontId);
      lastBaseAdvance = xpos - lastBaseX;
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  int dx = abs(x2 - x1);
  int dy = abs(y2 - y1);
  int sx = (x1 < x2) ? 1 : -1;
  int sy = (y1 < y2) ? 1 : -1;
  int err = dx - dy;

  while (true) {
    drawPixel(x1, y1, state);

    if (x1 == x2 && y1 == y2) break;

    int e2 = 2 * err;

    if (e2 > -dy) {
      err -= dy;
      x1 += sx;
    }

    if (e2 < dx) {
      err += dx;
      y1 += sy;
    }
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  // TODO: Rotate bits
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY);
  einkDisplay.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                             const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // Use pre-allocated row buffers to avoid per-call heap allocation
  // Verify bitmap fits within our pre-allocated buffer sizes
  const size_t outputRowSize = static_cast<size_t>((bitmap.getWidth() + 3) / 4);
  const size_t rowBytesSize = static_cast<size_t>(bitmap.getRowBytes());

  if (!bitmapOutputRow_ || !bitmapRowBytes_) {
    LOG_ERR(TAG, "!! Bitmap row buffers not allocated");
    return;
  }

  if (outputRowSize > BITMAP_OUTPUT_ROW_SIZE || rowBytesSize > BITMAP_ROW_BYTES_SIZE) {
    LOG_ERR(TAG, "!! Bitmap too large for pre-allocated buffers (%zu > %zu or %zu > %zu)", outputRowSize,
            BITMAP_OUTPUT_ROW_SIZE, rowBytesSize, BITMAP_ROW_BYTES_SIZE);
    return;
  }

  // Inverse mapping: iterate destination pixels, sample from source.
  // This avoids gaps/overlaps that forward mapping causes when downscaling.
  const int destWidth = isScaled ? static_cast<int>(bitmap.getWidth() * scale) : bitmap.getWidth();
  const int destHeight = isScaled ? static_cast<int>(bitmap.getHeight() * scale) : bitmap.getHeight();
  const float invScale = isScaled ? (1.0f / scale) : 1.0f;

  int lastSrcY = -1;
  for (int destY = 0; destY < destHeight; destY++) {
    // For bottom-up BMPs, flip screen placement since readRow reads sequentially from file
    const int screenY = bitmap.isTopDown() ? (y + destY) : (y + destHeight - 1 - destY);
    if (screenY < 0) continue;
    if (screenY >= getScreenHeight()) continue;

    int srcY = isScaled ? static_cast<int>(destY * invScale) : destY;
    if (srcY >= bitmap.getHeight()) srcY = bitmap.getHeight() - 1;

    if (srcY != lastSrcY) {
      if (bitmap.readRow(bitmapOutputRow_, bitmapRowBytes_, srcY) != BmpReaderError::Ok) {
        LOG_ERR(TAG, "Failed to read row %d from bitmap", srcY);
        return;
      }
      lastSrcY = srcY;
    }

    for (int destX = 0; destX < destWidth; destX++) {
      const int screenX = x + destX;
      if (screenX < 0) continue;
      if (screenX >= getScreenWidth()) break;

      int bmpX = isScaled ? static_cast<int>(destX * invScale) : destX;
      if (bmpX >= bitmap.getWidth()) bmpX = bitmap.getWidth() - 1;

      const uint8_t val = bitmapOutputRow_[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }
}

static unsigned long renderStartMs = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  renderStartMs = millis();
  einkDisplay.clearScreen(color);
}

void GfxRenderer::clearArea(const int x, const int y, const int width, const int height, const uint8_t color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Rotate logical rectangle to physical coordinates
  int physX, physY, physW, physH;
  switch (orientation) {
    case Portrait:
      physX = y;
      physY = EInkDisplay::DISPLAY_HEIGHT - 1 - (x + width - 1);
      physW = height;
      physH = width;
      break;
    case LandscapeClockwise:
      physX = EInkDisplay::DISPLAY_WIDTH - 1 - (x + width - 1);
      physY = EInkDisplay::DISPLAY_HEIGHT - 1 - (y + height - 1);
      physW = width;
      physH = height;
      break;
    case PortraitInverted:
      physX = EInkDisplay::DISPLAY_WIDTH - 1 - (y + height - 1);
      physY = x;
      physW = height;
      physH = width;
      break;
    case LandscapeCounterClockwise:
    default:
      physX = x;
      physY = y;
      physW = width;
      physH = height;
      break;
  }

  // Validate bounds - region entirely outside display
  if (physX >= static_cast<int>(EInkDisplay::DISPLAY_WIDTH) || physY >= static_cast<int>(EInkDisplay::DISPLAY_HEIGHT) ||
      physX + physW <= 0 || physY + physH <= 0) {
    return;
  }

  // Clamp to display boundaries
  const int x_start = std::max(physX, 0);
  const int y_start = std::max(physY, 0);
  const int x_end = std::min(physX + physW - 1, static_cast<int>(EInkDisplay::DISPLAY_WIDTH - 1));
  const int y_end = std::min(physY + physH - 1, static_cast<int>(EInkDisplay::DISPLAY_HEIGHT - 1));

  // Calculate byte boundaries (8 pixels per byte)
  const int x_byte_start = x_start / 8;
  const int x_byte_end = x_end / 8;
  const int byte_width = x_byte_end - x_byte_start + 1;

  // Clear each row in the region
  for (int row = y_start; row <= y_end; row++) {
    const uint32_t buffer_offset = row * EInkDisplay::DISPLAY_WIDTH_BYTES + x_byte_start;
    memset(&frameBuffer[buffer_offset], color, byte_width);
  }
}

void GfxRenderer::invertScreen() const {
  for (int i = 0; i < EInkDisplay::BUFFER_SIZE; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(const EInkDisplay::RefreshMode refreshMode, bool turnOffScreen) const {
  if (renderStartMs > 0) {
    LOG_DBG(TAG, "Render took %lu ms", millis() - renderStartMs);
    renderStartMs = 0;
  }
  einkDisplay.displayBuffer(refreshMode, turnOffScreen);
}

void GfxRenderer::displayWindow(int x, int y, int width, int height, bool turnOffScreen) const {
  int physX, physY, physW, physH;
  switch (orientation) {
    case Portrait:
      physX = y;
      physY = EInkDisplay::DISPLAY_HEIGHT - x - width;
      physW = height;
      physH = width;
      break;
    case PortraitInverted:
      physX = EInkDisplay::DISPLAY_WIDTH - y - height;
      physY = x;
      physW = height;
      physH = width;
      break;
    case LandscapeClockwise:
      physX = EInkDisplay::DISPLAY_WIDTH - x - width;
      physY = EInkDisplay::DISPLAY_HEIGHT - y - height;
      physW = width;
      physH = height;
      break;
    case LandscapeCounterClockwise:
    default:
      physX = x;
      physY = y;
      physW = width;
      physH = height;
      break;
  }
  // E-ink controller requires x and width to be byte-aligned (multiples of 8 pixels).
  // Expand the window outward to the nearest byte boundaries.
  int alignedEnd = (physX + physW + 7) & ~7;
  physX = physX & ~7;
  physW = alignedEnd - physX;
  einkDisplay.displayWindow(physX, physY, physW, physH, turnOffScreen);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  std::string item = text;
  int itemWidth = getTextWidth(fontId, item.c_str(), style);
  while (itemWidth > maxWidth && item.length() > 8) {
    // Remove "..." first, then remove one UTF-8 char, then add "..." back
    if (item.length() >= 3 && item.compare(item.length() - 3, 3, "...") == 0) {
      item.resize(item.length() - 3);
    }
    utf8RemoveLastChar(item);
    item.append("...");
    itemWidth = getTextWidth(fontId, item.c_str(), style);
  }
  return item;
}

std::vector<std::string> GfxRenderer::breakWordWithHyphenation(const int fontId, const char* word, const int maxWidth,
                                                               const EpdFontFamily::Style style) const {
  std::vector<std::string> chunks;
  if (!word || *word == '\0') return chunks;

  std::string remaining = word;
  while (!remaining.empty()) {
    const int remainingWidth = getTextWidth(fontId, remaining.c_str(), style);
    if (remainingWidth <= maxWidth) {
      chunks.push_back(remaining);
      break;
    }

    // Find max chars that fit with hyphen
    std::string chunk;
    const char* ptr = remaining.c_str();
    const char* lastGoodPos = ptr;

    while (*ptr) {
      const char* nextChar = ptr;
      utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&nextChar));

      std::string testChunk = chunk;
      testChunk.append(ptr, nextChar - ptr);
      const int testWidth = getTextWidth(fontId, (testChunk + "-").c_str(), style);

      if (testWidth > maxWidth && !chunk.empty()) break;

      chunk = testChunk;
      lastGoodPos = nextChar;
      ptr = nextChar;
    }

    if (chunk.empty()) {
      // Single char too wide - force it
      const char* nextChar = remaining.c_str();
      utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&nextChar));
      chunk.append(remaining.c_str(), nextChar - remaining.c_str());
      lastGoodPos = nextChar;
    }

    if (lastGoodPos < remaining.c_str() + remaining.size()) {
      chunks.push_back(chunk + "-");
      remaining = remaining.substr(lastGoodPos - remaining.c_str());
    } else {
      chunks.push_back(chunk);
      remaining.clear();
    }
  }
  return chunks;
}

std::vector<std::string> GfxRenderer::wrapTextWithHyphenation(const int fontId, const char* text, const int maxWidth,
                                                              const int maxLines,
                                                              const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;
  if (!text || *text == '\0' || maxLines <= 0) {
    return lines;
  }

  std::string remaining = text;

  while (!remaining.empty() && static_cast<int>(lines.size()) < maxLines) {
    const bool isLastLine = static_cast<int>(lines.size()) == maxLines - 1;

    // Check if remaining text fits on current line
    const int remainingWidth = getTextWidth(fontId, remaining.c_str(), style);
    if (remainingWidth <= maxWidth) {
      lines.push_back(remaining);
      break;
    }

    // Find where to break the line
    std::string currentLine;
    const char* ptr = remaining.c_str();
    const char* lastBreakPoint = nullptr;
    std::string lineAtBreak;

    while (*ptr) {
      // Skip to end of current word
      const char* wordEnd = ptr;
      while (*wordEnd && *wordEnd != ' ') {
        utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&wordEnd));
      }

      // Build line up to this word
      std::string testLine = currentLine;
      if (!testLine.empty()) {
        testLine += ' ';
      }
      testLine.append(ptr, wordEnd - ptr);

      const int testWidth = getTextWidth(fontId, testLine.c_str(), style);

      if (testWidth <= maxWidth) {
        // Word fits, update current line and remember this as potential break point
        currentLine = testLine;
        lastBreakPoint = wordEnd;
        lineAtBreak = currentLine;

        // Move past the word and any spaces
        ptr = wordEnd;
        while (*ptr == ' ') {
          ptr++;
        }
      } else {
        // Word doesn't fit
        if (currentLine.empty()) {
          // Word alone is too long - use helper
          auto wordChunks = breakWordWithHyphenation(fontId, std::string(ptr, wordEnd - ptr).c_str(), maxWidth, style);
          for (size_t i = 0; i < wordChunks.size() && static_cast<int>(lines.size()) < maxLines; i++) {
            lines.push_back(wordChunks[i]);
          }
          // Update remaining to skip past the word
          ptr = wordEnd;
          while (*ptr == ' ') ptr++;
          remaining = ptr;
          break;
        } else if (lastBreakPoint) {
          // Line has content, break at last good point
          lines.push_back(lineAtBreak);
          // Skip spaces after break point
          const char* nextStart = lastBreakPoint;
          while (*nextStart == ' ') {
            nextStart++;
          }
          remaining = nextStart;
          break;
        }
      }

      if (*ptr == '\0') {
        // Reached end of text
        if (!currentLine.empty()) {
          lines.push_back(currentLine);
        }
        remaining.clear();
        break;
      }
    }

  }

  // If text remains after hitting maxLines, fold it into the last line and truncate with "..."
  if (!remaining.empty() && static_cast<int>(lines.size()) == maxLines) {
    std::string& lastLine = lines.back();
    if (getTextWidth(fontId, lastLine.c_str(), style) < maxWidth) {
      std::string combined = lastLine + " " + remaining;
      lastLine = truncatedText(fontId, combined.c_str(), maxWidth, style);
    } else {
      lastLine = truncatedText(fontId, lastLine.c_str(), maxWidth, style);
    }
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return EInkDisplay::DISPLAY_HEIGHT;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return EInkDisplay::DISPLAY_WIDTH;
  }
  return EInkDisplay::DISPLAY_HEIGHT;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return EInkDisplay::DISPLAY_WIDTH;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return EInkDisplay::DISPLAY_HEIGHT;
  }
  return EInkDisplay::DISPLAY_WIDTH;
}

int GfxRenderer::getSpaceWidth(const int fontId) const {
  auto it = fontMap.find(fontId);
  if (it == fontMap.end()) {
    LOG_ERR(TAG, "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* glyph = it->second.getGlyph(' ', EpdFontFamily::REGULAR);
  return glyph ? glyph->advanceX : 0;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    LOG_ERR(TAG, "Font %d not found", fontId);
    return 0;
  }

  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    LOG_ERR(TAG, "Font %d not found", fontId);
    return 0;
  }

  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getEffectiveLineHeight(const int fontId) const {
  int h = getLineHeight(fontId);
  if (isExternalFontAllowed(fontId) && _externalFont && _externalFont->isLoaded()) {
    int extH = _externalFont->getCharHeight() + 2;
    if (extH > h) h = extH;
  }
  return h;
}

bool GfxRenderer::fontSupportsGrayscale(const int fontId) const {
  auto it = fontMap.find(fontId);
  if (it == fontMap.end()) {
    return false;
  }
  const EpdFontData* data = it->second.getData();
  return data != nullptr && data->is2Bit;
}

void GfxRenderer::drawButtonHints(const int fontId, const char* btn1, const char* btn2, const char* btn3,
                                  const char* btn4, const bool black) const {
  const int pageHeight = getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = 46;
  constexpr int buttonY = 46;      // Distance from bottom (= buttonHeight, flush with screen edge)
  constexpr int textYOffset = 10;  // Distance from top of button to text baseline
  constexpr int buttonPositions[] = {38, 154, 268, 384};  // X3 528px portrait width
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      const int textWidth = getTextWidth(fontId, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      drawText(fontId, textX, pageHeight - buttonY + textYOffset, labels[i], black);
    }
  }
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X (simple bubble sort, numPoints is small)
    for (int i = 0; i < nodes - 1; i++) {
      for (int k = i + 1; k < nodes; k++) {
        if (nodeX[i] > nodeX[k]) {
          int temp = nodeX[i];
          nodeX[i] = nodeX[k];
          nodeX[k] = temp;
        }
      }
    }

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() { return EInkDisplay::BUFFER_SIZE; }

void GfxRenderer::grayscaleRevert() const { einkDisplay.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { einkDisplay.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { einkDisplay.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer(bool turnOffScreen) const { einkDisplay.displayGrayBuffer(turnOffScreen); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing one large contiguous X3-sized buffer.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR(TAG, "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t remaining = static_cast<size_t>(EInkDisplay::BUFFER_SIZE) - offset;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, remaining);
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR(TAG, "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG(TAG, "Stored BW buffer in %zu chunks (%zu bytes each)", BW_BUFFER_NUM_CHUNKS, BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if any all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if chunk is missing
    if (!bwBufferChunks[i]) {
      LOG_ERR(TAG, "!! BW buffer chunks not stored - this is likely a bug");
      freeBwBufferChunks();
      return;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t remaining = static_cast<size_t>(EInkDisplay::BUFFER_SIZE) - offset;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, remaining);
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  einkDisplay.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG(TAG, "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const { einkDisplay.cleanupGrayscaleBuffers(frameBuffer); }

// Pixel loop for glyph rendering, templated on Is2Bit to hoist the bitmap-format
// branch out of the inner loop. The compiler generates two separate loop bodies
// with no per-pixel branch, yielding 15-23% faster drawText.
template <bool Is2Bit>
static void renderGlyphPixels(const GfxRenderer& renderer, const uint8_t* bitmap,
                               uint8_t width, uint8_t height, int baseX, int baseY,
                               int left, int top, int screenWidth, int screenHeight,
                               bool pixelState, GfxRenderer::RenderMode renderMode) {
  for (int glyphY = 0; glyphY < height; glyphY++) {
    const int screenY = baseY - top + glyphY;
    if (screenY < 0 || screenY >= screenHeight) continue;

    for (int glyphX = 0; glyphX < width; glyphX++) {
      const int screenX = baseX + left + glyphX;
      if (screenX < 0 || screenX >= screenWidth) continue;

      const int pixelPosition = glyphY * width + glyphX;

      if constexpr (Is2Bit) {
        const uint8_t byte = bitmap[pixelPosition / 4];
        const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
        // Font encoding: 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black.
        // Swap to screen convention: 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white.
        const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

        if (renderMode == GfxRenderer::BW && bmpVal < 3) {
          renderer.drawPixel(screenX, screenY, pixelState);
        } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
          renderer.drawPixel(screenX, screenY, false);
        } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
          renderer.drawPixel(screenX, screenY, false);
        }
      } else {
        const uint8_t byte = bitmap[pixelPosition / 8];
        const uint8_t bit_index = 7 - (pixelPosition % 8);

        if ((byte >> bit_index) & 1) {
          renderer.drawPixel(screenX, screenY, pixelState);
        }
      }
    }
  }
}

void GfxRenderer::renderChar(const EpdFontFamily& fontFamily, const uint32_t cp, int* x, const int* y,
                             const bool pixelState, const EpdFontFamily::Style style, const int fontId) const {
  // Try external font first — covers CJK and optionally Latin from .bin fonts
  if (isExternalFontAllowed(fontId) && (_externalFont || tryResolveExternalFont()) && _externalFont->isLoaded()) {
    const uint8_t* extBitmap = _externalFont->getGlyph(cp);
    if (extBitmap) {
      renderExternalGlyph(cp, x, *y - fontFamily.getData(EpdFontFamily::REGULAR)->ascender, pixelState, extBitmap);
      return;
    }
  }

  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    // For whitespace characters missing from font, advance by space width instead of rendering '?'
    if (cp == 0x2002 || cp == 0x2003 || cp == 0x00A0) {  // EN SPACE, EM SPACE, NBSP
      const EpdGlyph* spaceGlyph = fontFamily.getGlyph(' ', style);
      if (spaceGlyph) {
        *x += spaceGlyph->advanceX;
        if (cp == 0x2003) *x += spaceGlyph->advanceX;  // EM SPACE = 2x width
        return;
      }
    }
    glyph = fontFamily.getGlyph('?', style);
  }

  // no glyph?
  if (!glyph) {
    LOG_ERR(TAG, "No glyph for codepoint %d", cp);
    return;
  }

  const int is2Bit = fontFamily.getData(style)->is2Bit;
  const uint32_t offset = glyph->dataOffset;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;

  // Bitmap lookup bypasses getStreamingFont() (no lazy resolver) for performance.
  // Font variants are already resolved during layout (word width measurement).
  const uint8_t* bitmap = nullptr;
  auto streamingIt = _streamingFonts.find(fontId);
  if (streamingIt != _streamingFonts.end()) {
    int idx = EpdFontFamily::externalStyleIndex(style);
    StreamingEpdFont* sf = streamingIt->second[idx];
    if (!sf) sf = streamingIt->second[EpdFontFamily::REGULAR];
    if (sf) {
      bitmap = sf->getGlyphBitmap(glyph);
    }
  }
  if (!bitmap && fontFamily.getData(style)->bitmap) {
    // Fall back to standard EpdFont bitmap access
    bitmap = &fontFamily.getData(style)->bitmap[offset];
  }

  if (bitmap != nullptr) {
    const int screenHeight = getScreenHeight();
    const int screenWidth = getScreenWidth();
    // Dispatch to the appropriate template instantiation so the is2Bit branch
    // is resolved at compile time rather than tested on every pixel.
    if (is2Bit) {
      renderGlyphPixels<true>(*this, bitmap, width, height, *x, *y, left, glyph->top,
                              screenWidth, screenHeight, pixelState, renderMode);
    } else {
      renderGlyphPixels<false>(*this, bitmap, width, height, *x, *y, left, glyph->top,
                               screenWidth, screenHeight, pixelState, renderMode);
    }
  }

  if (!utf8IsCombiningMark(cp)) {
    *x += glyph->advanceX;
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}

void GfxRenderer::allocateBitmapRowBuffers() {
  bitmapOutputRow_ = static_cast<uint8_t*>(malloc(BITMAP_OUTPUT_ROW_SIZE));
  bitmapRowBytes_ = static_cast<uint8_t*>(malloc(BITMAP_ROW_BYTES_SIZE));

  if (!bitmapOutputRow_ || !bitmapRowBytes_) {
    LOG_ERR(TAG, "!! Failed to allocate bitmap row buffers");
    freeBitmapRowBuffers();
  }
}

void GfxRenderer::freeBitmapRowBuffers() {
  if (bitmapOutputRow_) {
    free(bitmapOutputRow_);
    bitmapOutputRow_ = nullptr;
  }
  if (bitmapRowBytes_) {
    free(bitmapRowBytes_);
    bitmapRowBytes_ = nullptr;
  }
}

void GfxRenderer::renderExternalGlyph(const uint32_t cp, int* x, const int y, const bool pixelState,
                                      const uint8_t* bitmap) const {
  if (!_externalFont || !_externalFont->isLoaded()) {
    return;
  }

  if (!bitmap) bitmap = _externalFont->getGlyph(cp);
  if (!bitmap) {
    // Glyph not found - advance by 1/3 char width as fallback
    *x += _externalFont->getCharWidth() / 3;
    return;
  }

  uint8_t minX = 0;
  uint8_t advanceX = 0;
  if (!_externalFont->getGlyphMetrics(cp, &minX, &advanceX)) {
    advanceX = _externalFont->getCharWidth();
  }

  const int w = _externalFont->getCharWidth();
  const int h = _externalFont->getCharHeight();
  const int bytesPerRow = _externalFont->getBytesPerRow();
  const int screenHeight = getScreenHeight();
  const int screenWidth = getScreenWidth();

  for (int glyphY = 0; glyphY < h; glyphY++) {
    const int screenY = y + glyphY;
    if (screenY < 0 || screenY >= screenHeight) continue;

    for (int glyphX = minX; glyphX < w; glyphX++) {
      const int screenX = *x + glyphX - minX;
      if (screenX < 0 || screenX >= screenWidth) continue;

      const int byteIdx = glyphY * bytesPerRow + (glyphX / 8);
      const int bitIdx = 7 - (glyphX % 8);
      if ((bitmap[byteIdx] >> bitIdx) & 1) {
        drawPixel(screenX, screenY, pixelState);
      }
    }
  }

  *x += advanceX;
}

int GfxRenderer::getExternalGlyphWidth(const uint32_t cp) const {
  if (!_externalFont && isCjkCodepoint(cp)) {
    tryResolveExternalFont();
  }
  if (!_externalFont || !_externalFont->isLoaded()) {
    return 0;
  }

  // Ensure glyph is loaded to get metrics; return 0 if not found
  // so caller falls back to builtin font width
  if (!_externalFont->getGlyph(cp)) {
    return 0;
  }

  uint8_t minX = 0;
  uint8_t advanceX = _externalFont->getCharWidth();
  if (_externalFont->getGlyphMetrics(cp, &minX, &advanceX)) {
    return advanceX;
  }

  // Fallback to full character width
  return _externalFont->getCharWidth();
}
