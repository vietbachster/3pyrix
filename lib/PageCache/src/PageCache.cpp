#include "PageCache.h"

#include <Logging.h>

#define TAG "CACHE"

#include <Page.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include "ContentParser.h"
#include "PageCacheHeader.h"

namespace {
bool evictCacheFile(const std::string& cachePath, const char* reason) {
  if (!SdMan.exists(cachePath.c_str())) {
    return true;
  }

  if (SdMan.remove(cachePath.c_str())) {
    LOG_INF(TAG, "Removed stale cache file reason=%s path=%s", reason ? reason : "unknown", cachePath.c_str());
    return true;
  }

  const std::string quarantinePath = cachePath + ".stale";
  if (SdMan.exists(quarantinePath.c_str())) {
    SdMan.remove(quarantinePath.c_str());
  }

  if (SdMan.rename(cachePath.c_str(), quarantinePath.c_str())) {
    LOG_INF(TAG, "Quarantined stale cache file reason=%s path=%s quarantine=%s", reason ? reason : "unknown",
            cachePath.c_str(), quarantinePath.c_str());
    return true;
  }

  LOG_ERR(TAG, "Failed to evict stale cache file reason=%s path=%s", reason ? reason : "unknown", cachePath.c_str());
  return false;
}
}  // namespace

PageCache::PageCache(std::string cachePath) : cachePath_(std::move(cachePath)) {}

void PageCache::resetState() {
  if (file_) {
    file_.close();
  }
  pageCount_ = 0;
  isPartial_ = false;
  config_ = RenderConfig{};
  lutOffset_ = 0;
}

bool PageCache::writeHeader(bool isPartial) {
  file_.seek(0);
  serialization::writePod(file_, pagecache::kFileVersion);
  serialization::writePod(file_, config_.fontId);
  serialization::writePod(file_, config_.lineCompression);
  serialization::writePod(file_, config_.readerFontSize);
  serialization::writePod(file_, config_.indentLevel);
  serialization::writePod(file_, config_.spacingLevel);
  serialization::writePod(file_, config_.paragraphAlignment);
  serialization::writePod(file_, config_.hyphenation);
  serialization::writePod(file_, config_.showImages);
  serialization::writePod(file_, config_.viewportWidth);
  serialization::writePod(file_, config_.viewportHeight);
  serialization::writePod(file_, pageCount_);
  serialization::writePod(file_, static_cast<uint8_t>(isPartial ? 1 : 0));
  serialization::writePod(file_, static_cast<uint32_t>(0));  // LUT offset placeholder
  return true;
}

bool PageCache::writeLut(const std::vector<uint32_t>& lut) {
  const uint32_t lutOffset = file_.position();

  for (const uint32_t pos : lut) {
    if (pos == 0) {
      LOG_ERR(TAG, "Invalid page position in LUT");
      return false;
    }
    serialization::writePod(file_, pos);
  }

  // Update header with final values
  file_.seek(pagecache::kHeaderSize - 4 - 1 - 2);  // Seek to pageCount
  serialization::writePod(file_, pageCount_);
  serialization::writePod(file_, static_cast<uint8_t>(isPartial_ ? 1 : 0));
  serialization::writePod(file_, lutOffset);
  lutOffset_ = lutOffset;

  return true;
}

bool PageCache::loadLut(std::vector<uint32_t>& lut) {
  if (!SdMan.openFileForRead("CACHE", cachePath_, file_)) {
    return false;
  }

  const size_t fileSize = file_.size();
  if (fileSize < pagecache::kHeaderSize) {
    LOG_ERR(TAG, "File too small: %zu (need %u)", fileSize, pagecache::kHeaderSize);
    file_.close();
    evictCacheFile(cachePath_, "load-lut-file-too-small");
    resetState();
    return false;
  }

  pagecache::HeaderInfo header;
  const auto status = pagecache::readHeader(file_, header, false);
  if (status != pagecache::HeaderReadStatus::Ok) {
    file_.close();
    evictCacheFile(cachePath_, status == pagecache::HeaderReadStatus::VersionMismatch ? "load-lut-version-mismatch"
                                                                                      : "load-lut-read-header-failed");
    resetState();
    return false;
  }

  if (!pagecache::validateIndexBounds(cachePath_.c_str(), fileSize, header.pageCount, header.lutOffset)) {
    file_.close();
    evictCacheFile(cachePath_, "load-lut-invalid-header");
    resetState();
    return false;
  }

  // Read existing LUT entries
  std::vector<uint32_t> loadedLut;
  file_.seek(header.lutOffset);
  loadedLut.reserve(header.pageCount);
  for (uint16_t i = 0; i < header.pageCount; i++) {
    uint32_t pos;
    if (!serialization::readPodChecked(file_, pos)) {
      file_.close();
      evictCacheFile(cachePath_, "load-lut-read-entry-failed");
      resetState();
      return false;
    }
    if (pos < pagecache::kHeaderSize || pos >= header.lutOffset || pos >= fileSize) {
      LOG_ERR(TAG, "Invalid page position in LUT: %u (lutOffset=%u file size=%zu)", pos,
              static_cast<unsigned>(header.lutOffset), fileSize);
      file_.close();
      evictCacheFile(cachePath_, "load-lut-invalid-entry");
      resetState();
      return false;
    }
    loadedLut.push_back(pos);
  }

  file_.close();
  lut = std::move(loadedLut);
  pageCount_ = header.pageCount;
  lutOffset_ = header.lutOffset;
  return true;
}

