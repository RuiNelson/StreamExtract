#include "progress.hpp"

namespace rarftp {

Progress::Progress() : start_(std::chrono::steady_clock::now()) {}

void Progress::set_totals(uint64_t files, uint64_t bytes, uint64_t skipped_files, uint64_t skipped_bytes) {
  total_files_ = files;
  total_bytes_ = bytes;
  files_skipped_ = skipped_files;
  skipped_bytes_ = skipped_bytes;
  start_ = std::chrono::steady_clock::now();
}

void Progress::set_streamed() { totals_known_ = false; }

void Progress::add_upload(uint64_t bytes) {
  ++total_files_;
  total_bytes_ += bytes;
}

void Progress::add_skipped(uint64_t bytes) {
  ++files_skipped_;
  skipped_bytes_ += bytes;
}

void Progress::set_totals_known() { totals_known_ = true; }

void Progress::set_archive_size(uint64_t size) { archive_size_ = size; }

void Progress::set_archive_read(uint64_t read) { archive_read_ = read; }

void Progress::set_buffer_capacity(uint64_t capacity) { buffer_capacity_ = capacity; }

void Progress::set_buffer_used(uint64_t used) { buffer_used_ = used; }

void Progress::begin_file(uint64_t number, std::string name, uint64_t size) {
  std::lock_guard lock(mutex_);
  current_number_ = number;
  current_file_ = std::move(name);
  current_size_ = size;
  current_sent_ = 0;
}

void Progress::file_progress(uint64_t sent) { current_sent_ = sent; }

void Progress::end_file() {
  std::lock_guard lock(mutex_);
  done_bytes_ += current_size_;
  ++files_done_;
  current_sent_ = 0;
  current_size_ = 0;
  current_file_.clear();
}

void Progress::add_unpacked(uint64_t bytes) { unpacked_bytes_ += bytes; }

void Progress::set_activity(std::string activity) {
  std::lock_guard lock(mutex_);
  activity_ = std::move(activity);
}

Progress::Snapshot Progress::snapshot() const {
  Snapshot s;
  {
    std::lock_guard lock(mutex_);
    s.current_number = current_number_;
    s.current_file = current_file_;
    s.current_size = current_size_;
    s.activity = activity_;
    s.current_sent = current_sent_;
    s.sent_bytes = done_bytes_ + s.current_sent;
    s.files_done = files_done_;
  }
  s.total_files = total_files_;
  s.total_bytes = total_bytes_;
  s.totals_known = totals_known_;
  s.archive_read = archive_read_;
  s.archive_size = archive_size_;
  s.unpacked_bytes = unpacked_bytes_;
  s.files_skipped = files_skipped_;
  s.skipped_bytes = skipped_bytes_;
  s.buffer_used = buffer_used_;
  s.buffer_capacity = buffer_capacity_;
  s.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  return s;
}

void SpeedMeter::add_sample(double time, uint64_t total) {
  samples_.emplace_back(time, total);
  // Keep one sample older than the window so the span covers all of it.
  while (samples_.size() > 2 && time - samples_[1].first >= window_) {
    samples_.pop_front();
  }
}

double SpeedMeter::rate() const {
  if (samples_.size() < 2) {
    return 0.0;
  }
  const auto& [t0, b0] = samples_.front();
  const auto& [t1, b1] = samples_.back();
  const double span = t1 - t0;
  if (span < 0.5 || b1 < b0) {
    return 0.0;
  }
  return static_cast<double>(b1 - b0) / span;
}

double eta_seconds(uint64_t remaining, double rate) {
  if (remaining == 0) {
    return 0.0;
  }
  if (rate <= 0.0) {
    return -1.0;
  }
  return static_cast<double>(remaining) / rate;
}

}  // namespace rarftp
