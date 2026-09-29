#include "logger.hpp"

#include <ctime>

namespace rarftp {

namespace {

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO ";
    case LogLevel::Warn:
      return "WARN ";
    case LogLevel::Error:
      return "ERROR";
  }
  return "";
}

}  // namespace

std::string format_log_line(const LogLine& line) {
  const std::time_t t = std::chrono::system_clock::to_time_t(line.time);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return fmt::format("{:02}:{:02}:{:02} {} {}", tm.tm_hour, tm.tm_min, tm.tm_sec, level_name(line.level),
                     line.text);
}

void Logger::set_sink(Sink sink) {
  std::lock_guard lock(mutex_);
  sink_ = std::move(sink);
}

void Logger::set_verbose(bool verbose) {
  std::lock_guard lock(mutex_);
  verbose_ = verbose;
}

bool Logger::verbose() const {
  std::lock_guard lock(mutex_);
  return verbose_;
}

void Logger::log(LogLevel level, std::string text) {
  std::lock_guard lock(mutex_);
  if (level == LogLevel::Debug && !verbose_) {
    return;
  }
  LogLine line{std::chrono::system_clock::now(), level, std::move(text)};
  if (level == LogLevel::Warn || level == LogLevel::Error) {
    if (problems_.size() < kMaxProblems) {
      problems_.push_back(line);
    } else {
      ++problems_dropped_;
    }
  }
  if (sink_) {
    sink_(line);
  }
  lines_.push_back(std::move(line));
  if (lines_.size() > kMaxLines) {
    lines_.pop_front();
  }
}

std::vector<LogLine> Logger::tail(size_t count) const {
  std::lock_guard lock(mutex_);
  const size_t n = count < lines_.size() ? count : lines_.size();
  return {lines_.end() - static_cast<std::ptrdiff_t>(n), lines_.end()};
}

std::vector<LogLine> Logger::problems() const {
  std::lock_guard lock(mutex_);
  return problems_;
}

size_t Logger::problems_dropped() const {
  std::lock_guard lock(mutex_);
  return problems_dropped_;
}

}  // namespace rarftp
