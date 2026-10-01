#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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
  uint64_t skipped_files = 0;  // Already on the server with the same size.
  uint64_t skipped_bytes = 0;
  uint64_t ignored = 0;  // Links, special files, unusable names.
};

// Runs the pipeline: an extractor thread decompresses and verifies entries in
// memory (nothing touches the disk) into a bounded Pipe, and an uploader thread
// streams them to the FTP server. Any error stops both (fail-fast) and the
// incomplete remote file is deleted. With a streamed plan (compressed tar) the
// extractor plans each entry as it reads it, and the uploader checks the
// server before each file, discarding the data of those already there.
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
  void extract_streamed(ArchiveCallbacks& callbacks, const std::function<bool()>& flush);
  const PlannedEntry& entry(size_t index);
  void run_uploader();
  void upload_loop();
  bool upload_file(size_t index, uint64_t number);
  bool await_file_end(const PlannedEntry& planned);
  bool discard_file();
  void remove_partial(const PlannedEntry& planned, uint64_t bytes_sent);
  void fail(const std::string& message);
  std::string describe_archive_error(const ArchiveError& error, const std::string& what) const;

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

  // Streamed plans: the entries met so far (references stay valid as it grows)
  // and the remote checks (uploader only).
  std::mutex entries_mutex_;
  std::deque<PlannedEntry> streamed_;
  std::unique_ptr<RemoteProbe> probe_;
  const Archive* reading_ = nullptr;  // Extractor only: for the position in the archive.
  uint64_t files_uploaded_ = 0;
  uint64_t bytes_uploaded_ = 0;
  uint64_t skipped_files_ = 0;  // Streamed plans; otherwise the plan's counts.
  uint64_t skipped_bytes_ = 0;
  uint64_t ignored_ = 0;
  bool timestamp_warning_shown_ = false;
};

}  // namespace rarftp
