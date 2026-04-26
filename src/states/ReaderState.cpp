#include "ReaderState.h"

#include <Arduino.h>
#include <ContentParser.h>
#include <CoverHelpers.h>
#include <EpubChapterParser.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Page.h>
#include <PageCache.h>
#include <PlainTextParser.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <esp_system.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>

#include "../Battery.h"
#include "../FontManager.h"
#include "../config.h"
#include "../content/ProgressManager.h"
#include "../content/ReaderNavigation.h"
#include "../core/BootMode.h"
#include "../core/Core.h"
#include "../ui/Elements.h"
#include "../ui/views/ReaderViews.h"
#include "reader/ReaderTocNavigation.h"
#include "ThemeManager.h"

#define TAG "READER"

namespace papyrix {

static constexpr int kCacheTaskStackSize = 12288;
static constexpr int kCacheTaskStopTimeoutMs = 10000;  // 10s - generous for slow SD operations
static constexpr int kReaderMarginTop = 12;
static constexpr int kReaderMarginRight = 12;
static constexpr int kReaderMarginBottom = 6;
static constexpr int kReaderMarginLeft = 12;
static constexpr uint32_t kConfirmDoubleClickMs = 350;

namespace {

// Cache path helpers
inline std::string epubSectionCachePath(const std::string& epubCachePath, int spineIndex) {
  return epubCachePath + "/sections/" + std::to_string(spineIndex) + ".bin";
}

inline std::string contentCachePath(const char* cacheDir, int fontId) {
  return std::string(cacheDir) + "/pages_" + std::to_string(fontId) + ".bin";
}

std::string normalizeAnchorText(const std::string& text) {
  std::string normalized;
  normalized.reserve(text.size());

  bool lastWasSpace = true;
  for (unsigned char ch : text) {
    if (std::isspace(ch)) {
      if (!lastWasSpace && !normalized.empty()) {
        normalized.push_back(' ');
      }
      lastWasSpace = true;
      continue;
    }

    normalized.push_back(static_cast<char>(ch));
    lastWasSpace = false;
  }

  if (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }
  return normalized;
}

int countPageWords(const Page& page) {
  int count = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    count += static_cast<int>(line.getTextBlock().getWords().size());
  }
  return count;
}

std::string pageTextForAnchorMatch(const Page& page) {
  std::string text;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) {
      continue;
    }

    const auto& line = static_cast<const PageLine&>(*element);
    for (const auto& word : line.getTextBlock().getWords()) {
      if (!text.empty()) {
        text.push_back(' ');
      }
      text += word.word;
    }
  }

  return normalizeAnchorText(text);
}
}  // namespace

ReaderState::ReaderState(GfxRenderer& renderer)
    : renderer_(renderer),
      xtcRenderer_(renderer),
      currentPage_(0),
      needsRender_(true),
      contentLoaded_(false),
      framePreservedOnWake_(false),
      currentSpineIndex_(0),
      currentSectionPage_(0),
      cacheController_(renderer),
      pageCache_(cacheController_.pageCacheRef()),
      parser_(cacheController_.parserRef()),
      parserSpineIndex_(cacheController_.parserSpineIndexRef()),
      pagesUntilFullRefresh_(1),
      thumbnailDone_(cacheController_.thumbnailDoneRef()),
      tocView_{} {
  contentPath_[0] = '\0';
}

ReaderState::~ReaderState() { stopBackgroundCaching(); }

ReaderState::ResourceSession ReaderState::acquireForegroundResources(const char* reason) {
  auto session = cacheController_.acquireForeground(reason);
  if (!session) {
    LOG_ERR(TAG, "Failed to acquire foreground reader resources (%s)", reason ? reason : "-");
  }
  return session;
}

ReaderState::ResourceSession ReaderState::acquireWorkerResources(const char* reason) {
  auto session = cacheController_.acquireWorker(reason);
  if (!session) {
    LOG_ERR(TAG, "Failed to acquire worker reader resources (%s)", reason ? reason : "-");
  }
  return session;
}

void ReaderState::setContentPath(const char* path) {
  if (path) {
    strncpy(contentPath_, path, sizeof(contentPath_) - 1);
    contentPath_[sizeof(contentPath_) - 1] = '\0';
  } else {
    contentPath_[0] = '\0';
  }
  cacheController_.setContentPath(contentPath_);
}

bool ReaderState::contentSupportsAutoPageTurn(const Core& core) const {
  return core.content.metadata().type != ContentType::Xtc;
}

bool ReaderState::isAutoPageTurnEnabled(const Core& core) const {
  return contentSupportsAutoPageTurn(core) && core.settings.autoPageTurnMode == Settings::AutoPageOn;
}

bool ReaderState::isTiltPageTurnEnabled(const Core& core) const { return core.settings.tiltPageTurn != 0; }

bool ReaderState::setAutoPageTurnMode(Core& core, uint8_t mode) {
  if (core.settings.autoPageTurnMode == mode) {
    return false;
  }
  core.settings.autoPageTurnMode = mode;
  core.settings.save(core.storage);
  return true;
}

void ReaderState::scheduleAutoPageTurn(Core& core, int wordCount) {
  autoPageTurnTargetMs_ = 0;
  if (!isAutoPageTurnEnabled(core)) {
    return;
  }
  const uint32_t delayMs = core.settings.getAutoPageTurnDelayMs(wordCount);
  if (delayMs != 0) {
    autoPageTurnTargetMs_ = millis() + delayMs;
  }
}