bool PageCache::loadRaw() {
  if (!SdMan.openFileForRead("CACHE", cachePath_, file_)) {
    resetState();
    return false;
  }

  pagecache::HeaderInfo header;
  const auto status = pagecache::readHeader(file_, header, false);
  if (status == pagecache::HeaderReadStatus::VersionMismatch) {
    file_.close();
    resetState();
    return false;
  }
  if (status != pagecache::HeaderReadStatus::Ok) {
    file_.close();
    evictCacheFile(cachePath_, "load-raw-read-header-failed");
    resetState();
    return false;
  }

  if (!pagecache::validateIndexBounds(cachePath_.c_str(), file_.size(), header.pageCount, header.lutOffset)) {
    file_.close();
    evictCacheFile(cachePath_, "load-raw-invalid-header");
    resetState();
    return false;
  }

  file_.close();
  pageCount_ = header.pageCount;
  isPartial_ = header.isPartial;
  lutOffset_ = header.lutOffset;
  return true;
}

bool PageCache::load(const RenderConfig& config) {
  if (!SdMan.openFileForRead("CACHE", cachePath_, file_)) {
    resetState();
    return false;
  }

  pagecache::HeaderInfo header;
  const auto status = pagecache::readHeader(file_, header, true);
  if (status == pagecache::HeaderReadStatus::VersionMismatch) {
    file_.close();
    clear();
    return false;
  }
  if (status != pagecache::HeaderReadStatus::Ok) {
    file_.close();
    evictCacheFile(cachePath_, "load-read-header-failed");
    resetState();
    return false;
  }

  if (config != header.config) {
    file_.close();
    LOG_INF(TAG, "Config mismatch, invalidating cache");
    clear();
    return false;
  }

  if (!pagecache::validateIndexBounds(cachePath_.c_str(), file_.size(), header.pageCount, header.lutOffset)) {
    file_.close();
    clear();
    return false;
  }

  file_.close();
  pageCount_ = header.pageCount;
  isPartial_ = header.isPartial;
  config_ = config;
  lutOffset_ = header.lutOffset;
  LOG_INF(TAG, "Loaded: %d pages, partial=%d", pageCount_, isPartial_);
  return true;
}

