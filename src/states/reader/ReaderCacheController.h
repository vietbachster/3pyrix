#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ReaderResourceController.h"

class ContentParser;
class GfxRenderer;
class PageCache;
struct RenderConfig;

namespace papyrix {
class Core;
enum class ContentType : uint8_t;
}

namespace papyrix::reader {

class ReaderCacheController {
 public:
  using Session = ReaderResourceController::Session;
  using AbortCallback = std::function<bool()>;

  explicit ReaderCacheController(GfxRenderer& renderer);

  void setContentPath(const char* path);
  void resetSession();
  void clearDocumentResources();

  Session acquireForeground(const char* reason);
  Session acquireWorker(const char* reason);

  std::unique_ptr<PageCache>& pageCacheRef();
  std::unique_ptr<ContentParser>& parserRef();
  int& parserSpineIndexRef();
  std::atomic<bool>& thumbnailDoneRef();
  bool thumbnailDone() const;

  void loadCacheFromDisk(Core& core, int currentSpineIndex, uint16_t viewportWidth, uint16_t viewportHeight);
  void createOrExtendCache(Core& core, int currentSpineIndex, uint16_t viewportWidth, uint16_t viewportHeight);
  void runBackgroundCache(Core& core, int currentSpineIndex, int currentSectionPage, bool hasCover, int textStartIndex,
                          uint16_t viewportWidth, uint16_t viewportHeight, const AbortCallback& shouldAbort);

  std::string cachePathForPosition(Core& core, ContentType type, int spineIndex, const RenderConfig& config) const;

  static int calcFirstContentSpine(bool hasCover, int textStartIndex, size_t spineCount);
  static void saveAnchorMap(const ContentParser& parser, const std::string& cachePath);
  static int loadAnchorPage(const std::string& cachePath, const std::string& anchor);
  static std::vector<std::pair<std::string, uint16_t>> loadAnchorMap(const std::string& cachePath);

 private:
  void createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath, const RenderConfig& config);
  void backgroundCacheImpl(ContentParser& parser, const std::string& cachePath, const RenderConfig& config,
                           int currentSectionPage, const AbortCallback& shouldAbort);

  GfxRenderer& renderer_;
  ReaderResourceController resourceController_;
  std::string contentPath_;
};

}  // namespace papyrix::reader
