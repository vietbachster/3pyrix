#include "test_utils.h"

#include <climits>
#include <cstdint>

#include <PageCache.h>

#include "ReaderNavigation.h"

namespace {

bool expectWasAdvanced(TestUtils::TestRunner& runner, const papyrix::ReaderNavigation::NavResult& result,
                       int expectedSpine, int expectedSectionPage, bool expectedCacheReset,
                       const std::string& prefix) {
  bool ok = true;
  ok &= runner.expectEq(expectedSpine, result.position.spineIndex, prefix + " spine");
  ok &= runner.expectEq(expectedSectionPage, result.position.sectionPage, prefix + " section");
  ok &= runner.expectEq(expectedCacheReset, result.needsCacheReset, prefix + " cache-reset");
  ok &= runner.expectTrue(result.needsRender, prefix + " needs-render");
  return ok;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("ReaderNavigation");

  {
    PageCache cache(6, false);
    papyrix::ReaderNavigation::Position pos{2, 3, 0};
    const auto result = papyrix::ReaderNavigation::next(papyrix::ContentType::Epub, pos, &cache, 0);
    expectWasAdvanced(runner, result, 2, 4, false, "epub_next_within_section");
  }

  {
    PageCache cache(6, false);
    papyrix::ReaderNavigation::Position pos{2, 5, 0};
    const auto result = papyrix::ReaderNavigation::next(papyrix::ContentType::Epub, pos, &cache, 0);
    expectWasAdvanced(runner, result, 3, 0, true, "epub_next_across_spine");
  }

  {
    PageCache cache(5, true);
    papyrix::ReaderNavigation::Position pos{1, 4, 0};
    const auto result = papyrix::ReaderNavigation::next(papyrix::ContentType::Epub, pos, &cache, 0);
    expectWasAdvanced(runner, result, 1, 5, false, "epub_next_partial_extends");
  }

  {
    PageCache cache(6, false);
    papyrix::ReaderNavigation::Position pos{4, 0, 0};
    const auto result = papyrix::ReaderNavigation::prev(papyrix::ContentType::Epub, pos, &cache);
    expectWasAdvanced(runner, result, 3, INT16_MAX, true, "epub_prev_across_spine");
  }

  {
    papyrix::ReaderNavigation::Position pos{0, 0, 7};
    const auto result = papyrix::ReaderNavigation::next(papyrix::ContentType::Xtc, pos, nullptr, 10);
    runner.expectEq(uint32_t(8), result.position.flatPage, "xtc_next_increments_flat_page");
    runner.expectTrue(result.needsRender, "xtc_next_needs_render");
    runner.expectFalse(result.needsCacheReset, "xtc_next_no_cache_reset");
  }

  {
    papyrix::ReaderNavigation::Position pos{0, 0, 0};
    const auto result = papyrix::ReaderNavigation::prev(papyrix::ContentType::Xtc, pos, nullptr);
    runner.expectEq(uint32_t(0), result.position.flatPage, "xtc_prev_stops_at_zero");
    runner.expectFalse(result.needsRender, "xtc_prev_boundary_no_render");
  }

  {
    PageCache cache(4, true, true);
    runner.expectTrue(papyrix::ReaderNavigation::needsCacheExtension(&cache, 3),
                      "needs_cache_extension_returns_cache_answer");
    runner.expectEq(3, cache.lastNeedsExtensionPage(), "needs_cache_extension_passes_current_page");
  }

  return runner.allPassed() ? 0 : 1;
}
