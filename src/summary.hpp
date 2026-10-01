#pragma once

#include <string>
#include <vector>

#include "transfer.hpp"

namespace rarftp {

// The outcome of a transfer, one line per entry and without the trailing
// newline: Done/FAILED/Cancelled, then what was skipped or not uploaded.
std::vector<std::string> summary_lines(const TransferResult& result);

}  // namespace rarftp
