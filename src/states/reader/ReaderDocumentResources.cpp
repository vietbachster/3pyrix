#include "ReaderDocumentResources.h"

#include <GfxRenderer.h>
#include <Logging.h>

#define TAG "RDR_RES"

namespace papyrix::reader {

ReaderDocumentResources::Session::Session(ReaderDocumentResources& owner, const Owner kind) : owner_(&owner), kind_(kind) {}

ReaderDocumentResources::Session::Session(Session&& other) noexcept : owner_(other.owner_), kind_(other.kind_) {
  other.owner_ = nullptr;
  other.kind_ = Owner::None;
}

ReaderDocumentResources::Session& ReaderDocumentResources::Session::operator=(Session&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  release();
  owner_ = other.owner_;
  kind_ = other.kind_;
  other.owner_ = nullptr;
  other.kind_ = Owner::None;
  return *this;
}

ReaderDocumentResources::Session::~Session() { release(); }

ReaderDocumentResources::State& ReaderDocumentResources::Session::state() { return owner_->state_; }

const ReaderDocumentResources::State& ReaderDocumentResources::Session::state() const { return owner_->state_; }

GfxRenderer& ReaderDocumentResources::Session::renderer() const { return owner_->renderer_; }

void ReaderDocumentResources::Session::release() {
  if (!owner_) {
    return;
  }
  owner_->release(kind_);
  owner_ = nullptr;
  kind_ = Owner::None;
}

ReaderDocumentResources::ReaderDocumentResources(GfxRenderer& renderer) : renderer_(renderer) {}

ReaderDocumentResources::Session ReaderDocumentResources::acquireForeground(const char* reason) {
  return acquire(Owner::Foreground, reason) ? Session(*this, Owner::Foreground) : Session();
}

ReaderDocumentResources::Session ReaderDocumentResources::acquireWorker(const char* reason) {
  return acquire(Owner::Worker, reason) ? Session(*this, Owner::Worker) : Session();
}

bool ReaderDocumentResources::acquire(const Owner kind, const char* reason) {
  Owner current = owner_.load(std::memory_order_acquire);

  while (true) {
    if (current == kind) {
      depth_.fetch_add(1, std::memory_order_acq_rel);
      return true;
    }

    if (current != Owner::None) {
      const char* currentReason = ownerReason_.load(std::memory_order_acquire);
      LOG_INF(TAG, "[OWNERSHIP] deny request=%d reason=%s current=%d currentReason=%s", static_cast<int>(kind),
              reason ? reason : "-", static_cast<int>(current), currentReason ? currentReason : "-");
      return false;
    }

    if (owner_.compare_exchange_weak(current, kind, std::memory_order_acq_rel, std::memory_order_acquire)) {
      ownerReason_.store(reason, std::memory_order_release);
      depth_.store(1, std::memory_order_release);
      return true;
    }
  }
}

void ReaderDocumentResources::release(const Owner kind) {
  const Owner current = owner_.load(std::memory_order_acquire);
  if (current != kind) {
    LOG_ERR(TAG, "[OWNERSHIP] release mismatch requested=%d current=%d", static_cast<int>(kind), static_cast<int>(current));
    return;
  }

  const uint32_t previousDepth = depth_.fetch_sub(1, std::memory_order_acq_rel);
  if (previousDepth == 0) {
    depth_.store(0, std::memory_order_release);
    LOG_ERR(TAG, "[OWNERSHIP] release underflow requested=%d", static_cast<int>(kind));
    return;
  }

  if (previousDepth == 1) {
    ownerReason_.store(nullptr, std::memory_order_release);
    owner_.store(Owner::None, std::memory_order_release);
  }
}

}  // namespace papyrix::reader
