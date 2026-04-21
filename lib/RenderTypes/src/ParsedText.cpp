#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Hyphenation.h>
#include <Logging.h>
#include <Utf8.h>

#define TAG "TEXT"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

// Knuth-Plass algorithm constants
constexpr float INFINITY_PENALTY = 10000.0f;
constexpr float LINE_PENALTY = 50.0f;

// Soft hyphen (U+00AD) as UTF-8 bytes
constexpr unsigned char SOFT_HYPHEN_BYTE1 = 0xC2;
constexpr unsigned char SOFT_HYPHEN_BYTE2 = 0xAD;

// Known attaching punctuation (including UTF-8 sequences)
const std::vector<std::string> punctuation = {
    ".",
    ",",
    "!",
    "?",
    ";",
    ":",
    "\"",
    "'",
    "\xE2\x80\x99",  // ' (U+2019 right single quote)
    "\xE2\x80\x9D"   // " (U+201D right double quote)
};

// Check if a word consists entirely of attaching punctuation
// These should attach to the previous word without extra spacing
bool isAttachingPunctuationWord(const std::string& word) {
  if (word.empty()) return false;
  size_t pos = 0;
  while (pos < word.size()) {
    bool matched = false;
    for (const auto& p : punctuation) {
      if (word.compare(pos, p.size(), p) == 0) {
        pos += p.size();
        matched = true;
        break;
      }
    }
    if (!matched) return false;
  }
  return true;
}

