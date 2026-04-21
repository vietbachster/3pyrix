#include "EpdFontLoader.h"

#include <LittleFS.h>
#include <Logging.h>
#include <SDCardManager.h>

#include <cstring>

#define TAG "FONT_LOAD"

namespace {

constexpr uint32_t MAX_BITMAP_SIZE = 512 * 1024;  // 512KB
constexpr int GLYPH_BINARY_SIZE = 14;

bool isSupportedVersion(uint16_t version) {
  return version == EpdFontLoader::LEGACY_VERSION || version == EpdFontLoader::VERSION;
}

size_t calculateTableBytes(uint16_t kernLeftEntryCount, uint16_t kernRightEntryCount, uint8_t kernLeftClassCount,
                           uint8_t kernRightClassCount, uint32_t ligaturePairCount) {
  const size_t kernEntryBytes =
      static_cast<size_t>(kernLeftEntryCount + kernRightEntryCount) * sizeof(EpdKernClassEntry);
  const size_t kernMatrixBytes = static_cast<size_t>(kernLeftClassCount) * kernRightClassCount;
  const size_t ligatureBytes = static_cast<size_t>(ligaturePairCount) * sizeof(EpdLigaturePair);
  return kernEntryBytes + kernMatrixBytes + ligatureBytes;
}

void freeFontTables(EpdFontData& fontData) {
  delete[] fontData.kernLeftClasses;
  delete[] fontData.kernRightClasses;
  delete[] fontData.kernMatrix;
  delete[] fontData.ligaturePairs;

  fontData.kernLeftClasses = nullptr;
  fontData.kernRightClasses = nullptr;
  fontData.kernMatrix = nullptr;
  fontData.kernLeftEntryCount = 0;
  fontData.kernRightEntryCount = 0;
  fontData.kernLeftClassCount = 0;
  fontData.kernRightClassCount = 0;
  fontData.ligaturePairs = nullptr;
  fontData.ligaturePairCount = 0;
}

template <typename FileT>
bool readExact(FileT& file, void* buffer, size_t size) {
  return file.read(reinterpret_cast<uint8_t*>(buffer), size) == size;
}

template <typename FileT>
bool readGlyphs(FileT& file, EpdGlyph* glyphs, uint32_t glyphCount) {
  for (uint32_t i = 0; i < glyphCount; i++) {
    uint8_t glyphData[GLYPH_BINARY_SIZE];
    if (!readExact(file, glyphData, GLYPH_BINARY_SIZE)) {
      return false;
    }

    glyphs[i].width = glyphData[0];
    glyphs[i].height = glyphData[1];
    glyphs[i].advanceX = glyphData[2];
    glyphs[i].left = static_cast<int16_t>(glyphData[4] | (glyphData[5] << 8));
    glyphs[i].top = static_cast<int16_t>(glyphData[6] | (glyphData[7] << 8));
    glyphs[i].dataLength = static_cast<uint16_t>(glyphData[8] | (glyphData[9] << 8));
    glyphs[i].dataOffset =
        static_cast<uint32_t>(glyphData[10] | (glyphData[11] << 8) | (glyphData[12] << 16) | (glyphData[13] << 24));
  }
  return true;
}

template <typename FileT>
bool readExtendedTables(FileT& file, EpdFontData& fontData, uint16_t kernLeftEntryCount, uint16_t kernRightEntryCount,
                        uint8_t kernLeftClassCount, uint8_t kernRightClassCount, uint32_t ligaturePairCount) {
  if (kernLeftEntryCount > 0) {
    auto* entries = new (std::nothrow) EpdKernClassEntry[kernLeftEntryCount];
    if (!entries || !readExact(file, entries, static_cast<size_t>(kernLeftEntryCount) * sizeof(EpdKernClassEntry))) {
      delete[] entries;
      return false;
    }
    fontData.kernLeftClasses = entries;
    fontData.kernLeftEntryCount = kernLeftEntryCount;
  }

  if (kernRightEntryCount > 0) {
    auto* entries = new (std::nothrow) EpdKernClassEntry[kernRightEntryCount];
    if (!entries || !readExact(file, entries, static_cast<size_t>(kernRightEntryCount) * sizeof(EpdKernClassEntry))) {
      delete[] entries;
      return false;
    }
    fontData.kernRightClasses = entries;
    fontData.kernRightEntryCount = kernRightEntryCount;
  }

  const size_t kernMatrixSize = static_cast<size_t>(kernLeftClassCount) * kernRightClassCount;
  if (kernMatrixSize > 0) {
    auto* matrix = new (std::nothrow) int8_t[kernMatrixSize];
    if (!matrix || !readExact(file, matrix, kernMatrixSize)) {
      delete[] matrix;
      return false;
    }
    fontData.kernMatrix = matrix;
    fontData.kernLeftClassCount = kernLeftClassCount;
    fontData.kernRightClassCount = kernRightClassCount;
  }

  if (ligaturePairCount > 0) {
    auto* pairs = new (std::nothrow) EpdLigaturePair[ligaturePairCount];
    if (!pairs || !readExact(file, pairs, static_cast<size_t>(ligaturePairCount) * sizeof(EpdLigaturePair))) {
      delete[] pairs;
      return false;
    }
    fontData.ligaturePairs = pairs;
    fontData.ligaturePairCount = ligaturePairCount;
  }

  return true;
}

}  // namespace

