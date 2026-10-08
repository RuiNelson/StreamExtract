#include <atomic>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "logger.hpp"

using namespace streamextract;

TEST_CASE("debug lines are kept only in verbose mode") {
  Logger log;
  CHECK_FALSE(log.verbose());
  log.debug("hidden");
  CHECK(log.tail(10).empty());
  log.set_verbose(true);
  CHECK(log.verbose());
  log.debug("shown");
  const auto lines = log.tail(10);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].level == LogLevel::Debug);
  CHECK(lines[0].text == "shown");
  CHECK(log.problems().empty());  // Only warnings and errors are problems.
}

TEST_CASE("the log keeps the most recent lines, oldest first") {
  Logger log;
  for (int i = 0; i < 2100; ++i) {
    log.info("line {}", i);
  }
  const auto last = log.tail(3);
  REQUIRE(last.size() == 3);
  CHECK(last[0].text == "line 2097");
  CHECK(last[2].text == "line 2099");
  const auto all = log.tail(5000);
  REQUIRE(all.size() == 2000);  // Bounded.
  CHECK(all.front().text == "line 100");
  CHECK(log.tail(0).empty());
}

TEST_CASE("warnings and errors are kept apart, up to a limit, for the final summary") {
  Logger log;
  log.info("not a problem");
  log.warn("first warning");
  log.error("an error: {}", 42);
  auto problems = log.problems();
  REQUIRE(problems.size() == 2);
  CHECK(problems[0].level == LogLevel::Warn);
  CHECK(problems[0].text == "first warning");
  CHECK(problems[1].level == LogLevel::Error);
  CHECK(problems[1].text == "an error: 42");
  CHECK(log.problems_dropped() == 0);

  for (int i = 0; i < 600; ++i) {
    log.warn("warning {}", i);
  }
  problems = log.problems();
  CHECK(problems.size() == 500);
  CHECK(problems.front().text == "first warning");  // The first ones are kept.
  CHECK(log.problems_dropped() == 102);
  CHECK(log.tail(1)[0].text == "warning 599");  // Dropped problems still reach the log.
}

TEST_CASE("the sink sees every accepted line, in order") {
  Logger log;
  std::vector<std::string> seen;
  log.set_sink([&](const LogLine& line) { seen.push_back(line.text); });
  log.info("a");
  log.debug("filtered");
  log.warn("b");
  CHECK(seen == std::vector<std::string>{"a", "b"});
  log.set_sink(nullptr);
  log.info("c");
  CHECK(seen.size() == 2);
}

TEST_CASE("concurrent writers lose no lines") {
  Logger log;
  std::atomic<int> sunk{0};
  log.set_sink([&](const LogLine&) { ++sunk; });
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&log, t] {
      for (int i = 0; i < 500; ++i) {
        if (i % 2 == 0) {
          log.info("thread {} line {}", t, i);
        } else {
          log.warn("thread {} line {}", t, i);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(sunk.load() == 2000);
  CHECK(log.tail(5000).size() == 2000);
  CHECK(log.problems().size() == 500);
  CHECK(log.problems_dropped() == 500);
}

TEST_CASE("log lines show the local time and a fixed-width level") {
  std::tm local{};
  local.tm_year = 2026 - 1900;
  local.tm_mon = 0;
  local.tm_mday = 15;
  local.tm_hour = 13;
  local.tm_min = 5;
  local.tm_sec = 9;
  local.tm_isdst = -1;
  const auto time = std::chrono::system_clock::from_time_t(std::mktime(&local));
  CHECK(format_log_time(time) == "13:05:09");
  CHECK(format_log_line({time, LogLevel::Debug, "d"}) == "13:05:09 DEBUG d");
  CHECK(format_log_line({time, LogLevel::Info, "i"}) == "13:05:09 INFO  i");
  CHECK(format_log_line({time, LogLevel::Warn, "w"}) == "13:05:09 WARN  w");
  CHECK(format_log_line({time, LogLevel::Error, "e"}) == "13:05:09 ERROR e");
}

TEST_CASE("the display units are fixed per logger") {
  CHECK(Logger().units() == ByteUnits::Binary);
  CHECK(Logger(ByteUnits::Si).units() == ByteUnits::Si);
}
