#include "PageCacheHeader.h"

#include <Logging.h>
#include <Serialization.h>

namespace {

constexpr const char* TAG = "CACHE";

}  // namespace

namespace pagecache {

HeaderReadStatus readHeader(FsFile& file, HeaderInfo& info, const bool readConfig) {
  uint8_t version = 0;
  if (!serialization::readPodChecked(file, version)) {
    return HeaderReadStatus::Truncated;
  }

  if (version != kFileVersion) {
    LOG_ERR(TAG, "Version mismatch: got %u, expected %u", version, kFileVersion);
    return HeaderReadStatus::VersionMismatch;
  }

  if (readConfig) {
    if (!serialization::readPodChecked(file, info.config.fontId) ||
        !serialization::readPodChecked(file, info.config.lineCompression) ||
        !serialization::readPodChecked(file, info.config.readerFontSize) ||
        !serialization::readPodChecked(file, info.config.indentLevel) ||
        !serialization::readPodChecked(file, info.config.spacingLevel) ||
        !serialization::readPodChecked(file, info.config.paragraphAlignment) ||
        !serialization::readPodChecked(file, info.config.hyphenation) ||
        !serialization::readPodChecked(file, info.config.showImages) ||
        !serialization::readPodChecked(file, info.config.viewportWidth) ||
        !serialization::readPodChecked(file, info.config.viewportHeight)) {
      return HeaderReadStatus::Truncated;
    }
  } else if (!file.seek(kHeaderSize - 4 - 1 - 2)) {
    return HeaderReadStatus::Truncated;
  }

  uint8_t partial = 0;
  if (!serialization::readPodChecked(file, info.pageCount) || !serialization::readPodChecked(file, partial) ||
      !serialization::readPodChecked(file, info.lutOffset)) {
    return HeaderReadStatus::Truncated;
  }

  info.isPartial = (partial != 0);
  return HeaderReadStatus::Ok;
}

bool validateIndexBounds(const char* cachePath, const size_t fileSize, const uint16_t pageCount,
                         const uint32_t lutOffset) {
  if (pageCount == 0) {
    LOG_ERR(TAG, "Rejecting empty/incomplete cache file: %s", cachePath);
    return false;
  }

  if (pageCount > kMaxReasonablePageCount) {
    LOG_ERR(TAG, "Rejecting cache with implausible page count %u: %s", static_cast<unsigned>(pageCount), cachePath);
    return false;
  }

  if (lutOffset < kHeaderSize || lutOffset >= fileSize) {
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

}  // namespace pagecache
