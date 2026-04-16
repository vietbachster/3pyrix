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

bool isWhitespace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
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
                                      config_.indentLevel, config_.hyphenation, true, isRtl_));
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

    for (size_t i = 0; i < bytesRead; i++) {
      char c = static_cast<char>(buffer[i]);

      // Handle newlines as paragraph breaks
      if (c == '\n') {
        // Flush partial word
        if (!partialWord.empty()) {
          partialWord.resize(utf8NormalizeNfc(&partialWord[0], partialWord.size()));
          currentBlock->addWord(partialWord, EpdFontFamily::REGULAR);
          partialWord.clear();
        }

        // Flush current block (paragraph)
        if (!flushBlock()) {
          // Rewind to the \n so next parse re-reads it and flushes pendingBlock_
          currentOffset_ = file.position() - (bytesRead - i);
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
                                          config_.indentLevel, config_.hyphenation, true, isRtl_));

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

      if (isWhitespace(c)) {
        if (!partialWord.empty()) {
          partialWord.resize(utf8NormalizeNfc(&partialWord[0], partialWord.size()));
          currentBlock->addWord(partialWord, EpdFontFamily::REGULAR);
          partialWord.clear();
        }
        continue;
      }

      partialWord += c;

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
      size_t backupBytes = partialWord.length();
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