namespace {

// Find all soft hyphen byte positions in a UTF-8 string
std::vector<size_t> findSoftHyphenPositions(const std::string& word) {
  std::vector<size_t> positions;
  for (size_t i = 0; i + 1 < word.size(); ++i) {
    if (static_cast<unsigned char>(word[i]) == SOFT_HYPHEN_BYTE1 &&
        static_cast<unsigned char>(word[i + 1]) == SOFT_HYPHEN_BYTE2) {
      positions.push_back(i);
    }
  }
  return positions;
}

// Remove all soft hyphens from a string
std::string stripSoftHyphens(const std::string& word) {
  std::string result;
  result.reserve(word.size());
  size_t i = 0;
  while (i < word.size()) {
    if (i + 1 < word.size() && static_cast<unsigned char>(word[i]) == SOFT_HYPHEN_BYTE1 &&
        static_cast<unsigned char>(word[i + 1]) == SOFT_HYPHEN_BYTE2) {
      i += 2;  // Skip soft hyphen
    } else {
      result += word[i++];
    }
  }
  return result;
}

// Check if word ends with a soft hyphen marker (U+00AD = 0xC2 0xAD)
bool hasTrailingSoftHyphen(const std::string& word) {
  return word.size() >= 2 && static_cast<unsigned char>(word[word.size() - 2]) == SOFT_HYPHEN_BYTE1 &&
         static_cast<unsigned char>(word[word.size() - 1]) == SOFT_HYPHEN_BYTE2;
}

// Replace trailing soft hyphen with visible ASCII hyphen for rendering
std::string replaceTrailingSoftHyphen(std::string word) {
  if (hasTrailingSoftHyphen(word)) {
    word.resize(word.size() - 2);
    word += '-';
  }
  return word;
}

// Get word prefix before soft hyphen position (stripped) + visible hyphen
std::string getWordPrefix(const std::string& word, size_t softHyphenPos) {
  std::string prefix = word.substr(0, softHyphenPos);
  return stripSoftHyphens(prefix) + "-";
}

// Get word suffix after soft hyphen position (keep soft hyphens for further splitting)
std::string getWordSuffix(const std::string& word, size_t softHyphenPos) {
  return word.substr(softHyphenPos + 2);  // Skip past soft hyphen bytes, DON'T strip
}

// Check if codepoint is CJK ideograph (Unicode Line Break Class ID)
// Based on UAX #14 - allows line break before/after these characters
bool isCjkCodepoint(uint32_t cp) {
  // CJK Unified Ideographs
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  // CJK Extension A
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  // CJK Compatibility Ideographs
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  // Hiragana
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  // Katakana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  // Hangul Syllables
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;
  // CJK Extension B and beyond (Plane 2)
  if (cp >= 0x20000 && cp <= 0x2A6DF) return true;
  // Fullwidth ASCII variants (often used in CJK context)
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  return false;
}

uint32_t firstCodepoint(const std::string& text) {
  if (text.empty()) return 0;

  const auto* ptr = reinterpret_cast<const uint8_t*>(text.c_str());
  return utf8NextCodepoint(&ptr);
}

uint32_t lastCodepoint(const std::string& text) {
  if (text.empty()) return 0;

  const auto* ptr = reinterpret_cast<const uint8_t*>(text.c_str());
  uint32_t cp = 0;
  uint32_t nextCp = 0;
  while ((nextCp = utf8NextCodepoint(&ptr))) {
    cp = nextCp;
  }
  return cp;
}

int getNaturalGapAdvance(const GfxRenderer& renderer, const int fontId, const std::string& leftWord,
                         const std::string& rightWord, const bool rightContinues,
                         const EpdFontFamily::Style style) {
  const uint32_t leftCp = lastCodepoint(leftWord);
  const uint32_t rightCp = firstCodepoint(rightWord);
  if (leftCp == 0 || rightCp == 0) {
    return rightContinues ? 0 : renderer.getSpaceWidth(fontId, style);
  }

  return rightContinues ? renderer.getKerning(fontId, leftCp, rightCp, style)
                        : renderer.getSpaceAdvance(fontId, leftCp, rightCp, style);
}

// Knuth-Plass: Calculate badness (looseness) of a line
// Returns cubic ratio penalty - loose lines are penalized more heavily
float calculateBadness(int lineWidth, int targetWidth) {
  if (targetWidth <= 0) return INFINITY_PENALTY;
  if (lineWidth > targetWidth) return INFINITY_PENALTY;
  if (lineWidth == targetWidth) return 0.0f;
  float ratio = static_cast<float>(targetWidth - lineWidth) / static_cast<float>(targetWidth);
  return ratio * ratio * ratio * 100.0f;
}

// Knuth-Plass: Calculate demerits for a line based on its badness
// Last line gets 0 demerits (allowed to be loose)
float calculateDemerits(float badness, bool isLastLine) {
  if (badness >= INFINITY_PENALTY) return INFINITY_PENALTY;
  if (isLastLine) return 0.0f;
  return (1.0f + badness) * (1.0f + badness);
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool attachToPrevious) {
  if (word.empty()) return;

  // Check if word contains any CJK characters
  bool hasCjk = false;
  const unsigned char* check = reinterpret_cast<const unsigned char*>(word.c_str());
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&check))) {
    if (isCjkCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }

  if (!hasCjk) {
    // No CJK - keep as single word (Latin, accented Latin, Cyrillic, etc.)
    words.push_back(std::move(word));
    wordStyles.push_back(fontStyle);
    // continues = true if caller marked it, or if it's pure attaching punctuation
    wordContinues.push_back(attachToPrevious || isAttachingPunctuationWord(words.back()));
    return;
  }

  // Mixed content: group non-CJK runs together, split CJK individually.
  // Only the first sub-word inherits attachToPrevious; all others are independent.
  const unsigned char* p = reinterpret_cast<const unsigned char*>(word.c_str());
  std::string nonCjkBuf;
  bool isFirstSubWord = true;

  while ((cp = utf8NextCodepoint(&p))) {
    if (isCjkCodepoint(cp)) {
      // CJK character - flush non-CJK buffer first, then add this char alone
      if (!nonCjkBuf.empty()) {
        words.push_back(std::move(nonCjkBuf));
        wordStyles.push_back(fontStyle);
        wordContinues.push_back(isFirstSubWord ? attachToPrevious : false);
        isFirstSubWord = false;
        nonCjkBuf.clear();
      }

      // Re-encode CJK codepoint to UTF-8
      std::string buf;
      if (cp < 0x10000) {
        buf += static_cast<char>(0xE0 | (cp >> 12));
        buf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        buf += static_cast<char>(0xF0 | (cp >> 18));
        buf += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf += static_cast<char>(0x80 | (cp & 0x3F));
      }
      words.push_back(buf);
      wordStyles.push_back(fontStyle);
      wordContinues.push_back(isFirstSubWord ? attachToPrevious : false);
      isFirstSubWord = false;
    } else {
      // Non-CJK character - accumulate into buffer
      if (cp < 0x80) {
        nonCjkBuf += static_cast<char>(cp);
      } else if (cp < 0x800) {
        nonCjkBuf += static_cast<char>(0xC0 | (cp >> 6));
        nonCjkBuf += static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        nonCjkBuf += static_cast<char>(0xE0 | (cp >> 12));
        nonCjkBuf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        nonCjkBuf += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        nonCjkBuf += static_cast<char>(0xF0 | (cp >> 18));
        nonCjkBuf += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        nonCjkBuf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        nonCjkBuf += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
  }

  // Flush any remaining non-CJK buffer
  if (!nonCjkBuf.empty()) {
    words.push_back(std::move(nonCjkBuf));
    wordStyles.push_back(fontStyle);
    wordContinues.push_back(isFirstSubWord ? attachToPrevious : false);
  }
}

// Consumes data to minimize memory usage
// Returns false if aborted, true otherwise
bool ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine, const AbortCallback& shouldAbort) {
  if (words.empty()) {
    return true;
  }

  // Check for abort before starting
  if (shouldAbort && shouldAbort()) {
    return false;
  }

  const int pageWidth = viewportWidth;
  const int firstLineIndentWidth = firstLineIndentPending ? getFirstLineIndentWidth(renderer, fontId) : 0;

  // Rejoin words that were split by a previous interrupted greedy layout pass.
  // Split prefixes are marked with trailing U+00AD; rejoin with the following suffix word.
  {
    auto it = words.begin();
    auto sIt = wordStyles.begin();
    auto cIt = wordContinues.begin();
    while (it != words.end()) {
      auto nextIt = std::next(it);
      if (nextIt != words.end() && hasTrailingSoftHyphen(*it)) {
        it->resize(it->size() - 2);  // Remove trailing U+00AD
        *it += *nextIt;              // Rejoin with suffix
        words.erase(nextIt);
        wordStyles.erase(std::next(sIt));
        wordContinues.erase(std::next(cIt));
        // Don't advance - check if rejoined word also has marker (nested splits)
      } else {
        ++it;
        ++sIt;
        ++cIt;
      }
    }
  }

  // Pre-split oversized words at soft hyphen positions
  if (hyphenationEnabled) {
    if (!preSplitOversizedWords(renderer, fontId, pageWidth, shouldAbort)) {
      return false;  // Aborted
    }
  }

  auto wordWidths = calculateWordWidths(renderer, fontId);
  const auto lineBreakIndices =
      useGreedyBreaking
          ? computeLineBreaksGreedy(renderer, fontId, pageWidth, firstLineIndentWidth, wordWidths, shouldAbort)
          : computeLineBreaks(renderer, fontId, pageWidth, firstLineIndentWidth, wordWidths, shouldAbort);

  // Check if we were aborted during line break computation
  if (shouldAbort && shouldAbort()) {
    return false;
  }

  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    if (shouldAbort && shouldAbort()) {
      return false;
    }
    extractLine(i, pageWidth, firstLineIndentWidth, wordWidths, lineBreakIndices, processLine, renderer, fontId);
  }
  return true;
}

