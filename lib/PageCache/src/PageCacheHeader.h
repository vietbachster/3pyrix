#pragma once

#include <RenderConfig.h>
#include <SdFat.h>

#include <cstddef>
#include <cstdint>

namespace pagecache {

enum class HeaderReadStatus : uint8_t {
  Ok = 0,
  VersionMismatch,
  Truncated,
};

struct HeaderInfo {
  RenderConfig config{};
  uint16_t pageCount = 0;
  bool isPartial = false;
  uint32_t lutOffset = 0;
};

inline constexpr uint8_t kFileVersion = 20;
inline constexpr uint16_t kMaxReasonablePageCount = 8192;
inline constexpr uint32_t kHeaderSize = 1 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 1 + 4;

HeaderReadStatus readHeader(FsFile& file, HeaderInfo& info, bool readConfig);
bool validateIndexBounds(const char* cachePath, size_t fileSize, uint16_t pageCount, uint32_t lutOffset);

}  // namespace pagecache
