#include "plan.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "ftp_client.hpp"
#include "logger.hpp"
#include "util/remote_path.hpp"
#include "util/text.hpp"

namespace rarftp {

namespace {

std::string ascii_lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

}  // namespace

PasswordSource::PasswordSource(std::optional<std::string> password, Prompt prompt)
    : password_(std::move(password)), prompt_(std::move(prompt)) {}

std::optional<std::string> PasswordSource::get() {
  if (password_ || !prompt_) {
    return password_;
  }
  const Prompt prompt = std::move(prompt_);
  prompt_ = nullptr;  // Ask only once.
  password_ = prompt();
  return password_;
}

ArchiveListing list_archive(const std::string& path, PasswordSource& passwords, Logger& log) {
  ArchiveListing listing;
  std::string missing_volume;

  RarCallbacks callbacks;
  callbacks.on_password = [&] { return passwords.get(); };
  callbacks.on_volume = [&](const std::string& volume) {
    ++listing.volumes;
    log.debug(fmt::format("next volume: {}", volume));
  };
  callbacks.on_missing_volume = [&](const std::string& volume) { missing_volume = volume; };
  callbacks.on_large_dictionary = [&](uint64_t dictionary, uint64_t limit) {
    log.warn(
        "the archive uses a {} dictionary (UnRAR's default limit is {}); decompression needs that much memory",
        format_bytes(dictionary), format_bytes(limit));
    return true;
  };

  try {
    RarArchive archive(path, RarArchive::Mode::List, callbacks);
    listing.flags = archive.flags();
    if (listing.flags.volume && !listing.flags.first_volume) {
      throw std::runtime_error(fmt::format(
          "{} is not the first volume of the set; pass the first one (e.g. .part1.rar or .rar)", path));
    }
    ArchiveEntry entry;
    while (archive.next(entry)) {
      listing.entries.push_back(entry);
      const int code = archive.skip();
      if (code != 0) {
        throw RarError(code, rar_error_message(code));
      }
    }
  } catch (const RarError& error) {
    if (!missing_volume.empty()) {
      throw std::runtime_error(fmt::format("volume not found: {}", missing_volume));
    }
    if (rar_is_bad_password(error.code())) {
      throw ArchivePasswordError("wrong archive password");
    }
    if (rar_is_missing_password(error.code())) {
      throw std::runtime_error("the archive is encrypted: pass --rar-password");
    }
    throw std::runtime_error(fmt::format("cannot read {}: {}", path, error.what()));
  }
  return listing;
}

void TransferPlan::recount() {
  upload_files = upload_bytes = skip_files = skip_bytes = ignored = 0;
  for (const auto& e : entries) {
    switch (e.action) {
      case PlannedEntry::Action::Upload:
        ++upload_files;
        upload_bytes += e.entry.size;
        break;
      case PlannedEntry::Action::Skip:
        ++skip_files;
        skip_bytes += e.entry.size;
        break;
      case PlannedEntry::Action::Ignore:
        ++ignored;
        break;
      case PlannedEntry::Action::MakeDir:
        break;
    }
  }
}

TransferPlan build_plan(const std::string& archive_path, const ArchiveListing& listing,
                        const std::string& remote_root, Logger& log) {
  TransferPlan plan;
  plan.archive_path = archive_path;
  plan.remote_root = remote_root;
  plan.solid = listing.flags.solid;
  plan.entries.reserve(listing.entries.size());

  std::unordered_set<std::string> files_seen;
  std::unordered_map<std::string, std::string> folded;  // Lower-cased path -> first spelling.

  for (const auto& entry : listing.entries) {
    PlannedEntry planned;
    planned.entry = entry;

    const SanitizedPath safe = sanitize_archive_path(entry.name, kNativeWindowsPaths);
    if (safe.path.empty()) {
      planned.action = PlannedEntry::Action::Ignore;
      if (entry.kind != EntryKind::Directory) {  // A directory entry for the root itself is harmless.
        log.warn("skipping entry with an unusable name: \"{}\"", entry.name);
      }
      plan.entries.push_back(std::move(planned));
      continue;
    }
    if (safe.traversal) {
      log.warn("\"{}\" points outside the destination directory; using \"{}\"", entry.name, safe.path);
    }
    if (safe.control_chars) {
      log.warn("\"{}\" contains control characters; using \"{}\"", entry.name, safe.path);
    }
    planned.relative = safe.path;
    planned.remote = join_remote_path(remote_root, safe.path);

    switch (entry.kind) {
      case EntryKind::File:
        planned.action = PlannedEntry::Action::Upload;
        break;
      case EntryKind::Directory:
        planned.action = PlannedEntry::Action::MakeDir;
        break;
      case EntryKind::Symlink:
        planned.action = PlannedEntry::Action::Ignore;
        log.warn("skipping symbolic link \"{}\": FTP cannot create links", planned.relative);
        break;
      case EntryKind::Hardlink:
        planned.action = PlannedEntry::Action::Ignore;
        log.warn("skipping hard link \"{}\": FTP cannot create links", planned.relative);
        break;
      case EntryKind::FileCopy:
        planned.action = PlannedEntry::Action::Ignore;
        log.warn("skipping \"{}\": stored as a reference to an identical file (rar -oi), not supported yet",
                 planned.relative);
        break;
    }

    if (planned.action == PlannedEntry::Action::Upload && !files_seen.insert(planned.relative).second) {
      log.warn("\"{}\" appears more than once in the archive; the last copy wins", planned.relative);
    }
    if (planned.action != PlannedEntry::Action::Ignore) {
      const auto [it, inserted] = folded.emplace(ascii_lower(planned.relative), planned.relative);
      if (!inserted && it->second != planned.relative) {
        log.warn("\"{}\" and \"{}\" differ only in letter case: they collide on case-insensitive servers",
                 it->second, planned.relative);
      }
    }
    plan.entries.push_back(std::move(planned));
  }
  plan.recount();
  return plan;
}

void probe_remote(TransferPlan& plan, FtpClient& ftp, Logger& log,
                  const std::function<void(size_t, size_t)>& on_progress) {
  // Files to check, grouped by remote directory (parents sort before children).
  std::map<std::string, std::vector<size_t>> by_dir;
  size_t total = 0;
  for (size_t i = 0; i < plan.entries.size(); ++i) {
    if (plan.entries[i].action == PlannedEntry::Action::Upload) {
      by_dir[remote_parent(plan.entries[i].remote)].push_back(i);
      ++total;
    }
  }

  std::set<std::string> missing;
  const auto under_missing = [&](std::string dir) {
    while (true) {
      if (missing.count(dir) != 0) {
        return true;
      }
      if (dir == "/") {
        return false;
      }
      dir = remote_parent(dir);
    }
  };

  size_t done = 0;
  for (const auto& [dir, indices] : by_dir) {
    bool exists = true;
    if (dir != plan.remote_root) {
      exists = !under_missing(dir) && ftp.directory_exists(dir);
    }
    if (!exists) {
      missing.insert(dir);
      done += indices.size();
      on_progress(done, total);
      continue;
    }

    // One listing per directory, then SIZE only for names that are there. If
    // the listing fails (some servers refuse NLST on empty directories), ask
    // for every file.
    const auto names = ftp.list_names(dir);
    std::unordered_set<std::string> present;
    if (names) {
      present.insert(names->begin(), names->end());
    }
    for (const size_t i : indices) {
      PlannedEntry& planned = plan.entries[i];
      if (!names || present.count(remote_basename(planned.remote)) != 0) {
        const RemoteFile remote = ftp.stat_file(planned.remote);
        if (remote.exists && remote.size && *remote.size == planned.entry.size) {
          planned.action = PlannedEntry::Action::Skip;
          log.debug(fmt::format("already on the server: {}", planned.relative));
        } else if (remote.exists && !remote.size) {
          log.debug(fmt::format("remote size unknown, will overwrite: {}", planned.relative));
        }
      }
      on_progress(++done, total);
    }
  }
  plan.recount();
}

}  // namespace rarftp
