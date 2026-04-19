#include "PlainTextParser.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Page.h>
#include <ParsedText.h>
#include <SDCardManager.h>
#include <Utf8.h>

#define TAG "TXT_PARSE"

#include <utility>

namespace {
constexpr size_t READ_CHUNK_SIZE = 4096;

int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

size_t utf8CompletePrefixLength(const char* data, const size_t len) {
  if (len == 0) return 0;

  size_t continuationBytes = 0;
  size_t i = len;
  while (i > 0 && continuationBytes < 3 &&
         (static_cast<unsigned char>(data[i - 1]) & 0xC0) == 0x80) {
    --i;
    ++continuationBytes;
  }

  if (i == 0) return len;

  const int expectedBytes = utf8CodepointLen(static_cast<unsigned char>(data[i - 1]));
  const size_t actualBytes = continuationBytes + 1;
  if (expectedBytes > static_cast<int>(actualBytes)) {
    return i - 1;
  }
  return len;
}

bool isBreakWhitespaceCodepoint(const uint32_t cp) {
  return cp == ' ' || cp == '\r' || cp == '\n' || cp == '\t' || cp == 0x00A0 || cp == 0x2002 || cp == 0x2003;
}
}  // namespace

PlainTextParser::PlainTextParser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config)
    : filepath_(std::move(filepath)), renderer_(renderer), config_(config) {}

void PlainTextParser::reset() {
  currentOffset_ = 0;
  hasMore_ = true;
  isRtl_ = false;
  pendingBlock_.reset();
}

