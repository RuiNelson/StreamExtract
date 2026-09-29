#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <fmt/format.h>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "logger.hpp"
#include "progress.hpp"
#include "transfer.hpp"
#include "ui.hpp"
#include "util/terminal.hpp"
#include "util/text.hpp"

namespace rarftp {

namespace {

using namespace ftxui;

// Rows used by everything but the log lines: header, two progress windows
// (2 lines + borders each), the status bar and the log window borders.
constexpr int kFixedRows = 1 + 4 + 4 + 1 + 2;

float ratio(uint64_t done, uint64_t total) {
  if (total == 0) {
    return done > 0 ? 1.0f : 0.0f;
  }
  return std::min(1.0f, static_cast<float>(static_cast<double>(done) / static_cast<double>(total)));
}

uint64_t remaining(uint64_t total, uint64_t done) { return total > done ? total - done : 0; }

Element log_panel(const Logger& log, int rows) {
  Elements lines;
  for (const LogLine& line : log.tail(static_cast<size_t>(std::max(rows, 1)))) {
    Element element = text(format_log_line(line));
    switch (line.level) {
      case LogLevel::Warn:
        element = element | color(Color::Yellow);
        break;
      case LogLevel::Error:
        element = element | color(Color::Red) | bold;
        break;
      case LogLevel::Debug:
        element = element | dim;
        break;
      case LogLevel::Info:
        break;
    }
    lines.push_back(std::move(element));
  }
  return window(text(" Log "), vbox(std::move(lines)) | flex) | flex;
}

Element progress_line(float fraction, const std::string& details) {
  return hbox({gauge(fraction) | flex, text(" " + details)});
}

}  // namespace

void run_tui(Transfer& transfer, Progress& progress, Logger& log, const UiHeader& header) {
  auto app = App::Fullscreen();
  app.ForceHandleCtrlC(false);

  SpeedMeter upload_meter;
  SpeedMeter unpack_meter;

  auto renderer = Renderer([&] {
    const Progress::Snapshot s = progress.snapshot();
    upload_meter.add_sample(s.elapsed, s.sent_bytes);
    unpack_meter.add_sample(s.elapsed, s.unpacked_bytes);
    const double upload_rate = upload_meter.rate();
    const double average = s.elapsed > 0.0 ? static_cast<double>(s.sent_bytes) / s.elapsed : 0.0;

    Element title = hbox({
        text(" rarftp ") | bold | inverted,
        text(fmt::format(" {} → {}  ({}, {})", header.archive, header.target, header.mode, header.user)),
    });

    const bool uploading = !s.current_file.empty();
    std::string file_title = " File ";
    if (uploading) {
      file_title = fmt::format(" File {}/{} ", s.current_number, s.total_files);
    }
    const std::string file_label = uploading ? s.current_file : (s.activity.empty() ? "-" : s.activity);
    const std::string file_details =
        uploading
            ? fmt::format("{:5.1f}%  {} / {}  ETA {}", 100.0 * ratio(s.current_sent, s.current_size),
                          format_bytes(s.current_sent), format_bytes(s.current_size),
                          format_duration(eta_seconds(remaining(s.current_size, s.current_sent), upload_rate)))
            : std::string("");
    Element file_box =
        window(text(file_title),
               vbox({
                   text(file_label),
                   progress_line(uploading ? ratio(s.current_sent, s.current_size) : 0.0f, file_details),
               }));

    const std::string total_details =
        fmt::format("{:5.1f}%  {} / {}  ETA {}", 100.0 * ratio(s.sent_bytes, s.total_bytes),
                    format_bytes(s.sent_bytes), format_bytes(s.total_bytes),
                    format_duration(eta_seconds(remaining(s.total_bytes, s.sent_bytes), upload_rate)));
    std::string files_line = fmt::format("{} of {} files uploaded", s.files_done, s.total_files);
    if (s.files_skipped > 0) {
      files_line +=
          fmt::format(" · {} skipped, already on the server ({})", s.files_skipped, format_bytes(s.skipped_bytes));
    }
    Element total_box =
        window(text(" Archive total "), vbox({
                                            progress_line(ratio(s.sent_bytes, s.total_bytes), total_details),
                                            text(files_line),
                                        }));

    const double buffer_percent = 100.0 * ratio(s.buffer_used, s.buffer_capacity);
    Element status = hbox({
        text(fmt::format(" Upload {} (avg {}) ", format_speed(upload_rate), format_speed(average))),
        separator(),
        text(fmt::format(" Unpack {} ", format_speed(unpack_meter.rate()))),
        separator(),
        text(fmt::format(" Buffer {:3.0f}% of {} ", buffer_percent, format_bytes(s.buffer_capacity))),
        separator(),
        text(fmt::format(" Elapsed {} ", format_duration(s.elapsed))),
        filler(),
        text(transfer.cancelling() ? " Cancelling... " : " q: cancel ") | dim,
    });

    const int log_rows = Terminal::Size().dimy - kFixedRows;
    return vbox({title, log_panel(log, log_rows), file_box, total_box, status});
  });

  auto component = CatchEvent(renderer, [&](const Event& event) {
    if (event == Event::Character('q') || event == Event::Character('Q') || event == Event::Escape ||
        event == Event::CtrlC) {
      transfer.cancel();
      return true;
    }
    return false;
  });

  // Redraws a few times per second and leaves the loop once the transfer is
  // over (or a signal asked for a cancellation).
  std::atomic<bool> stop{false};
  std::thread ticker([&] {
    while (!stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (interrupt_requested()) {
        transfer.cancel();
      }
      if (transfer.finished()) {
        app.Exit();
        return;
      }
      app.PostEvent(Event::Custom);
    }
  });

  app.Loop(component);
  stop = true;
  ticker.join();

  // The loop can also end because of a signal FTXUI caught itself.
  if (!transfer.finished()) {
    transfer.cancel();
  }
}

}  // namespace rarftp
