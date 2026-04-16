#pragma once
#include <cmath>
#include <cstdint>

struct RenderConfig {
  int fontId = 0;
  float lineCompression = 0.0f;
  uint8_t readerFontSize = 1;
  uint8_t indentLevel = 0;
  uint8_t spacingLevel = 0;
  uint8_t paragraphAlignment = 0;
  bool hyphenation = false;
  bool showImages = false;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;

  RenderConfig() = default;
  RenderConfig(int fontId, float lineCompression, uint8_t readerFontSize, uint8_t indentLevel, uint8_t spacingLevel,
               uint8_t paragraphAlignment, bool hyphenation, bool showImages, uint16_t viewportWidth,
               uint16_t viewportHeight)
      : fontId(fontId),
        lineCompression(lineCompression),
        readerFontSize(readerFontSize),
        indentLevel(indentLevel),
        spacingLevel(spacingLevel),
        paragraphAlignment(paragraphAlignment),
        hyphenation(hyphenation),
        showImages(showImages),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight) {}

  bool isLandscape() const { return viewportWidth > viewportHeight; }

  int getReadingLineHeight() const {
    const bool landscape = isLandscape();
    return readerFontSize == 3 ? (landscape ? 55 : 54) : (landscape ? 45 : 42);
  }

  bool operator==(const RenderConfig& o) const {
    return fontId == o.fontId && std::abs(lineCompression - o.lineCompression) < 1e-6f &&
           readerFontSize == o.readerFontSize && indentLevel == o.indentLevel && spacingLevel == o.spacingLevel &&
           paragraphAlignment == o.paragraphAlignment && hyphenation == o.hyphenation && showImages == o.showImages &&
           viewportWidth == o.viewportWidth && viewportHeight == o.viewportHeight;
  }
  bool operator!=(const RenderConfig& o) const { return !(*this == o); }
};