void ReaderState::enter(Core& core) {
  // Free memory from other states before loading book
  THEME_MANAGER.clearCache();
  renderer_.clearWidthCache();

  contentLoaded_ = false;
  loadFailed_ = false;
  needsRender_ = true;
  endOfBookShown_ = false;
  framePreservedOnWake_ = false;
  holdNavigated_ = false;
  confirmClickPending_ = false;
  lastConfirmReleaseMs_ = 0;
  powerPressStartedMs_ = 0;
  autoPageTurnTargetMs_ = 0;
  lastPageWordCount_ = 0;
  directUiTransition_ = core.pendingDirectReaderTransition;
  core.pendingDirectReaderTransition = false;
  stopBackgroundCaching();  // Ensure any previous task is stopped
  {
    auto resources = acquireForegroundResources("enter-reset-session");
    if (resources) {
      cacheController_.resetSession();
    }
  }
  currentSpineIndex_ = 0;
  currentSectionPage_ = 0;  // Will be set to -1 after progress load if at start

  // Read path from shared buffer if not already set
  if (contentPath_[0] == '\0' && core.buf.path[0] != '\0') {
    strncpy(contentPath_, core.buf.path, sizeof(contentPath_) - 1);
    contentPath_[sizeof(contentPath_) - 1] = '\0';
    core.buf.path[0] = '\0';
  }
  cacheController_.setContentPath(contentPath_);

  // Determine source state from boot transition
  const auto& transition = getTransition();
  if (directUiTransition_) {
    sourceState_ = core.pendingReaderReturnState;
  } else {
    sourceState_ =
        (transition.isValid() && transition.returnTo == ReturnTo::FILE_MANAGER) ? StateId::FileList : StateId::Home;
  }

  LOG_INF(TAG, "Entering with path: %s", contentPath_);

  if (contentPath_[0] == '\0') {
    LOG_ERR(TAG, "No content path set");
    return;
  }

  // Apply orientation setting to renderer
  switch (core.settings.orientation) {
    case Settings::Portrait:
      renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case Settings::LandscapeCW:
      renderer_.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case Settings::Inverted:
      renderer_.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case Settings::LandscapeCCW:
      renderer_.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
  }

  // Open content using ContentHandle
  auto result = core.content.open(contentPath_, PAPYRIX_CACHE_DIR);
  if (!result.ok()) {
    LOG_ERR(TAG, "Failed to open content: %s", errorToString(result.err));
    // Store error message for ErrorState to display
    snprintf(core.buf.text, sizeof(core.buf.text), "Cannot open file:\n%s", errorToString(result.err));
    loadFailed_ = true;  // Mark as failed for update() to transition to error state
    return;
  }

  contentLoaded_ = true;

  // Save last book path to settings
  strncpy(core.settings.lastBookPath, contentPath_, sizeof(core.settings.lastBookPath) - 1);
  core.settings.lastBookPath[sizeof(core.settings.lastBookPath) - 1] = '\0';
  core.settings.save(core.storage);

  // Setup cache directories for all content types
  // Reset state for new book
  textStartIndex_ = 0;
  hasCover_ = false;
  thumbnailDone_.store(false, std::memory_order_release);
  switch (core.content.metadata().type) {
    case ContentType::Epub: {
      auto* provider = core.content.asEpub();
      if (provider && provider->getEpub()) {
        const auto* epub = provider->getEpub();
        epub->setupCacheDir();
        // Get the spine index for the first text content (from <guide> element)
        textStartIndex_ = epub->getSpineIndexForTextReference();
        LOG_DBG(TAG, "Text starts at spine index %d", textStartIndex_);
      }
      break;
    }
    case ContentType::Txt: {
      auto* provider = core.content.asTxt();
      if (provider && provider->getTxt()) {
        provider->getTxt()->setupCacheDir();
      }
      break;
    }
    default:
      break;
  }

  ContentType type = core.content.metadata().type;
  ProgressManager::Progress progress;
  const auto& sleepResume = getSleepResumeTransition();
  const bool canUseSleepResume =
      hasSleepResumeTransition() && strcmp(sleepResume.bookPath, contentPath_) == 0;

  if (canUseSleepResume) {
    progress = sleepResume.progress;
    framePreservedOnWake_ = true;
    LOG_INF(TAG, "Using sleep-resume progress: spine=%d page=%d flat=%u", progress.spineIndex, progress.sectionPage,
            progress.flatPage);
  } else {
    progress = ProgressManager::load(core, core.content.cacheDir(), type);
    progress = ProgressManager::validate(core, type, progress);
  }

  currentSpineIndex_ = progress.spineIndex;
  currentSectionPage_ = progress.sectionPage;
  currentPage_ = progress.flatPage;
  pendingProgress_ = progress;
  pendingProgressRestore_ = progress.hasTextAnchor();

  // If at start of book and showImages enabled, begin at cover
  // Skip for XTC — uses flat page indexing, no cover page concept in reader
  if (type != ContentType::Xtc && currentSpineIndex_ == 0 && currentSectionPage_ == 0 && core.settings.showImages) {
    currentSectionPage_ = -1;  // Cover page
  }

  // Initialize last rendered to loaded position (until first render)
  lastRenderedSpineIndex_ = currentSpineIndex_;
  lastRenderedSectionPage_ = currentSectionPage_;

  if (framePreservedOnWake_) {
    needsRender_ = false;
    pagesUntilFullRefresh_ = core.settings.getPagesPerRefreshValue();
  }

  LOG_INF(TAG, "Loaded: %s", core.content.metadata().title);

  // Start background caching (includes thumbnail generation)
  // This runs once per book open regardless of starting position
  startBackgroundCaching(core);
}

void ReaderState::exit(Core& core) {
  LOG_INF(TAG, "Exiting");

  // Stop background caching task first - BackgroundTask::stop() waits properly
  stopBackgroundCaching();

  if (contentLoaded_) {
    ProgressManager::Progress progress;
    {
      auto resources = acquireForegroundResources("exit-save-progress");
      if (resources) {
        progress = buildProgressSnapshot(core);
        cacheController_.clearDocumentResources();
      }
    }
    ProgressManager::save(core, core.content.cacheDir(), core.content.metadata().type, progress);

    if (core.preserveReaderPageOnSleep) {
      ReturnTo returnTo = ReturnTo::HOME;
      const auto& transition = getTransition();
      if (transition.isValid()) {
        returnTo = transition.returnTo;
      } else if (sourceState_ == StateId::FileList) {
        returnTo = ReturnTo::FILE_MANAGER;
      }
      saveTransition(BootMode::READER, contentPath_, returnTo);
      saveSleepResumeTransition(contentPath_, returnTo, progress);
    } else {
      clearSleepResumeTransition();
    }

    core.content.close();
  }

  // Unload custom reader fonts to free memory
  // Note: device may restart after this (dual-boot system), but explicit cleanup
  // ensures predictable memory behavior and better logging
  FONT_MANAGER.unloadReaderFonts();

  contentLoaded_ = false;
  contentPath_[0] = '\0';
  framePreservedOnWake_ = false;
  directUiTransition_ = false;
  pendingProgress_.reset();
  pendingProgressRestore_ = false;

  // Keep the reader orientation intact when sleep is entered directly from reader
  // so SleepState can draw on top of the existing page buffer in the same space.
  if (!core.preserveReaderPageOnSleep) {
    renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
  }
}

StateTransition ReaderState::update(Core& core) {
  // Handle load failure - transition to error state or back to file list
  if (loadFailed_ || !contentLoaded_) {
    // If error message was set, show ErrorState; otherwise just go back to FileList
    if (core.buf.text[0] != '\0') {
      return StateTransition::to(StateId::Error);
    }
    return StateTransition::to(StateId::FileList);
  }

  if (confirmClickPending_ && (millis() - lastConfirmReleaseMs_ > kConfirmDoubleClickMs)) {
    commitPendingConfirmClick(core);
  }

  // Auto page turn timer
  if (autoPageTurnTargetMs_ != 0 && millis() >= autoPageTurnTargetMs_) {
    autoPageTurnTargetMs_ = 0;
    core.input.resetIdleTimer();
    navigateNext(core);
    return StateTransition::stay(StateId::Reader);
  }

  Event e;
  while (core.events.pop(e)) {
    if (tocMode_) {
      handleTocInput(core, e);
      continue;
    }

    switch (e.type) {
      case EventType::ButtonPress:
        switch (e.button) {
          case Button::Center:
            break;
          case Button::Back:
            if (confirmClickPending_) {
              commitPendingConfirmClick(core);
            }
            return exitToUI(core);
          case Button::Power:
            if (core.settings.shortPwrBtn == Settings::PowerPageTurn) {
              powerPressStartedMs_ = millis();
            }
            break;
          default:
            break;
        }
        break;

      case EventType::ButtonRepeat:
        if (!holdNavigated_) {
          switch (e.button) {
            case Button::Right:
            case Button::Down:
              if (contentSupportsAutoPageTurn(core)) {
                const uint8_t nextMode = isAutoPageTurnEnabled(core) ? Settings::AutoPageOff : Settings::AutoPageOn;
                const bool changed = setAutoPageTurnMode(core, nextMode);
                if (nextMode == Settings::AutoPageOn) {
                  scheduleAutoPageTurn(core, lastPageWordCount_);
                } else {
                  autoPageTurnTargetMs_ = 0;
                }
                needsRender_ = changed;
              }
              holdNavigated_ = true;
              break;
            case Button::Left:
            case Button::Up:
              core.settings.tiltPageTurn = core.settings.tiltPageTurn ? 0 : 1;
              core.settings.save(core.storage);
              needsRender_ = true;
              holdNavigated_ = true;
              break;
            default:
              break;
          }
        }
        break;

      case EventType::ButtonRelease:
        if (e.button == Button::Center && suppressNextCenterRelease_) {
          suppressNextCenterRelease_ = false;
          holdNavigated_ = false;
          break;
        }

        if (e.button != Button::Center && confirmClickPending_) {
          commitPendingConfirmClick(core);
        }

        if (!holdNavigated_) {
          switch (e.button) {
            case Button::Right:
            case Button::Down:
              navigateNext(core);
              break;
            case Button::Left:
            case Button::Up:
              navigatePrev(core);
              break;
            case Button::Power:
              if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
                const uint32_t heldMs = millis() - powerPressStartedMs_;
                if (heldMs < core.settings.getPowerButtonDuration()) {
                  navigateNext(core);
                }
              }
              break;
            case Button::Center: {
              const uint32_t now = millis();
              if (confirmClickPending_ && (now - lastConfirmReleaseMs_ <= kConfirmDoubleClickMs)) {
                confirmClickPending_ = false;
                lastConfirmReleaseMs_ = 0;
                toggleReadingOrientation(core);
              } else {
                confirmClickPending_ = true;
                lastConfirmReleaseMs_ = now;
              }
              break;
            }
            default:
              break;
          }
        }
        if (e.button == Button::Power) {
          powerPressStartedMs_ = 0;
        }
        holdNavigated_ = false;
        break;

      case EventType::TiltForward:
        if (confirmClickPending_) {
          commitPendingConfirmClick(core);
        }
        navigateNext(core);
        return StateTransition::stay(StateId::Reader);

      case EventType::TiltBack:
        if (confirmClickPending_) {
          commitPendingConfirmClick(core);
        }
        navigatePrev(core);
        return StateTransition::stay(StateId::Reader);

      default:
        break;
    }
  }

  return StateTransition::stay(StateId::Reader);
}

