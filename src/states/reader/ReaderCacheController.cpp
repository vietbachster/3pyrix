#include "ReaderCacheController.h"

#include <Arduino.h>
#include <ContentParser.h>
#include <EpubChapterParser.h>
#include <Logging.h>
#include <PageCache.h>
#include <PlainTextParser.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <new>

#include "../../ThemeManager.h"
#include "../../core/Core.h"

#define TAG "RDR_CACHE"

namespace papyrix::reader {

namespace {

inline std::string epubSectionCachePath(const std::string& epubCachePath, const int spineIndex) {
  return epubCachePath + "/sections/" + std::to_string(spineIndex) + ".bin";
}

inline std::string contentCachePath(const char* cacheDir, const int fontId) {
  return std::string(cacheDir) + "/pages_" + std::to_string(fontId) + ".bin";
}

}  // namespace

ReaderCacheController::ReaderCacheController(GfxRenderer& renderer) : renderer_(renderer), resourceController_(renderer) {}

void ReaderCacheController::setContentPath(const char* path) { contentPath_ = path ? path : ""; }

void ReaderCacheController::resetSession() {
  resourceController_.resetSession();
  contentPath_.clear();
}

void ReaderCacheController::clearDocumentResources() { resourceController_.clearDocumentResources(); }

ReaderCacheController::Session ReaderCacheController::acquireForeground(const char* reason) {
  return resourceController_.acquireForeground(reason);
}

ReaderCacheController::Session ReaderCacheController::acquireWorker(const char* reason) {
  return resourceController_.acquireWorker(reason);
}

std::unique_ptr<PageCache>& ReaderCacheController::pageCacheRef() { return resourceController_.pageCacheRef(); }

std::unique_ptr<ContentParser>& ReaderCacheController::parserRef() { return resourceController_.parserRef(); }

int& ReaderCacheController::parserSpineIndexRef() { return resourceController_.parserSpineIndexRef(); }

std::atomic<bool>& ReaderCacheController::thumbnailDoneRef() { return resourceController_.thumbnailDoneRef(); }

bool ReaderCacheController::thumbnailDone() const { return resourceController_.thumbnailDone(); }

int ReaderCacheController::calcFirstContentSpine(const bool hasCover, const int textStartIndex, const size_t spineCount) {
  if (hasCover && textStartIndex == 0 && spineCount > 1) {
    return 1;
  }
  return textStartIndex;
}

void ReaderCacheController::saveAnchorMap(const ContentParser& parser, const std::string& cachePath) {
  const auto& anchors = parser.getAnchorMap();

  std::string anchorPath = cachePath + ".anchors";
  FsFile file;
  if (!SdMan.openFileForWrite("RDR", anchorPath, file)) return;

  if (anchors.size() > UINT16_MAX) {
    const uint16_t zero = 0;
    serialization::writePod(file, zero);
    file.close();
    return;
  }
  const uint16_t count = static_cast<uint16_t>(anchors.size());
  serialization::writePod(file, count);
  for (const auto& entry : anchors) {
    serialization::writeString(file, entry.first);
    serialization::writePod(file, entry.second);
  }
  if (file.getWriteError()) {
    LOG_ERR(TAG, "Write error while saving anchor map (disk full?)");
  }
  file.close();
}

int ReaderCacheController::loadAnchorPage(const std::string& cachePath, const std::string& anchor) {
  std::string anchorPath = cachePath + ".anchors";
  FsFile file;
  if (!SdMan.openFileForRead("RDR", anchorPath, file)) return -1;

  uint16_t count;
  if (!serialization::readPodChecked(file, count)) {
    file.close();
    return -1;
  }

  for (uint16_t i = 0; i < count; i++) {
    std::string anchorId;
    uint16_t page;
    if (!serialization::readString(file, anchorId) || !serialization::readPodChecked(file, page)) {
      file.close();
      return -1;
    }
    if (anchorId == anchor) {
      file.close();
      return page;
    }
  }

  file.close();
  return -1;
}

std::vector<std::pair<std::string, uint16_t>> ReaderCacheController::loadAnchorMap(const std::string& cachePath) {
  std::vector<std::pair<std::string, uint16_t>> anchors;
  std::string anchorPath = cachePath + ".anchors";
  FsFile file;
  if (!SdMan.openFileForRead("RDR", anchorPath, file)) return anchors;

  uint16_t count;
  if (serialization::readPodChecked(file, count)) {
    for (uint16_t i = 0; i < count; i++) {
      std::string anchorId;
      uint16_t page;
      if (!serialization::readString(file, anchorId) || !serialization::readPodChecked(file, page)) break;
      anchors.emplace_back(std::move(anchorId), page);
    }
  }
  file.close();
  return anchors;
}

std::string ReaderCacheController::cachePathForPosition(Core& core, const ContentType type, const int spineIndex,
                                                        const RenderConfig& config) const {
  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) {
      return {};
    }
    return epubSectionCachePath(provider->getEpub()->getCachePath(), spineIndex);
  }

  if (type == ContentType::Txt) {
    return contentCachePath(core.content.cacheDir(), config.fontId);
  }

  return {};
}

