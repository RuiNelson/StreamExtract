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
struct RemoteFile;

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
    Skip,  // Same name and size on the server.
    MakeDir,
    Ignore,  // Links, file references, special files, unusable names.
  };

  ArchiveEntry entry;
  std::string relative;  // Sanitized, '/'-separated.
  std::string remote;    // Absolute remote path.
  Action action = Action::Upload;
  uint64_t resume_offset = 0;         // Discard this prefix locally, then append the remaining bytes.
  bool delete_before_upload = false;  // The remote file is larger than the archive entry.
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
  uint64_t upload_bytes = 0;  // Bytes still to send; existing prefixes are excluded.
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

// Plans skips, resumes and replacements, one file at a time: one existence
// check and one listing per directory, then SIZE only for
// the listed names and dotfiles (which some servers omit from NLST).
class RemoteProbe {
 public:
  RemoteProbe(FtpClient& ftp, std::string remote_root, Logger& log);
  void check(PlannedEntry& planned);

 private:
  struct Directory {
    bool exists = false;
    std::optional<std::unordered_set<std::string>> names;  // std::nullopt: no listing, ask for every file.
  };
  bool under_missing(std::string dir) const;
  RemoteFile stat(const PlannedEntry& planned);

  FtpClient& ftp_;
  std::string remote_root_;
  Logger& log_;
  std::set<std::string> missing_;
  std::map<std::string, Directory> directories_;
  // A successful transfer leaves each checked path at this size. Later copies
  // of the same name must use it, rather than the size before the first copy.
  std::unordered_map<std::string, uint64_t> expected_sizes_;
};

// Skips equal-size files, resumes smaller ones, and replaces larger ones.
// `on_progress(done, total)` is called after each checked file.
void probe_remote(TransferPlan& plan, FtpClient& ftp, Logger& log,
                  const std::function<void(size_t, size_t)>& on_progress);

}  // namespace rarftp
