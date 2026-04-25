#include "test_utils.h"

#include <cstdint>
#include <vector>

#include "reader/ReaderTocNavigation.h"

int main() {
  TestUtils::TestRunner runner("ReaderTocNavigation");

  {
    const std::vector<papyrix::reader::FlatTocEntry> tocEntries = {
        {0, 4},
        {3, 12},
        {7, 20},
    };

    runner.expectEq(3, papyrix::reader::findFlatTocEntryForPage(tocEntries, 18),
                    "flat_toc_returns_last_entry_before_current_page");
    runner.expectEq(7, papyrix::reader::findFlatTocEntryForPage(tocEntries, 20),
                    "flat_toc_matches_exact_page");
    runner.expectEq(-1, papyrix::reader::findFlatTocEntryForPage(tocEntries, 1),
                    "flat_toc_before_first_page_has_no_match");
  }

  {
    const auto plan = papyrix::reader::planEpubTocJump(4, 4);
    runner.expectEq(4, plan.spineIndex, "same_spine_jump_keeps_spine");
    runner.expectEq(0, plan.sectionPage, "same_spine_jump_defaults_to_page_zero");
    runner.expectFalse(plan.needsResourceReset, "same_spine_jump_does_not_reset_resources");
  }

  {
    const auto plan = papyrix::reader::planEpubTocJump(4, 4, 9);
    runner.expectEq(9, plan.sectionPage, "same_spine_anchor_jump_uses_anchor_page");
    runner.expectFalse(plan.needsResourceReset, "same_spine_anchor_jump_does_not_reset_resources");
  }

  {
    const auto plan = papyrix::reader::planEpubTocJump(2, 5);
    runner.expectEq(5, plan.spineIndex, "cross_spine_jump_moves_to_target_spine");
    runner.expectEq(0, plan.sectionPage, "cross_spine_jump_defaults_to_page_zero");
    runner.expectTrue(plan.needsResourceReset, "cross_spine_jump_requires_resource_reset");
  }

  {
    const auto plan = papyrix::reader::planEpubTocJump(2, 5, 13);
    runner.expectEq(13, plan.sectionPage, "cross_spine_anchor_jump_uses_precise_page");
    runner.expectTrue(plan.needsResourceReset, "cross_spine_anchor_jump_requires_resource_reset");
  }

  return runner.allPassed() ? 0 : 1;
}
