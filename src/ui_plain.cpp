#include <chrono>
#include <cstdio>
#include <thread>

#include <fmt/format.h>

#include "progress.hpp"
#include "transfer.hpp"
#include "ui.hpp"
#include "util/terminal.hpp"
#include "util/text.hpp"

namespace rarftp {

namespace {

double percent(uint64_t done, uint64_t total) {
  return total > 0 ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : 100.0;
}

}  // namespace

void run_plain(Transfer& transfer, Progress& progress) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kReportInterval = std::chrono::seconds(5);

  SpeedMeter meter;
  SpeedMeter read_meter;
  auto last_report = Clock::now();
  while (!transfer.finished()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (interrupt_requested() && !transfer.cancelling()) {
      transfer.cancel();
    }
    const Progress::Snapshot s = progress.snapshot();
    meter.add_sample(s.elapsed, s.sent_bytes);
    read_meter.add_sample(s.elapsed, s.archive_read);
    if (Clock::now() - last_report < kReportInterval || transfer.cancelling()) {
      continue;
    }
    last_report = Clock::now();

    const double rate = meter.rate();
    std::string line;
    if (s.totals_known) {
      const uint64_t left = s.total_bytes > s.sent_bytes ? s.total_bytes - s.sent_bytes : 0;
      line = fmt::format("Progress {:5.1f}%  {} of {}  {}  ETA {}", percent(s.sent_bytes, s.total_bytes),
                         format_bytes(s.sent_bytes), format_bytes(s.total_bytes), format_speed(rate),
                         format_duration(eta_seconds(left, rate)));
    } else {
      // A streamed archive: no totals until its end, progress by position.
      const uint64_t left = s.archive_size > s.archive_read ? s.archive_size - s.archive_read : 0;
      line = fmt::format("Progress {:5.1f}% of the archive read  {} uploaded  {}  ETA {}",
                         percent(s.archive_read, s.archive_size), format_bytes(s.sent_bytes), format_speed(rate),
                         format_duration(eta_seconds(left, read_meter.rate())));
    }
    if (!s.current_file.empty()) {
      const double file_percent = percent(s.current_sent, s.current_size);
      const std::string number = s.totals_known ? fmt::format("{}/{}", s.current_number, s.total_files)
                                                : std::to_string(s.current_number);
      line += fmt::format("  |  file {} {:.0f}%: {}", number, file_percent, s.current_file);
    } else if (!s.activity.empty()) {
      line += "  |  " + s.activity;
    }
    std::fprintf(stdout, "%s\n", line.c_str());
    std::fflush(stdout);
  }
}

}  // namespace rarftp
