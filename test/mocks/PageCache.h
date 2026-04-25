#pragma once

#include <cstdint>

class PageCache {
 public:
  PageCache(int pageCount = 0, bool partial = false, bool needsExtensionResult = false)
      : pageCount_(pageCount), partial_(partial), needsExtensionResult_(needsExtensionResult) {}

  int pageCount() const { return pageCount_; }
  bool isPartial() const { return partial_; }

  bool needsExtension(uint16_t page) const {
    lastNeedsExtensionPage_ = page;
    return needsExtensionResult_;
  }

  void setPageCount(int pageCount) { pageCount_ = pageCount; }
  void setPartial(bool partial) { partial_ = partial; }
  void setNeedsExtensionResult(bool value) { needsExtensionResult_ = value; }
  int lastNeedsExtensionPage() const { return lastNeedsExtensionPage_; }

 private:
  int pageCount_ = 0;
  bool partial_ = false;
  bool needsExtensionResult_ = false;
  mutable int lastNeedsExtensionPage_ = -1;
};
