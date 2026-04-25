#include "ReaderResourceController.h"

namespace papyrix::reader {

ReaderResourceController::ReaderResourceController(GfxRenderer& renderer) : resources_(renderer) {}

void ReaderResourceController::resetSession() {
  clearDocumentResources();
  thumbnailDone_.store(false, std::memory_order_release);
}

void ReaderResourceController::clearDocumentResources() {
  auto& state = resources_.unsafeState();
  state.pageCache.reset();
  state.parser.reset();
  state.parserSpineIndex = -1;
}

ReaderResourceController::Session ReaderResourceController::acquireForeground(const char* reason) {
  return resources_.acquireForeground(reason);
}

ReaderResourceController::Session ReaderResourceController::acquireWorker(const char* reason) {
  return resources_.acquireWorker(reason);
}

std::unique_ptr<PageCache>& ReaderResourceController::pageCacheRef() { return resources_.unsafeState().pageCache; }

std::unique_ptr<ContentParser>& ReaderResourceController::parserRef() { return resources_.unsafeState().parser; }

int& ReaderResourceController::parserSpineIndexRef() { return resources_.unsafeState().parserSpineIndex; }

}  // namespace papyrix::reader
