#pragma once

#include <RenderConfig.h>
#include <SdFat.h>

#include <memory>
#include <string>

#include "ContentParser.h"

class GfxRenderer;
class ParsedText;

/**
 * Content parser for plain text files (TXT, Markdown).
 * Reads text, wraps into lines, and creates Page objects.
 */
class PlainTextParser : public ContentParser {
  std::string filepath_;
  GfxRenderer& renderer_;
  RenderConfig config_;
  size_t fileSize_ = 0;
  size_t currentOffset_ = 0;
  bool hasMore_ = true;
  bool isRtl_ = false;
  // Carries over unconsumed words from a paragraph interrupted at a page boundary
  std::unique_ptr<ParsedText> pendingBlock_;

 public:
  PlainTextParser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config);
  ~PlainTextParser() override = default;

  bool parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages = 0,
                  const AbortCallback& shouldAbort = nullptr) override;
  bool hasMoreContent() const override { return hasMore_; }
  bool canResume() const override { return currentOffset_ > 0 && hasMore_; }
  void reset() override;
};
