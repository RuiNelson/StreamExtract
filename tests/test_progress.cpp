#include <doctest/doctest.h>

#include "progress.hpp"

using namespace rarftp;

TEST_CASE("speed meter follows the recent window") {
  SpeedMeter meter(5.0);
  CHECK(meter.rate() == 0.0);
  for (int t = 0; t <= 10; ++t) {
    meter.add_sample(t, static_cast<uint64_t>(t) * 1000);  // 1000 B/s.
  }
  CHECK(meter.rate() == doctest::Approx(1000.0));
  for (int t = 11; t <= 20; ++t) {
    meter.add_sample(t, 10000 + static_cast<uint64_t>(t - 10) * 2000);  // Then 2000 B/s.
  }
  CHECK(meter.rate() == doctest::Approx(2000.0));
}

TEST_CASE("speed meter needs some history") {
  SpeedMeter meter(5.0);
  meter.add_sample(0.0, 0);
  meter.add_sample(0.1, 1000);
  CHECK(meter.rate() == 0.0);
}

TEST_CASE("eta") {
  CHECK(eta_seconds(0, 0.0) == 0.0);
  CHECK(eta_seconds(100, 0.0) < 0.0);
  CHECK(eta_seconds(1000, 100.0) == doctest::Approx(10.0));
}

TEST_CASE("progress snapshots") {
  Progress progress;
  progress.set_totals(2, 300, 1, 50);
  progress.begin_file(1, "a.bin", 100);
  progress.file_progress(40);
  Progress::Snapshot s = progress.snapshot();
  CHECK(s.total_files == 2);
  CHECK(s.total_bytes == 300);
  CHECK(s.sent_bytes == 40);
  CHECK(s.current_sent == 40);
  CHECK(s.current_file == "a.bin");
  CHECK(s.files_skipped == 1);
  CHECK(s.skipped_bytes == 50);

  progress.end_file();
  s = progress.snapshot();
  CHECK(s.sent_bytes == 100);
  CHECK(s.files_done == 1);
  CHECK(s.current_file.empty());
}

TEST_CASE("resumed file progress includes the prefix but upload totals exclude it") {
  Progress progress;
  progress.set_totals(2, 250, 0, 0);
  progress.begin_file(1, "partial.bin", 100, 50);
  auto snapshot = progress.snapshot();
  CHECK(snapshot.current_size == 100);
  CHECK(snapshot.current_sent == 50);
  CHECK(snapshot.sent_bytes == 0);
  progress.file_progress(75);
  CHECK(progress.snapshot().sent_bytes == 25);
  progress.end_file();
  snapshot = progress.snapshot();
  CHECK(snapshot.sent_bytes == 50);
  CHECK(snapshot.files_done == 1);
  CHECK(snapshot.current_sent == 0);
  progress.begin_file(2, "new.bin", 200);
  progress.file_progress(150);
  CHECK(progress.snapshot().sent_bytes == 200);
  progress.end_file();
  CHECK(progress.snapshot().sent_bytes == 250);
  CHECK(progress.snapshot().files_done == 2);
}
