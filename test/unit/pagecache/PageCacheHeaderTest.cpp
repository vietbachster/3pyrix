#include "test_utils.h"

#include <cstdint>
#include <string>

#include "PageCacheHeader.h"
#include "Serialization.h"

namespace {

RenderConfig makeConfig() {
  return RenderConfig{
      42,
      1.15f,
      3,
      2,
      1,
      3,
      true,
      false,
      528,
      792,
  };
}

std::string makeCacheBuffer(const RenderConfig& config, uint16_t pageCount, bool isPartial, int lutEntries = -1,
                            uint8_t version = pagecache::kFileVersion, uint32_t lutOffset = pagecache::kHeaderSize) {
  FsFile file;
  file.setBuffer("");

  serialization::writePod(file, version);
  serialization::writePod(file, config.fontId);
  serialization::writePod(file, config.lineCompression);
  serialization::writePod(file, config.readerFontSize);
  serialization::writePod(file, config.indentLevel);
  serialization::writePod(file, config.spacingLevel);
  serialization::writePod(file, config.paragraphAlignment);
  serialization::writePod(file, config.hyphenation);
  serialization::writePod(file, config.showImages);
  serialization::writePod(file, config.viewportWidth);
  serialization::writePod(file, config.viewportHeight);
  serialization::writePod(file, pageCount);
  serialization::writePod(file, static_cast<uint8_t>(isPartial ? 1 : 0));
  serialization::writePod(file, lutOffset);

  const int entryCount = lutEntries >= 0 ? lutEntries : pageCount;
  for (int i = 0; i < entryCount; ++i) {
    const uint32_t pagePos = pagecache::kHeaderSize + static_cast<uint32_t>(pageCount) * sizeof(uint32_t) + i;
    serialization::writePod(file, pagePos);
  }

  return file.getBuffer();
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("PageCacheHeader");

  runner.expectEq(uint32_t(26), pagecache::kHeaderSize, "header_size_matches_current_layout");

  {
    const auto config = makeConfig();
    FsFile file;
    file.setBuffer(makeCacheBuffer(config, 7, true));

    pagecache::HeaderInfo header;
    const auto status = pagecache::readHeader(file, header, false);

    runner.expectEq(static_cast<int>(pagecache::HeaderReadStatus::Ok), static_cast<int>(status),
                    "load_raw_header_reads_without_config");
    runner.expectEq(uint16_t(7), header.pageCount, "load_raw_header_reads_page_count");
    runner.expectTrue(header.isPartial, "load_raw_header_reads_partial_flag");
    runner.expectEq(pagecache::kHeaderSize, header.lutOffset, "load_raw_header_reads_lut_offset");
  }

  {
    const auto config = makeConfig();
    FsFile file;
    file.setBuffer(makeCacheBuffer(config, 5, false));

    pagecache::HeaderInfo header;
    const auto status = pagecache::readHeader(file, header, true);

    runner.expectEq(static_cast<int>(pagecache::HeaderReadStatus::Ok), static_cast<int>(status),
                    "load_full_header_reads_config");
    runner.expectEq(config.fontId, header.config.fontId, "load_full_header_font_id");
    runner.expectFloatEq(config.lineCompression, header.config.lineCompression, "load_full_header_line_compression");
    runner.expectEq(config.readerFontSize, header.config.readerFontSize, "load_full_header_reader_font_size");
    runner.expectEq(config.viewportWidth, header.config.viewportWidth, "load_full_header_viewport_width");
    runner.expectEq(config.viewportHeight, header.config.viewportHeight, "load_full_header_viewport_height");
  }

  {
    FsFile file;
    file.setBuffer(makeCacheBuffer(makeConfig(), 5, false, -1, 99));

    pagecache::HeaderInfo header;
    const auto status = pagecache::readHeader(file, header, false);
    runner.expectEq(static_cast<int>(pagecache::HeaderReadStatus::VersionMismatch), static_cast<int>(status),
                    "version_mismatch_is_reported");
  }

  {
    FsFile file;
    const std::string truncated = makeCacheBuffer(makeConfig(), 5, false).substr(0, 10);
    file.setBuffer(truncated);

    pagecache::HeaderInfo header;
    const auto status = pagecache::readHeader(file, header, true);
    runner.expectEq(static_cast<int>(pagecache::HeaderReadStatus::Truncated), static_cast<int>(status),
                    "truncated_header_is_reported");
  }

  {
    const std::string buffer = makeCacheBuffer(makeConfig(), 9, false);
    runner.expectTrue(pagecache::validateIndexBounds("/cache/good.bin", buffer.size(), 9, pagecache::kHeaderSize),
                      "valid_bounds_are_accepted");
  }

  {
    const std::string buffer = makeCacheBuffer(makeConfig(), 0, false);
    runner.expectFalse(pagecache::validateIndexBounds("/cache/zero.bin", buffer.size(), 0, pagecache::kHeaderSize),
                       "zero_page_count_is_rejected");
  }

  {
    const std::string buffer = makeCacheBuffer(makeConfig(), 4, false, 1);
    runner.expectFalse(pagecache::validateIndexBounds("/cache/truncated.bin", buffer.size(), 4, pagecache::kHeaderSize),
                       "truncated_lut_is_rejected");
  }

  {
    const std::string buffer = makeCacheBuffer(makeConfig(), pagecache::kMaxReasonablePageCount + 1, false,
                                               pagecache::kMaxReasonablePageCount + 1);
    runner.expectFalse(pagecache::validateIndexBounds("/cache/huge.bin", buffer.size(),
                                                      pagecache::kMaxReasonablePageCount + 1, pagecache::kHeaderSize),
                       "oversized_page_count_is_rejected");
  }

  return runner.allPassed() ? 0 : 1;
}
