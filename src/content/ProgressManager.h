#pragma once

#include <cstdint>
#include <cstring>

#include "ContentTypes.h"

namespace papyrix {

struct Core;

// ProgressManager - Handles reading position persistence
// Stores format-specific progress data to cache directory
class ProgressManager {
 public:
  // Progress data for different content types
  struct Progress {
    static constexpr size_t kTextAnchorSize = 96;

    int spineIndex = 0;     // EPUB: chapter index in spine
    int sectionPage = 0;    // All formats: current page within section/document
    uint32_t flatPage = 0;  // XTC: absolute page number (1-indexed internally)
    char textAnchor[kTextAnchorSize] = {};

    void reset() {
      spineIndex = 0;
      sectionPage = 0;
      flatPage = 0;
      textAnchor[0] = '\0';
    }

    bool hasTextAnchor() const { return textAnchor[0] != '\0'; }

    void setTextAnchor(const char* anchor) {
      if (!anchor) {
        textAnchor[0] = '\0';
        return;
      }
      strncpy(textAnchor, anchor, kTextAnchorSize - 1);
      textAnchor[kTextAnchorSize - 1] = '\0';
    }
  };

  // Save progress to cache directory
  // Returns true on success
  static bool save(Core& core, const char* cacheDir, ContentType type, const Progress& progress);

  // Load progress from cache directory
  // Returns loaded progress (or default values if no saved progress)
  static Progress load(Core& core, const char* cacheDir, ContentType type);

  // Validate progress against content bounds
  // Returns validated (possibly clamped) progress
  static Progress validate(Core& core, ContentType type, const Progress& progress);
};

}  // namespace papyrix
