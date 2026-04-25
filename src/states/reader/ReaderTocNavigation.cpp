#include "ReaderTocNavigation.h"

namespace papyrix::reader {

int findFlatTocEntryForPage(const std::vector<FlatTocEntry>& tocEntries, const uint32_t currentPage) {
  int lastMatch = -1;

  for (const auto& entry : tocEntries) {
    if (entry.pageIndex <= currentPage) {
      lastMatch = entry.tocIndex;
    }
  }

  return lastMatch;
}

EpubTocJumpPlan planEpubTocJump(const int currentSpineIndex, const uint32_t targetSpineIndex, const int anchorPage) {
  EpubTocJumpPlan plan;
  plan.spineIndex = static_cast<int>(targetSpineIndex);
  plan.sectionPage = anchorPage >= 0 ? anchorPage : 0;
  plan.needsResourceReset = plan.spineIndex != currentSpineIndex;
  return plan;
}

}  // namespace papyrix::reader
