#pragma once

#include <atomic>
#include <memory>

#include "ReaderDocumentResources.h"

class ContentParser;
class GfxRenderer;
class PageCache;

namespace papyrix::reader {

class ReaderResourceController {
 public:
  using Session = ReaderDocumentResources::Session;

  explicit ReaderResourceController(GfxRenderer& renderer);

  void resetSession();
  void clearDocumentResources();

  Session acquireForeground(const char* reason);
  Session acquireWorker(const char* reason);

  std::unique_ptr<PageCache>& pageCacheRef();
  std::unique_ptr<ContentParser>& parserRef();
  int& parserSpineIndexRef();
  std::atomic<bool>& thumbnailDoneRef() { return thumbnailDone_; }
  bool thumbnailDone() const { return thumbnailDone_.load(std::memory_order_acquire); }

 private:
  ReaderDocumentResources resources_;
  std::atomic<bool> thumbnailDone_{false};
};

}  // namespace papyrix::reader