int ParsedText::getFirstLineIndentWidth(const GfxRenderer& renderer, const int fontId) const {
  if (isRtl || words.empty() || indentLevel == 0 || style == TextBlock::CENTER_ALIGN) {
    return 0;
  }

  const EpdFontFamily::Style firstWordStyle = wordStyles.empty() ? EpdFontFamily::REGULAR : wordStyles.front();
  const int spaceWidth = renderer.getSpaceWidth(fontId, firstWordStyle);
  if (spaceWidth <= 0) return 0;

  switch (indentLevel) {
    case 2:
      return spaceWidth * 2;  // Match U+2003 fallback in renderChar()
    case 3:
      return spaceWidth * 3;  // Match U+2003 + U+2002 fallback
    default:
      return spaceWidth;  // Match U+2002 fallback in renderChar()
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    // Strip soft hyphens before measuring (they should be invisible)
    // After preSplitOversizedWords, words shouldn't contain soft hyphens,
    // but we strip here for safety and for when hyphenation is disabled
    std::string displayWord = stripSoftHyphens(*wordsIt);
    wordWidths.push_back(renderer.getTextAdvanceX(fontId, displayWord.c_str(), *wordStylesIt));
    // Update the word in the list with the stripped version for rendering
    *wordsIt = std::move(displayWord);

    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int firstLineIndentWidth,
                                                  const std::vector<uint16_t>& wordWidths,
                                                  const AbortCallback& shouldAbort) const {
  const size_t n = words.size();

  if (n == 0) {
    return {};
  }

  // Forward DP: minDemerits[i] = minimum demerits to reach position i (before word i)
  std::vector<float> minDemerits(n + 1, INFINITY_PENALTY);
  std::vector<int> prevBreak(n + 1, -1);
  minDemerits[0] = 0.0f;

  for (size_t i = 0; i < n; i++) {
    // Check for abort periodically (every 100 words in outer loop)
    if (shouldAbort && (i % 100 == 0) && shouldAbort()) {
      return {};  // Return empty to signal abort
    }

    if (minDemerits[i] >= INFINITY_PENALTY) continue;

    const int effectivePageWidth = std::max(1, (i == 0) ? pageWidth - firstLineIndentWidth : pageWidth);
    int lineWidth = 0;
    for (size_t j = i; j < n; j++) {
      int gapBefore = 0;
      if (j > i) {
        gapBefore =
            getNaturalGapAdvance(renderer, fontId, words[j - 1], words[j], wordContinues[j], wordStyles[j - 1]);
      }
      lineWidth += gapBefore + wordWidths[j];

      if (lineWidth > effectivePageWidth) {
        if (j == i) {
          // Oversized word: force onto its own line with high penalty
          float demerits = 100.0f + LINE_PENALTY;
          if (minDemerits[i] + demerits < minDemerits[j + 1]) {
            minDemerits[j + 1] = minDemerits[i] + demerits;
            prevBreak[j + 1] = static_cast<int>(i);
          }
        }
        break;
      }

      // Don't allow a break after j if the next word continues (attaches without space gap)
      if (j + 1 < n && wordContinues[j + 1]) {
        continue;
      }

      bool isLastLine = (j == n - 1);
      float badness = calculateBadness(lineWidth, effectivePageWidth);
      float demerits = calculateDemerits(badness, isLastLine) + LINE_PENALTY;

      if (minDemerits[i] + demerits < minDemerits[j + 1]) {
        minDemerits[j + 1] = minDemerits[i] + demerits;
        prevBreak[j + 1] = static_cast<int>(i);
      }
    }
  }

  // Backtrack to reconstruct line break indices
  std::vector<size_t> lineBreakIndices;
  int pos = static_cast<int>(n);
  while (pos > 0 && prevBreak[pos] >= 0) {
    lineBreakIndices.push_back(static_cast<size_t>(pos));
    pos = prevBreak[pos];
  }
  std::reverse(lineBreakIndices.begin(), lineBreakIndices.end());

  // Fallback: if backtracking failed or chain is incomplete, use single-word-per-line
  // After the loop, pos should be 0 if we successfully traced back to the start.
  // If pos > 0, the chain is incomplete (no valid path from position 0 to n).
  if (lineBreakIndices.empty() || pos != 0) {
    lineBreakIndices.clear();
    for (size_t i = 1; i <= n; i++) {
      lineBreakIndices.push_back(i);
    }
  }

  return lineBreakIndices;
}

std::vector<size_t> ParsedText::computeLineBreaksGreedy(const GfxRenderer& renderer, const int fontId,
                                                        const int pageWidth, const int firstLineIndentWidth,
                                                        std::vector<uint16_t>& wordWidths,
                                                        const AbortCallback& shouldAbort) {
  std::vector<size_t> breaks;
  size_t n = wordWidths.size();

  if (n == 0) {
    return breaks;
  }

  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  int currentLineWidthLimit = std::max(1, pageWidth - firstLineIndentWidth);
  int lineWidth = 0;
  for (size_t i = 0; i < n; i++, ++wordIt, ++styleIt) {
    // Check for abort periodically (every 200 words)
    if (shouldAbort && (i % 200 == 0) && shouldAbort()) {
      return {};  // Return empty to signal abort
    }

    const int wordWidth = wordWidths[i];
    const int gapBefore =
        (lineWidth > 0 && i > 0)
            ? getNaturalGapAdvance(renderer, fontId, words[i - 1], words[i], wordContinues[i], wordStyles[i - 1])
            : 0;

    // Check if adding this word would overflow the line.
    // Continuation words (attaching punctuation etc.) are never allowed to start a new
    // line — force them onto the current line even if it causes a slight overflow.
    if (lineWidth + gapBefore + wordWidth > currentLineWidthLimit && lineWidth > 0 && !wordContinues[i]) {
      // Try to hyphenate: split the overflowing word so its first part fits on this line
      const int remainingWidth = currentLineWidthLimit - lineWidth - gapBefore;
      if (remainingWidth > 0 &&
          trySplitWordForLineEnd(renderer, fontId, remainingWidth, wordIt, styleIt, i, wordWidths)) {
        // Word was split: prefix is at index i (fits on current line), suffix at i+1.
        // Re-derive iterators: vector insert may have reallocated, invalidating wordIt/styleIt.
        wordIt = words.begin() + static_cast<ptrdiff_t>(i);
        styleIt = wordStyles.begin() + static_cast<ptrdiff_t>(i);
        lineWidth += gapBefore + wordWidths[i];
        n = wordWidths.size();  // Vector grew by one
        // End this line after the prefix
        breaks.push_back(i + 1);
        // Next iteration (i+1) starts the suffix on a new line
        lineWidth = 0;
        currentLineWidthLimit = pageWidth;
      } else {
        // No hyphenation possible - start a new line at this word
        breaks.push_back(i);
        lineWidth = wordWidth;
        currentLineWidthLimit = pageWidth;
      }
    } else {
      lineWidth += gapBefore + wordWidth;
    }
  }

  // Final break at end of all words
  breaks.push_back(n);
  return breaks;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int firstLineIndentWidth,
                             const std::vector<uint16_t>& wordWidths,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;
  const bool isParagraphFirstExtractedLine = breakIndex == 0 && firstLineIndentPending;
  const int lineIndent = isParagraphFirstExtractedLine ? firstLineIndentWidth : 0;

  // Calculate line width from measured words plus the natural gap between each
  // adjacent pair. Continuation words contribute cross-boundary kerning only.
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx > 0) {
      totalNaturalGaps += getNaturalGapAdvance(renderer, fontId, words[wordIdx - 1], words[wordIdx],
                                               wordContinues[wordIdx], wordStyles[wordIdx - 1]);
      if (!wordContinues[wordIdx]) {
        actualGapCount++;
      }
    }
  }

  const int effectivePageWidth = std::max(1, pageWidth - lineIndent);
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const auto effectiveStyle = (isRtl && style == TextBlock::LEFT_ALIGN) ? TextBlock::RIGHT_ALIGN : style;

  int justifyExtra = 0;
  int justifyRemainder = 0;
  if (effectiveStyle == TextBlock::JUSTIFIED && !isLastLine && actualGapCount >= 1) {
    justifyExtra = spareSpace / static_cast<int>(actualGapCount);
    justifyRemainder = spareSpace % static_cast<int>(actualGapCount);
  }

  std::vector<int> gapAfter(lineWordCount, 0);
  int emittedGapCount = 0;
  for (size_t wordIdx = 0; wordIdx + 1 < lineWordCount; wordIdx++) {
    const bool nextContinues = wordContinues[wordIdx + 1];
    int gap = getNaturalGapAdvance(renderer, fontId, words[wordIdx], words[wordIdx + 1], nextContinues,
                                   wordStyles[wordIdx]);

    if (!nextContinues && effectiveStyle == TextBlock::JUSTIFIED && !isLastLine) {
      gap += justifyExtra;
      if (justifyRemainder != 0 && emittedGapCount < std::abs(justifyRemainder)) {
        gap += justifyRemainder > 0 ? 1 : -1;
      }
      emittedGapCount++;
    }

    gapAfter[wordIdx] = gap;
  }

  int laidOutWidth = lineWordWidthSum;
  for (size_t wordIdx = 0; wordIdx + 1 < lineWordCount; wordIdx++) {
    laidOutWidth += gapAfter[wordIdx];
  }
  const int remainingSpace = effectivePageWidth - laidOutWidth;

  std::vector<int> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (isRtl) {
    int rightEdge = pageWidth;
    if (effectiveStyle == TextBlock::CENTER_ALIGN) {
      rightEdge = lineIndent + effectivePageWidth - remainingSpace / 2;
    }

    int xpos = rightEdge;
    for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
      xpos -= wordWidths[lastBreakAt + wordIdx];
      lineXPos.push_back(xpos);
      if (wordIdx + 1 < lineWordCount) {
        xpos -= gapAfter[wordIdx];
      }
    }
  } else {
    int xpos = lineIndent;
    if (effectiveStyle == TextBlock::RIGHT_ALIGN) {
      xpos = lineIndent + remainingSpace;
    } else if (effectiveStyle == TextBlock::CENTER_ALIGN) {
      xpos = lineIndent + remainingSpace / 2;
    }

    for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
      lineXPos.push_back(xpos);
      xpos += wordWidths[lastBreakAt + wordIdx];
      if (wordIdx + 1 < lineWordCount) {
        xpos += gapAfter[wordIdx];
      }
    }
  }

  std::vector<TextBlock::WordData> lineData;
  lineData.reserve(lineWordCount);

  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  auto continuesIt = wordContinues.begin();

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const uint16_t xPos = static_cast<uint16_t>(std::max(0, lineXPos[wordIdx]));
    lineData.push_back({replaceTrailingSoftHyphen(std::move(*wordIt)), xPos, *styleIt});
    ++wordIt;
    ++styleIt;
    ++continuesIt;
  }

  // Remove consumed elements from all three parallel lists
  words.erase(words.begin(), wordIt);
  wordStyles.erase(wordStyles.begin(), styleIt);
  wordContinues.erase(wordContinues.begin(), continuesIt);
  if (isParagraphFirstExtractedLine) {
    firstLineIndentPending = false;
  }

  processLine(std::make_shared<TextBlock>(std::move(lineData), effectiveStyle));
}

