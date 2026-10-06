#include "plan.hpp"

#include <map>
#include <memory>
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

namespace streamextract {

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

  ArchiveCallbacks callbacks;
  callbacks.on_password = [&] { return passwords.get(); };
  callbacks.on_volume = [&](const std::string& volume) {
    ++listing.volumes;
    log.debug(fmt::format("next volume: {}", volume));
  };
  callbacks.on_missing_volume = [&](const std::string& volume) { missing_volume = volume; };
  callbacks.on_large_dictionary = [&](uint64_t dictionary, uint64_t limit) {
    log.warn(
        "the archive uses a {} dictionary (UnRAR's default limit is {}); decompression needs that much memory",
        format_bytes(dictionary, log.units()), format_bytes(limit, log.units()));
    return true;
  };

  try {
    const std::unique_ptr<Archive> archive = open_archive(path, Archive::Mode::List, callbacks);
    listing.format = archive->format();
    listing.flags = archive->flags();
    if (listing.flags.volume && !listing.flags.first_volume) {
      throw std::runtime_error(fmt::format(
          "{} is not the first volume of the set; pass the first one (e.g. .part1.rar or .rar)", path));
    }
    if (listing.flags.volume_count > 1) {
      listing.volumes = listing.flags.volume_count;
    }
    if (!listing.flags.checksums) {
      log.warn("{} {} have no checksum of the file contents: files are uploaded without verification",
               format_name(listing.format),
               listing.format == ArchiveFormat::Exfat || listing.format == ArchiveFormat::Pfs ||
                       listing.format == ArchiveFormat::Ufs
                   ? "images"
                   : "archives");
    }
    if (listing.flags.stream_only) {
      return listing;  // Listing would decompress it all: the transfer plans each file as it reads it.
    }
    ArchiveEntry entry;
    while (archive->next(entry)) {
      listing.entries.push_back(entry);
      archive->skip();
    }
  } catch (const ArchiveError& error) {
    if (!missing_volume.empty()) {
      throw std::runtime_error(fmt::format("volume not found: {}", missing_volume));
    }
    switch (error.kind()) {
      case ArchiveError::Kind::BadPassword:
        throw ArchivePasswordError("wrong archive password");
      case ArchiveError::Kind::MissingPassword:
        throw std::runtime_error("the archive is encrypted: pass --archive-password");
      case ArchiveError::Kind::Other:
        break;
    }
    throw std::runtime_error(fmt::format("cannot read {}: {}", path, error.what()));
  }
  return listing;
}

std::string describe_archive(const ArchiveListing& listing, bool encrypted, ByteUnits units) {
  std::string text = format_name(listing.format);
  if (!listing.flags.compression.empty()) {
    text += fmt::format(" ({})", listing.flags.compression);
  }
  if (listing.flags.stream_only) {
    text += ", read as it is uploaded: each file is checked against the server when it is reached";
  } else {
    uint64_t files = 0;
    uint64_t bytes = 0;
    for (const auto& entry : listing.entries) {
      if (entry.kind == EntryKind::File) {
        ++files;
        bytes += entry.size;
      }
    }
    text += fmt::format(", {} file(s), {}", files, format_bytes(bytes, units));
  }
  if (listing.volumes > 1) {
    text += fmt::format(", {} volumes", listing.volumes);
  }
  if (listing.flags.solid) {
    text += ", solid";
  }
  if (encrypted) {
    text += ", encrypted";
  }
  return text;
}

