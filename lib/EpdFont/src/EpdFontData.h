// From
// https://github.com/vroland/epdiy/blob/c61e9e923ce2418150d54f88cea5d196cdc40c54/src/epd_internals.h

#pragma once
#include <Arduino.h>

#include <cstdint>

// Helpers for positioning Unicode combining marks (U+0300 ff.) over a
// preceding base glyph without GPOS anchor tables.
namespace combiningMark {

constexpr int MIN_GAP_PX = 1;

// Compute the cursor X at which to render a combining mark so its bitmap
// is visually centered over the base glyph's bitmap.
constexpr int centerOver(int baseCursorPos, int baseLeft, int baseWidth, int markLeft, int markWidth) {
  return baseCursorPos + baseLeft + baseWidth / 2 - markWidth / 2 - markLeft;
}

// For combining marks that sit entirely above the baseline, compute how many
// pixels to raise the mark so there is at least MIN_GAP_PX between its bottom
// edge and the top of the base glyph. Returns 0 for marks that extend to or
// below the baseline (cedilla, dot-below, ogonek, etc.).
constexpr int raiseAboveBase(int markTop, int markHeight, int baseTop) {
  if (markTop - markHeight <= 0) return 0;
  const int gap = markTop - markHeight - baseTop;
  return (gap < MIN_GAP_PX) ? (MIN_GAP_PX - gap) : 0;
}

}  // namespace combiningMark

/// Font data stored PER GLYPH
typedef struct {
  uint8_t width;        ///< Bitmap dimensions in pixels
  uint8_t height;       ///< Bitmap dimensions in pixels
  uint8_t advanceX;     ///< Distance to advance cursor (x axis)
  int16_t left;         ///< X dist from cursor pos to UL corner
  int16_t top;          ///< Y dist from cursor pos to UL corner
  uint16_t dataLength;  ///< Size of the font data.
  uint32_t dataOffset;  ///< Pointer into EpdFont->bitmap
} EpdGlyph;

/// Glyph interval structure
typedef struct {
  uint32_t first;   ///< The first unicode code point of the interval
  uint32_t last;    ///< The last unicode code point of the interval
  uint32_t offset;  ///< Index of the first code point into the glyph array
} EpdUnicodeInterval;

// Maps a codepoint to a kerning class ID, sorted by codepoint for binary search.
// Class IDs are 1-based; codepoints not in the table have implicit class 0 (no kerning).
typedef struct {
  uint16_t codepoint;  ///< Unicode codepoint
  uint8_t classId;     ///< 1-based kerning class ID
} __attribute__((packed)) EpdKernClassEntry;

// Ligature substitution for a specific glyph pair, sorted by `pair` for binary search.
// `pair` encodes (leftCodepoint << 16 | rightCodepoint) for single-key lookup.
typedef struct {
  uint32_t pair;        ///< Packed codepoint pair (left << 16 | right)
  uint32_t ligatureCp;  ///< Codepoint of the replacement ligature glyph
} __attribute__((packed)) EpdLigaturePair;

/// Data stored for FONT AS A WHOLE
typedef struct {
  const uint8_t* bitmap;                ///< Glyph bitmaps, concatenated
  const EpdGlyph* glyph;                ///< Glyph array
  const EpdUnicodeInterval* intervals;  ///< Valid unicode intervals for this font
  uint32_t intervalCount;               ///< Number of unicode intervals.
  uint8_t advanceY;                     ///< Newline distance (y axis)
  int ascender;                         ///< Maximal height of a glyph above the base line
  int descender;                        ///< Maximal height of a glyph below the base line
  bool is2Bit;
  const EpdKernClassEntry* kernLeftClasses;   ///< Sorted left-side class map (nullptr if none)
  const EpdKernClassEntry* kernRightClasses;  ///< Sorted right-side class map (nullptr if none)
  const int8_t* kernMatrix;                   ///< Flat leftClassCount x rightClassCount matrix, in pixel units
  uint16_t kernLeftEntryCount;                ///< Entries in kernLeftClasses
  uint16_t kernRightEntryCount;               ///< Entries in kernRightClasses
  uint8_t kernLeftClassCount;                 ///< Number of distinct left classes (matrix rows)
  uint8_t kernRightClassCount;                ///< Number of distinct right classes (matrix columns)
  const EpdLigaturePair* ligaturePairs;       ///< Sorted ligature pair table (nullptr if none)
  uint32_t ligaturePairCount;                 ///< Number of entries in ligaturePairs
} EpdFontData;
