#include "capi/job.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "ftp_client.hpp"
#include "summary.hpp"
#include "util/json.hpp"
#include "util/remote_path.hpp"
#include "util/ssh_settings.hpp"
#include "util/text.hpp"

namespace streamextract {

namespace {

constexpr size_t kMaxLogLines = 5000;
constexpr unsigned kDefaultBufferMib = 64;
constexpr unsigned kMaxBufferMib = 4096;

std::string file_name_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
  }
  return "info";
}

const char* status_name(TransferResult::Status status) {
  switch (status) {
    case TransferResult::Status::Success:
      return "success";
    case TransferResult::Status::Failed:
      return "failed";
    case TransferResult::Status::Cancelled:
      return "cancelled";
  }
  return "failed";
}

uint64_t remaining(uint64_t total, uint64_t done) { return total > done ? total - done : 0; }

void write_log_line(JsonWriter& json, uint64_t seq, const LogLine& line) {
  json.begin_object();
  json.member("seq", seq);
  json.member("time", format_log_time(line.time));
  json.member("level", level_name(line.level));
  json.member("text", line.text);
  json.end_object();
}

void write_progress(JsonWriter& json, const Progress::Snapshot& s, double upload_rate, double unpack_rate,
                    double read_rate, ByteUnits units) {
  const double average = s.elapsed > 0.0 ? static_cast<double>(s.sent_bytes) / s.elapsed : 0.0;
  const bool uploading = !s.current_file.empty();
  const double eta_file = uploading ? eta_seconds(remaining(s.current_size, s.current_sent), upload_rate) : -1.0;
  // Without totals (a streamed archive), from the position in the archive.
  const double eta_total = s.totals_known       ? eta_seconds(remaining(s.total_bytes, s.sent_bytes), upload_rate)
                           : s.archive_size > 0 ? eta_seconds(remaining(s.archive_size, s.archive_read), read_rate)
                                                : -1.0;

  json.begin_object();
  json.member("total_files", s.total_files);
  json.member("total_bytes", s.total_bytes);
  json.member("totals_known", s.totals_known);
  json.member("archive_read", s.archive_read);
  json.member("archive_size", s.archive_size);
  json.member("sent_bytes", s.sent_bytes);
  json.member("unpacked_bytes", s.unpacked_bytes);
  json.member("files_done", s.files_done);
  json.member("files_skipped", s.files_skipped);
  json.member("skipped_bytes", s.skipped_bytes);
  json.member("buffer_used", s.buffer_used);
  json.member("buffer_capacity", s.buffer_capacity);
  json.member("current_number", s.current_number);
  json.member("current_file", s.current_file);
  json.member("current_size", s.current_size);
  json.member("current_sent", s.current_sent);
  json.member("activity", s.activity);
  json.member("elapsed", s.elapsed);
  json.member("upload_rate", upload_rate);
  json.member("average_rate", average);
  json.member("unpack_rate", unpack_rate);
  json.member("eta_file", eta_file);
  json.member("eta_total", eta_total);
  json.key("text");
  json.begin_object();
  json.member("total_bytes", format_bytes(s.total_bytes, units));
  json.member("archive_read", format_bytes(s.archive_read, units));
  json.member("archive_size", format_bytes(s.archive_size, units));
  json.member("sent_bytes", format_bytes(s.sent_bytes, units));
  json.member("skipped_bytes", format_bytes(s.skipped_bytes, units));
  json.member("current_size", format_bytes(s.current_size, units));
  json.member("current_sent", format_bytes(s.current_sent, units));
  json.member("buffer_capacity", format_bytes(s.buffer_capacity, units));
  json.member("upload_rate", format_speed(upload_rate, units));
  json.member("average_rate", format_speed(average, units));
  json.member("unpack_rate", format_speed(unpack_rate, units));
  json.member("eta_file", format_duration(eta_file));
  json.member("eta_total", format_duration(eta_total));
  json.member("elapsed", format_duration(s.elapsed));
  json.end_object();
  json.end_object();
}

}  // namespace

Job::Job(JobConfig config)
    : config_(std::move(config)), archive_name_(file_name_of(config_.archive)), log_(config_.units) {
  login_user_ = config_.user.empty()
                    ? (config_.protocol == STREAMEXTRACT_PROTOCOL_SFTP ? "current local user" : "anonymous")
                    : config_.user;
  log_.set_verbose(config_.verbose);
  log_.set_sink([this](const LogLine& line) { add_log_line(line); });
  thread_ = std::thread([this] { run(); });
}