bool EpdFontLoader::validateMetricsAndMemory(const FileMetrics& metrics, const size_t extraTableBytes) {
  if (metrics.intervalCount > 10000 || metrics.glyphCount > 100000 || metrics.bitmapSize > MAX_BITMAP_SIZE) {
    LOG_ERR(TAG, "Font exceeds size limits (bitmap=%u, max=%u). Using default font.", metrics.bitmapSize,
            MAX_BITMAP_SIZE);
    return false;
  }

  const size_t requiredMemory =
      metrics.intervalCount * sizeof(EpdUnicodeInterval) + metrics.glyphCount * sizeof(EpdGlyph) + metrics.bitmapSize +
      sizeof(EpdFontData) + extraTableBytes;
  const size_t availableHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (requiredMemory > availableHeap * 0.8) {
    LOG_ERR(TAG, "Insufficient memory: need %zu, available %zu. Using default font.", requiredMemory, availableHeap);
    return false;
  }

  return true;
}

EpdFontLoader::LoadResult EpdFontLoader::loadFromFile(const char* path) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(50);

    LoadResult result = {false, nullptr, nullptr, nullptr, nullptr, 0, 0, 0};

    FsFile file = SdMan.open(path, O_RDONLY);
    if (!file) {
      LOG_ERR(TAG, "Cannot open file: %s (attempt %d)", path, attempt + 1);
      continue;
    }

    FileHeader header;
    if (!readExact(file, &header, sizeof(header))) {
      LOG_ERR(TAG, "Failed to read header");
      file.close();
      continue;
    }

    if (header.magic != MAGIC) {
      LOG_ERR(TAG, "Invalid magic: 0x%08X (expected 0x%08X)", header.magic, MAGIC);
      file.close();
      return result;
    }

    if (!isSupportedVersion(header.version)) {
      LOG_ERR(TAG, "Unsupported version: %d", header.version);
      file.close();
      return result;
    }

    const bool is2Bit = (header.flags & 0x01) != 0;

    FileMetrics metrics;
    if (!readExact(file, &metrics, sizeof(metrics))) {
      LOG_ERR(TAG, "Failed to read metrics");
      file.close();
      continue;
    }

    FileTablesV2 tableCounts = {};
    size_t extraTableBytes = 0;
    if (header.version >= VERSION) {
      if (!readExact(file, &tableCounts, sizeof(tableCounts))) {
        LOG_ERR(TAG, "Failed to read table counts");
        file.close();
        continue;
      }
      extraTableBytes =
          calculateTableBytes(tableCounts.kernLeftEntryCount, tableCounts.kernRightEntryCount, tableCounts.kernLeftClassCount,
                              tableCounts.kernRightClassCount, tableCounts.ligaturePairCount);
    }

    LOG_INF(TAG, "Font: advanceY=%d, ascender=%d, descender=%d, intervals=%u, glyphs=%u, bitmap=%u", metrics.advanceY,
            metrics.ascender, metrics.descender, metrics.intervalCount, metrics.glyphCount, metrics.bitmapSize);

    if (!validateMetricsAndMemory(metrics, extraTableBytes)) {
      file.close();
      return result;
    }

    result.intervals = new (std::nothrow) EpdUnicodeInterval[metrics.intervalCount];
    result.glyphs = new (std::nothrow) EpdGlyph[metrics.glyphCount];
    result.bitmap = new (std::nothrow) uint8_t[metrics.bitmapSize];
    result.fontData = new (std::nothrow) EpdFontData{};

    if (!result.intervals || !result.glyphs || !result.bitmap || !result.fontData) {
      LOG_ERR(TAG, "Memory allocation failed");
      freeLoadResult(result);
      file.close();
      return result;
    }

    const size_t intervalsSize = metrics.intervalCount * sizeof(EpdUnicodeInterval);
    if (!readExact(file, result.intervals, intervalsSize)) {
      LOG_ERR(TAG, "Failed to read intervals");
      freeLoadResult(result);
      file.close();
      continue;
    }

    if (!readGlyphs(file, result.glyphs, metrics.glyphCount)) {
      LOG_ERR(TAG, "Failed to read glyphs");
      freeLoadResult(result);
      file.close();
      continue;
    }

    if (header.version >= VERSION &&
        !readExtendedTables(file, *result.fontData, tableCounts.kernLeftEntryCount, tableCounts.kernRightEntryCount,
                            tableCounts.kernLeftClassCount, tableCounts.kernRightClassCount,
                            tableCounts.ligaturePairCount)) {
      LOG_ERR(TAG, "Failed to read kerning / ligature tables");
      freeLoadResult(result);
      file.close();
      continue;
    }

    if (!readExact(file, result.bitmap, metrics.bitmapSize)) {
      LOG_ERR(TAG, "Failed to read bitmap");
      freeLoadResult(result);
      file.close();
      continue;
    }

    result.fontData->bitmap = result.bitmap;
    result.fontData->glyph = result.glyphs;
    result.fontData->intervals = result.intervals;
    result.fontData->intervalCount = metrics.intervalCount;
    result.fontData->advanceY = metrics.advanceY;
    result.fontData->ascender = metrics.ascender;
    result.fontData->descender = metrics.descender;
    result.fontData->is2Bit = is2Bit;

    result.bitmapSize = metrics.bitmapSize;
    result.glyphsSize = metrics.glyphCount * sizeof(EpdGlyph);
    result.intervalsSize = intervalsSize;
    result.success = true;

    file.close();
    LOG_INF(TAG, "Loaded %s: %zu bytes (bitmap=%u, glyphs=%zu, intervals=%zu)", path, result.totalSize(),
            metrics.bitmapSize, result.glyphsSize, result.intervalsSize);
    return result;
  }

  return {false, nullptr, nullptr, nullptr, nullptr, 0, 0, 0};
}