void ReaderState::render(Core& core) {
  if (!contentLoaded_) {
    return;
  }

  if (tocMode_) {
    if (needsRender_) {
      renderTocOverlay(core);
      needsRender_ = false;
    }
    return;
  }

  if (needsRender_) {
    renderCurrentPage(core);
    lastRenderedSpineIndex_ = currentSpineIndex_;
    lastRenderedSectionPage_ = currentSectionPage_;
    needsRender_ = false;
  }
}

void ReaderState::navigateNext(Core& core) {
  autoPageTurnTargetMs_ = 0;  // Clear timer; new page render will reschedule

  if (endOfBookShown_) {
    exitToFileList(core);
    return;
  }

  // Stop background task before accessing pageCache_ (ownership model)
  stopBackgroundCaching();

  ContentType type = core.content.metadata().type;

  // XTC uses flatPage navigation, not spine/section - skip to navigation logic
  if (type == ContentType::Xtc) {
    ReaderNavigation::Position pos;
    pos.flatPage = currentPage_;
    auto result = ReaderNavigation::next(type, pos, nullptr, core.content.pageCount());
    applyNavResult(result, core);
    return;
  }

  // Spine/section logic for EPUB, TXT, Markdown
  // From cover (-1) -> first text content page
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    auto* provider = core.content.asEpub();
    size_t spineCount = 1;
    if (provider && provider->getEpub()) {
      spineCount = provider->getEpub()->getSpineItemsCount();
    }
    int firstContentSpine = reader::ReaderCacheController::calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);

    {
      auto resources = acquireForegroundResources("navigate-next-cover");
      if (!resources) {
        startBackgroundCaching(core);
        return;
      }

      if (firstContentSpine != currentSpineIndex_) {
        currentSpineIndex_ = firstContentSpine;
        cacheController_.clearDocumentResources();
      }
    }
    currentSectionPage_ = 0;
    needsRender_ = true;
    startBackgroundCaching(core);
    return;
  }

  ReaderNavigation::NavResult result;
  {
    auto resources = acquireForegroundResources("navigate-next");
    if (!resources) {
      startBackgroundCaching(core);
      return;
    }

    ReaderNavigation::Position pos;
    pos.spineIndex = currentSpineIndex_;
    pos.sectionPage = currentSectionPage_;
    pos.flatPage = currentPage_;
    result = ReaderNavigation::next(type, pos, pageCache_.get(), core.content.pageCount());
  }
  applyNavResult(result, core);
}

void ReaderState::navigatePrev(Core& core) {
  autoPageTurnTargetMs_ = 0;  // Clear timer; new page render will reschedule

  // Stop background task before accessing pageCache_ (ownership model)
  stopBackgroundCaching();

  ContentType type = core.content.metadata().type;

  // XTC uses flatPage navigation, not spine/section - skip to navigation logic
  if (type == ContentType::Xtc) {
    ReaderNavigation::Position pos;
    pos.flatPage = currentPage_;
    auto result = ReaderNavigation::prev(type, pos, nullptr);
    applyNavResult(result, core);
    return;
  }

  // Spine/section logic for EPUB, TXT, Markdown
  auto* provider = core.content.asEpub();
  size_t spineCount = 1;
  if (provider && provider->getEpub()) {
    spineCount = provider->getEpub()->getSpineItemsCount();
  }
  int firstContentSpine = reader::ReaderCacheController::calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);

  // Prevent going back from cover
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    startBackgroundCaching(core);  // Resume task before returning
    return;                        // Already at cover
  }

  bool handledAtStart = false;
  ReaderNavigation::NavResult result;
  {
    auto resources = acquireForegroundResources("navigate-prev");
    if (!resources) {
      startBackgroundCaching(core);
      return;
    }

    if (currentSpineIndex_ == firstContentSpine && currentSectionPage_ == 0) {
      // Only go to cover if it exists and images enabled
      if (hasCover_ && core.settings.showImages) {
        currentSpineIndex_ = 0;
        currentSectionPage_ = -1;
        cacheController_.clearDocumentResources();  // Don't need cache for cover
        needsRender_ = true;
      }
      handledAtStart = true;
    } else {
      ReaderNavigation::Position pos;
      pos.spineIndex = currentSpineIndex_;
      pos.sectionPage = currentSectionPage_;
      pos.flatPage = currentPage_;
      result = ReaderNavigation::prev(type, pos, pageCache_.get());
    }
  }

  if (handledAtStart) {
    startBackgroundCaching(core);
    return;
  }
  applyNavResult(result, core);
}

void ReaderState::applyNavResult(const ReaderNavigation::NavResult& result, Core& core) {
  currentSpineIndex_ = result.position.spineIndex;
  currentSectionPage_ = result.position.sectionPage;
  currentPage_ = result.position.flatPage;
  needsRender_ = result.needsRender;
  if (result.needsCacheReset) {
    auto resources = acquireForegroundResources("apply-nav-reset");
    if (resources) {
      cacheController_.clearDocumentResources();
    }
  }
  startBackgroundCaching(core);  // Resume caching
}

void ReaderState::navigateNextChapter(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    const uint16_t count = core.content.tocCount();
    if (count == 0) return;

    // Find current chapter
    int currentChapter = -1;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= currentPage_) {
        currentChapter = i;
      }
    }

    if (currentChapter + 1 >= static_cast<int>(count)) return;

    auto next = core.content.getTocEntry(currentChapter + 1);
    if (!next.ok()) return;

    currentPage_ = next.value.pageIndex;
    needsRender_ = true;
    return;
  }

  if (type != ContentType::Epub) return;

  auto* provider = core.content.asEpub();
  if (!provider || !provider->getEpub()) return;

  size_t spineCount = provider->getEpub()->getSpineItemsCount();
  if (currentSpineIndex_ + 1 >= static_cast<int>(spineCount)) return;

  stopBackgroundCaching();
  {
    auto resources = acquireForegroundResources("next-chapter-reset");
    if (!resources) {
      startBackgroundCaching(core);
      return;
    }
    currentSpineIndex_++;
    currentSectionPage_ = 0;
    cacheController_.clearDocumentResources();
  }
  needsRender_ = true;
  startBackgroundCaching(core);
}

