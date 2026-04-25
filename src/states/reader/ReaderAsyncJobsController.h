#pragma once

#include <BackgroundTask.h>

#include <cstdint>
#include <functional>

namespace papyrix::reader {

class ReaderAsyncJobsController {
 public:
  using AbortCallback = BackgroundTask::AbortCallback;
  using JobHandler = std::function<void(const AbortCallback&)>;

  static constexpr uint32_t kDefaultTaskStackSize = 12288;
  static constexpr uint32_t kDefaultStopTimeoutMs = 10000;

  ReaderAsyncJobsController() = default;

  bool startWorkerJob(const char* taskName, JobHandler handler, int priority = 0);
  bool stopWorker(uint32_t maxWaitMs = kDefaultStopTimeoutMs);

  bool isWorkerRunning() const { return workerTask_.isRunning(); }
  BackgroundTask::State workerState() const { return workerTask_.getState(); }
  AbortCallback abortCallback() const { return workerTask_.getAbortCallback(); }

 private:
  BackgroundTask workerTask_;
  JobHandler currentJob_;
};

}  // namespace papyrix::reader