void ReaderCacheController::createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath,
                                                    const RenderConfig& config) {
  auto& pageCache = pageCacheRef();
  bool needsCreate = false;
  bool needsExtend = false;

  if (!pageCache) {
    pageCache.reset(new (std::nothrow) PageCache(cachePath));
    if (!pageCache) {
      LOG_ERR(TAG, "createOrExtendCache: OOM allocating PageCache");
      return;
    }
    if (pageCache->load(config)) {
      if (!SdMan.exists((cachePath + ".anchors").c_str())) {
        needsCreate = true;
      } else {
        needsExtend = pageCache->isPartial();
      }
    } else {
      needsCreate = true;
    }
  } else {
    if (!SdMan.exists((cachePath + ".anchors").c_str())) {
      needsCreate = true;
    } else {
      needsExtend = pageCache->isPartial();
    }
  }

  if (!pageCache) {
    return;
  }

  if (needsExtend) {
    pageCache->extend(parser, PageCache::DEFAULT_CACHE_CHUNK);
    saveAnchorMap(parser, cachePath);
  } else if (needsCreate) {
    parser.reset();
    pageCache->create(parser, config, PageCache::DEFAULT_CACHE_CHUNK);
    saveAnchorMap(parser, cachePath);
  }
}

void ReaderCacheController::backgroundCacheImpl(ContentParser& parser, const std::string& cachePath,
                                                const RenderConfig& config, const int currentSectionPage,
                                                const AbortCallback& shouldAbort) {
  auto& pageCache = pageCacheRef();
  pageCache.reset(new (std::nothrow) PageCache(cachePath));
  if (!pageCache) {
    LOG_ERR(TAG, "backgroundCache: OOM allocating PageCache");
    return;
  }

  bool loaded = pageCache->load(config);
  if (loaded && !SdMan.exists((cachePath + ".anchors").c_str())) {
    loaded = false;
  }
  const bool needsExtend = loaded && pageCache->needsExtension(currentSectionPage);

  if (shouldAbort && shouldAbort()) {
    pageCache.reset();
    LOG_DBG(TAG, "Background cache aborted after setup");
    return;
  }

  if (!loaded || needsExtend) {
    bool success;
    if (needsExtend) {
      success = pageCache->extend(parser, PageCache::DEFAULT_CACHE_CHUNK, shouldAbort);
    } else {
      parser.reset();
      success = pageCache->create(parser, config, PageCache::DEFAULT_CACHE_CHUNK, 0, shouldAbort);
    }

    if (success && !(shouldAbort && shouldAbort())) {
      saveAnchorMap(parser, cachePath);
    }

    if (!success || (shouldAbort && shouldAbort())) {
      LOG_ERR(TAG, "Cache creation failed or aborted, clearing pageCache");
      pageCache.reset();
    }
  }
}

void ReaderCacheController::loadCacheFromDisk(Core& core, const int currentSpineIndex, const uint16_t viewportWidth,
                                              const uint16_t viewportHeight) {
  const Theme& theme = THEME_MANAGER.current();
  const ContentType type = core.content.metadata().type;
  const RenderConfig config = core.settings.getRenderConfig(theme, viewportWidth, viewportHeight);
  const std::string cachePath = cachePathForPosition(core, type, currentSpineIndex, config);
  if (cachePath.empty()) {
    LOG_ERR(TAG, "loadCacheFromDisk: unsupported content type %d", static_cast<int>(type));
    return;
  }

  auto& pageCache = pageCacheRef();
  if (!pageCache) {
    pageCache.reset(new (std::nothrow) PageCache(cachePath));
    if (pageCache && !pageCache->load(config)) {
      pageCache.reset();
    }
  }
}

