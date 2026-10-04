#pragma once

#include <string>

namespace streamextract {

class Logger;
class Progress;
class Transfer;

struct UiHeader {
  std::string archive;  // Archive file name.
  std::string target;   // ftp://host:port/dir
  std::string mode;     // "passive" / "active"
  std::string user;     // User name or "anonymous".
};

// Full-screen dashboard: fixed log panel, per-file and total progress,
// speeds and ETAs. Returns once the transfer has finished (q/Esc/Ctrl-C
// cancels it).
void run_tui(Transfer& transfer, Progress& progress, Logger& log, const UiHeader& header);

// Line-based output for --no-tui and non-terminals: log lines as they come
// plus a progress line every few seconds.
void run_plain(Transfer& transfer, Progress& progress);

}  // namespace streamextract
