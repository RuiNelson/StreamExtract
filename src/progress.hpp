#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace rarftp {

// Transfer counters shared between the worker threads and the UI.
class Progress {
 public:
  struct Snapshot {
    uint64_t total_files = 0;  // Files that will be uploaded (skipped ones excluded).
    uint64_t total_bytes = 0;
    uint64_t sent_bytes = 0;  // Completed files plus the current file's progress.
    uint64_t unpacked_bytes = 0;
    uint64_t files_done = 0;
    uint64_t files_skipped = 0;
    uint64_t skipped_bytes = 0;
    uint64_t buffer_used = 0;
    uint64_t buffer_capacity = 0;
    uint64_t current_number = 0;  // 1-based position among uploaded files, 0 = none.
    std::string current_file;
    uint64_t current_size = 0;
    uint64_t current_sent = 0;
    std::string activity;
    double elapsed = 0.0;
  };

  Progress();

  void set_totals(uint64_t files, uint64_t bytes, uint64_t skipped_files, uint64_t skipped_bytes);
  void set_buffer_capacity(uint64_t capacity);
  void set_buffer_used(uint64_t used);

  // Uploader side.
  void begin_file(uint64_t number, std::string name, uint64_t size);
  void file_progress(uint64_t sent);
  void end_file();

  // Extractor side.
  void add_unpacked(uint64_t bytes);
  void set_activity(std::string activity);

  Snapshot snapshot() const;

 private:
  std::chrono::steady_clock::time_point start_;

  std::atomic<uint64_t> total_files_{0};
  std::atomic<uint64_t> total_bytes_{0};
  std::atomic<uint64_t> done_bytes_{0};
  std::atomic<uint64_t> unpacked_bytes_{0};
  std::atomic<uint64_t> files_done_{0};
  std::atomic<uint64_t> files_skipped_{0};
  std::atomic<uint64_t> skipped_bytes_{0};
  std::atomic<uint64_t> buffer_used_{0};
  std::atomic<uint64_t> buffer_capacity_{0};
  std::atomic<uint64_t> current_sent_{0};

  mutable std::mutex mutex_;
  uint64_t current_number_ = 0;
  std::string current_file_;
  uint64_t current_size_ = 0;
  std::string activity_;
};

// Throughput over a sliding time window.
class SpeedMeter {
 public:
  explicit SpeedMeter(double window_seconds = 5.0) : window_(window_seconds) {}

  // `total` is a monotonically increasing byte counter sampled at `time` seconds.
  void add_sample(double time, uint64_t total);
  // Bytes per second; 0 until enough time has been observed.
  double rate() const;

 private:
  double window_;
  std::deque<std::pair<double, uint64_t>> samples_;
};

// Seconds needed to transfer `remaining` bytes at `rate`; negative if unknown.
double eta_seconds(uint64_t remaining, double rate);

}  // namespace rarftp