void ReaderState::navigatePrevChapter(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    const uint16_t count = core.content.tocCount();
    if (count == 0) return;

    // Find current chapter
    int currentChapter = -1;
    uint32_t currentChapterStart = 0;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= currentPage_) {
        currentChapter = i;
        currentChapterStart = result.value.pageIndex;
      }
    }

    if (currentChapter < 0) return;

    if (currentPage_ > currentChapterStart) {
      // Mid-chapter: go to start of current chapter
      currentPage_ = currentChapterStart;
    } else if (currentChapter > 0) {
      // At start of chapter: go to previous chapter
      auto prev = core.content.getTocEntry(currentChapter - 1);
      if (!prev.ok()) return;
      currentPage_ = prev.value.pageIndex;
    } else {
      return;
    }

    needsRender_ = true;
    return;
  }

  if (type != ContentType::Epub) return;

  stopBackgroundCaching();

  bool reachedStartOfBook = false;
  {
    auto resources = acquireForegroundResources("prev-chapter-reset");
    if (!resources) {
      startBackgroundCaching(core);
      return;
    }

    if (currentSectionPage_ > 0) {
      // Go to beginning of current chapter
      currentSectionPage_ = 0;
    } else {
      // Go to previous chapter
      auto* provider = core.content.asEpub();
      size_t spineCount = 1;
      if (provider && provider->getEpub()) {
        spineCount = provider->getEpub()->getSpineItemsCount();
      }
      int firstContentSpine =
          reader::ReaderCacheController::calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);
      if (currentSpineIndex_ <= firstContentSpine) {
        reachedStartOfBook = true;
      } else {
        currentSpineIndex_--;
        currentSectionPage_ = 0;
        cacheController_.clearDocumentResources();
      }
    }
  }

  if (reachedStartOfBook) {
    startBackgroundCaching(core);
    return;
  }

  needsRender_ = true;
  startBackgroundCaching(core);
}

void ReaderState::renderCurrentPage(Core& core) {
  ContentType type = core.content.metadata().type;
  const Theme& theme = THEME_MANAGER.current();

  // Always clear screen first (prevents previous content from showing through)
  renderer_.clearScreen(theme.backgroundColor);
  endOfBookShown_ = false;

  // Cover page: spineIndex=0, sectionPage=-1 (only when showImages enabled)
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    if (core.settings.showImages) {
      if (renderCoverPage(core)) {
        hasCover_ = true;
        core.display.markDirty();
        return;
      }
      // No cover - skip spine 0 if textStartIndex is 0 (likely empty cover document)
      hasCover_ = false;
      currentSectionPage_ = 0;
      if (textStartIndex_ == 0) {
        // Only skip to spine 1 if it exists
        auto* provider = core.content.asEpub();
        if (provider && provider->getEpub()) {
          const auto* epub = provider->getEpub();
          if (epub->getSpineItemsCount() > 1) {
            currentSpineIndex_ = 1;
          }
        }
      }
      // Fall through to render content
    } else {
      currentSectionPage_ = 0;
    }
  }

  switch (type) {
    case ContentType::Epub:
    case ContentType::Txt:
      renderCachedPage(core);
      break;
    case ContentType::Xtc:
      renderXtcPage(core);
      break;
    default:
      break;
  }

  bool shouldResumeCaching = false;
  if (!asyncJobsController_.isWorkerRunning()) {
    auto resources = acquireForegroundResources("render-post-check");
    if (resources) {
      shouldResumeCaching = !pageCache_ || pageCache_->isPartial() || !cacheController_.thumbnailDone();
    }
  }

  if (shouldResumeCaching) {
    startBackgroundCaching(core);
  }

  core.display.markDirty();
}

void ReaderState::renderCachedPage(Core& core) {
  Theme& theme = THEME_MANAGER.mutableCurrent();
  ContentType type = core.content.metadata().type;
  const auto vp = getReaderViewport();

  // Handle EPUB bounds
  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;

    auto epub = provider->getEpubShared();
    if (currentSpineIndex_ < 0) currentSpineIndex_ = 0;
    if (currentSpineIndex_ >= static_cast<int>(epub->getSpineItemsCount())) {
      endOfBookShown_ = true;
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "End of book");
      return;
    }
  }

  // Stop background task to ensure we own pageCache_ (ownership model)
  stopBackgroundCaching();

  auto resources = acquireForegroundResources("render-cached-page");
  if (!resources) {
    needsRender_ = false;
    return;
  }

  // Background task may have left parser in inconsistent state
  if (!pageCache_ && parser_ && parserSpineIndex_ == currentSpineIndex_) {
    parser_.reset();
    parserSpineIndex_ = -1;
  }

  // Create or load cache if needed
  if (!pageCache_) {
    // Try to load existing cache silently first
    loadCacheFromDisk(core);

    bool pageIsCached =
        pageCache_ && currentSectionPage_ >= 0 && currentSectionPage_ < static_cast<int>(pageCache_->pageCount());

    if (!pageIsCached) {
      // Current page not cached - show "Indexing..." and create/extend
      renderer_.clearScreen(theme.backgroundColor);
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Indexing...");
      renderer_.displayBuffer();

      createOrExtendCache(core);

      // Backward navigation: cache entire chapter to find actual last page
      if (currentSectionPage_ == INT16_MAX) {
        while (pageCache_ && pageCache_->isPartial()) {
          const size_t pagesBefore = pageCache_->pageCount();
          createOrExtendCache(core);
          if (pageCache_ && pageCache_->pageCount() <= pagesBefore) {
            break;  // No progress - avoid infinite loop
          }
        }
      }

      // Clear overlay
      renderer_.clearScreen(theme.backgroundColor);
    }

    // Clamp page number (handle negative values and out-of-bounds)
    if (pageCache_) {
      const int cachedPages = static_cast<int>(pageCache_->pageCount());
      if (currentSectionPage_ < 0) {
        currentSectionPage_ = 0;
      } else if (currentSectionPage_ >= cachedPages) {
        currentSectionPage_ = cachedPages > 0 ? cachedPages - 1 : 0;
      }
    }
  }

  // Restore reading position via text anchor after cache is available,
  // so ensureCacheLoaded() reuses the existing pageCache_ without rebuilding.
  if (pendingProgressRestore_) {
    restoreProgressFromTextAnchor(core);
  }

  // Check if we need to extend cache
  if (!ensurePageCached(core, currentSectionPage_)) {
    renderer_.drawCenteredText(theme.uiFontId, 300, "Failed to load page", theme.primaryTextBlack);
    renderer_.displayBuffer();
    needsRender_ = false;  // Prevent infinite render loop on cache failure
    return;
  }

  // ensurePageCached may have used the frame buffer as ZIP decompression dictionary
  renderer_.clearScreen(theme.backgroundColor);

  // Load and render page (cache is now guaranteed to exist, we own it)
  size_t pageCount = pageCache_ ? pageCache_->pageCount() : 0;
  auto page = pageCache_ ? pageCache_->loadPage(currentSectionPage_) : nullptr;

  if (!page) {
    LOG_ERR(TAG, "Failed to load page, clearing cache");
    if (pageCache_) {
      pageCache_->clear();
      pageCache_.reset();
    }
    needsRender_ = true;
    return;
  }

  const int fontId = core.settings.getReaderFontId(theme);
  page->warmGlyphs(renderer_, fontId);

  renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);


  const bool aaEnabled = core.settings.textAntiAliasing && renderer_.fontSupportsGrayscale(fontId);
  const bool imagePageWithAA = aaEnabled && page->hasImages();

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking:
    // HALF_REFRESH sets e-ink particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes (~1200ms total
    // vs ~1720ms for HALF_REFRESH) with better visual quality.
    const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Step 1: Display page with image area blanked (text appears, image area white)
      renderer_.fillRect(imgX + vp.marginLeft, imgY + vp.marginTop, imgW, imgH, !theme.primaryTextBlack);
      renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);

      // Step 2: Re-render with images and display again (images appear clean)
      renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);
    
      renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    } else {
      renderer_.displayBuffer(EInkDisplay::HALF_REFRESH, turnOffScreen);
    }
    // Double FAST_REFRESH handles ghosting; don't count toward full refresh cadence
  } else {
    displayWithRefresh(core);
  }

  // Grayscale text rendering (anti-aliasing)
  if (aaEnabled) {
    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer_, fontId, vp.marginLeft, vp.marginTop, theme.primaryTextBlack);
  
    renderer_.copyGrayscaleLsbBuffers();

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer_, fontId, vp.marginLeft, vp.marginTop, theme.primaryTextBlack);
  
    renderer_.copyGrayscaleMsbBuffers();

    const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
    renderer_.displayGrayBuffer(turnOffScreen);
    renderer_.setRenderMode(GfxRenderer::BW);

    // Re-render BW instead of restoring from backup (saves 48KB peak allocation)
    renderer_.clearScreen(theme.backgroundColor);
    renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);
  
    renderer_.cleanupGrayscaleWithFrameBuffer();
  }

  // Always track word count for immediate scheduling on user activation
  lastPageWordCount_ = countPageWords(*page);
  scheduleAutoPageTurn(core, lastPageWordCount_);
  prefetchAdjacentPage(core);

  LOG_DBG(TAG, "Rendered page %d/%d", currentSectionPage_ + 1, pageCount);
}

