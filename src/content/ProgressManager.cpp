#include "ProgressManager.h"

#include <Arduino.h>
#include <Logging.h>
#include <SdFat.h>

#include <algorithm>
#include <cstdio>

#include "../content/ContentHandle.h"
#include "../core/Core.h"

#define TAG "PROGRESS"

namespace papyrix {

namespace {
constexpr uint32_t kProgressMagic = 0x32504750;  // "PGP2"
constexpr uint8_t kProgressVersion = 2;
constexpr size_t kLegacyProgressSize = 4;
constexpr size_t kProgressHeaderSize = 4 + 1 + 2 + 2 + 4 + 1;
}

bool ProgressManager::save(Core& core, const char* cacheDir, ContentType type, const Progress& progress) {
  if (!cacheDir || cacheDir[0] == '\0') {
    return false;
  }

  char progressPath[280];
  snprintf(progressPath, sizeof(progressPath), "%s/progress.bin", cacheDir);

  FsFile file;
  auto result = core.storage.openWrite(progressPath, file);
  if (!result.ok()) {
    LOG_ERR(TAG, "Failed to save progress to %s", progressPath);
    return false;
  }

  const uint8_t anchorLen = static_cast<uint8_t>(
      std::min(strnlen(progress.textAnchor, Progress::kTextAnchorSize - 1), size_t{255}));
  uint8_t header[kProgressHeaderSize];
  header[0] = kProgressMagic & 0xFF;
  header[1] = (kProgressMagic >> 8) & 0xFF;
  header[2] = (kProgressMagic >> 16) & 0xFF;
  header[3] = (kProgressMagic >> 24) & 0xFF;
  header[4] = kProgressVersion;
  header[5] = progress.spineIndex & 0xFF;
  header[6] = (progress.spineIndex >> 8) & 0xFF;
  header[7] = progress.sectionPage & 0xFF;
  header[8] = (progress.sectionPage >> 8) & 0xFF;
  header[9] = progress.flatPage & 0xFF;
  header[10] = (progress.flatPage >> 8) & 0xFF;
  header[11] = (progress.flatPage >> 16) & 0xFF;
  header[12] = (progress.flatPage >> 24) & 0xFF;
  header[13] = anchorLen;

  if (file.write(header, sizeof(header)) != sizeof(header)) {
    LOG_ERR(TAG, "Failed to write progress header");
    file.close();
    return false;
  }
  if (anchorLen > 0) {
    if (file.write(reinterpret_cast<const uint8_t*>(progress.textAnchor), anchorLen) != anchorLen) {
      LOG_ERR(TAG, "Failed to write progress anchor");
      file.close();
      return false;
    }
  }

  LOG_DBG(TAG, "Saved progress: spine=%d page=%d flat=%u anchor=%u", progress.spineIndex, progress.sectionPage,
          progress.flatPage, anchorLen);

  file.close();
  return true;
}

ProgressManager::Progress ProgressManager::load(Core& core, const char* cacheDir, ContentType type) {
  Progress progress;
  progress.reset();

  if (!cacheDir || cacheDir[0] == '\0') {
    return progress;
  }

  char progressPath[280];
  snprintf(progressPath, sizeof(progressPath), "%s/progress.bin", cacheDir);

  FsFile file;
  auto result = core.storage.openRead(progressPath, file);
  if (!result.ok()) {
    LOG_DBG(TAG, "No saved progress found");
    return progress;
  }

  const size_t fileSize = file.size();
  if (fileSize < kLegacyProgressSize) {
    LOG_ERR(TAG, "Corrupted file (too small), using defaults");
    file.close();
    return progress;
  }

  uint8_t legacy[kLegacyProgressSize];
  if (file.read(legacy, sizeof(legacy)) != static_cast<int>(sizeof(legacy))) {
    LOG_ERR(TAG, "Read failed, using defaults");
    file.close();
    return progress;
  }

  const uint32_t magic = static_cast<uint32_t>(legacy[0]) | (static_cast<uint32_t>(legacy[1]) << 8) |
                         (static_cast<uint32_t>(legacy[2]) << 16) | (static_cast<uint32_t>(legacy[3]) << 24);
  if (magic == kProgressMagic && fileSize >= kProgressHeaderSize) {
    uint8_t version = 0;
    if (file.read(&version, 1) != 1 || version != kProgressVersion) {
      LOG_ERR(TAG, "Unsupported progress version %u", version);
      file.close();
      return progress;
    }

    uint8_t fields[2 + 2 + 4 + 1];
    if (file.read(fields, sizeof(fields)) != static_cast<int>(sizeof(fields))) {
      LOG_ERR(TAG, "Failed to read progress payload");
      file.close();
      return progress;
    }

    progress.spineIndex = static_cast<int16_t>(static_cast<uint16_t>(fields[0]) | (static_cast<uint16_t>(fields[1]) << 8));
    progress.sectionPage = static_cast<int16_t>(static_cast<uint16_t>(fields[2]) | (static_cast<uint16_t>(fields[3]) << 8));
    progress.flatPage = static_cast<uint32_t>(fields[4]) | (static_cast<uint32_t>(fields[5]) << 8) |
                        (static_cast<uint32_t>(fields[6]) << 16) | (static_cast<uint32_t>(fields[7]) << 24);
    const uint8_t anchorLen = std::min<uint8_t>(fields[8], Progress::kTextAnchorSize - 1);
    if (anchorLen > 0) {
      if (file.read(reinterpret_cast<uint8_t*>(progress.textAnchor), anchorLen) != anchorLen) {
        progress.textAnchor[0] = '\0';
      } else {
        progress.textAnchor[anchorLen] = '\0';
      }
    }

    LOG_DBG(TAG, "Loaded progress: spine=%d page=%d flat=%u anchor=%u", progress.spineIndex, progress.sectionPage,
            progress.flatPage, anchorLen);
  } else {
    if (type == ContentType::Epub) {
      progress.spineIndex = static_cast<int16_t>(static_cast<uint16_t>(legacy[0]) | (static_cast<uint16_t>(legacy[1]) << 8));
      progress.sectionPage = static_cast<int16_t>(static_cast<uint16_t>(legacy[2]) | (static_cast<uint16_t>(legacy[3]) << 8));
      LOG_DBG(TAG, "Loaded legacy EPUB: spine=%d page=%d", progress.spineIndex, progress.sectionPage);
    } else if (type == ContentType::Xtc) {
      progress.flatPage = static_cast<uint32_t>(legacy[0]) | (static_cast<uint32_t>(legacy[1]) << 8) |
                          (static_cast<uint32_t>(legacy[2]) << 16) | (static_cast<uint32_t>(legacy[3]) << 24);
      LOG_DBG(TAG, "Loaded legacy XTC: page %u", progress.flatPage);
    } else {
      progress.sectionPage = static_cast<int16_t>(static_cast<uint16_t>(legacy[0]) | (static_cast<uint16_t>(legacy[1]) << 8));
      LOG_DBG(TAG, "Loaded legacy text: page %d", progress.sectionPage);
    }
  }

  file.close();
  return progress;
}

ProgressManager::Progress ProgressManager::validate(Core& core, ContentType type, const Progress& progress) {
  Progress validated = progress;

  if (type == ContentType::Epub) {
    // Validate spine index
    auto* provider = core.content.asEpub();
    if (provider && provider->getEpub()) {
      uint32_t spineCount = provider->getEpub()->getSpineItemsCount();
      if (validated.spineIndex < 0) {
        validated.spineIndex = 0;
      }
      if (validated.spineIndex >= static_cast<int>(spineCount)) {
        validated.spineIndex = spineCount > 0 ? spineCount - 1 : 0;
        validated.sectionPage = 0;
      }
    }
  } else if (type == ContentType::Xtc) {
    // Validate flat page
    uint32_t total = core.content.pageCount();
    if (validated.flatPage >= total) {
      validated.flatPage = total > 0 ? total - 1 : 0;
    }
  }
  // TXT/Markdown: page validation happens during cache creation

  return validated;
}

}  // namespace papyrix
