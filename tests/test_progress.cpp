#include <doctest/doctest.h>

#include "progress.hpp"

using namespace streamextract;

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

TEST_CASE("retries count retained bytes once and add an original prefix if the server loses it") {
  Progress progress;
  progress.set_totals(1, 50, 0, 0);
  progress.begin_file(1, "partial.bin", 100, 50);
  progress.file_progress(80);
  progress.retry_file(75);  // Some buffered bytes never reached the server.
  CHECK(progress.snapshot().sent_bytes == 25);
  CHECK(progress.snapshot().total_bytes == 50);
  progress.retry_file(20);  // The server also lost 30 bytes of the original prefix.
  CHECK(progress.snapshot().sent_bytes == 0);
  CHECK(progress.snapshot().total_bytes == 80);
  progress.file_progress(100);
  CHECK(progress.snapshot().sent_bytes == 80);
  progress.end_file();
  CHECK(progress.snapshot().sent_bytes == 80);
  CHECK(progress.snapshot().files_done == 1);
}

TEST_CASE("streamed archives grow their totals until the end of the archive") {
  Progress progress;
  progress.set_totals(0, 0, 0, 0);
  progress.set_streamed();
  CHECK_FALSE(progress.snapshot().totals_known);
  progress.set_archive_size(1000);
  progress.add_upload(100);
  progress.add_skipped(30);
  progress.add_upload(50);
  progress.set_archive_read(400);
  auto s = progress.snapshot();
  CHECK(s.total_files == 2);
  CHECK(s.total_bytes == 150);
  CHECK(s.files_skipped == 1);
  CHECK(s.skipped_bytes == 30);
  CHECK(s.archive_read == 400);
  CHECK(s.archive_size == 1000);
  CHECK_FALSE(s.totals_known);
  progress.set_totals_known();
  CHECK(progress.snapshot().totals_known);
}

TEST_CASE("progress reports the buffer, the unpacked bytes, the activity and the current file") {
  Progress progress;
  auto s = progress.snapshot();
  CHECK(s.totals_known);  // Listed archives know their totals up front.
  CHECK(s.current_number == 0);
  CHECK(s.current_file.empty());
  CHECK(s.activity.empty());
  CHECK(s.elapsed >= 0.0);

  progress.set_buffer_capacity(64);
  progress.set_buffer_used(10);
  progress.add_unpacked(5);
  progress.add_unpacked(7);
  progress.set_activity("Unpacking a.bin");
  progress.begin_file(3, "a.bin", 9);
  s = progress.snapshot();
  CHECK(s.buffer_capacity == 64);
  CHECK(s.buffer_used == 10);
  CHECK(s.unpacked_bytes == 12);
  CHECK(s.activity == "Unpacking a.bin");
  CHECK(s.current_number == 3);
  CHECK(s.current_size == 9);
  progress.set_activity("");
  CHECK(progress.snapshot().activity.empty());
}

TEST_CASE("a retry from a larger server offset does not count bytes twice") {
  Progress progress;
  progress.set_totals(1, 100, 0, 0);
  progress.begin_file(1, "a.bin", 100);
  progress.file_progress(60);
  progress.retry_file(40);  // The server kept 40 bytes; the totals do not change.
  auto s = progress.snapshot();
  CHECK(s.total_bytes == 100);
  CHECK(s.sent_bytes == 40);
  CHECK(s.current_sent == 40);
  progress.file_progress(100);
  progress.end_file();
  s = progress.snapshot();
  CHECK(s.sent_bytes == 100);
  CHECK(s.total_bytes == 100);
}

TEST_CASE("speed meter ignores a counter that goes backwards and drops samples older than its window") {
  SpeedMeter backwards(5.0);
  backwards.add_sample(0.0, 1000);
  backwards.add_sample(2.0, 500);
  CHECK(backwards.rate() == 0.0);

  // A burst long ago no longer counts once the window has moved past it.
  SpeedMeter meter(2.0);
  meter.add_sample(0.0, 0);
  meter.add_sample(1.0, 1000000);
  for (int t = 2; t <= 10; ++t) {
    meter.add_sample(t, 1000000 + static_cast<uint64_t>(t - 1) * 10);
  }
  CHECK(meter.rate() == doctest::Approx(10.0));
}
