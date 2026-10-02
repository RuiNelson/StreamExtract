#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "archive.hpp"
#include "util/text.hpp"

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

// "ZIP, 8 file(s), 13.3 MiB, 3 volumes, encrypted"; "tar (gzip), read as it is
// uploaded" for a stream_only archive, whose contents are not known yet.
std::string describe_archive(const ArchiveListing& listing, bool encrypted, ByteUnits units = ByteUnits::Binary);

// Reads every header of every volume; for a stream_only archive (compressed tar) only opens it, leaving the
// entries empty. Throws std::runtime_error, or ArchivePasswordError when the password is wrong.
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
  ArchiveFormat format = ArchiveFormat::Rar;
  bool skip_decompresses = false;  // See ArchiveFlags.
  // A stream_only archive: no entries here; the transfer plans each one as it
  // reads it and checks the server just before uploading it.
  bool streamed = false;
  std::vector<PlannedEntry> entries;  // Same order as ArchiveListing::entries.

  uint64_t upload_files = 0;
  uint64_t upload_bytes = 0;
  uint64_t skip_files = 0;
  uint64_t skip_bytes = 0;
  uint64_t ignored = 0;

  void recount();
};

// Maps archive entries to remote paths, one at a time, and logs anything that
// will not be uploaded as-is (links, sanitized names, duplicates, case
// collisions).
class Planner {
 public:
  Planner(ArchiveFormat format, std::string remote_root, Logger& log);
  PlannedEntry plan(const ArchiveEntry& entry);

 private:
  std::string remote_root_;
  Logger& log_;
  bool backslash_separators_;
  std::unordered_set<std::string> files_seen_;
  std::unordered_map<std::string, std::string> folded_;  // Lower-cased path -> first spelling.
};

// The plan of a listed archive (Planner over every entry). For a stream_only
// one, an empty plan marked `streamed`.
TransferPlan build_plan(const std::string& archive_path, const ArchiveListing& listing,
                        const std::string& remote_root, Logger& log);

// Tells whether files are already on the server with a given size, one at a
// time: one existence check and one listing per directory, then SIZE only for
// the names that are there.
class RemoteProbe {
 public:
  RemoteProbe(FtpClient& ftp, std::string remote_root, Logger& log);
  bool same_size(const PlannedEntry& planned);

 private:
  struct Directory {
    bool exists = false;
    std::optional<std::unordered_set<std::string>> names;  // std::nullopt: no listing, ask for every file.
  };
  bool under_missing(std::string dir) const;

  FtpClient& ftp_;
  std::string remote_root_;
  Logger& log_;
  std::set<std::string> missing_;
  std::map<std::string, Directory> directories_;
};

// Marks files already present on the server with the same size as Skip.
// `on_progress(done, total)` is called after each checked file.
void probe_remote(TransferPlan& plan, FtpClient& ftp, Logger& log,
                  const std::function<void(size_t, size_t)>& on_progress);

}  // namespace rarftp
