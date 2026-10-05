#include "transfer.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "archive.hpp"
#include "ftp_client.hpp"
#include "logger.hpp"
#include "progress.hpp"
#include "util/text.hpp"

namespace streamextract {

namespace {

constexpr size_t kBlockSize = 1 << 20;
constexpr size_t kMinBlockCapacity = 4096;

std::string file_name_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

Transfer::Transfer(const TransferPlan& plan, FtpClient& ftp, PasswordSource& passwords, Logger& log,
                   Progress& progress, size_t buffer_bytes, unsigned attempts)
    : plan_(plan),
      ftp_(ftp),
      passwords_(passwords),
      log_(log),
      progress_(progress),
      pipe_(buffer_bytes),
      attempts_(std::max(1u, attempts)) {
  progress_.set_buffer_capacity(buffer_bytes);
}

Transfer::~Transfer() {
  if (extractor_.joinable() || uploader_.joinable()) {
    cancel();
    wait();
  }
}

void Transfer::start() {
  // Directory creation and streamed probes also wait for FTP replies.
  ftp_.set_cancel_check([this] { return cancel_requested_.load(); });
  progress_.set_totals(plan_.upload_files, plan_.upload_bytes, plan_.skip_files, plan_.skip_bytes);
  if (plan_.streamed) {
    progress_.set_streamed();
  }
  started_ = std::chrono::steady_clock::now();
  running_ = 2;
  extractor_ = std::thread([this] { run_extractor(); });
  uploader_ = std::thread([this] { run_uploader(); });
}

void Transfer::cancel() {
  if (cancel_requested_.exchange(true)) {
    return;
  }
  log_.info("Cancelling...");
  progress_.set_activity("Cancelling...");
  pipe_.abort();
  std::lock_guard lock(reply_mutex_);
  reply_ready_.notify_all();
}

TransferResult Transfer::wait() {
  if (extractor_.joinable()) {
    extractor_.join();
  }
  if (uploader_.joinable()) {
    uploader_.join();
  }
  ftp_.set_cancel_check(nullptr);  // The callback must not outlive this Transfer.
  std::lock_guard lock(mutex_);
  TransferResult result;
  result.files_uploaded = files_uploaded_;
  result.bytes_uploaded = bytes_uploaded_;
  result.seconds = std::chrono::duration<double>(stopped_ - started_).count();
  result.skipped_files = plan_.streamed ? skipped_files_ : plan_.skip_files;
  result.skipped_bytes = plan_.streamed ? skipped_bytes_ : plan_.skip_bytes;
  result.ignored = plan_.streamed ? ignored_ : plan_.ignored;
  if (!error_.empty()) {
    result.status = TransferResult::Status::Failed;
    result.error = error_;
  } else if (cancel_requested_) {
    result.status = TransferResult::Status::Cancelled;
  }
  return result;
}

void Transfer::fail(const std::string& message) {
  bool first = false;
  {
    std::lock_guard lock(mutex_);
    // Errors caused by a cancellation or by an earlier error are noise.
    if (error_.empty() && !cancel_requested_) {
      error_ = message;
      first = true;
    }
  }
  if (first) {
    log_.error(message);
  }
  pipe_.abort();
  std::lock_guard lock(reply_mutex_);
  reply_ready_.notify_all();
}

std::string Transfer::describe_archive_error(const ArchiveError& error, const std::string& what) const {
  if (!missing_volume_.empty()) {
    return fmt::format("volume not found: {}", missing_volume_);
  }
  switch (error.kind()) {
    case ArchiveError::Kind::BadPassword:
      return fmt::format("{}: wrong password", what);
    case ArchiveError::Kind::MissingPassword:
      return fmt::format("{} is encrypted: pass --archive-password", what);
    case ArchiveError::Kind::Other:
      break;
  }
  return fmt::format("{}: {}", what, error.what());
}

// ---------------------------------------------------------------------------
// Extractor thread

void Transfer::run_extractor() {
  try {
    extract();
  } catch (const ArchiveError& error) {
    fail(describe_archive_error(error, "cannot read the archive"));
  } catch (const std::exception& error) {
    fail(fmt::format("cannot read the archive: {}", error.what()));
  }
  progress_.set_activity("");
  {
    std::lock_guard lock(mutex_);
    stopped_ = std::chrono::steady_clock::now();
  }
  --running_;
}

void Transfer::extract() {
  std::vector<uint8_t> block;
  bool discard = false;  // Decompressing a skipped file (solid archive).

  const auto flush = [&]() -> bool {
    if (block.empty()) {
      return true;
    }
    PipeMessage message;
    message.kind = PipeMessage::Kind::Data;
    message.data = std::move(block);
    block = std::vector<uint8_t>();
    const bool ok = pipe_.push(std::move(message));
    progress_.set_buffer_used(pipe_.buffered());
    return ok;
  };

  ArchiveCallbacks callbacks;
  callbacks.on_data = [&](const uint8_t* data, size_t size) -> bool {
    if (cancel_requested_ || pipe_.aborted()) {
      return false;
    }
    progress_.add_unpacked(size);
    if (reading_ != nullptr) {
      progress_.set_archive_read(reading_->bytes_read());
    }
    if (discard) {
      return true;
    }
    while (size > 0) {
      if (block.capacity() == 0) {
        // A tiny file must not retain a 1 MiB allocation while it waits for
        // FTP. The queue bounds payload bytes, so thousands of small files
        // can be waiting at once. Size from the file header, not this callback,
        // keeps large files from repeatedly growing buffers for small chunks.
        size_t capacity = kMinBlockCapacity;
        while (capacity < unpacking_size_ && capacity < kBlockSize) {
          capacity *= 2;
        }
        block = pipe_.acquire_buffer(capacity);
      }
      const size_t take = std::min(size, kBlockSize - block.size());
      block.insert(block.end(), data, data + take);
      data += take;
      size -= take;
      if (block.size() >= kBlockSize && !flush()) {
        return false;
      }
    }
    return true;
  };
  callbacks.on_password = [&] { return passwords_.get(); };
  callbacks.on_volume = [&](const std::string& volume) { log_.info("Reading volume {}", file_name_of(volume)); };
  callbacks.on_missing_volume = [&](const std::string& volume) { missing_volume_ = volume; };
  callbacks.on_large_dictionary = [](uint64_t, uint64_t) { return true; };  // Warned while listing.

  if (plan_.streamed) {
    extract_streamed(callbacks, flush);
    return;
  }

  // Nothing after the last file to upload needs decompressing: stop reading
  // there (skipped files of a solid archive would otherwise be unpacked for
  // nothing) and only create the remaining directories.
  size_t end_of_uploads = 0;
  for (size_t i = 0; i < plan_.entries.size(); ++i) {
    if (plan_.entries[i].action == PlannedEntry::Action::Upload) {
      end_of_uploads = i + 1;
    }
  }

  size_t index = 0;
  if (end_of_uploads > 0) {
    const std::unique_ptr<Archive> archive = open_archive(plan_.archive_path, Archive::Mode::Extract, callbacks);
    ArchiveEntry entry;
    while (index < end_of_uploads && archive->next(entry)) {
      if (cancel_requested_ || pipe_.aborted()) {
        return;
      }
      if (entry.split_before) {  // Continuation of a split file handled earlier.
        try {
          archive->skip();
        } catch (const ArchiveError& error) {
          fail(describe_archive_error(error, entry.name));
          return;
        }
        continue;
      }
      if (index >= plan_.entries.size() || plan_.entries[index].entry.name != entry.name) {
        fail("the archive changed while it was being read");
        return;
      }
      const size_t current = index++;
      const PlannedEntry& planned = plan_.entries[current];

      try {
        switch (planned.action) {
          case PlannedEntry::Action::MakeDir: {
            PipeMessage message;
            message.kind = PipeMessage::Kind::EnsureDir;
            message.entry = current;
            if (!pipe_.push(std::move(message))) {
              return;
            }
            archive->skip();
            break;
          }
          case PlannedEntry::Action::Upload: {
            unpacking_size_ = planned.entry.size;
            progress_.set_activity(fmt::format("Unpacking {}", planned.relative));
            PipeMessage begin;
            begin.kind = PipeMessage::Kind::FileBegin;
            begin.entry = current;
            if (!pipe_.push(std::move(begin))) {
              return;
            }
            archive->test();
            if (!flush()) {
              return;
            }
            if (!finish_extraction(current, callbacks, flush)) {
              return;
            }
            break;
          }
          case PlannedEntry::Action::Skip:
            if (plan_.skip_decompresses) {
              // It has to be decompressed anyway. test() (instead of skip())
              // keeps cancellation responsive.
              progress_.set_activity(
                  fmt::format("Skipping {} (already on the server), decompressing it to reach the next files",
                              planned.relative));
              discard = true;
              archive->test();
              discard = false;
            } else {
              archive->skip();
            }
            break;
          case PlannedEntry::Action::Ignore:
            archive->skip();
            break;
        }
      } catch (const ArchiveError& error) {
        if (archive->aborted_by_callback() && (cancel_requested_ || pipe_.aborted())) {
          return;  // Stopped on purpose.
        }
        fail(describe_archive_error(error, planned.relative));
        return;
      }
    }
    if (index < end_of_uploads) {
      fail("the archive ended before all its files were read");
      return;
    }
  }

  for (; index < plan_.entries.size(); ++index) {
    if (plan_.entries[index].action == PlannedEntry::Action::MakeDir) {
      PipeMessage message;
      message.kind = PipeMessage::Kind::EnsureDir;
      message.entry = index;
      if (!pipe_.push(std::move(message))) {
        return;
      }
    }
  }
  PipeMessage end;
  end.kind = PipeMessage::Kind::End;
  pipe_.push(std::move(end));
}

const PlannedEntry& Transfer::entry(size_t index) {
  if (!plan_.streamed) {
    return plan_.entries[index];
  }
  std::lock_guard lock(entries_mutex_);
  return streamed_[index];  // A deque: the reference survives later push_back()s.
}

void Transfer::extract_streamed(ArchiveCallbacks& callbacks, const std::function<bool()>& flush) {
  const std::unique_ptr<Archive> archive = open_archive(plan_.archive_path, Archive::Mode::Extract, callbacks);
  progress_.set_archive_size(archive->flags().size);
  reading_ = archive.get();
  struct Unset {
    const Archive*& reading;
    ~Unset() { reading = nullptr; }
  } unset{reading_};

  Planner planner(plan_.format, plan_.remote_root, log_);
  ArchiveEntry header;
  while (archive->next(header)) {
    if (cancel_requested_ || pipe_.aborted()) {
      return;
    }
    size_t index = 0;
    {
      std::lock_guard lock(entries_mutex_);
      streamed_.push_back(planner.plan(header));
      index = streamed_.size() - 1;
    }
    const PlannedEntry& planned = entry(index);
    try {
      switch (planned.action) {
        case PlannedEntry::Action::MakeDir: {
          PipeMessage message;
          message.kind = PipeMessage::Kind::EnsureDir;
          message.entry = index;
          if (!pipe_.push(std::move(message))) {
            return;
          }
          archive->skip();
          break;
        }
        case PlannedEntry::Action::Upload: {
          unpacking_size_ = planned.entry.size;
          // The uploader checks the server first, and drops the data of a file
          // that is already there: it has to be decompressed anyway.
          progress_.set_activity(fmt::format("Unpacking {}", planned.relative));
          PipeMessage begin;
          begin.kind = PipeMessage::Kind::FileBegin;
          begin.entry = index;
          if (!pipe_.push(std::move(begin))) {
            return;
          }
          archive->test();
          if (!flush()) {
            return;
          }
          if (!finish_extraction(index, callbacks, flush)) {
            return;
          }
          break;
        }
        case PlannedEntry::Action::Skip:  // Not decided here.
        case PlannedEntry::Action::Ignore: {
          {
            std::lock_guard lock(mutex_);
            ++ignored_;
          }
          archive->skip();
          break;
        }
      }
    } catch (const ArchiveError& error) {
      if (archive->aborted_by_callback() && (cancel_requested_ || pipe_.aborted())) {
        return;  // Stopped on purpose.
      }
      fail(describe_archive_error(error, planned.relative));
      return;
    }
    progress_.set_archive_read(archive->bytes_read());
  }
  PipeMessage end;
  end.kind = PipeMessage::Kind::End;
  pipe_.push(std::move(end));
}

bool Transfer::finish_extraction(size_t index, ArchiveCallbacks& callbacks, const std::function<bool()>& flush) {
  while (true) {
    PipeMessage end;
    end.kind = PipeMessage::Kind::FileEnd;
    end.entry = index;
    if (!pipe_.push(std::move(end))) {
      return false;
    }
    std::unique_lock lock(reply_mutex_);
    reply_ready_.wait(lock, [&] { return file_reply_ != FileReply::Waiting || pipe_.aborted(); });
    if (pipe_.aborted()) {
      return false;
    }
    const bool retry = file_reply_ == FileReply::Retry;
    file_reply_ = FileReply::Waiting;
    lock.unlock();
    if (!retry) {
      return true;
    }
    if (!reread_file(index, callbacks) || !flush()) {
      return false;
    }
  }
}

bool Transfer::reread_file(size_t index, ArchiveCallbacks callbacks) {
  bool target = false;
  const auto on_data = callbacks.on_data;
  callbacks.on_data = [&](const uint8_t* data, size_t size) {
    if (cancel_requested_ || pipe_.aborted()) {
      return false;
    }
    if (!target) {
      progress_.add_unpacked(size);
      return true;
    }
    return on_data(data, size);
  };
  // Rereading compressed tar must not move its archive-level progress backwards
  // or inflate it; the original reader resumes after this attempt.
  const Archive* original = reading_;
  reading_ = nullptr;
  struct Restore {
    const Archive*& reading;
    const Archive* original;
    ~Restore() { reading = original; }
  } restore{reading_, original};
  progress_.set_activity(fmt::format("Rereading {} for retry", entry(index).relative));
  const auto archive = open_archive(plan_.archive_path, Archive::Mode::Extract, std::move(callbacks));
  ArchiveEntry header;
  size_t current = 0;
  while (archive->next(header)) {
    if (cancel_requested_ || pipe_.aborted()) {
      return false;
    }
    if (header.split_before) {
      archive->skip();
      continue;
    }
    const ArchiveEntry& expected = entry(current).entry;
    if (header.name != expected.name || header.size != expected.size || header.kind != expected.kind) {
      fail("the archive changed while it was being read");
      return false;
    }
    if (current == index) {
      target = true;
      archive->test();
      return true;
    }
    ++current;
    if (archive->flags().skip_decompresses) {
      archive->test();
    } else {
      archive->skip();
    }
  }
  fail("the archive ended before the file to retry was read");
  return false;
}

void Transfer::acknowledge_file(bool retry) {
  std::lock_guard lock(reply_mutex_);
  file_reply_ = retry ? FileReply::Retry : FileReply::Continue;
  reply_ready_.notify_all();
}

// ---------------------------------------------------------------------------
// Uploader thread

void Transfer::run_uploader() {
  try {
    upload_loop();
  } catch (const std::exception& error) {
    fail(error.what());
  }
  {
    std::lock_guard lock(mutex_);
    stopped_ = std::chrono::steady_clock::now();
  }
  --running_;
}

void Transfer::upload_loop() {
  uint64_t number = 0;
  while (true) {
    std::optional<PipeMessage> message = pipe_.pop();
    progress_.set_buffer_used(pipe_.buffered());
    if (!message) {
      return;  // Aborted.
    }
    switch (message->kind) {
      case PipeMessage::Kind::EnsureDir:
        ftp_.ensure_directory(entry(message->entry).remote);
        break;
      case PipeMessage::Kind::FileBegin: {
        PlannedEntry planned = entry(message->entry);
        if (plan_.streamed) {
          if (!probe_) {
            probe_ = std::make_unique<RemoteProbe>(ftp_, plan_.remote_root, log_);
          }
          probe_->check(planned);
          if (planned.action == PlannedEntry::Action::Skip) {
            progress_.add_skipped(planned.entry.size);
            progress_.set_activity(fmt::format("Skipping {} (already on the server)", planned.relative));
            {
              std::lock_guard lock(mutex_);
              ++skipped_files_;
              skipped_bytes_ += planned.entry.size;
            }
            if (!discard_file()) {
              return;
            }
            acknowledge_file(false);
            break;
          }
          progress_.add_upload(planned.entry.size - planned.resume_offset);
        }
        if (!upload_file(planned, ++number)) {
          return;
        }
        acknowledge_file(false);
        break;
      }
      case PipeMessage::Kind::End:
        progress_.set_totals_known();
        return;
      case PipeMessage::Kind::Data:
      case PipeMessage::Kind::FileEnd:
        throw std::logic_error("internal error: unexpected data outside of a file");
    }
  }
}

bool Transfer::upload_file(const PlannedEntry& planned, uint64_t number) {
  const uint64_t size = planned.entry.size;
  uint64_t initial_remaining = size - planned.resume_offset;
  uint64_t counted_prefix = planned.resume_offset;
  if (cancel_requested_ || pipe_.aborted()) {
    return false;
  }
  if (planned.delete_before_upload) {
    if (!ftp_.delete_file(planned.remote)) {
      fail(fmt::format("cannot delete the larger remote file {}", planned.relative));
      return false;
    }
    log_.info("Removed the larger remote file {}", planned.remote);
  }
  if (planned.resume_offset > 0) {
    log_.info("Resuming {} at byte {} ({} remaining)", planned.relative, planned.resume_offset,
              format_bytes(initial_remaining, log_.units()));
  }
  // Bytes already stored during this run still count after a retry. If the
  // server loses an original prefix, include its replacement in the totals.
  progress_.begin_file(number, planned.relative, size, planned.resume_offset);
  const auto started = std::chrono::steady_clock::now();
  uint64_t resume_offset = planned.resume_offset;
  bool remote_started = false;
  uint64_t bytes_sent = 0;
  bool complete = false;
  UploadResult result;

  for (unsigned attempt = 1;; ++attempt) {
    uint64_t remaining = size - resume_offset;
    bool ready = true;
    if (attempt > 1) {
      // SIZE is authoritative: curl may have buffered bytes which never
      // reached the server. Never use its bytes_sent as the append offset.
      result = UploadResult{};
      try {
        const RemoteFile remote = ftp_.stat_file(planned.remote);
        if (remote.exists && remote.size && *remote.size == size) {
          log_.info("{} is complete on the server", planned.relative);
          progress_.file_progress(size);
          result.ok = true;  // The source was verified before requesting a retry.
          break;
        }
        resume_offset = remote.exists && remote.size && *remote.size < size ? *remote.size : 0;
        if (remote.exists && remote.size && *remote.size > size && !ftp_.delete_file(planned.remote)) {
          result.error = fmt::format("cannot delete the larger remote file {}", planned.relative);
          ready = false;
        }
      } catch (const FtpError& error) {
        result.error = error.what();
        ready = false;
      }
      remaining = size - resume_offset;
      if (ready && !cancel_requested_ && !pipe_.aborted()) {
        log_.info("Retrying {} at byte {} (attempt {}/{})", planned.relative, resume_offset, attempt, attempts_);
        progress_.retry_file(resume_offset);
        if (resume_offset < counted_prefix) {
          initial_remaining += counted_prefix - resume_offset;
          counted_prefix = resume_offset;
        }
        complete = false;
        acknowledge_file(true);
      }
    }

    if (ready && !cancel_requested_ && !pipe_.aborted()) {
      std::vector<uint8_t> block;
      size_t offset = 0;
      uint64_t discard = resume_offset;
      const FtpClient::ReadFn read = [&](char* buffer, size_t capacity) -> std::optional<size_t> {
        while (true) {
          if (offset < block.size()) {
            if (discard > 0) {
              const size_t take = static_cast<size_t>(std::min<uint64_t>(discard, block.size() - offset));
              offset += take;
              discard -= take;
              continue;
            }
            const size_t n = std::min(capacity, block.size() - offset);
            std::memcpy(buffer, block.data() + offset, n);
            offset += n;
            return n;
          }
          if (complete) {
            return 0;
          }
          if (block.capacity() > 0) {
            pipe_.release_buffer(std::move(block));
            block = std::vector<uint8_t>();
          }
          offset = 0;
          std::optional<PipeMessage> message = pipe_.pop();
          progress_.set_buffer_used(pipe_.buffered());
          if (!message) {
            return std::nullopt;
          }
          if (message->kind == PipeMessage::Kind::Data) {
            block = std::move(message->data);
          } else if (message->kind == PipeMessage::Kind::FileEnd) {
            complete = true;
            return 0;
          } else {
            return std::nullopt;
          }
        }
      };
      const FtpClient::ProgressFn on_progress = [&](uint64_t sent) {
        progress_.file_progress(resume_offset + std::min(sent, remaining));
        return !cancel_requested_.load() && !pipe_.aborted();
      };
      result = ftp_.upload(planned.remote, remaining, planned.entry.mtime, read, on_progress, resume_offset > 0);
      remote_started = remote_started || result.remote_started;
      bytes_sent += result.bytes_sent;
      const bool unsent_data = offset < block.size();
      if (block.capacity() > 0) {
        pipe_.release_buffer(std::move(block));
      }
      if (result.ok && unsent_data) {
        fail(fmt::format("{} is larger than its size in the archive header", planned.relative));
        result.ok = false;
        result.aborted_by_source = true;
      }
      // curl can stop at INFILESIZE before reading the checksum-gated marker.
      if (!complete && !result.aborted_by_source && (result.ok || result.bytes_sent >= remaining)) {
        complete = await_file_end(planned);
        if (!complete) {
          result.ok = false;
          result.aborted_by_source = true;
        }
      }
      if (!result.ok && complete && result.bytes_sent >= remaining && !cancel_requested_ && !pipe_.aborted()) {
        log_.warn("no confirmation from the server for {} ({}); checking its size", planned.relative,
                  result.error);
        try {
          const RemoteFile remote = ftp_.stat_file(planned.remote);
          if (remote.exists && remote.size && *remote.size == size) {
            log_.warn("{} is complete on the server", planned.relative);
            result.ok = true;
          }
        } catch (const FtpError&) {
        }
      }
    }

    if (result.ok) {
      break;
    }
    if (result.aborted_by_source || cancel_requested_ || pipe_.aborted() || attempt == attempts_) {
      if (!result.aborted_by_source && !cancel_requested_ && !pipe_.aborted()) {
        fail(fmt::format("upload of {} failed after {} attempt(s): {}", planned.relative, attempt, result.error));
      }
      // Wake the extractor even when curl aborted before requesting data.
      pipe_.abort();
      {
        std::lock_guard lock(reply_mutex_);
        reply_ready_.notify_all();
      }
      if (remote_started) {
        remove_partial(planned, bytes_sent);
      }
      return false;
    }

    // Finish verifying this attempt before retrying. Drop its remaining data,
    // leaving the original archive reader waiting at the same file boundary.
    if (!complete) {
      complete = discard_file();
      if (!complete) {
        if (remote_started) {
          remove_partial(planned, bytes_sent);
        }
        return false;
      }
    }
    log_.warn("upload of {} failed (attempt {}/{}): {}; retrying immediately", planned.relative, attempt,
              attempts_, result.error);
  }

  progress_.end_file();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  log_.info("Uploaded {} ({}, {})", planned.relative, format_bytes(initial_remaining, log_.units()),
            format_speed(seconds > 0.0 ? static_cast<double>(initial_remaining) / seconds : 0.0, log_.units()));
  if (result.timestamp_failed) {
    if (!ftp_.timestamps_unsupported()) {
      log_.warn("could not set the modification time of {}", planned.relative);
    } else if (!timestamp_warning_shown_) {
      timestamp_warning_shown_ = true;
      log_.warn("the server cannot set modification times: file dates are not preserved");
    }
  }
  std::lock_guard lock(mutex_);
  ++files_uploaded_;
  bytes_uploaded_ += initial_remaining;
  return true;
}

bool Transfer::await_file_end(const PlannedEntry& planned) {
  while (true) {
    std::optional<PipeMessage> message = pipe_.pop();
    progress_.set_buffer_used(pipe_.buffered());
    if (!message) {
      return false;  // The extractor failed (e.g. checksum error) or a cancellation.
    }
    switch (message->kind) {
      case PipeMessage::Kind::FileEnd:
        return true;
      case PipeMessage::Kind::Data:
        if (message->data.empty()) {
          pipe_.release_buffer(std::move(message->data));
          continue;
        }
        fail(fmt::format("{} is larger than its size in the archive header", planned.relative));
        return false;
      default:
        fail("internal error: missing end of file marker");
        return false;
    }
  }
}

bool Transfer::discard_file() {
  while (true) {
    std::optional<PipeMessage> message = pipe_.pop();
    progress_.set_buffer_used(pipe_.buffered());
    if (!message) {
      return false;  // The extractor failed or a cancellation.
    }
    switch (message->kind) {
      case PipeMessage::Kind::Data:
        pipe_.release_buffer(std::move(message->data));
        break;
      case PipeMessage::Kind::FileEnd:
        return true;
      default:
        fail("internal error: missing end of file marker");
        return false;
    }
  }
}

void Transfer::remove_partial(const PlannedEntry& planned, uint64_t bytes_sent) {
  // Cleanup must still run after the cancellation/error that stopped the upload.
  ftp_.set_cancel_check(nullptr);
  if (ftp_.delete_file(planned.remote)) {
    log_.info("Removed the incomplete remote file {}", planned.remote);
  } else if (bytes_sent > 0) {
    log_.warn("could not remove the incomplete remote file {}", planned.remote);
  }
}

}  // namespace streamextract