bool PageCache::create(ContentParser& parser, const RenderConfig& config, uint16_t maxPages, uint16_t skipPages,
                       const AbortCallback& shouldAbort) {
  const unsigned long startMs = millis();

  std::vector<uint32_t> lut;
  const bool isExtendPass = skipPages > 0;
  uint16_t initialPageCount = 0;

  if (isExtendPass) {
    // Extending: load existing LUT
    if (!loadLut(lut)) {
      LOG_ERR(TAG, "Failed to load existing LUT for extend");
      return false;
    }
    initialPageCount = pageCount_;

    // Append new pages AFTER old LUT (crash-safe: old LUT remains valid until header update)
    if (!file_.open(cachePath_.c_str(), O_RDWR)) {
      LOG_ERR(TAG, "Failed to open cache file for append");
      return false;
    }
    file_.seekEnd();  // Append after old LUT
  } else {
    // Fresh create
    if (!SdMan.openFileForWrite("CACHE", cachePath_, file_)) {
      LOG_ERR(TAG, "Failed to open cache file for writing");
      return false;
    }

    config_ = config;
    pageCount_ = 0;
    isPartial_ = false;

    // Write placeholder header
    writeHeader(false);
  }

  // Check for abort before starting expensive parsing
  if (shouldAbort && shouldAbort()) {
    file_.close();
    if (!isExtendPass) {
      evictCacheFile(cachePath_, "create-aborted-before-parse");
      resetState();
      return false;
    }
    pageCount_ = initialPageCount;
    isPartial_ = true;
    lutOffset_ = 0;
    LOG_INF(TAG, "Aborted before parsing");
    return true;
  }

  uint16_t parsedPages = 0;
  bool hitMaxPages = false;
  bool aborted = false;

  bool success = parser.parsePages(
      [this, &lut, &hitMaxPages, &parsedPages, maxPages, skipPages](std::unique_ptr<Page> page) {
        if (hitMaxPages) return;

        parsedPages++;

        // Skip pages we already have cached
        if (parsedPages <= skipPages) {
          return;
        }

        // Serialize new page
        const uint32_t position = file_.position();
        if (!page->serialize(file_)) {
          LOG_ERR(TAG, "Failed to serialize page %d", pageCount_);
          return;
        }

        lut.push_back(position);
        pageCount_++;
        LOG_DBG(TAG, "Page %d cached", pageCount_ - 1);

        if (maxPages > 0 && pageCount_ >= maxPages) {
          hitMaxPages = true;
        }
      },
      maxPages, shouldAbort);

  // Check if we were aborted
  if (shouldAbort && shouldAbort()) {
    aborted = true;
    LOG_INF(TAG, "Aborted during parsing");
  }

  const bool madeForwardProgress = pageCount_ > initialPageCount;

  if (!success && pageCount_ == 0) {
    file_.close();
    // Remove file to prevent corrupt/incomplete cache
    evictCacheFile(cachePath_, "create-empty-failure");
    resetState();
    LOG_ERR(TAG, "Parsing failed or aborted with %d pages", pageCount_);
    return false;
  }

  if (aborted || !success) {
    if (!madeForwardProgress) {
      file_.close();
      if (isExtendPass && initialPageCount > 0) {
        pageCount_ = initialPageCount;
        isPartial_ = true;
        lutOffset_ = 0;
        LOG_INF(TAG, "Create aborted without progress, keeping previous cache (%u pages)",
                static_cast<unsigned>(pageCount_));
        return true;
      }
      evictCacheFile(cachePath_, "create-no-progress");
      resetState();
      LOG_ERR(TAG, "Parsing failed or aborted with no forward progress");
      return false;
    }

    // Preserve whatever was serialized so far as a partial cache instead of
    // throwing away progress after an abort or parser failure.
    isPartial_ = true;
    if (!writeLut(lut)) {
      file_.close();
      if (isExtendPass && initialPageCount > 0) {
        pageCount_ = initialPageCount;
        isPartial_ = true;
        lutOffset_ = 0;
        LOG_INF(TAG, "Failed to finalize partial extend cache, keeping previous cache");
        return true;
      }
      evictCacheFile(cachePath_, "create-partial-write-lut-failed");
      resetState();
      return false;
    }

    file_.sync();
    file_.close();
    LOG_INF(TAG, "Created partial cache in %lu ms: %d pages, partial=%d", millis() - startMs, pageCount_,
            isPartial_);
    return true;
  }

  isPartial_ = parser.hasMoreContent();

  if (!writeLut(lut)) {
    file_.close();
    if (isExtendPass && initialPageCount > 0) {
      pageCount_ = initialPageCount;
      isPartial_ = true;
      lutOffset_ = 0;
      LOG_INF(TAG, "Failed to update LUT for extend, keeping previous cache");
      return true;
    }
    evictCacheFile(cachePath_, "create-write-lut-failed");
    resetState();
    return false;
  }

  file_.sync();
  file_.close();
  LOG_INF(TAG, "Created in %lu ms: %d pages, partial=%d", millis() - startMs, pageCount_, isPartial_);
  return true;
}