void ReaderState::prefetchAdjacentPage(Core& core) {
  if (!pageCache_ || currentSectionPage_ < 0) {
    return;
  }

  const int pageCount = static_cast<int>(pageCache_->pageCount());
  if (pageCount <= 1) {
    return;
  }

  int direction = 1;
  if (lastRenderedSpineIndex_ == currentSpineIndex_ && lastRenderedSectionPage_ > currentSectionPage_) {
    direction = -1;
  }

  const int nextPage = currentSectionPage_ + direction;
  if (nextPage < 0 || nextPage >= pageCount) {
    return;
  }

  pageCache_->prefetchWindow(static_cast<uint16_t>(currentSectionPage_), direction, 2);

  const Theme& theme = THEME_MANAGER.current();
  const int fontId = core.settings.getReaderFontId(theme);
  auto warmedPage = pageCache_->loadPage(static_cast<uint16_t>(nextPage));
  if (warmedPage) {
    warmedPage->warmGlyphs(renderer_, fontId);
  }
}

bool ReaderState::ensurePageCached(Core& core, uint16_t pageNum) {
  // Caller must have stopped background task (we own pageCache_)
  if (!pageCache_) {
    return false;
  }

  // If page is already cached, we're good
  size_t pageCount = pageCache_->pageCount();
  bool needsExtension = pageCache_->needsExtension(pageNum);
  bool isPartial = pageCache_->isPartial();

  if (pageNum < pageCount) {
    // Check if we should pre-extend (approaching end of partial cache)
    if (needsExtension) {
      LOG_DBG(TAG, "Pre-extending cache at page %d", pageNum);
      createOrExtendCache(core);
    }
    return true;
  }

  // Page not cached yet - need to extend
  if (!isPartial) {
    LOG_DBG(TAG, "Page %d not available (cache complete at %d pages)", pageNum, pageCount);
    return false;
  }

  LOG_DBG(TAG, "Extending cache for page %d", pageNum);

  const Theme& theme = THEME_MANAGER.current();
  ui::centeredMessage(renderer_, theme, theme.uiFontId, "Loading...");

  createOrExtendCache(core);

  pageCount = pageCache_ ? pageCache_->pageCount() : 0;
  return pageNum < pageCount;
}

void ReaderState::loadCacheFromDisk(Core& core) {
  const auto vp = getReaderViewport();
  cacheController_.loadCacheFromDisk(core, currentSpineIndex_, vp.width, vp.height);
}

void ReaderState::createOrExtendCache(Core& core) {
  const auto vp = getReaderViewport();
  cacheController_.createOrExtendCache(core, currentSpineIndex_, vp.width, vp.height);
}

void ReaderState::renderPageContents(Core& core, Page& page, int marginTop, int marginRight, int marginBottom,
                                     int marginLeft) {
  (void)marginRight;
  (void)marginBottom;

  const Theme& theme = THEME_MANAGER.current();
  const int fontId = core.settings.getReaderFontId(theme);
  page.render(renderer_, fontId, marginLeft, marginTop, theme.primaryTextBlack);

  // Auto page turn indicator — › (U+203A), bottom-right corner.
  // Drawn into the same buffer before displayWithRefresh so it persists on every page.
  constexpr int kInset = 6;
  constexpr char kTiltIndicator[] = "\xe2\x80\xa2";
  constexpr char kAutoIndicator[] = "\xe2\x80\xba";
  int tiltMinX = 0;
  int tiltMinY = 0;
  int tiltMaxX = 0;
  int tiltMaxY = 0;
  int autoMinX = 0;
  int autoMinY = 0;
  int autoMaxX = 0;
  int autoMaxY = 0;
  renderer_.getTextBounds(theme.smallFontId, kTiltIndicator, &tiltMinX, &tiltMinY, &tiltMaxX, &tiltMaxY);
  renderer_.getTextBounds(theme.smallFontId, kAutoIndicator, &autoMinX, &autoMinY, &autoMaxX, &autoMaxY);

  // tiltMinY / autoMinY = glyph bottom in math Y-up space (baselineY + top - height).
  // Screen Y of glyph bottom = ty + 2*ascender - minY - 1.
  // To place glyph bottom at screenH - kInset - 1: ty = screenH - kInset - 2*ascender + minY.
  const int ascender = renderer_.getFontAscenderSize(theme.smallFontId);

  if (isTiltPageTurnEnabled(core)) {
    const int tx = kInset - tiltMinX;
    const int ty = renderer_.getScreenHeight() - kInset - 2 * ascender + tiltMinY;
    renderer_.drawText(theme.smallFontId, tx, ty, kTiltIndicator, theme.primaryTextBlack);
  }

  if (isAutoPageTurnEnabled(core)) {
    const int tx = renderer_.getScreenWidth() - kInset - autoMaxX;
    const int ty = renderer_.getScreenHeight() - kInset - 2 * ascender + autoMinY;
    renderer_.drawText(theme.smallFontId, tx, ty, kAutoIndicator, theme.primaryTextBlack);
  }
}

void ReaderState::renderXtcPage(Core& core) {
  auto* provider = core.content.asXtc();
  if (!provider) {
    return;
  }

  const Theme& theme = THEME_MANAGER.current();
  auto result = xtcRenderer_.render(provider->getParser(), currentPage_, [this, &core]() { displayWithRefresh(core); });

  switch (result) {
    case XtcPageRenderer::RenderResult::Success:
      if (provider->getParser().getBitDepth() == 2) {
        pagesUntilFullRefresh_ = 1;
      }
      autoPageTurnTargetMs_ = 0;
      break;
    case XtcPageRenderer::RenderResult::EndOfBook:
      endOfBookShown_ = true;
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "End of book");
      break;
    case XtcPageRenderer::RenderResult::InvalidDimensions:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Invalid file");
      break;
    case XtcPageRenderer::RenderResult::AllocationFailed:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Memory error");
      break;
    case XtcPageRenderer::RenderResult::PageLoadFailed:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Page load error");
      break;
  }
}

