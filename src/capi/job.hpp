#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capi/streamextract.h"
#include "logger.hpp"
#include "plan.hpp"
#include "progress.hpp"
#include "transfer.hpp"

namespace streamextract {

struct JobConfig {
  std::string archive;
  std::string extraction_root;
  std::optional<std::string> archive_password;
  std::string host;
  int port = 21;
  int protocol = STREAMEXTRACT_PROTOCOL_FTP;  // Validated before the archive is opened.
  std::optional<std::string> ca_certificate;
  bool active_mode = false;
  std::string user;  // Empty: anonymous for FTP, local username for SFTP.
  std::string password;
  bool password_supplied = false;
  std::optional<std::string> private_key;
  std::string private_key_passphrase;
  std::optional<std::string> known_hosts;
  std::string directory;  // Empty: the login directory.
  bool mkdir = false;
  bool verbose = false;
  unsigned buffer_mib = 0;  // 0: the default.
  unsigned retries = 3;     // Total connection/login and per-file upload attempts, including the first.
  ByteUnits units = ByteUnits::Binary;
};

// One upload (what the CLI's run() does) on its own controller thread. The
// owner polls the state as JSON and answers the archive password prompt.
// Every member function may be called from any thread.
class Job {
 public:
  // Starts the controller thread.
  explicit Job(JobConfig config);
  // Cancels the job, wakes a pending prompt and waits for the thread.
  ~Job();
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  // Current state as JSON, with the log lines numbered `log_cursor` and later.
  std::string poll(uint64_t log_cursor);
  // std::nullopt declines a pending prompt. Ignored without one.
  void answer_password(std::optional<std::string> password);
  // Returns right away.
  void cancel();

 private:
  enum class Phase { Reading, Connecting, Checking, Transferring, Finished };

  struct ArchiveInfo {
    std::string name;
    ArchiveFormat format = ArchiveFormat::Rar;
    std::string compression;        // Compressed tar; empty otherwise.
    std::optional<uint64_t> files;  // Unknown for a streamed archive.
    std::optional<uint64_t> bytes;
    unsigned volumes = 1;
    bool solid = false;
    bool encrypted = false;
  };

  struct Result {
    TransferResult::Status status = TransferResult::Status::Failed;
    std::string error;
    uint64_t files_uploaded = 0;
    uint64_t bytes_uploaded = 0;
    double seconds = 0.0;
    uint64_t skipped_files = 0;
    uint64_t skipped_bytes = 0;
    uint64_t ignored = 0;
    std::vector<std::string> summary;
    std::vector<LogLine> problems;
    size_t problems_dropped = 0;
  };

  struct SequencedLine {
    uint64_t seq;
    LogLine line;
  };

  struct Cancelled {};  // Thrown inside the controller to unwind after a cancel request.

  // Controller thread.
  void run();
  Result pipeline();
  ArchiveListing read_archive(std::optional<PasswordSource>& passwords);
  Result failed_result(const std::string& error);
  Result cancelled_result();
  std::optional<std::string> ask_password(std::optional<std::string> error);
  void throw_if_cancelled() const;
  void set_phase(Phase phase);
  void finish(Result result);

  void add_log_line(const LogLine& line);

  const JobConfig config_;
  std::string login_user_;
  const std::string archive_name_;
  Logger log_;
  Progress progress_;

  // The Transfer only exists while it runs; cancel() reaches it through here.
  std::mutex transfer_mutex_;
  Transfer* transfer_ = nullptr;

  // Everything poll() reports. Never held while calling into log_ or transfer_.
  std::mutex state_mutex_;
  std::condition_variable state_changed_;
  std::atomic<bool> cancel_requested_{false};
  Phase phase_ = Phase::Reading;
  bool prompt_pending_ = false;
  std::optional<std::string> prompt_error_;
  bool answered_ = false;
  std::optional<std::string> answer_;
  std::optional<ArchiveInfo> archive_;
  std::optional<std::string> target_;
  std::optional<std::pair<size_t, size_t>> probe_;
  bool transfer_started_ = false;
  std::optional<Progress::Snapshot> final_progress_;
  std::optional<Result> result_;
  SpeedMeter upload_meter_;
  SpeedMeter unpack_meter_;
  SpeedMeter read_meter_;  // Position in a streamed archive.

  // The job's own copy of the log, numbered. Never held while calling into log_.
  std::mutex log_mutex_;
  std::deque<SequencedLine> lines_;
  uint64_t next_seq_ = 0;

  std::thread thread_;  // Last: started by the constructor.
};

}  // namespace streamextract
