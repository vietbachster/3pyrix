#pragma once

#include <cstdint>
#include <vector>

namespace papyrix::reader {

struct FlatTocEntry {
  int tocIndex = -1;
  uint32_t pageIndex = 0;
};

struct EpubTocJumpPlan {
  int spineIndex = 0;
  int sectionPage = 0;
  bool needsResourceReset = false;
};

int findFlatTocEntryForPage(const std::vector<FlatTocEntry>& tocEntries, uint32_t currentPage);
EpubTocJumpPlan planEpubTocJump(int currentSpineIndex, uint32_t targetSpineIndex, int anchorPage = -1);

}  // namespace papyrix::reader