ProgressManager::Progress ReaderState::buildProgressSnapshot(Core& core) const {
  ProgressManager::Progress progress;
  progress.spineIndex = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSpineIndex_;
  progress.sectionPage = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSectionPage_;
  progress.flatPage = currentPage_;

  const ContentType type = core.content.metadata().type;
  if (type == ContentType::Xtc || lastRenderedSectionPage_ < 0) {
    return progress;
  }

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport();
  const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);
  const std::string cachePath = cachePathForPosition(core, type, progress.spineIndex, config);
  if (cachePath.empty()) {
    return progress;
  }

  std::shared_ptr<Page> page;
  if (pageCache_) {
    page = pageCache_->loadPage(static_cast<uint16_t>(progress.sectionPage));
  }
  if (!page) {
    PageCache cache(cachePath);
    if (cache.load(config)) {
      page = cache.loadPage(static_cast<uint16_t>(progress.sectionPage));
    }
  }

  if (page) {
    const std::string anchor = extractTextAnchor(*page);
    if (!anchor.empty()) {
      progress.setTextAnchor(anchor.c_str());
    }
  }

  return progress;
}

void ReaderState::restoreProgressFromTextAnchor(Core& core) {
  pendingProgressRestore_ = false;

  if (!pendingProgress_.hasTextAnchor()) {
    return;
  }

  const ContentType type = core.content.metadata().type;
  if (type == ContentType::Xtc) {
    return;
  }

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport();
  const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);
  const std::string cachePath = cachePathForPosition(core, type, currentSpineIndex_, config);
  if (cachePath.empty()) {
    return;
  }

  auto ensureCacheLoaded = [&]() -> bool {
    if (!pageCache_) {
      pageCache_.reset(new (std::nothrow) PageCache(cachePath));
      if (pageCache_ && !pageCache_->load(config)) {
        pageCache_.reset();
      }
    }

    if (!pageCache_) {
      createOrExtendCache(core);
    }
    return pageCache_ != nullptr;
  };

  if (!ensureCacheLoaded()) {
    return;
  }

  int resolvedPage = findPageForTextAnchor(*pageCache_, pendingProgress_.textAnchor, pendingProgress_.sectionPage);

  // Cap extensions to avoid spending minutes on a very long book whose anchor is
  // not yet cached.  Each chunk caches ~20 pages, so 10 iterations = ~200 pages.
  constexpr int kMaxAnchorSearchExtensions = 10;
  int extensionCount = 0;
  while (resolvedPage < 0 && pageCache_ && pageCache_->isPartial() &&
         extensionCount < kMaxAnchorSearchExtensions) {
    const size_t pagesBefore = pageCache_->pageCount();
    createOrExtendCache(core);
    extensionCount++;
    if (!pageCache_ || pageCache_->pageCount() <= pagesBefore) {
      break;  // No progress — SD full or parse error
    }
    resolvedPage = findPageForTextAnchor(*pageCache_, pendingProgress_.textAnchor, pendingProgress_.sectionPage);
  }

  if (extensionCount >= kMaxAnchorSearchExtensions && resolvedPage < 0) {
    LOG_DBG(TAG, "Anchor search limit reached after %d extensions, falling back", extensionCount);
  }

  if (resolvedPage >= 0) {
    currentSectionPage_ = resolvedPage;
    LOG_INF(TAG, "Restored reading position from text anchor: spine=%d page=%d", currentSpineIndex_, resolvedPage);
  } else {
    LOG_INF(TAG, "Falling back to saved page index: spine=%d page=%d", currentSpineIndex_, currentSectionPage_);
  }
}

std::string ReaderState::extractTextAnchor(const Page& page) const {
  constexpr size_t kMaxAnchorBytes = ProgressManager::Progress::kTextAnchorSize - 1;
  std::vector<const TextBlock*> lines;
  lines.reserve(page.elements.size());

  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) {
      continue;
    }

    const auto& line = static_cast<const PageLine&>(*element);
    lines.push_back(&line.getTextBlock());
  }

  if (lines.empty()) {
    return {};
  }

  const size_t center = lines.size() / 2;
  const size_t start = (center > 1) ? center - 1 : 0;
  const size_t end = std::min(lines.size(), start + 3);

  std::string anchor;
  for (size_t i = start; i < end; i++) {
    for (const auto& word : lines[i]->getWords()) {
      if (!anchor.empty()) {
        anchor.push_back(' ');
      }
      anchor += word.word;
      if (anchor.size() >= kMaxAnchorBytes) {
        anchor.resize(kMaxAnchorBytes);
        return normalizeAnchorText(anchor);
      }
    }
  }

  return normalizeAnchorText(anchor);
}

std::string ReaderState::cachePathForPosition(Core& core, ContentType type, int spineIndex,
                                              const RenderConfig& config) const {
  return cacheController_.cachePathForPosition(core, type, spineIndex, config);
}

int ReaderState::findPageForTextAnchor(PageCache& cache, const char* textAnchor, int pageHint) const {
  if (!textAnchor || textAnchor[0] == '\0') {
    return -1;
  }

  const std::string anchor = normalizeAnchorText(textAnchor);
  if (anchor.empty()) {
    return -1;
  }

  const int total = static_cast<int>(cache.pageCount());
  if (total == 0) return -1;

  const int clampedHint = std::max(0, std::min(pageHint, total - 1));

  // Search outward from pageHint — the first match found is guaranteed closest
  for (int radius = 0; radius <= total; radius++) {
    const int above = clampedHint + radius;
    if (above < total) {
      auto page = cache.loadPage(static_cast<uint16_t>(above));
      if (page && pageTextForAnchorMatch(*page).find(anchor) != std::string::npos) {
        return above;
      }
    }
    if (radius > 0) {
      const int below = clampedHint - radius;
      if (below >= 0) {
        auto page = cache.loadPage(static_cast<uint16_t>(below));
        if (page && pageTextForAnchorMatch(*page).find(anchor) != std::string::npos) {
          return below;
        }
      }
    }
  }

  return -1;
}