void EpdFontLoader::freeLoadResult(LoadResult& result) {
  if (result.fontData) {
    freeFontTables(*result.fontData);
  }
  delete result.fontData;
  delete[] result.bitmap;
  delete[] result.glyphs;
  delete[] result.intervals;
  result = {false, nullptr, nullptr, nullptr, nullptr, 0, 0, 0};
}

EpdFontLoader::StreamingLoadResult EpdFontLoader::loadForStreaming(const char* path) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(50);

    StreamingLoadResult result = {false, {}, nullptr, nullptr, 0, 0, 0, 0};

    FsFile file = SdMan.open(path, O_RDONLY);
    if (!file) {
      continue;
    }

    FileHeader header;
    if (!readExact(file, &header, sizeof(header))) {
      file.close();
      continue;
    }

    if (header.magic != MAGIC || !isSupportedVersion(header.version)) {
      file.close();
      return result;
    }

    const bool is2Bit = (header.flags & 0x01) != 0;

    FileMetrics metrics;
    if (!readExact(file, &metrics, sizeof(metrics))) {
      file.close();
      continue;
    }

    FileTablesV2 tableCounts = {};
    size_t extraTableBytes = 0;
    if (header.version >= VERSION) {
      if (!readExact(file, &tableCounts, sizeof(tableCounts))) {
        file.close();
        continue;
      }
      extraTableBytes =
          calculateTableBytes(tableCounts.kernLeftEntryCount, tableCounts.kernRightEntryCount, tableCounts.kernLeftClassCount,
                              tableCounts.kernRightClassCount, tableCounts.ligaturePairCount);
    }

    if (metrics.intervalCount > 10000 || metrics.glyphCount > 100000) {
      file.close();
      return result;
    }

    const size_t requiredMemory =
        metrics.intervalCount * sizeof(EpdUnicodeInterval) + metrics.glyphCount * sizeof(EpdGlyph) +
        sizeof(EpdFontData) + extraTableBytes;
    const size_t availableHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (requiredMemory > availableHeap * 0.8) {
      file.close();
      return result;
    }

    result.intervals = new (std::nothrow) EpdUnicodeInterval[metrics.intervalCount];
    result.glyphs = new (std::nothrow) EpdGlyph[metrics.glyphCount];
    if (!result.intervals || !result.glyphs) {
      freeStreamingResult(result);
      file.close();
      return result;
    }

    const size_t intervalsSize = metrics.intervalCount * sizeof(EpdUnicodeInterval);
    if (!readExact(file, result.intervals, intervalsSize)) {
      freeStreamingResult(result);
      file.close();
      continue;
    }

    if (!readGlyphs(file, result.glyphs, metrics.glyphCount)) {
      freeStreamingResult(result);
      file.close();
      continue;
    }

    if (header.version >= VERSION &&
        !readExtendedTables(file, result.fontData, tableCounts.kernLeftEntryCount, tableCounts.kernRightEntryCount,
                            tableCounts.kernLeftClassCount, tableCounts.kernRightClassCount,
                            tableCounts.ligaturePairCount)) {
      freeStreamingResult(result);
      file.close();
      continue;
    }

    result.bitmapOffset = file.position();
    result.fontData.bitmap = nullptr;
    result.fontData.glyph = result.glyphs;
    result.fontData.intervals = result.intervals;
    result.fontData.intervalCount = metrics.intervalCount;
    result.fontData.advanceY = metrics.advanceY;
    result.fontData.ascender = metrics.ascender;
    result.fontData.descender = metrics.descender;
    result.fontData.is2Bit = is2Bit;

    result.glyphCount = metrics.glyphCount;
    result.glyphsSize = metrics.glyphCount * sizeof(EpdGlyph);
    result.intervalsSize = intervalsSize;
    result.success = true;

    file.close();
    return result;
  }

  return {false, {}, nullptr, nullptr, 0, 0, 0, 0};
}

