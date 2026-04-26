#include "ReaderAsyncJobsController.h"

#include <Logging.h>

#include <utility>

#define TAG "RDR_ASYNC"

namespace papyrix::reader {

bool ReaderAsyncJobsController::startWorkerJob(const char* taskName, JobHandler handler, const int priority) {
  if (workerTask_.isRunning()) {
    LOG_ERR(TAG, "Worker still running, stopping before restart");
    stopWorker();
  }

  currentJob_ = std::move(handler);
  return workerTask_.start(taskName, kDefaultTaskStackSize, [this]() {
    if (currentJob_) {
      currentJob_(workerTask_.getAbortCallback());
    }
  }, priority);
}

bool ReaderAsyncJobsController::stopWorker(const uint32_t maxWaitMs) {
  if (!workerTask_.isRunning()) {
    return true;
  }

  if (!workerTask_.stop(maxWaitMs)) {
    LOG_ERR(TAG, "Worker did not stop within timeout");
    LOG_ERR(TAG, "Task may be blocked on SD card I/O");
    return false;
  }

  // Let the idle task reclaim the worker's TCB before foreground code tears
  // down parser/page-cache state that the task may have just released.
  vTaskDelay(10 / portTICK_PERIOD_MS);
  return true;
}

}  // namespace papyrix::reader