void TransferPlan::recount() {
  upload_files = upload_bytes = skip_files = skip_bytes = ignored = 0;
  for (const auto& e : entries) {
    switch (e.action) {
      case PlannedEntry::Action::Upload:
        ++upload_files;
        upload_bytes += e.entry.size - e.resume_offset;
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

Planner::Planner(ArchiveFormat format, std::string remote_root, Logger& log)
    : remote_root_(std::move(remote_root)),
      log_(log),
      // UnRAR gives native separators; the others use '/', but some Windows tools write '\'.
      backslash_separators_(kNativeWindowsPaths || format != ArchiveFormat::Rar) {}

PlannedEntry Planner::plan(const ArchiveEntry& entry) {
  PlannedEntry planned;
  planned.entry = entry;

  const SanitizedPath safe = sanitize_archive_path(entry.name, backslash_separators_);
  if (safe.path.empty()) {
    planned.action = PlannedEntry::Action::Ignore;
    if (entry.kind != EntryKind::Directory) {  // A directory entry for the root itself is harmless.
      log_.warn("skipping entry with an unusable name: \"{}\"", entry.name);
    }
    return planned;
  }
  if (safe.traversal) {
    log_.warn("\"{}\" points outside the destination directory; using \"{}\"", entry.name, safe.path);
  }
  if (safe.control_chars) {
    log_.warn("\"{}\" contains control characters; using \"{}\"", entry.name, safe.path);
  }
  planned.relative = safe.path;
  planned.remote = join_remote_path(remote_root_, safe.path);

  switch (entry.kind) {
    case EntryKind::File:
      planned.action = PlannedEntry::Action::Upload;
      break;
    case EntryKind::Directory:
      planned.action = PlannedEntry::Action::MakeDir;
      break;
    case EntryKind::Symlink:
      planned.action = PlannedEntry::Action::Ignore;
      log_.warn("skipping symbolic link \"{}\": FTP cannot create links", planned.relative);
      break;
    case EntryKind::Hardlink:
      planned.action = PlannedEntry::Action::Ignore;
      log_.warn("skipping hard link \"{}\": FTP cannot create links", planned.relative);
      break;
    case EntryKind::FileCopy:
      planned.action = PlannedEntry::Action::Ignore;
      log_.warn("skipping \"{}\": stored as a reference to an identical file (rar -oi), not supported yet",
                planned.relative);
      break;
    case EntryKind::Special:
      planned.action = PlannedEntry::Action::Ignore;
      log_.warn("skipping special file \"{}\" (device, FIFO or socket)", planned.relative);
      break;
  }

  if (planned.action == PlannedEntry::Action::Upload && !files_seen_.insert(planned.relative).second) {
    log_.warn("\"{}\" appears more than once in the archive; copies are processed in archive order",
              planned.relative);
  }
  if (planned.action != PlannedEntry::Action::Ignore) {
    const auto [it, inserted] = folded_.emplace(ascii_lower(planned.relative), planned.relative);
    if (!inserted && it->second != planned.relative) {
      log_.warn("\"{}\" and \"{}\" differ only in letter case: they collide on case-insensitive servers",
                it->second, planned.relative);
    }
  }
  return planned;
}

TransferPlan build_plan(const std::string& archive_path, const ArchiveListing& listing,
                        const std::string& remote_root, Logger& log) {
  TransferPlan plan;
  plan.archive_path = archive_path;
  plan.remote_root = remote_root;
  plan.format = listing.format;
  plan.skip_decompresses = listing.flags.skip_decompresses;
  plan.streamed = listing.flags.stream_only;
  plan.entries.reserve(listing.entries.size());
  Planner planner(listing.format, remote_root, log);
  for (const auto& entry : listing.entries) {
    plan.entries.push_back(planner.plan(entry));
  }
  plan.recount();
  return plan;
}

RemoteProbe::RemoteProbe(FtpClient& ftp, std::string remote_root, Logger& log)
    : ftp_(ftp), remote_root_(std::move(remote_root)), log_(log) {}

bool RemoteProbe::under_missing(std::string dir) const {
  while (true) {
    if (missing_.count(dir) != 0) {
      return true;
    }
    if (dir == "/") {
      return false;
    }
    dir = remote_parent(dir);
  }
}

RemoteFile RemoteProbe::stat(const PlannedEntry& planned) {
  const std::string dir = remote_parent(planned.remote);
  auto it = directories_.find(dir);
  if (it == directories_.end()) {
    Directory directory;
    directory.exists = dir == remote_root_ || (!under_missing(dir) && ftp_.directory_exists(dir));
    if (!directory.exists) {
      missing_.insert(dir);
    } else if (const auto names = ftp_.list_names(dir)) {
      // If the listing fails (some servers refuse NLST on empty directories),
      // every file is asked for.
      directory.names.emplace(names->begin(), names->end());
    }
    it = directories_.emplace(dir, std::move(directory)).first;
  }
  const Directory& directory = it->second;
  if (!directory.exists) {
    return {};
  }
  const std::string name = remote_basename(planned.remote);
  // NLST can omit dotfiles even though SIZE can query them. Keep the listing
  // shortcut for ordinary names, but ask for hidden files individually.
  const bool hidden = !name.empty() && name.front() == '.';
  if (directory.names && !hidden && directory.names->count(name) == 0) {
    return {};
  }
  return ftp_.stat_file(planned.remote);
}

void RemoteProbe::check(PlannedEntry& planned) {
  planned.action = PlannedEntry::Action::Upload;
  planned.resume_offset = 0;
  planned.delete_before_upload = false;
  const auto previous = expected_sizes_.find(planned.remote);
  const RemoteFile remote = previous == expected_sizes_.end() ? stat(planned) : RemoteFile{true, previous->second};
  expected_sizes_[planned.remote] = planned.entry.size;
  if (!remote.exists) {
    return;
  }
  if (remote.size && *remote.size == planned.entry.size) {
    planned.action = PlannedEntry::Action::Skip;
    log_.debug(fmt::format("already on the server: {}", planned.relative));
  } else if (remote.size && *remote.size < planned.entry.size) {
    planned.resume_offset = *remote.size;
    log_.debug(
        fmt::format("incomplete on the server, will resume at byte {}: {}", *remote.size, planned.relative));
  } else if (remote.size) {
    planned.delete_before_upload = true;
    log_.debug(fmt::format("larger on the server, will delete before uploading: {}", planned.relative));
  } else {
    log_.debug(fmt::format("remote size unknown, will overwrite: {}", planned.relative));
  }
}

void probe_remote(TransferPlan& plan, FtpClient& ftp, Logger& log,
                  const std::function<void(size_t, size_t)>& on_progress) {
  // Directory by directory (parents sort before children), so that a missing
  // directory spares checking anything below it.
  std::map<std::string, std::vector<size_t>> by_dir;
  size_t total = 0;
  for (size_t i = 0; i < plan.entries.size(); ++i) {
    if (plan.entries[i].action == PlannedEntry::Action::Upload) {
      by_dir[remote_parent(plan.entries[i].remote)].push_back(i);
      ++total;
    }
  }
  RemoteProbe probe(ftp, plan.remote_root, log);
  size_t done = 0;
  for (const auto& [dir, indices] : by_dir) {
    for (const size_t i : indices) {
      probe.check(plan.entries[i]);
      on_progress(++done, total);
    }
  }
  plan.recount();
}

}  // namespace streamextract