Job::~Job() {
  cancel();
  if (thread_.joinable()) {
    thread_.join();
  }
}

// ---------------------------------------------------------------------------
// Public interface (any thread)

void Job::cancel() {
  {
    std::lock_guard lock(state_mutex_);
    if (cancel_requested_ || phase_ == Phase::Finished) {
      return;
    }
    cancel_requested_ = true;  // Under the lock, so a waiting prompt cannot miss the wake-up.
  }
  state_changed_.notify_all();

  std::lock_guard lock(transfer_mutex_);
  if (transfer_ != nullptr) {
    transfer_->cancel();
  } else {
    log_.info("Cancelling...");  // The controller notices at its next step.
  }
}

void Job::answer_password(std::optional<std::string> password) {
  {
    std::lock_guard lock(state_mutex_);
    if (!prompt_pending_ || answered_) {
      return;
    }
    answer_ = std::move(password);
    answered_ = true;
  }
  state_changed_.notify_all();
}

std::string Job::poll(uint64_t log_cursor) {
  JsonWriter json;
  std::lock_guard lock(state_mutex_);

  // The log after the state lock: every line logged before the job finished is
  // then part of a "finished" snapshot. The Logger calls our sink with its lock
  // held, so nothing here may call into it while holding the log lock.
  std::vector<SequencedLine> lines;
  uint64_t next = 0;
  {
    std::lock_guard log_lock(log_mutex_);
    next = next_seq_;
    if (!lines_.empty() && log_cursor < next) {
      const uint64_t first = lines_.front().seq;
      const auto skip = static_cast<std::ptrdiff_t>(log_cursor > first ? log_cursor - first : 0);
      lines.assign(lines_.begin() + skip, lines_.end());
    }
  }

  json.begin_object();

  static constexpr const char* kPhases[] = {"reading", "connecting", "checking", "transferring", "finished"};
  json.member("phase", kPhases[static_cast<int>(phase_)]);
  json.member("cancelling", cancel_requested_.load() && phase_ != Phase::Finished);

  json.key("prompt");
  if (prompt_pending_ && !answered_) {
    json.begin_object();
    json.member("kind", "archive_password");
    json.member("archive", archive_name_);
    json.key("error");
    if (prompt_error_) {
      json.value(*prompt_error_);
    } else {
      json.null();
    }
    json.end_object();
  } else {
    json.null();
  }

  json.key("archive");
  if (archive_) {
    json.begin_object();
    json.member("name", archive_name_);
    json.member("format", format_name(archive_->format));
    json.key("compression");
    if (archive_->compression.empty()) {
      json.null();
    } else {
      json.value(archive_->compression);
    }
    json.key("files");
    if (archive_->files) {
      json.value(*archive_->files);
    } else {
      json.null();
    }
    json.key("bytes");
    if (archive_->bytes) {
      json.value(*archive_->bytes);
    } else {
      json.null();
    }
    json.key("bytes_text");
    if (archive_->bytes) {
      json.value(format_bytes(*archive_->bytes, config_.units));
    } else {
      json.null();
    }
    json.member("volumes", archive_->volumes);
    json.member("solid", archive_->solid);
    json.member("encrypted", archive_->encrypted);
    json.end_object();
  } else {
    json.null();
  }

  json.key("target");
  if (target_) {
    json.value(*target_);
  } else {
    json.null();
  }
  json.member("mode", config_.active_mode ? "active" : "passive");
  json.member("user", login_user_);

  json.key("probe");
  if (probe_) {
    json.begin_object();
    json.member("done", probe_->first);
    json.member("total", probe_->second);
    json.end_object();
  } else {
    json.null();
  }

  json.key("progress");
  if (final_progress_) {
    write_progress(json, *final_progress_, 0.0, 0.0, 0.0, config_.units);
  } else if (transfer_started_) {
    const Progress::Snapshot s = progress_.snapshot();
    upload_meter_.add_sample(s.elapsed, s.sent_bytes);
    unpack_meter_.add_sample(s.elapsed, s.unpacked_bytes);
    read_meter_.add_sample(s.elapsed, s.archive_read);
    write_progress(json, s, upload_meter_.rate(), unpack_meter_.rate(), read_meter_.rate(), config_.units);
  } else {
    json.null();
  }

  json.key("log");
  json.begin_object();
  json.member("next", next);
  json.key("lines");
  json.begin_array();
  for (const SequencedLine& entry : lines) {
    write_log_line(json, entry.seq, entry.line);
  }
  json.end_array();
  json.end_object();

  json.key("result");
  if (result_) {
    const Result& r = *result_;
    json.begin_object();
    json.member("status", status_name(r.status));
    json.member("error", r.error);
    json.member("files_uploaded", r.files_uploaded);
    json.member("bytes_uploaded", r.bytes_uploaded);
    json.member("seconds", r.seconds);
    json.member("skipped_files", r.skipped_files);
    json.member("skipped_bytes", r.skipped_bytes);
    json.member("ignored", r.ignored);
    json.key("summary");
    json.begin_array();
    for (const std::string& line : r.summary) {
      json.value(line);
    }
    json.end_array();
    json.key("problems");
    json.begin_array();
    for (const LogLine& line : r.problems) {
      write_log_line(json, 0, line);
    }
    json.end_array();
    json.member("problems_dropped", r.problems_dropped);
    json.end_object();
  } else {
    json.null();
  }

  json.end_object();
  return json.str();
}

