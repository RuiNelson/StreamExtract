#include "summary.hpp"

#include <fmt/format.h>

#include "util/text.hpp"

namespace rarftp {

std::vector<std::string> summary_lines(const TransferResult& result, const TransferPlan& plan) {
  std::vector<std::string> lines;
  switch (result.status) {
    case TransferResult::Status::Success: {
      const double average =
          result.seconds > 0.0 ? static_cast<double>(result.bytes_uploaded) / result.seconds : 0.0;
      lines.push_back(fmt::format("Done: {} file(s), {} uploaded in {} ({} on average).", result.files_uploaded,
                                  format_bytes(result.bytes_uploaded), format_duration(result.seconds),
                                  format_speed(average)));
      break;
    }
    case TransferResult::Status::Failed:
      lines.push_back(fmt::format("FAILED: {}", result.error));
      lines.push_back(fmt::format("{} file(s), {} uploaded before the failure.", result.files_uploaded,
                                  format_bytes(result.bytes_uploaded)));
      break;
    case TransferResult::Status::Cancelled:
      lines.push_back(fmt::format("Cancelled: {} file(s), {} uploaded.", result.files_uploaded,
                                  format_bytes(result.bytes_uploaded)));
      break;
  }
  if (plan.skip_files > 0) {
    lines.push_back(fmt::format("Skipped {} file(s), {} already on the server with the same size.",
                                plan.skip_files, format_bytes(plan.skip_bytes)));
  }
  if (plan.ignored > 0) {
    lines.push_back(
        fmt::format("Not uploaded: {} link(s) or unsupported entries (see the warnings).", plan.ignored));
  }
  return lines;
}

}  // namespace rarftp