bool PlainTextParser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages,
                                 const AbortCallback& shouldAbort) {
  FsFile file;
  if (!SdMan.openFileForRead("TXT", filepath_, file)) {
    LOG_ERR(TAG, "Failed to open file: %s", filepath_.c_str());
    return false;
  }

  fileSize_ = file.size();
  if (currentOffset_ > 0) {
    file.seek(currentOffset_);
  }

  const int lineHeight = config_.getReadingLineHeight();
  const int maxLinesPerPage = config_.viewportHeight / lineHeight;

  uint8_t buffer[READ_CHUNK_SIZE + 1];
  std::unique_ptr<ParsedText> currentBlock;
  std::unique_ptr<Page> currentPage;
  int16_t currentPageY = 0;
  uint16_t pagesCreated = 0;
  std::string partialWord;
  std::string utf8Carry;
  uint16_t abortCheckCounter = 0;

  auto startNewPage = [&]() {
    currentPage.reset(new Page());
    currentPageY = 0;
  };

  auto addLineToPage = [&](std::shared_ptr<TextBlock> line) {
    if (!currentPage) {
      startNewPage();
    }

    if (currentPageY + lineHeight > config_.viewportHeight) {
      onPageComplete(std::move(currentPage));
      pagesCreated++;
      startNewPage();

      if (maxPages > 0 && pagesCreated >= maxPages) {
        // Add the line to the new page before stopping so it isn't lost.
        // extractLine() already erased these words from pendingBlock_; if we
        // discard the line here the content disappears at page transitions.
        currentPage->elements.push_back(std::make_shared<PageLine>(line, 0, currentPageY));
        currentPageY += lineHeight;
        return false;
      }
    }

    currentPage->elements.push_back(std::make_shared<PageLine>(line, 0, currentPageY));
    currentPageY += lineHeight;
    return true;
  };

  auto flushBlock = [&]() -> bool {
    if (!currentBlock || currentBlock->isEmpty()) return true;

    bool continueProcessing = true;
    currentBlock->layoutAndExtractLines(renderer_, config_.fontId, config_.viewportWidth,
                                        [&](const std::shared_ptr<TextBlock>& line) {
                                          if (!continueProcessing) return;
                                          if (!addLineToPage(line)) {
                                            continueProcessing = false;
                                          }
                                        },
                                        true, [&]() -> bool { return !continueProcessing; });

    if (continueProcessing) {
      currentBlock.reset();
    } else {
      pendingBlock_ = std::move(currentBlock);
    }
    return continueProcessing;
  };

  startNewPage();
  if (pendingBlock_) {
    currentBlock = std::move(pendingBlock_);
  } else {
    currentBlock.reset(new ParsedText(static_cast<TextBlock::BLOCK_STYLE>(config_.paragraphAlignment),
                                      config_.indentLevel, config_.hyphenation, config_.hyphenation, isRtl_));
  }

  while (file.available() > 0) {
    // Check for abort every few iterations
    if (shouldAbort && (++abortCheckCounter % 10 == 0) && shouldAbort()) {
      LOG_INF(TAG, "Aborted by external request");
      currentOffset_ = file.position();
      hasMore_ = true;
      file.close();
      return false;
    }

    size_t bytesRead = file.read(buffer, READ_CHUNK_SIZE);
    if (bytesRead == 0) break;

    buffer[bytesRead] = '\0';

    std::string chunkData = utf8Carry;
    chunkData.append(reinterpret_cast<const char*>(buffer), bytesRead);
    const size_t processLen = utf8CompletePrefixLength(chunkData.data(), chunkData.size());
    utf8Carry.assign(chunkData.data() + processLen, chunkData.size() - processLen);

    std::string processChunk = chunkData.substr(0, processLen);
    processChunk.push_back('\0');
    const size_t carryLenBeforeRead = chunkData.size() - bytesRead;

    const char* ptr = processChunk.c_str();
    while (*ptr) {
      const char* cpStart = ptr;
      const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr));
      const size_t cpBytes = static_cast<size_t>(ptr - cpStart);
      const size_t cpStartOffset = static_cast<size_t>(cpStart - processChunk.c_str());
      const size_t cpStartOffsetInRead = cpStartOffset >= carryLenBeforeRead ? cpStartOffset - carryLenBeforeRead : 0;

      // Handle newlines as paragraph breaks
      if (cp == '\n') {
        // Flush partial word
        if (!partialWord.empty()) {
          partialWord.resize(utf8NormalizeNfc(&partialWord[0], partialWord.size()));
          currentBlock->addWord(partialWord, EpdFontFamily::REGULAR);
          partialWord.clear();
        }

        // Flush current block (paragraph)
        if (!flushBlock()) {
          // Rewind to the \n so next parse re-reads it and flushes pendingBlock_
          currentOffset_ = file.position() - (bytesRead - cpStartOffsetInRead);
          hasMore_ = true;
          file.close();

          // Complete final page if it has content
          if (currentPage && !currentPage->elements.empty()) {
            onPageComplete(std::move(currentPage));
          }
          return true;
        }

        // Start new paragraph
        currentBlock.reset(new ParsedText(static_cast<TextBlock::BLOCK_STYLE>(config_.paragraphAlignment),
                                          config_.indentLevel, config_.hyphenation, config_.hyphenation, isRtl_));

        // Add paragraph spacing
        switch (config_.spacingLevel) {
          case 1:
            currentPageY += lineHeight / 4;
            break;
          case 2:
            currentPageY += lineHeight / 2;
            break;
          case 3:
            currentPageY += lineHeight;
            break;
        }
        continue;
      }

      if (isBreakWhitespaceCodepoint(cp)) {
        if (!partialWord.empty()) {
          partialWord.resize(utf8NormalizeNfc(&partialWord[0], partialWord.size()));
          currentBlock->addWord(partialWord, EpdFontFamily::REGULAR);
          partialWord.clear();
        }
        continue;
      }

      partialWord.append(cpStart, cpBytes);

      // Prevent extremely long words from accumulating
      if (partialWord.length() > 100) {
        // Back up to last valid UTF-8 codepoint boundary to avoid splitting multi-byte chars
        size_t safeLen = partialWord.length();
        while (safeLen > 0 && (static_cast<unsigned char>(partialWord[safeLen - 1]) & 0xC0) == 0x80) {
          safeLen--;
        }
        if (safeLen > 0 && static_cast<unsigned char>(partialWord[safeLen - 1]) >= 0xC0) {
          safeLen--;
        }

        if (safeLen > 0) {
          std::string overflow = partialWord.substr(safeLen);
          partialWord.resize(safeLen);
          partialWord.resize(utf8NormalizeNfc(&partialWord[0], partialWord.size()));
          currentBlock->addWord(partialWord, EpdFontFamily::REGULAR);
          partialWord = std::move(overflow);
        } else {
          partialWord.resize(utf8NormalizeNfc(&partialWord[0], partialWord.size()));
          currentBlock->addWord(partialWord, EpdFontFamily::REGULAR);
          partialWord.clear();
        }
      }
    }

    // Check if we hit max pages
    if (maxPages > 0 && pagesCreated >= maxPages) {
      // Back up offset to re-read the partial word on next parse
      size_t backupBytes = partialWord.length() + utf8Carry.length();
      currentOffset_ = file.position() - backupBytes;
      // Preserve the partially-accumulated paragraph block for next parse
      if (currentBlock && !currentBlock->isEmpty()) {
        pendingBlock_ = std::move(currentBlock);
      }
      hasMore_ = (currentOffset_ < fileSize_) || bool(pendingBlock_);
      file.close();
      return true;
    }
  }

  // Flush remaining content
  if (!utf8Carry.empty()) {
    partialWord += utf8Carry;
  }
  if (!partialWord.empty()) {
    partialWord.resize(utf8NormalizeNfc(&partialWord[0], partialWord.size()));
    currentBlock->addWord(partialWord, EpdFontFamily::REGULAR);
  }
  flushBlock();

  // Complete final page
  if (currentPage && !currentPage->elements.empty()) {
    onPageComplete(std::move(currentPage));
    pagesCreated++;
  }

  file.close();
  currentOffset_ = fileSize_;
  hasMore_ = false;

  LOG_INF(TAG, "Parsed %d pages from %s", pagesCreated, filepath_.c_str());
  return true;
}
