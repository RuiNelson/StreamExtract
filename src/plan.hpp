#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "archive.hpp"

namespace rarftp {

class FtpClient;
class Logger;

// Supplies the archive password: the one given up front, or the answer to
// `prompt` (asked at most once, e.g. on the terminal or in a dialog).
class PasswordSource {
 public:
  using Prompt = std::function<std::optional<std::string>()>;

  // `prompt` may be empty: never ask.
  PasswordSource(std::optional<std::string> password, Prompt prompt);
  std::optional<std::string> get();
  bool has_password() const { return password_.has_value(); }
  // After the interactive phase: never prompt again (worker threads).
  void disable_prompt() { prompt_ = nullptr; }

 private:
  std::optional<std::string> password_;
  Prompt prompt_;
};

// The archive password is wrong; the caller may ask for another one.
class ArchivePasswordError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ArchiveListing {
  ArchiveFormat format = ArchiveFormat::Rar;
  std::vector<ArchiveEntry> entries;  // Archive order; continuation headers excluded.
  ArchiveFlags flags;
  unsigned volumes = 1;
};

// Reads every header of every volume. Throws std::runtime_error, or ArchivePasswordError when the password is
// wrong.
ArchiveListing list_archive(const std::string& path, PasswordSource& passwords, Logger& log);

struct PlannedEntry {
  enum class Action {
    Upload,
    Skip,  // Already on the server with the same size.
    MakeDir,
    Ignore,  // Links, file references, special files, unusable names.
  };

  ArchiveEntry entry;
  std::string relative;  // Sanitized, '/'-separated.
  std::string remote;    // Absolute remote path.
  Action action = Action::Upload;
};

struct TransferPlan {
  std::string archive_path;
  std::string remote_root;
  bool skip_decompresses = false;  // See ArchiveFlags.
  std::vector<PlannedEntry> entries;  // Same order as ArchiveListing::entries.

  uint64_t upload_files = 0;
  uint64_t upload_bytes = 0;
  uint64_t skip_files = 0;
  uint64_t skip_bytes = 0;
  uint64_t ignored = 0;

  void recount();
};

// Maps archive entries to remote paths and logs anything that will not be
// uploaded as-is (links, sanitized names, duplicates, case collisions).
TransferPlan build_plan(const std::string& archive_path, const ArchiveListing& listing,
                        const std::string& remote_root, Logger& log);

// Marks files already present on the server with the same size as Skip.
// `on_progress(done, total)` is called after each checked file.
void probe_remote(TransferPlan& plan, FtpClient& ftp, Logger& log,
                  const std::function<void(size_t, size_t)>& on_progress);

}  // namespace rarftp