bool ParsedText::preSplitOversizedWords(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                        const AbortCallback& shouldAbort) {
  std::vector<std::string> newWords;
  std::vector<EpdFontFamily::Style> newStyles;
  std::vector<bool> newContinues;

  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  auto continuesIt = wordContinues.begin();
  size_t wordCount = 0;

  while (wordIt != words.end()) {
    // Check for abort periodically (every 50 words)
    if (shouldAbort && (++wordCount % 50 == 0) && shouldAbort()) {
      return false;  // Aborted
    }

    const std::string& word = *wordIt;
    const EpdFontFamily::Style wordStyle = *styleIt;
    const bool originalContinues = *continuesIt;

    // Measure word without soft hyphens
    const std::string stripped = stripSoftHyphens(word);
    const int wordWidth = renderer.getTextWidth(fontId, stripped.c_str(), wordStyle);

    if (wordWidth <= pageWidth) {
      // Word fits, keep as-is (will be stripped later in calculateWordWidths)
      newWords.push_back(word);
      newStyles.push_back(wordStyle);
      newContinues.push_back(originalContinues);
    } else {
      // Word is too wide - try to split at soft hyphen positions.
      // First piece inherits originalContinues; subsequent pieces get false.
      bool isFirstPiece = true;
      auto pushPiece = [&](std::string piece) {
        newWords.push_back(std::move(piece));
        newStyles.push_back(wordStyle);
        newContinues.push_back(isFirstPiece ? originalContinues : false);
        isFirstPiece = false;
      };

      auto shyPositions = findSoftHyphenPositions(word);

      if (shyPositions.empty()) {
        // No soft hyphens - use dictionary-based hyphenation
        // Compute all break points on the full word once (Liang patterns
        // need full-word context for correct results).
        auto breaks = Hyphenation::breakOffsets(word, true);

        if (breaks.empty()) {
          pushPiece(word);
        } else {
          size_t prevOffset = 0;

          for (size_t bi = 0; bi <= breaks.size(); ++bi) {
            const std::string remaining = word.substr(prevOffset);
            const int remainingWidth = renderer.getTextWidth(fontId, remaining.c_str(), wordStyle);

            if (remainingWidth <= pageWidth) {
              pushPiece(remaining);
              break;
            }

            // Find the rightmost break where prefix + hyphen fits
            int bestIdx = -1;
            std::string bestPrefix;
            for (int i = static_cast<int>(breaks.size()) - 1; i >= 0; --i) {
              if (breaks[i].byteOffset <= prevOffset) continue;
              std::string prefix = word.substr(prevOffset, breaks[i].byteOffset - prevOffset);
              if (breaks[i].requiresInsertedHyphen) {
                prefix += "-";
              }
              const int prefixWidth = renderer.getTextWidth(fontId, prefix.c_str(), wordStyle);
              if (prefixWidth <= pageWidth) {
                bestIdx = i;
                bestPrefix = std::move(prefix);
                break;
              }
            }

            if (bestIdx < 0) {
              pushPiece(remaining);
              break;
            }

            pushPiece(std::move(bestPrefix));
            prevOffset = breaks[bestIdx].byteOffset;
          }
        }
      } else {
        // Split word at soft hyphen positions
        std::string remaining = word;
        size_t splitIterations = 0;
        constexpr size_t MAX_SPLIT_ITERATIONS = 100;  // Safety limit

        while (splitIterations++ < MAX_SPLIT_ITERATIONS) {
          if (splitIterations == MAX_SPLIT_ITERATIONS) {
            LOG_ERR(TAG, "Warning: hit max split iterations for oversized word");
          }
          const std::string strippedRemaining = stripSoftHyphens(remaining);
          const int remainingWidth = renderer.getTextWidth(fontId, strippedRemaining.c_str(), wordStyle);

          if (remainingWidth <= pageWidth) {
            pushPiece(remaining);
            break;
          }

          // Find soft hyphen positions in remaining string
          auto localPositions = findSoftHyphenPositions(remaining);
          if (localPositions.empty()) {
            pushPiece(remaining);
            break;
          }

          // Find the rightmost soft hyphen where prefix + hyphen fits
          int bestPos = -1;
          for (int i = static_cast<int>(localPositions.size()) - 1; i >= 0; --i) {
            std::string prefix = getWordPrefix(remaining, localPositions[i]);
            int prefixWidth = renderer.getTextWidth(fontId, prefix.c_str(), wordStyle);
            if (prefixWidth <= pageWidth) {
              bestPos = i;
              break;
            }
          }

          if (bestPos < 0) {
            pushPiece(remaining);
            break;
          }

          // Split at this position
          std::string prefix = getWordPrefix(remaining, localPositions[bestPos]);
          std::string suffix = getWordSuffix(remaining, localPositions[bestPos]);

          pushPiece(std::move(prefix));  // Already includes visible hyphen "-"

          if (suffix.empty()) {
            break;
          }
          remaining = suffix;
        }
      }
    }

    ++wordIt;
    ++styleIt;
    ++continuesIt;
  }

  words = std::move(newWords);
  wordStyles = std::move(newStyles);
  wordContinues = std::move(newContinues);
  return true;
}