// ---------------------------------------------------------------------------
// Controller thread

void Job::run() {
  Result result;
  try {
    try {
      result = pipeline();
    } catch (const Cancelled&) {
      result = cancelled_result();
    } catch (const std::exception& error) {
      result = cancel_requested_ ? cancelled_result() : failed_result(error.what());
    } catch (...) {
      result = cancel_requested_ ? cancelled_result() : failed_result("unexpected error");
    }
    finish(std::move(result));
  } catch (...) {
    // Out of memory while finishing: still leave the finished state.
    std::lock_guard lock(state_mutex_);
    phase_ = Phase::Finished;
  }
}

Job::Result Job::failed_result(const std::string& error) {
  log_.error(error);
  Result result;
  result.status = TransferResult::Status::Failed;
  result.error = error;
  result.summary.push_back(fmt::format("FAILED: {}", error));
  return result;
}

Job::Result Job::cancelled_result() {
  TransferResult cancelled;
  cancelled.status = TransferResult::Status::Cancelled;
  Result result;
  result.status = cancelled.status;
  result.summary = summary_lines(cancelled, config_.units);
  return result;
}

void Job::finish(Result result) {
  result.problems = log_.problems();
  result.problems_dropped = log_.problems_dropped();
  {
    std::lock_guard lock(state_mutex_);
    result_ = std::move(result);
    phase_ = Phase::Finished;
  }
}

void Job::throw_if_cancelled() const {
  if (cancel_requested_) {
    throw Cancelled();
  }
}

void Job::set_phase(Phase phase) {
  std::lock_guard lock(state_mutex_);
  phase_ = phase;
}

void Job::add_log_line(const LogLine& line) {
  std::lock_guard lock(log_mutex_);
  lines_.push_back({next_seq_++, line});
  if (lines_.size() > kMaxLogLines) {
    lines_.pop_front();
  }
}

std::optional<std::string> Job::ask_password(std::optional<std::string> error) {
  std::unique_lock lock(state_mutex_);
  if (cancel_requested_) {
    return std::nullopt;
  }
  prompt_pending_ = true;
  prompt_error_ = std::move(error);
  answered_ = false;
  answer_.reset();
  state_changed_.wait(lock, [this] { return answered_ || cancel_requested_; });
  prompt_pending_ = false;
  if (!answered_) {
    return std::nullopt;  // Cancelled.
  }
  return std::exchange(answer_, std::nullopt);
}

