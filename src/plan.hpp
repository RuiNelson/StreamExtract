#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rar_archive.hpp"

namespace rarftp {

class FtpClient;
class Logger;

// Supplies the archive password: the one given on the command line, or asks
// for it once when interactive.
class PasswordSource {
 public:
  PasswordSource(std::optional<std::string> password, std::string archive_name, bool interactive);
  std::optional<std::string> get();
  bool has_password() const { return password_.has_value(); }
  // After the interactive phase: never prompt again (worker threads).
  void disable_prompt() { interactive_ = false; }

 private:
  std::optional<std::string> password_;
  std::string archive_name_;
  bool interactive_;
};

struct ArchiveListing {
  std::vector<ArchiveEntry> entries;  // Archive order; continuation headers excluded.
  ArchiveFlags flags;
  unsigned volumes = 1;
};

// Reads every header of every volume. Throws RarError.
ArchiveListing list_archive(const std::string& path, PasswordSource& passwords, Logger& log);

struct PlannedEntry {
  enum class Action {
    Upload,
    Skip,  // Already on the server with the same size.
    MakeDir,
    Ignore,  // Links, file references, unusable names.
  };

  ArchiveEntry entry;
  std::string relative;  // Sanitized, '/'-separated.
  std::string remote;    // Absolute remote path.
  Action action = Action::Upload;
};

struct TransferPlan {
  std::string archive_path;
  std::string remote_root;
  bool solid = false;
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
