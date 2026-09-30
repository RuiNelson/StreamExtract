#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "app_version.hpp"
#include "ftp_client.hpp"

namespace rarftp {

struct Options {
  std::string file;
  std::string host;
  int port = 21;
  FtpMode mode = FtpMode::Passive;
  std::optional<std::string> user;
  std::optional<std::string> password;
  std::optional<std::string> directory;
  bool mkdir = false;
  std::optional<std::string> rar_password;
  bool no_tui = false;
  bool verbose = false;
  size_t buffer_mib = 64;
};

struct ParsedOptions {
  std::optional<Options> options;  // Empty: exit right away with `exit_code`.
  int exit_code = 0;
};

// Parses and validates the command line. Help, version and usage errors are
// printed here.
ParsedOptions parse_options(int argc, char** argv);

}  // namespace rarftp