void EpdFontLoader::freeStreamingResult(StreamingLoadResult& result) {
  freeFontTables(result.fontData);
  delete[] result.glyphs;
  delete[] result.intervals;
  result = {false, {}, nullptr, nullptr, 0, 0, 0, 0};
}

EpdFontLoader::LoadResult EpdFontLoader::loadFromLittleFS(const char* path) {
  LoadResult result = {false, nullptr, nullptr, nullptr, nullptr, 0, 0, 0};

  File file = LittleFS.open(path, "r");
  if (!file) {
    LOG_ERR(TAG, "Cannot open LittleFS file: %s", path);
    return result;
  }

  FileHeader header;
  if (!readExact(file, &header, sizeof(header))) {
    LOG_ERR(TAG, "Failed to read header from LittleFS");
    file.close();
    return result;
  }

  if (header.magic != MAGIC) {
    LOG_ERR(TAG, "Invalid magic: 0x%08X (expected 0x%08X)", header.magic, MAGIC);
    file.close();
    return result;
  }

  if (!isSupportedVersion(header.version)) {
    LOG_ERR(TAG, "Unsupported version: %d", header.version);
    file.close();
    return result;
  }

  const bool is2Bit = (header.flags & 0x01) != 0;

  FileMetrics metrics;
  if (!readExact(file, &metrics, sizeof(metrics))) {
    LOG_ERR(TAG, "Failed to read metrics from LittleFS");
    file.close();
    return result;
  }

  FileTablesV2 tableCounts = {};
  size_t extraTableBytes = 0;
  if (header.version >= VERSION) {
    if (!readExact(file, &tableCounts, sizeof(tableCounts))) {
      LOG_ERR(TAG, "Failed to read table counts from LittleFS");
      file.close();
      return result;
    }
    extraTableBytes =
        calculateTableBytes(tableCounts.kernLeftEntryCount, tableCounts.kernRightEntryCount, tableCounts.kernLeftClassCount,
                            tableCounts.kernRightClassCount, tableCounts.ligaturePairCount);
  }

  LOG_INF(TAG, "Font: advanceY=%d, ascender=%d, descender=%d, intervals=%u, glyphs=%u, bitmap=%u", metrics.advanceY,
          metrics.ascender, metrics.descender, metrics.intervalCount, metrics.glyphCount, metrics.bitmapSize);

  if (!validateMetricsAndMemory(metrics, extraTableBytes)) {
    file.close();
    return result;
  }

  result.intervals = new (std::nothrow) EpdUnicodeInterval[metrics.intervalCount];
  result.glyphs = new (std::nothrow) EpdGlyph[metrics.glyphCount];
  result.bitmap = new (std::nothrow) uint8_t[metrics.bitmapSize];
  result.fontData = new (std::nothrow) EpdFontData{};
  if (!result.intervals || !result.glyphs || !result.bitmap || !result.fontData) {
    LOG_ERR(TAG, "Memory allocation failed");
    freeLoadResult(result);
    file.close();
    return result;
  }

  const size_t intervalsSize = metrics.intervalCount * sizeof(EpdUnicodeInterval);
  if (!readExact(file, result.intervals, intervalsSize)) {
    LOG_ERR(TAG, "Failed to read intervals from LittleFS");
    freeLoadResult(result);
    file.close();
    return result;
  }

  if (!readGlyphs(file, result.glyphs, metrics.glyphCount)) {
    LOG_ERR(TAG, "Failed to read glyphs from LittleFS");
    freeLoadResult(result);
    file.close();
    return result;
  }

  if (header.version >= VERSION &&
      !readExtendedTables(file, *result.fontData, tableCounts.kernLeftEntryCount, tableCounts.kernRightEntryCount,
                          tableCounts.kernLeftClassCount, tableCounts.kernRightClassCount,
                          tableCounts.ligaturePairCount)) {
    LOG_ERR(TAG, "Failed to read kerning / ligature tables from LittleFS");
    freeLoadResult(result);
    file.close();
    return result;
  }

  if (!readExact(file, result.bitmap, metrics.bitmapSize)) {
    LOG_ERR(TAG, "Failed to read bitmap from LittleFS");
    freeLoadResult(result);
    file.close();
    return result;
  }

  result.fontData->bitmap = result.bitmap;
  result.fontData->glyph = result.glyphs;
  result.fontData->intervals = result.intervals;
  result.fontData->intervalCount = metrics.intervalCount;
  result.fontData->advanceY = metrics.advanceY;
  result.fontData->ascender = metrics.ascender;
  result.fontData->descender = metrics.descender;
  result.fontData->is2Bit = is2Bit;

  result.bitmapSize = metrics.bitmapSize;
  result.glyphsSize = metrics.glyphCount * sizeof(EpdGlyph);
  result.intervalsSize = intervalsSize;
  result.success = true;

  file.close();
  LOG_INF(TAG, "Loaded %s: %zu bytes (bitmap=%u, glyphs=%zu, intervals=%zu)", path, result.totalSize(),
          metrics.bitmapSize, result.glyphsSize, result.intervalsSize);
  return result;
}