// Lists the archive, asking for the password as often as needed. On return
// `passwords` holds the source the transfer has to use.
ArchiveListing Job::read_archive(std::optional<PasswordSource>& passwords) {
  log_.info("Reading {}", config_.archive);

  std::optional<std::string> prompt_error;  // What the next prompt reports.
  bool declined = false;
  bool wrong_password = false;
  const PasswordSource::Prompt prompt = [&]() -> std::optional<std::string> {
    std::optional<std::string> answer = ask_password(std::exchange(prompt_error, std::nullopt));
    declined = !answer;
    return answer;
  };

  std::optional<std::string> initial = config_.archive_password;
  ArchiveListing listing;
  while (true) {
    declined = false;
    passwords.emplace(initial, prompt);
    try {
      listing = list_archive(config_.archive, *passwords, log_);
      break;
    } catch (const ArchivePasswordError&) {
      throw_if_cancelled();
      log_.info("Wrong archive password");
      wrong_password = true;
      initial.reset();
      prompt_error = "Wrong password";
    } catch (const std::runtime_error&) {
      throw_if_cancelled();
      if (declined) {
        throw std::runtime_error(wrong_password ? "wrong archive password"
                                                : "the archive is encrypted and no password was entered");
      }
      throw;
    }
  }

  const bool encrypted =
      listing.flags.encrypted_headers || std::any_of(listing.entries.begin(), listing.entries.end(),
                                                     [](const ArchiveEntry& e) { return e.encrypted; });
  if (encrypted && !passwords->has_password() && !passwords->get()) {
    throw_if_cancelled();
    throw std::runtime_error("the archive is encrypted and no password was entered");
  }
  passwords->disable_prompt();  // The transfer threads must never wait for an answer.

  ArchiveInfo info;
  info.name = archive_name_;
  info.format = listing.format;
  info.compression = listing.flags.compression;
  if (!listing.flags.stream_only) {
    info.files = 0;
    info.bytes = 0;
    for (const auto& entry : listing.entries) {
      if (entry.kind == EntryKind::File) {
        ++*info.files;
        *info.bytes += entry.size;
      }
    }
  }
  info.volumes = listing.volumes;
  info.solid = listing.flags.solid;
  info.encrypted = encrypted;
  log_.info("Archive: {}", describe_archive(listing, encrypted, config_.units));
  {
    std::lock_guard lock(state_mutex_);
    archive_ = std::move(info);
  }
  return listing;
}