void ReaderCacheController::createOrExtendCache(Core& core, const int currentSpineIndex, const uint16_t viewportWidth,
                                                const uint16_t viewportHeight) {
  const Theme& theme = THEME_MANAGER.current();
  const ContentType type = core.content.metadata().type;
  const RenderConfig config = core.settings.getRenderConfig(theme, viewportWidth, viewportHeight);
  const std::string cachePath = cachePathForPosition(core, type, currentSpineIndex, config);
  if (cachePath.empty()) {
    return;
  }

  auto& parser = parserRef();
  auto& parserSpineIndex = parserSpineIndexRef();

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;
    auto epub = provider->getEpubShared();

    if (!parser || parserSpineIndex != currentSpineIndex) {
      std::string imageCachePath = core.settings.showImages ? (epub->getCachePath() + "/images") : "";
      parser.reset(new (std::nothrow) EpubChapterParser(epub, currentSpineIndex, renderer_, config, imageCachePath));
      if (!parser) {
        LOG_ERR(TAG, "createOrExtendCache: OOM allocating EpubChapterParser");
        return;
      }
      parserSpineIndex = currentSpineIndex;
    }
  } else if (type == ContentType::Txt) {
    if (!parser) {
      parser.reset(new (std::nothrow) PlainTextParser(contentPath_.c_str(), renderer_, config));
      if (!parser) {
        LOG_ERR(TAG, "createOrExtendCache: OOM allocating PlainTextParser");
        return;
      }
      parserSpineIndex = 0;
    }
  } else {
    return;
  }

  createOrExtendCacheImpl(*parser, cachePath, config);
}

void ReaderCacheController::runBackgroundCache(Core& core, const int currentSpineIndex, const int currentSectionPage,
                                               const bool hasCover, const int textStartIndex,
                                               const uint16_t viewportWidth, const uint16_t viewportHeight,
                                               const AbortCallback& shouldAbort) {
  const Theme& theme = THEME_MANAGER.current();
  const ContentType type = core.content.metadata().type;
  const RenderConfig config = core.settings.getRenderConfig(theme, viewportWidth, viewportHeight);

  auto& parser = parserRef();
  auto& parserSpineIndex = parserSpineIndexRef();
  std::string cachePath;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (provider && provider->getEpub() && !(shouldAbort && shouldAbort())) {
      const auto* epub = provider->getEpub();
      std::string imageCachePath = core.settings.showImages ? (epub->getCachePath() + "/images") : "";
      int spineToCache = currentSpineIndex;
      if (currentSectionPage == -1) {
        spineToCache = calcFirstContentSpine(hasCover, textStartIndex, epub->getSpineItemsCount());
      }
      cachePath = epubSectionCachePath(epub->getCachePath(), spineToCache);

      if (!parser || parserSpineIndex != spineToCache) {
        parser.reset(new (std::nothrow)
                         EpubChapterParser(provider->getEpubShared(), spineToCache, renderer_, config, imageCachePath));
        if (!parser) {
          LOG_ERR(TAG, "runBackgroundCache: OOM allocating EpubChapterParser");
          return;
        }
        parserSpineIndex = spineToCache;
      }
    }
  } else if (type == ContentType::Txt && !(shouldAbort && shouldAbort())) {
    cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
    if (!parser) {
      parser.reset(new (std::nothrow) PlainTextParser(contentPath_.c_str(), renderer_, config));
      if (!parser) {
        LOG_ERR(TAG, "runBackgroundCache: OOM allocating PlainTextParser");
        return;
      }
      parserSpineIndex = 0;
    }
  }

  if (parser && !cachePath.empty() && !(shouldAbort && shouldAbort())) {
    backgroundCacheImpl(*parser, cachePath, config, currentSectionPage, shouldAbort);
  }

  if (type == ContentType::Epub && !(shouldAbort && shouldAbort())) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) {
      return;
    }

    auto epub = provider->getEpubShared();
    const int spineCount = epub->getSpineItemsCount();
    int activeSpine = currentSpineIndex;
    if (currentSectionPage == -1) {
      activeSpine = calcFirstContentSpine(hasCover, textStartIndex, spineCount);
    }

    const int nextSpine = activeSpine + 1;
    if (nextSpine < 0 || nextSpine >= spineCount) {
      return;
    }

    const std::string nextCachePath = epubSectionCachePath(epub->getCachePath(), nextSpine);
    PageCache nextCache(nextCachePath);
    if (nextCache.load(config)) {
      return;
    }

    const std::string imageCachePath = core.settings.showImages ? (epub->getCachePath() + "/images") : "";
    EpubChapterParser nextParser(epub, nextSpine, renderer_, config, imageCachePath);
    if (nextCache.create(nextParser, config, PageCache::DEFAULT_CACHE_CHUNK, 0, shouldAbort) &&
        !(shouldAbort && shouldAbort())) {
      saveAnchorMap(nextParser, nextCachePath);
      LOG_INF(TAG, "Prefetched next EPUB spine %d", nextSpine);
    }
  }
}

}  // namespace papyrix::reader