void ReaderState::displayWithRefresh(Core& core) {
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
  if (pagesUntilFullRefresh_ <= 1) {
    renderer_.displayBuffer(EInkDisplay::HALF_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_ = core.settings.getPagesPerRefreshValue();
  } else {
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_--;
  }
}

void ReaderState::commitPendingConfirmClick(Core& core) {
  if (!confirmClickPending_) {
    return;
  }

  confirmClickPending_ = false;
  lastConfirmReleaseMs_ = 0;
  if (core.content.tocCount() > 0) {
    enterTocMode(core);
    return;
  }

  const Theme& theme = THEME_MANAGER.current();
  renderer_.clearScreen(theme.backgroundColor);
  ui::overlayBox(renderer_, theme, theme.uiFontId, renderer_.getScreenHeight() / 2 - 20, "No chapters");
  renderer_.displayBuffer();
  core.display.markDirty();
}

void ReaderState::toggleReadingOrientation(Core& core) {
  // Stop background task before reading pageCache_ to avoid race condition.
  stopBackgroundCaching();

  {
    auto resources = acquireForegroundResources("toggle-orientation");
    if (resources && contentLoaded_) {
      pendingProgress_ = buildProgressSnapshot(core);
      pendingProgressRestore_ = pendingProgress_.hasTextAnchor();
      currentSpineIndex_ = pendingProgress_.spineIndex;
      currentSectionPage_ = pendingProgress_.sectionPage;
      currentPage_ = pendingProgress_.flatPage;
    }
  }

  core.settings.orientation =
      (core.settings.orientation == Settings::Portrait) ? Settings::LandscapeCCW : Settings::Portrait;
  core.settings.save(core.storage);

  switch (core.settings.orientation) {
    case Settings::LandscapeCCW:
      renderer_.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    case Settings::Portrait:
    default:
      renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
  }

  const Theme& theme = THEME_MANAGER.current();
  renderer_.clearScreen(theme.backgroundColor);
  renderer_.drawCenteredText(theme.uiFontId, renderer_.getScreenHeight() / 2 - renderer_.getLineHeight(theme.uiFontId) / 2,
                             "Indexing...", theme.primaryTextBlack);
  renderer_.displayBuffer();
  core.display.markDirty();

  {
    auto resources = acquireForegroundResources("toggle-orientation-reset");
    if (resources) {
      cacheController_.clearDocumentResources();
    }
  }
  pagesUntilFullRefresh_ = 1;
  endOfBookShown_ = false;
  needsRender_ = true;
  startBackgroundCaching(core);
}

ReaderState::Viewport ReaderState::getReaderViewport() const {
  Viewport vp{};
  vp.marginTop = kReaderMarginTop;
  vp.marginRight = kReaderMarginRight;
  vp.marginBottom = kReaderMarginBottom;
  vp.marginLeft = kReaderMarginLeft;
  vp.width = renderer_.getScreenWidth() - vp.marginLeft - vp.marginRight;
  vp.height = renderer_.getScreenHeight() - vp.marginTop - vp.marginBottom;
  return vp;
}

bool ReaderState::renderCoverPage(Core& core) {
  LOG_DBG(TAG, "Generating cover for reader...");
  std::string coverPath = core.content.generateCover(true);  // Always 1-bit in reader (saves ~48KB grayscale buffer)
  if (coverPath.empty()) {
    LOG_DBG(TAG, "No cover available, skipping cover page");
    return false;
  }

  LOG_DBG(TAG, "Rendering cover page from: %s", coverPath.c_str());
  const auto vp = getReaderViewport();
  int pagesUntilRefresh = pagesUntilFullRefresh_;
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;

  bool rendered = CoverHelpers::renderCoverFromBmp(renderer_, coverPath, vp.marginTop, vp.marginRight, vp.marginBottom,
                                                   vp.marginLeft, pagesUntilRefresh,
                                                   core.settings.getPagesPerRefreshValue(), turnOffScreen);

  // Force half refresh on next page to fully clear the cover image
  pagesUntilFullRefresh_ = 1;
  return rendered;
}

void ReaderState::startBackgroundCaching(Core& core) {
  // XTC content uses pre-rendered bitmaps — no page cache needed.
  // Generate cover + thumbnail synchronously since XTC has no background task.
  if (core.content.metadata().type == ContentType::Xtc) {
    if (!thumbnailDone_.load(std::memory_order_acquire)) {
      core.content.generateCover(true);
      core.content.generateThumbnail();
      thumbnailDone_.store(true, std::memory_order_release);
    }
    return;
  }

  LOG_INF(TAG, "Starting background page cache task");

  // Snapshot state for the background task
  const int sectionPage = currentSectionPage_;
  const int spineIndex = currentSpineIndex_;
  const bool coverExists = hasCover_;
  const int textStart = textStartIndex_;

  asyncJobsController_.startWorkerJob("PageCache", [this, &core, sectionPage, spineIndex, coverExists,
                                                    textStart](const reader::ReaderAsyncJobsController::AbortCallback& abort) {
    LOG_INF(TAG, "Background cache task started");

    auto resources = acquireWorkerResources("background-cache");
    if (!resources) {
      return;
    }

    const auto vp = getReaderViewport();
    cacheController_.runBackgroundCache(core, spineIndex, sectionPage, coverExists, textStart, vp.width, vp.height, abort);

    // Generate thumbnail from cover for HomeState (lower priority than page cache)
    // Only attempt once per book open — skip if already tried (success or failure)
    if (!cacheController_.thumbnailDone() && !(abort && abort())) {
      core.content.generateThumbnail();
      thumbnailDone_.store(true, std::memory_order_release);
    }

    if (!(abort && abort())) {
      LOG_INF(TAG, "Background cache task completed");
    } else {
      LOG_DBG(TAG, "Background cache task stopped");
    }
  }, 0);
}

void ReaderState::stopBackgroundCaching() {
  if (!asyncJobsController_.isWorkerRunning()) {
    return;
  }
  asyncJobsController_.stopWorker();
}

// ============================================================================
// TOC Overlay Mode
// ============================================================================

void ReaderState::enterTocMode(Core& core) {
  if (core.content.tocCount() == 0) {
    return;
  }

  // Stop background task before TOC overlay — both SD card I/O (thumbnail)
  // and e-ink display update share the same SPI bus
  stopBackgroundCaching();

  populateTocView(core);
  int currentIdx = findCurrentTocEntry(core);
  if (currentIdx >= 0) {
    tocView_.setCurrentChapter(static_cast<uint16_t>(currentIdx));
  }

  tocView_.buttons = ui::ButtonBar("Back", "Go", "<<", ">>");
  tocMode_ = true;
  needsRender_ = true;
  LOG_DBG(TAG, "Entered TOC mode");
}

void ReaderState::exitTocMode() {
  tocMode_ = false;
  needsRender_ = true;
  LOG_DBG(TAG, "Exited TOC mode");
}

void ReaderState::handleTocInput(Core& core, const Event& e) {
  if (e.type == EventType::ButtonRelease) {
    if (e.button != Button::Power) {
      return;
    }

    if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
      const uint32_t heldMs = millis() - powerPressStartedMs_;
      if (heldMs < core.settings.getPowerButtonDuration()) {
        tocView_.moveDown();
        needsRender_ = true;
      }
    }
    powerPressStartedMs_ = 0;
    return;
  }

  if (e.type != EventType::ButtonPress && e.type != EventType::ButtonRepeat) return;

  switch (e.button) {
    case Button::Up:
      tocView_.moveUp();
      needsRender_ = true;
      break;

    case Button::Down:
      tocView_.moveDown();
      needsRender_ = true;
      break;

    case Button::Left:
      tocView_.movePageUp(tocVisibleCount());
      needsRender_ = true;
      break;

    case Button::Right:
      tocView_.movePageDown(tocVisibleCount());
      needsRender_ = true;
      break;

    case Button::Center:
      jumpToTocEntry(core, tocView_.selected);
      suppressNextCenterRelease_ = true;
      exitTocMode();
      startBackgroundCaching(core);
      break;

    case Button::Back:
      exitTocMode();
      startBackgroundCaching(core);
      break;

    case Button::Power:
      if (e.type == EventType::ButtonPress && core.settings.shortPwrBtn == Settings::PowerPageTurn) {
        powerPressStartedMs_ = millis();
      }
      break;
  }
}

void ReaderState::populateTocView(Core& core) {
  tocView_.clear();
  const uint16_t count = core.content.tocCount();

  for (uint16_t i = 0; i < count && i < ui::ChapterListView::MAX_CHAPTERS; i++) {
    auto result = core.content.getTocEntry(i);
    if (result.ok()) {
      const TocEntry& entry = result.value;
      tocView_.addChapter(entry.title, static_cast<uint16_t>(entry.pageIndex), entry.depth);
    }
  }
}

