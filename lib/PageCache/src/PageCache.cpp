#include "PageCache.h"

#include <Logging.h>

#define TAG "CACHE"

#include <Page.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include "ContentParser.h"

namespace {
constexpr uint8_t CACHE_FILE_VERSION = 20;  // v20: invalidate caches after line-breaker behavior change
constexpr uint16_t MAX_REASONABLE_PAGE_COUNT = 8192;

// Header layout:
// - version (1 byte)
// - fontId (4 bytes)
// - lineCompression (4 bytes)
// - readerFontSize (1 byte)
// - indentLevel (1 byte)
// - spacingLevel (1 byte)
// - paragraphAlignment (1 byte)
// - hyphenation (1 byte)
// - showImages (1 byte)
// - viewportWidth (2 bytes)
// - viewportHeight (2 bytes)
// - pageCount (2 bytes)
// - isPartial (1 byte)
// - lutOffset (4 bytes)
constexpr uint32_t HEADER_SIZE = 1 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 1 + 4;

bool validateCacheIndexBounds(const char* cachePath, const size_t fileSize, const uint16_t pageCount,
                              const uint32_t lutOffset) {
  if (pageCount == 0) {
    LOG_ERR(TAG, "Rejecting empty/incomplete cache file: %s", cachePath);
    return false;
  }

  if (pageCount > MAX_REASONABLE_PAGE_COUNT) {
    LOG_ERR(TAG, "Rejecting cache with implausible page count %u: %s", static_cast<unsigned>(pageCount), cachePath);
    return false;
  }

  if (lutOffset < HEADER_SIZE || lutOffset >= fileSize) {
    LOG_ERR(TAG, "Invalid lutOffset: %u (file size: %zu)", static_cast<unsigned>(lutOffset), fileSize);
    return false;
  }

  const uint64_t lutBytes = static_cast<uint64_t>(pageCount) * sizeof(uint32_t);
  const uint64_t lutEnd = static_cast<uint64_t>(lutOffset) + lutBytes;
  if (lutEnd > fileSize) {
    LOG_ERR(TAG, "Rejecting cache with truncated LUT: path=%s pages=%u lutOffset=%u fileSize=%zu", cachePath,
            static_cast<unsigned>(pageCount), static_cast<unsigned>(lutOffset), fileSize);
    return false;
  }

  return true;
}

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
  serialization::writePod(file_, CACHE_FILE_VERSION);
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
  file_.seek(HEADER_SIZE - 4 - 1 - 2);  // Seek to pageCount
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
  if (fileSize < HEADER_SIZE) {
    LOG_ERR(TAG, "File too small: %zu (need %u)", fileSize, HEADER_SIZE);
    file_.close();
    evictCacheFile(cachePath_, "load-lut-file-too-small");
    resetState();
    return false;
  }

  // Read lutOffset from header
  file_.seek(HEADER_SIZE - 4);
  uint32_t lutOffset = 0;
  if (!serialization::readPodChecked(file_, lutOffset)) {
    file_.close();
    evictCacheFile(cachePath_, "load-lut-read-offset-failed");
    resetState();
    return false;
  }

  // Read pageCount from header
  file_.seek(HEADER_SIZE - 4 - 1 - 2);
  uint16_t pageCount = 0;
  if (!serialization::readPodChecked(file_, pageCount)) {
    file_.close();
    evictCacheFile(cachePath_, "load-lut-read-count-failed");
    resetState();
    return false;
  }

  if (!validateCacheIndexBounds(cachePath_.c_str(), fileSize, pageCount, lutOffset)) {
    file_.close();
    evictCacheFile(cachePath_, "load-lut-invalid-header");
    resetState();
    return false;
  }

  // Read existing LUT entries
  std::vector<uint32_t> loadedLut;
  file_.seek(lutOffset);
  loadedLut.reserve(pageCount);
  for (uint16_t i = 0; i < pageCount; i++) {
    uint32_t pos;
    if (!serialization::readPodChecked(file_, pos)) {
      file_.close();
      evictCacheFile(cachePath_, "load-lut-read-entry-failed");
      resetState();
      return false;
    }
    if (pos < HEADER_SIZE || pos >= lutOffset || pos >= fileSize) {
      LOG_ERR(TAG, "Invalid page position in LUT: %u (lutOffset=%u file size=%zu)", pos,
              static_cast<unsigned>(lutOffset), fileSize);
      file_.close();
      evictCacheFile(cachePath_, "load-lut-invalid-entry");
      resetState();
      return false;
    }
    loadedLut.push_back(pos);
  }

  file_.close();
  lut = std::move(loadedLut);
  pageCount_ = pageCount;
  lutOffset_ = lutOffset;
  return true;
}

