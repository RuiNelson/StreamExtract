#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "summary.hpp"

using namespace rarftp;

namespace {

using Lines = std::vector<std::string>;

TransferResult result_with(TransferResult::Status status) {
  TransferResult result;
  result.status = status;
  result.files_uploaded = 3;
  result.bytes_uploaded = 1024 * 1024;
  result.seconds = 2.0;
  return result;
}

}  // namespace

TEST_CASE("summary of a successful transfer") {
  const Lines lines = summary_lines(result_with(TransferResult::Status::Success));
  CHECK(lines == Lines{"Done: 3 file(s), 1.00 MiB uploaded in 00:00:02 (512 KiB/s on average)."});
}

TEST_CASE("summary of a failed transfer") {
  TransferResult result = result_with(TransferResult::Status::Failed);
  result.error = "connection lost";
  const Lines lines = summary_lines(result);
  CHECK(lines == Lines{"FAILED: connection lost", "3 file(s), 1.00 MiB uploaded before the failure."});
}

TEST_CASE("summary of a cancelled transfer") {
  const Lines lines = summary_lines(result_with(TransferResult::Status::Cancelled));
  CHECK(lines == Lines{"Cancelled: 3 file(s), 1.00 MiB uploaded."});
}

TEST_CASE("summary mentions skipped and ignored entries") {
  TransferResult result = result_with(TransferResult::Status::Success);
  result.skipped_files = 2;
  result.skipped_bytes = 2048;
  result.ignored = 1;
  const Lines lines = summary_lines(result);
  REQUIRE(lines.size() == 3);
  CHECK(lines[1] == "Skipped 2 file(s), 2.00 KiB already on the server with the same size.");
  CHECK(lines[2] == "Not uploaded: 1 link(s) or unsupported entries (see the warnings).");
}