bool PageCache::extend(ContentParser& parser, uint16_t additionalPages, const AbortCallback& shouldAbort) {
  if (!isPartial_) {
    LOG_INF(TAG, "Cache is complete, nothing to extend");
    return true;
  }

  const uint16_t chunk = pageCount_ >= 30 ? 50 : additionalPages;
  const uint16_t currentPages = pageCount_;

  if (parser.canResume()) {
    // HOT PATH: Parser has live session from previous extend, just append new pages.
    // No re-parsing — O(chunk) work instead of O(totalPages).
    LOG_INF(TAG, "Hot extend from %d pages (+%d)", currentPages, chunk);

    std::vector<uint32_t> lut;
    if (!loadLut(lut)) return false;

    bool opened = false;
    for (int attempt = 0; attempt < 3; attempt++) {
      if (attempt > 0) delay(50);
      if (file_.open(cachePath_.c_str(), O_RDWR)) {
        opened = true;
        break;
      }
    }
    if (!opened) {
      LOG_ERR(TAG, "Failed to open cache file for hot extend");
      return false;
    }
    file_.seekEnd();

    const uint16_t pagesBefore = pageCount_;
    bool parseOk = parser.parsePages(
        [this, &lut](std::unique_ptr<Page> page) {
          const uint32_t position = file_.position();
          if (!page->serialize(file_)) return;
          lut.push_back(position);
          pageCount_++;
        },
        chunk, shouldAbort);

    isPartial_ = parser.hasMoreContent();

    if (!parseOk && pageCount_ == pagesBefore) {
      file_.close();
      LOG_ERR(TAG, "Hot extend failed with no new pages");
      return false;
    }

    if (!writeLut(lut)) {
      file_.close();
      pageCount_ = pagesBefore;
      isPartial_ = true;
      lutOffset_ = 0;
      LOG_INF(TAG, "Failed to update LUT for hot extend, keeping previous cache");
      return true;
    }

    file_.sync();
    file_.close();
    LOG_INF(TAG, "Hot extend done: %d pages, partial=%d", pageCount_, isPartial_);
    return true;
  }

  // COLD PATH: Fresh parser (after exit/reboot) — re-parse from start, skip cached pages.
  const uint16_t targetPages = pageCount_ + chunk;
  LOG_INF(TAG, "Cold extend from %d to %d pages", currentPages, targetPages);

  parser.reset();
  bool result = create(parser, config_, targetPages, currentPages, shouldAbort);

  // No forward progress AND parser has no more content → content is truly finished.
  // Without the hasMoreContent() check, an aborted extend (timeout/memory pressure)
  // would permanently mark the chapter as complete, truncating it.
  if (result && pageCount_ <= currentPages && !parser.hasMoreContent()) {
    LOG_INF(TAG, "No progress during extend (%d pages), marking complete", pageCount_);
    isPartial_ = false;
  }

  return result;
}

std::unique_ptr<Page> PageCache::loadPage(uint16_t pageNum) {
  if (pageNum >= pageCount_) {
    LOG_ERR(TAG, "Page %d out of range (max %d)", pageNum, pageCount_);
    return nullptr;
  }

  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(50);

    if (!SdMan.openFileForRead("CACHE", cachePath_, file_)) {
      continue;
    }

    const size_t fileSize = file_.size();

    // Read pageCount from header to validate LUT bounds against on-disk state.
    pagecache::HeaderInfo header;
    const auto status = pagecache::readHeader(file_, header, false);
    if (status == pagecache::HeaderReadStatus::VersionMismatch) {
      file_.close();
      clear();
      return nullptr;
    }
    if (status != pagecache::HeaderReadStatus::Ok) {
      file_.close();
      evictCacheFile(cachePath_, "load-page-read-header-failed");
      resetState();
      return nullptr;
    }

    if (!pagecache::validateIndexBounds(cachePath_.c_str(), fileSize, header.pageCount, header.lutOffset)) {
      file_.close();
      clear();
      return nullptr;
    }

    if (pageNum >= header.pageCount) {
      LOG_ERR(TAG, "Page %d out of range in on-disk cache (max %d)", pageNum, header.pageCount);
      file_.close();
      pageCount_ = header.pageCount;
      return nullptr;
    }

    // Read page position from LUT
    file_.seek(header.lutOffset + static_cast<size_t>(pageNum) * sizeof(uint32_t));
    uint32_t pagePos;
    if (!serialization::readPodChecked(file_, pagePos)) {
      file_.close();
      evictCacheFile(cachePath_, "load-page-read-entry-failed");
      resetState();
      return nullptr;
    }

    // Validate page position
    if (pagePos < pagecache::kHeaderSize || pagePos >= header.lutOffset || pagePos >= fileSize) {
      LOG_ERR(TAG, "Invalid page position: %u (lutOffset=%u file size=%zu)", pagePos,
              static_cast<unsigned>(header.lutOffset), fileSize);
      file_.close();
      clear();
      return nullptr;
    }

    // Read page
    file_.seek(pagePos);
    auto page = Page::deserialize(file_);
    file_.close();

    if (page) return page;
  }

  return nullptr;
}

bool PageCache::clear() {
  resetState();
  return evictCacheFile(cachePath_, "clear");
}