bool ParsedText::trySplitWordForLineEnd(const GfxRenderer& renderer, const int fontId, const int remainingWidth,
                                        std::vector<std::string>::iterator wordIt,
                                        std::vector<EpdFontFamily::Style>::iterator styleIt, const size_t wordIndex,
                                        std::vector<uint16_t>& wordWidths) {
  if (!hyphenationEnabled) return false;

  const std::string& word = *wordIt;
  const EpdFontFamily::Style fontStyle = *styleIt;

  auto breaks = Hyphenation::breakOffsets(word, false);
  if (breaks.empty()) return false;

  // Find rightmost break where prefix+hyphen fits in remainingWidth
  for (int i = static_cast<int>(breaks.size()) - 1; i >= 0; --i) {
    std::string prefix = word.substr(0, breaks[i].byteOffset);
    // Measure with visible hyphen for accurate layout
    const std::string displayPrefix = breaks[i].requiresInsertedHyphen ? prefix + "-" : prefix;
    const int prefixWidth = renderer.getTextWidth(fontId, displayPrefix.c_str(), fontStyle);
    if (prefixWidth <= remainingWidth) {
      // Store with soft hyphen MARKER (not visible hyphen) so interrupted layouts
      // can rejoin the fragments on resume (calculateWordWidths strips U+00AD)
      if (breaks[i].requiresInsertedHyphen) prefix += "\xC2\xAD";
      std::string suffix = word.substr(breaks[i].byteOffset);
      const int suffixWidth = renderer.getTextWidth(fontId, suffix.c_str(), fontStyle);

      // Replace current word with prefix, insert suffix after
      *wordIt = std::move(prefix);
      auto nextWordIt = std::next(wordIt);
      auto nextStyleIt = std::next(styleIt);
      words.insert(nextWordIt, std::move(suffix));
      wordStyles.insert(nextStyleIt, fontStyle);
      // Suffix starts a new line — not a continuation of anything
      wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

      // Update widths vector
      wordWidths[wordIndex] = static_cast<uint16_t>(prefixWidth);
      wordWidths.insert(wordWidths.begin() + wordIndex + 1, static_cast<uint16_t>(suffixWidth));
      return true;
    }
  }
  return false;
}