Job::Result Job::pipeline() {
  if (config_.protocol != STREAMEXTRACT_PROTOCOL_FTP && config_.protocol != STREAMEXTRACT_PROTOCOL_FTPS_EXPLICIT &&
      config_.protocol != STREAMEXTRACT_PROTOCOL_FTPS_IMPLICIT &&
      config_.protocol != STREAMEXTRACT_PROTOCOL_SFTP) {
    throw std::runtime_error("invalid transfer protocol");
  }
  if (config_.host.empty() || config_.host.find_first_of("/ \t") != std::string::npos) {
    throw std::runtime_error("the host must be a host name or address, without ftp:// or a path");
  }
  if (config_.port < 1 || config_.port > 65535) {
    throw std::runtime_error(fmt::format("invalid port {}", config_.port));
  }

  // 1. Archive: list every volume first, so a missing volume or a wrong
  //    password fails before anything is sent.
  set_phase(Phase::Reading);
  std::optional<PasswordSource> passwords;
  const ArchiveListing listing = read_archive(passwords);
  throw_if_cancelled();

  // 2. FTP: log in and check the destination directory.
  set_phase(Phase::Connecting);
  FtpConfig ftp_config;
  ftp_config.host = config_.host;
  ftp_config.port = config_.port;
  ftp_config.protocol = config_.protocol == STREAMEXTRACT_PROTOCOL_SFTP  ? FtpProtocol::Sftp
                        : config_.protocol == STREAMEXTRACT_PROTOCOL_FTP ? FtpProtocol::Ftp
                                                                         : FtpProtocol::Ftps;
  ftp_config.ftps_mode =
      config_.protocol == STREAMEXTRACT_PROTOCOL_FTPS_IMPLICIT ? FtpsMode::Implicit : FtpsMode::Explicit;
  ftp_config.ca_certificate = config_.ca_certificate;
  ftp_config.mode = config_.active_mode ? FtpMode::Active : FtpMode::Passive;
  ftp_config.user = config_.user;
  if (is_ssh(ftp_config.protocol) && ftp_config.user.empty()) ftp_config.user = current_username();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    login_user_ = ftp_config.user.empty() ? "anonymous" : ftp_config.user;
  }
  ftp_config.password = config_.password;
  ftp_config.password_supplied = config_.password_supplied;
  ftp_config.private_key = config_.private_key;
  ftp_config.private_key_passphrase = config_.private_key_passphrase;
  ftp_config.known_hosts = config_.known_hosts;
  ftp_config.mention_flags = false;
  FtpClient ftp(ftp_config, log_, config_.verbose);
  // Lets a cancel request interrupt a server that is slow to answer.
  ftp.set_cancel_check([this] { return cancel_requested_.load(); });

  if (is_ssh(ftp_config.protocol))
    log_.info("Connecting to {}:{} (SFTP)", config_.host, config_.port);
  else
    log_.info("Connecting to {}:{} ({} mode)", config_.host, config_.port,
              config_.active_mode ? "active" : "passive");
  const std::string home = normalize_remote_path(ftp.connect(config_.retries));
  throw_if_cancelled();
  log_.info("Logged in as {}", ftp_config.user.empty() ? "anonymous" : ftp_config.user);
  std::string target;
  if (config_.directory.empty()) {
    target = home;
    log_.warn("no directory given: uploading to the server's default directory {}", target);
  } else {
    target = resolve_remote_path(home, config_.directory);
  }
  if (!ftp.directory_exists(target)) {
    if (!config_.mkdir) {
      throw std::runtime_error(
          fmt::format("the remote directory {} does not exist (enable \"Create directory if missing\")", target));
    }
    throw_if_cancelled();
    log_.info("Creating the remote directory {}", target);
    ftp.make_directory(target);
    if (!ftp.directory_exists(target)) {
      throw std::runtime_error(fmt::format("the remote directory {} is not accessible after creating it", target));
    }
  }
  throw_if_cancelled();
  const std::string url = ftp.url_for(target);
  log_.info("Destination: {}", url);
  {
    std::lock_guard lock(state_mutex_);
    target_ = url;
  }

  // 3. Plan: map entries to remote paths, skip what is already there.
  set_phase(Phase::Checking);
  TransferPlan plan = build_plan(config_.archive, listing, target, log_);
  if (!plan.streamed && plan.upload_files > 0) {
    const auto set_probe = [this](size_t done, size_t total) {
      std::lock_guard lock(state_mutex_);
      probe_ = std::make_pair(done, total);
    };
    set_probe(0, plan.upload_files);
    probe_remote(plan, ftp, log_, [&](size_t done, size_t total) {
      throw_if_cancelled();
      set_probe(done, total);
    });
  }
  throw_if_cancelled();
  if (plan.skip_files > 0) {
    log_.info("{} file(s), {} already on the server with the same size: skipping them", plan.skip_files,
              format_bytes(plan.skip_bytes, config_.units));
  }
  if (!plan.streamed) {
    log_.info("To upload: {} file(s), {}", plan.upload_files, format_bytes(plan.upload_bytes, config_.units));
  }

  // 4. Transfer.
  const size_t buffer_mib =
      config_.buffer_mib == 0 ? kDefaultBufferMib : std::min(config_.buffer_mib, kMaxBufferMib);
  ftp.set_cancel_check(nullptr);  // The transfer handles cancellation by itself.
  Transfer transfer(plan, ftp, *passwords, log_, progress_, buffer_mib << 20, config_.retries);
  struct Unpublish {
    Job& job;
    ~Unpublish() {
      std::lock_guard lock(job.transfer_mutex_);
      job.transfer_ = nullptr;
    }
  } unpublish{*this};
  {
    // cancel() takes the same lock, so it always sees a Transfer that has been started.
    std::lock_guard lock(transfer_mutex_);
    throw_if_cancelled();
    transfer_ = &transfer;
    transfer.start();
    if (cancel_requested_) {
      transfer.cancel();
    }
    std::lock_guard state_lock(state_mutex_);
    transfer_started_ = true;
    phase_ = Phase::Transferring;
  }
  const TransferResult transferred = transfer.wait();

  Progress::Snapshot final_progress = progress_.snapshot();
  final_progress.elapsed = transferred.seconds;
  final_progress.buffer_used = 0;
  {
    std::lock_guard lock(state_mutex_);
    final_progress_ = std::move(final_progress);
  }

  Result result;
  result.status = transferred.status;
  result.error = transferred.error;
  result.files_uploaded = transferred.files_uploaded;
  result.bytes_uploaded = transferred.bytes_uploaded;
  result.seconds = transferred.seconds;
  result.skipped_files = transferred.skipped_files;
  result.skipped_bytes = transferred.skipped_bytes;
  result.ignored = transferred.ignored;
  result.summary = summary_lines(transferred, config_.units);
  return result;
}

}  // namespace streamextract