int ReaderState::findCurrentTocEntry(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return -1;
    auto epub = provider->getEpubShared();

    // Start with spine-level match as fallback
    int bestMatch = epub->getTocIndexForSpineIndex(currentSpineIndex_);
    int bestMatchPage = -1;

    // Load anchor map once from disk (avoids reopening file per TOC entry)
    std::string cachePath = epubSectionCachePath(epub->getCachePath(), currentSpineIndex_);
    auto anchors = reader::ReaderCacheController::loadAnchorMap(cachePath);

    // Refine: find the latest TOC entry whose anchor page <= current page
    const int tocCount = epub->getTocItemsCount();

    for (int i = 0; i < tocCount; i++) {
      auto tocItem = epub->getTocItem(i);
      if (tocItem.spineIndex != currentSpineIndex_) continue;

      int entryPage = 0;  // No anchor = start of spine
      if (!tocItem.anchor.empty()) {
        int anchorPage = -1;
        for (const auto& a : anchors) {
          if (a.first == tocItem.anchor) {
            anchorPage = a.second;
            break;
          }
        }
        if (anchorPage < 0) continue;  // Anchor not resolved yet
        entryPage = anchorPage;
      }

      if (entryPage <= currentSectionPage_ && entryPage >= bestMatchPage) {
        bestMatch = i;
        bestMatchPage = entryPage;
      }
    }

    return bestMatch;
  } else if (type == ContentType::Xtc) {
    const uint16_t count = core.content.tocCount();
    std::vector<reader::FlatTocEntry> tocEntries;
    tocEntries.reserve(count);

    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok()) {
        tocEntries.push_back({static_cast<int>(i), result.value.pageIndex});
      }
    }

    return reader::findFlatTocEntryForPage(tocEntries, static_cast<uint32_t>(currentPage_));
  } else if (type == ContentType::Txt) {
    const uint16_t count = core.content.tocCount();
    std::vector<reader::FlatTocEntry> tocEntries;
    tocEntries.reserve(count);

    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok()) {
        tocEntries.push_back({static_cast<int>(i), result.value.pageIndex});
      }
    }

    return reader::findFlatTocEntryForPage(tocEntries, static_cast<uint32_t>(currentSectionPage_));
  }

  return -1;
}

void ReaderState::jumpToTocEntry(Core& core, int tocIndex) {
  if (tocIndex < 0 || tocIndex >= tocView_.chapterCount) {
    return;
  }

  const auto& chapter = tocView_.chapters[tocIndex];
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;
    auto epub = provider->getEpubShared();
    auto resources = acquireForegroundResources("toc-jump");
    if (!resources) {
      return;
    }

    const auto jumpPlan = reader::planEpubTocJump(currentSpineIndex_, chapter.pageNum);
    if (jumpPlan.needsResourceReset) {
      // Task already stopped by enterTocMode(); caller restarts after exitTocMode()
      cacheController_.clearDocumentResources();
    }
    currentSpineIndex_ = jumpPlan.spineIndex;
    currentSectionPage_ = jumpPlan.sectionPage;

    // Try anchor-based navigation for precise positioning
    auto tocItem = epub->getTocItem(tocIndex);
    if (!tocItem.anchor.empty()) {
      std::string cachePath = epubSectionCachePath(epub->getCachePath(), chapter.pageNum);
      int page = reader::ReaderCacheController::loadAnchorPage(cachePath, tocItem.anchor);

      // Anchor not resolved — build cache until found or chapter fully parsed
      if (page < 0) {
        const Theme& theme = THEME_MANAGER.current();
        renderer_.clearScreen(theme.backgroundColor);
        ui::centeredMessage(renderer_, theme, theme.uiFontId, "Indexing...");
        renderer_.displayBuffer();

        createOrExtendCache(core);
        page = reader::ReaderCacheController::loadAnchorPage(cachePath, tocItem.anchor);

        while (page < 0 && pageCache_ && pageCache_->isPartial()) {
          const size_t pagesBefore = pageCache_->pageCount();
          createOrExtendCache(core);
          if (!pageCache_ || pageCache_->pageCount() <= pagesBefore) break;
          page = reader::ReaderCacheController::loadAnchorPage(cachePath, tocItem.anchor);
        }
      }

      if (page >= 0) {
        currentSectionPage_ = page;
      }
    }
  } else if (type == ContentType::Xtc) {
    // For XTC, pageNum is page index
    currentPage_ = chapter.pageNum;
  } else if (type == ContentType::Txt) {
    // For flat-page formats, pageNum is the section page index
    currentSectionPage_ = chapter.pageNum;
  }

  needsRender_ = true;
  LOG_DBG(TAG, "Jumped to TOC entry %d (spine/page %d)", tocIndex, chapter.pageNum);
}

int ReaderState::tocVisibleCount() const {
  const Theme& theme = THEME_MANAGER.current();
  const int startY = ui::contentStartY(renderer_, theme);
  constexpr int bottomMargin = 70;
  const int itemHeight = theme.itemHeight + theme.itemSpacing;
  return (renderer_.getScreenHeight() - startY - bottomMargin) / itemHeight;
}

void ReaderState::renderTocOverlay(Core& core) {
  const Theme& theme = THEME_MANAGER.current();
  const int startY = ui::contentStartY(renderer_, theme);
  const int visibleCount = tocVisibleCount();

  // Adjust scroll to keep selection visible
  tocView_.ensureVisible(visibleCount);

  renderer_.clearScreen(theme.backgroundColor);
  ui::title(renderer_, theme, theme.screenMarginTop, "Chapters");

  // Use reader font only when external font is selected (for VN/Thai/CJK support),
  // otherwise use smaller UI font for better chapter list readability
  const ContentType type = core.content.metadata().type;
  const bool hasExternalFont = core.settings.hasExternalReaderFont(theme);
  (void)type;
  (void)hasExternalFont;
  const int tocFontId = theme.uiFontId;

  const int itemHeight = theme.itemHeight + theme.itemSpacing;
  const int end = std::min(tocView_.scrollOffset + visibleCount, static_cast<int>(tocView_.chapterCount));
  for (int i = tocView_.scrollOffset; i < end; i++) {
    const int y = startY + (i - tocView_.scrollOffset) * itemHeight;
    ui::chapterItem(renderer_, theme, tocFontId, y, tocView_.chapters[i].title, tocView_.chapters[i].depth,
                    i == tocView_.selected, i == tocView_.currentChapter);
  }

  ui::buttonBar(renderer_, theme, tocView_.buttons);
  renderer_.displayBuffer();
  core.display.markDirty();
}

StateTransition ReaderState::exitToUI(Core& core) {
  if (directUiTransition_) {
    LOG_INF(TAG, "Exiting to UI via direct state transition");
    core.pendingUiReturnFromReader = true;
    core.pendingReaderReturnState = sourceState_;
    return StateTransition::to(sourceState_);
  }

  LOG_INF(TAG, "Exiting to UI mode via restart");

  // Stop background caching first - BackgroundTask::stop() waits properly
  stopBackgroundCaching();

  // Save progress at last rendered position
  if (contentLoaded_) {
    ProgressManager::Progress progress;
    {
      auto resources = acquireForegroundResources("exit-to-ui-save-progress");
      if (resources) {
        progress = buildProgressSnapshot(core);
      }
    }
    ProgressManager::save(core, core.content.cacheDir(), core.content.metadata().type, progress);
    // Skip pageCache_.reset() and content.close() — ESP.restart() follows,
    // and if stopBackgroundCaching() timed out the task still uses them.
  }

  // Reader back action should always return to File List.
  const ReturnTo returnTo = ReturnTo::FILE_MANAGER;

  // Show notification and restart
  showTransitionNotification("Returning to library...");
  saveTransition(BootMode::UI, nullptr, returnTo);

  // Brief delay to ensure SD writes complete before restart
  vTaskDelay(50 / portTICK_PERIOD_MS);
  ESP.restart();
  return StateTransition::stay(StateId::Reader);
}

void ReaderState::exitToFileList(Core& core) {
  LOG_INF(TAG, "Exiting to file list via restart");

  stopBackgroundCaching();

  if (contentLoaded_) {
    ProgressManager::Progress progress;
    {
      auto resources = acquireForegroundResources("exit-to-file-list-save-progress");
      if (resources) {
        progress = buildProgressSnapshot(core);
      }
    }
    ProgressManager::save(core, core.content.cacheDir(), core.content.metadata().type, progress);
  }

  showTransitionNotification("Returning to library...");
  saveTransition(BootMode::UI, nullptr, ReturnTo::FILE_MANAGER);
  vTaskDelay(50 / portTICK_PERIOD_MS);
  ESP.restart();
}

}  // namespace papyrix
