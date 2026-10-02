#include "summary.hpp"

#include <fmt/format.h>

#include "util/text.hpp"

namespace rarftp {

std::vector<std::string> summary_lines(const TransferResult& result, ByteUnits units) {
  std::vector<std::string> lines;
  switch (result.status) {
    case TransferResult::Status::Success: {
      const double average =
          result.seconds > 0.0 ? static_cast<double>(result.bytes_uploaded) / result.seconds : 0.0;
      lines.push_back(fmt::format("Done: {} file(s), {} uploaded in {} ({} on average).", result.files_uploaded,
                                  format_bytes(result.bytes_uploaded, units), format_duration(result.seconds),
                                  format_speed(average, units)));
      break;
    }
    case TransferResult::Status::Failed:
      lines.push_back(fmt::format("FAILED: {}", result.error));
      lines.push_back(fmt::format("{} file(s), {} uploaded before the failure.", result.files_uploaded,
                                  format_bytes(result.bytes_uploaded, units)));
      break;
    case TransferResult::Status::Cancelled:
      lines.push_back(fmt::format("Cancelled: {} file(s), {} uploaded.", result.files_uploaded,
                                  format_bytes(result.bytes_uploaded, units)));
      break;
  }
  if (result.skipped_files > 0) {
    lines.push_back(fmt::format("Skipped {} file(s), {} already on the server with the same size.",
                                result.skipped_files, format_bytes(result.skipped_bytes, units)));
  }
  if (result.ignored > 0) {
    lines.push_back(
        fmt::format("Not uploaded: {} link(s) or unsupported entries (see the warnings).", result.ignored));
  }
  return lines;
}

}  // namespace rarftp
