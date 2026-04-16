#pragma once

#include <BackgroundTask.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../content/ProgressManager.h"
#include "../content/ReaderNavigation.h"
#include "../core/Types.h"
#include "../rendering/XtcPageRenderer.h"
#include "../ui/views/HomeView.h"
#include "../ui/views/ReaderViews.h"
#include "State.h"

class ContentParser;
class GfxRenderer;
class PageCache;
class Page;
struct RenderConfig;

namespace papyrix {

// Forward declarations
class Core;
struct Event;

// ReaderState - unified reader for all content types
// Uses ContentHandle to abstract Epub/Xtc/Txt/Markdown differences
// Uses PageCache for all formats with partial caching support
// Delegates to: XtcPageRenderer (binary rendering), ProgressManager (persistence),
//               ReaderNavigation (page traversal)
class ReaderState : public State {
 public:
  explicit ReaderState(GfxRenderer& renderer);
  ~ReaderState() override;

  void enter(Core& core) override;
  void exit(Core& core) override;
  StateTransition update(Core& core) override;
  void render(Core& core) override;
  StateId id() const override { return StateId::Reader; }

  // Set content path before entering state
  void setContentPath(const char* path);

  // Reading position
  uint32_t currentPage() const { return currentPage_; }
  void setCurrentPage(uint32_t page) { currentPage_ = page; }

 private:
  GfxRenderer& renderer_;
  XtcPageRenderer xtcRenderer_;
  char contentPath_[256];
  uint32_t currentPage_;
  bool needsRender_;
  bool contentLoaded_;
  bool loadFailed_ = false;  // Track if content loading failed (for error state transition)
  bool endOfBookShown_ = false;
  bool framePreservedOnWake_ = false;

  // Reading position (maps to ReaderNavigation::Position)
  int currentSpineIndex_;
  int currentSectionPage_;

  // Last successfully rendered position (for accurate progress saving)
  int lastRenderedSpineIndex_ = 0;
  int lastRenderedSectionPage_ = 0;
  ProgressManager::Progress pendingProgress_;
  bool pendingProgressRestore_ = false;

  // Whether book has a valid cover image
  bool hasCover_ = false;

  // First text content spine index (from EPUB guide, 0 if not specified)
  int textStartIndex_ = 0;

  // Unified page cache for all content types
  // Ownership model: main thread owns pageCache_/parser_ when !cacheTask_.isRunning()
  //                  background task owns pageCache_/parser_ when cacheTask_.isRunning()
  // Navigation ALWAYS stops task first, then accesses cache/parser
  std::unique_ptr<PageCache> pageCache_;

  // Persistent parser for incremental (hot) extends — kept alive between extend calls
  // so the parser can resume from where it left off instead of re-parsing from byte 0
  std::unique_ptr<ContentParser> parser_;
  int parserSpineIndex_ = -1;
  uint8_t pagesUntilFullRefresh_;

  // Background caching (uses BackgroundTask for proper lifecycle management)
  BackgroundTask cacheTask_;
  Core* coreForCacheTask_ = nullptr;
  bool thumbnailDone_ = false;
  void startBackgroundCaching(Core& core);
  void stopBackgroundCaching();

  // Navigation helpers (delegates to ReaderNavigation)
  void navigateNext(Core& core);
  void navigatePrev(Core& core);
  void navigateNextChapter(Core& core);
  void navigatePrevChapter(Core& core);
  void applyNavResult(const ReaderNavigation::NavResult& result, Core& core);

  // Track whether a chapter jump already fired during a button hold
  bool holdNavigated_ = false;

  // Confirm single/double click handling in reading mode.
  bool confirmClickPending_ = false;
  uint32_t lastConfirmReleaseMs_ = 0;
  bool suppressNextCenterRelease_ = false;

  // Track power press start when short power action is mapped to page turn.
  // This lets us execute page turn only on short release and avoid accidental
  // turns when the same press is held to enter sleep.
  uint32_t powerPressStartedMs_ = 0;

  // Auto page turn: absolute millis() timestamp when next page turn fires.
  // 0 = inactive. Set after each page render, cleared on manual navigation.
  uint32_t autoPageTurnTargetMs_ = 0;

  // Word count of the last successfully rendered page (used for immediate scheduling
  // after the user enables auto page turn via long-press, without re-rendering).
  int lastPageWordCount_ = 0;


  // Rendering
  void renderCurrentPage(Core& core);
  void renderCachedPage(Core& core);
  void renderXtcPage(Core& core);
  bool renderCoverPage(Core& core);

  // Helpers
  void renderPageContents(Core& core, Page& page, int marginTop, int marginRight, int marginBottom, int marginLeft);

  // Cache management
  bool ensurePageCached(Core& core, uint16_t pageNum);
  void loadCacheFromDisk(Core& core);
  void createOrExtendCache(Core& core);

  void createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath, const RenderConfig& config);
  void backgroundCacheImpl(ContentParser& parser, const std::string& cachePath, const RenderConfig& config);

  // Display helpers
  void displayWithRefresh(Core& core);
  bool contentSupportsAutoPageTurn(const Core& core) const;
  bool isAutoPageTurnEnabled(const Core& core) const;
  bool isTiltPageTurnEnabled(const Core& core) const;
  bool setAutoPageTurnMode(Core& core, uint8_t mode);
  void scheduleAutoPageTurn(Core& core, int wordCount);
  void commitPendingConfirmClick(Core& core);
  void toggleReadingOrientation(Core& core);
  ProgressManager::Progress buildProgressSnapshot(Core& core) const;
  void restoreProgressFromTextAnchor(Core& core);
  std::string extractTextAnchor(const Page& page) const;
  std::string cachePathForPosition(Core& core, ContentType type, int spineIndex, const RenderConfig& config) const;
  int findPageForTextAnchor(PageCache& cache, const char* textAnchor, int pageHint) const;

  // Viewport calculation
  struct Viewport {
    int marginTop;
    int marginRight;
    int marginBottom;
    int marginLeft;
    int width;
    int height;
  };
  Viewport getReaderViewport() const;

  // Get first content spine index (skips cover document when appropriate)
  static int calcFirstContentSpine(bool hasCover, int textStartIndex, size_t spineCount);

  // Anchor-to-page persistence for intra-spine TOC navigation
  static void saveAnchorMap(const ContentParser& parser, const std::string& cachePath);
  static int loadAnchorPage(const std::string& cachePath, const std::string& anchor);
  static std::vector<std::pair<std::string, uint16_t>> loadAnchorMap(const std::string& cachePath);

  // Source state (where reader was opened from)
  StateId sourceState_ = StateId::Home;

  // TOC overlay mode
  bool tocMode_ = false;
  ui::ChapterListView tocView_;

  void enterTocMode(Core& core);
  void exitTocMode();
  void handleTocInput(Core& core, const Event& e);
  void renderTocOverlay(Core& core);
  int tocVisibleCount() const;
  void populateTocView(Core& core);
  int findCurrentTocEntry(Core& core);
  void jumpToTocEntry(Core& core, int tocIndex);

  // Boot mode transition - exit to UI via restart
  void exitToUI(Core& core);
  void exitToFileList(Core& core);
};

}  // namespace papyrix