bool PageCache::loadRaw() {
  if (!SdMan.openFileForRead("CACHE", cachePath_, file_)) {
    resetState();
    return false;
  }

  uint8_t version;
  if (!serialization::readPodChecked(file_, version)) {
    file_.close();
    evictCacheFile(cachePath_, "load-raw-read-version-failed");
    resetState();
    return false;
  }
  if (version != CACHE_FILE_VERSION) {
    file_.close();
    resetState();
    LOG_ERR(TAG, "Version mismatch: got %u, expected %u", version, CACHE_FILE_VERSION);
    return false;
  }

  // Skip config fields, read pageCount and isPartial
  file_.seek(HEADER_SIZE - 4 - 1 - 2);
  uint16_t pageCount = 0;
  if (!serialization::readPodChecked(file_, pageCount)) {
    file_.close();
    evictCacheFile(cachePath_, "load-raw-read-count-failed");
    resetState();
    return false;
  }
  uint8_t partial;
  if (!serialization::readPodChecked(file_, partial)) {
    file_.close();
    evictCacheFile(cachePath_, "load-raw-read-partial-failed");
    resetState();
    return false;
  }
  uint32_t lutOffset = 0;
  if (!serialization::readPodChecked(file_, lutOffset)) {
    file_.close();
    evictCacheFile(cachePath_, "load-raw-read-offset-failed");
    resetState();
    return false;
  }

  if (!validateCacheIndexBounds(cachePath_.c_str(), file_.size(), pageCount, lutOffset)) {
    file_.close();
    evictCacheFile(cachePath_, "load-raw-invalid-header");
    resetState();
    return false;
  }

  file_.close();
  pageCount_ = pageCount;
  isPartial_ = (partial != 0);
  lutOffset_ = lutOffset;
  return true;
}

bool PageCache::load(const RenderConfig& config) {
  if (!SdMan.openFileForRead("CACHE", cachePath_, file_)) {
    resetState();
    return false;
  }

  // Read and validate header
  uint8_t version;
  if (!serialization::readPodChecked(file_, version)) {
    file_.close();
    evictCacheFile(cachePath_, "load-read-version-failed");
    resetState();
    return false;
  }
  if (version != CACHE_FILE_VERSION) {
    file_.close();
    LOG_ERR(TAG, "Version mismatch: got %u, expected %u", version, CACHE_FILE_VERSION);
    clear();
    return false;
  }

  RenderConfig fileConfig;
  if (!serialization::readPodChecked(file_, fileConfig.fontId) ||
      !serialization::readPodChecked(file_, fileConfig.lineCompression) ||
      !serialization::readPodChecked(file_, fileConfig.readerFontSize) ||
      !serialization::readPodChecked(file_, fileConfig.indentLevel) ||
      !serialization::readPodChecked(file_, fileConfig.spacingLevel) ||
      !serialization::readPodChecked(file_, fileConfig.paragraphAlignment) ||
      !serialization::readPodChecked(file_, fileConfig.hyphenation) ||
      !serialization::readPodChecked(file_, fileConfig.showImages) ||
      !serialization::readPodChecked(file_, fileConfig.viewportWidth) ||
      !serialization::readPodChecked(file_, fileConfig.viewportHeight)) {
    file_.close();
    evictCacheFile(cachePath_, "load-read-config-failed");
    resetState();
    return false;
  }

  if (config != fileConfig) {
    file_.close();
    LOG_INF(TAG, "Config mismatch, invalidating cache");
    clear();
    return false;
  }

  uint16_t pageCount = 0;
  if (!serialization::readPodChecked(file_, pageCount)) {
    file_.close();
    evictCacheFile(cachePath_, "load-read-count-failed");
    resetState();
    return false;
  }
  uint8_t partial;
  if (!serialization::readPodChecked(file_, partial)) {
    file_.close();
    evictCacheFile(cachePath_, "load-read-partial-failed");
    resetState();
    return false;
  }
  uint32_t lutOffset = 0;
  if (!serialization::readPodChecked(file_, lutOffset)) {
    file_.close();
    evictCacheFile(cachePath_, "load-read-offset-failed");
    resetState();
    return false;
  }

  if (!validateCacheIndexBounds(cachePath_.c_str(), file_.size(), pageCount, lutOffset)) {
    file_.close();
    clear();
    return false;
  }

  file_.close();
  pageCount_ = pageCount;
  isPartial_ = (partial != 0);
  config_ = config;
  lutOffset_ = lutOffset;
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
    file_.seek(HEADER_SIZE - 4 - 1 - 2);
    uint16_t storedPageCount = 0;
    if (!serialization::readPodChecked(file_, storedPageCount)) {
      file_.close();
      evictCacheFile(cachePath_, "load-page-read-count-failed");
      resetState();
      return nullptr;
    }

    // Read LUT offset from header
    file_.seek(HEADER_SIZE - 4);
    uint32_t lutOffset;
    if (!serialization::readPodChecked(file_, lutOffset)) {
      file_.close();
      evictCacheFile(cachePath_, "load-page-read-offset-failed");
      resetState();
      return nullptr;
    }

    if (!validateCacheIndexBounds(cachePath_.c_str(), fileSize, storedPageCount, lutOffset)) {
      file_.close();
      clear();
      return nullptr;
    }

    if (pageNum >= storedPageCount) {
      LOG_ERR(TAG, "Page %d out of range in on-disk cache (max %d)", pageNum, storedPageCount);
      file_.close();
      pageCount_ = storedPageCount;
      return nullptr;
    }

    // Read page position from LUT
    file_.seek(lutOffset + static_cast<size_t>(pageNum) * sizeof(uint32_t));
    uint32_t pagePos;
    if (!serialization::readPodChecked(file_, pagePos)) {
      file_.close();
      evictCacheFile(cachePath_, "load-page-read-entry-failed");
      resetState();
      return nullptr;
    }

    // Validate page position
    if (pagePos < HEADER_SIZE || pagePos >= lutOffset || pagePos >= fileSize) {
      LOG_ERR(TAG, "Invalid page position: %u (lutOffset=%u file size=%zu)", pagePos,
              static_cast<unsigned>(lutOffset), fileSize);
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
