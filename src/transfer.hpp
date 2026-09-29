#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "pipe.hpp"
#include "plan.hpp"

namespace rarftp {

class FtpClient;
class Logger;
class Progress;

struct TransferResult {
  enum class Status { Success, Failed, Cancelled };

  Status status = Status::Success;
  std::string error;
  uint64_t files_uploaded = 0;
  uint64_t bytes_uploaded = 0;
  double seconds = 0.0;
};

// Runs the pipeline: an extractor thread decompresses entries (RAR_TEST, so
// nothing touches the disk) into a bounded Pipe, and an uploader thread
// streams them to the FTP server. Any error stops both (fail-fast) and the
// incomplete remote file is deleted.
class Transfer {
 public:
  Transfer(const TransferPlan& plan, FtpClient& ftp, PasswordSource& passwords, Logger& log, Progress& progress,
           size_t buffer_bytes);
  ~Transfer();
  Transfer(const Transfer&) = delete;
  Transfer& operator=(const Transfer&) = delete;

  void start();
  // Thread-safe; can be called from any thread (UI, signal watcher).
  void cancel();
  bool cancelling() const { return cancel_requested_; }
  bool finished() const { return running_ == 0; }
  // Joins the workers.
  TransferResult wait();

 private:
  void run_extractor();
  void extract();
  void run_uploader();
  void upload_loop();
  bool upload_file(size_t index, uint64_t number);
  bool await_file_end(const PlannedEntry& planned);
  void remove_partial(const PlannedEntry& planned, uint64_t bytes_sent);
  void fail(const std::string& message);
  std::string describe_rar_error(int code, const std::string& what) const;

  const TransferPlan& plan_;
  FtpClient& ftp_;
  PasswordSource& passwords_;
  Logger& log_;
  Progress& progress_;
  Pipe pipe_;

  std::thread extractor_;
  std::thread uploader_;
  std::atomic<int> running_{0};
  std::atomic<bool> cancel_requested_{false};
  std::chrono::steady_clock::time_point started_;
  std::chrono::steady_clock::time_point stopped_;

  mutable std::mutex mutex_;
  std::string error_;
  std::string missing_volume_;  // Written by the extractor only.
  uint64_t files_uploaded_ = 0;
  uint64_t bytes_uploaded_ = 0;
  bool timestamp_warning_shown_ = false;
};

}  // namespace rarftp
