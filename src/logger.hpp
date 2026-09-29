#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace rarftp {

enum class LogLevel { Debug, Info, Warn, Error };

struct LogLine {
  std::chrono::system_clock::time_point time;
  LogLevel level;
  std::string text;
};

// "12:34:56 WARN  message".
std::string format_log_line(const LogLine& line);

// Thread-safe log shared by the workers and the UI.
class Logger {
 public:
  using Sink = std::function<void(const LogLine&)>;

  // Called (serialized) for every accepted line, e.g. to print it right away.
  void set_sink(Sink sink);
  void set_verbose(bool verbose);
  bool verbose() const;

  void log(LogLevel level, std::string text);
  void debug(std::string text) { log(LogLevel::Debug, std::move(text)); }
  void info(std::string text) { log(LogLevel::Info, std::move(text)); }
  void warn(std::string text) { log(LogLevel::Warn, std::move(text)); }
  void error(std::string text) { log(LogLevel::Error, std::move(text)); }

  template <typename... Args>
  void info(fmt::format_string<Args...> f, Args&&... args) {
    info(fmt::format(f, std::forward<Args>(args)...));
  }
  template <typename... Args>
  void warn(fmt::format_string<Args...> f, Args&&... args) {
    warn(fmt::format(f, std::forward<Args>(args)...));
  }
  template <typename... Args>
  void error(fmt::format_string<Args...> f, Args&&... args) {
    error(fmt::format(f, std::forward<Args>(args)...));
  }

  // Last `count` lines, oldest first.
  std::vector<LogLine> tail(size_t count) const;
  // Every warning and error seen so far (capped), oldest first.
  std::vector<LogLine> problems() const;
  size_t problems_dropped() const;

 private:
  static constexpr size_t kMaxLines = 2000;
  static constexpr size_t kMaxProblems = 500;

  mutable std::mutex mutex_;
  std::deque<LogLine> lines_;
  std::vector<LogLine> problems_;
  size_t problems_dropped_ = 0;
  bool verbose_ = false;
  Sink sink_;
};

}  // namespace rarftp
