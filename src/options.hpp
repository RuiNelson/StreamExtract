#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "app_version.hpp"
#include "ftp_client.hpp"

namespace streamextract {

struct Options {
  std::string file;
  std::string host;
  int port = 21;
  FtpMode mode = FtpMode::Passive;
  FtpProtocol protocol = FtpProtocol::Ftp;
  FtpsMode ftps_mode = FtpsMode::Explicit;
  std::optional<std::string> ca_certificate;
  std::optional<std::string> user;
  std::optional<std::string> password;
  std::optional<std::string> private_key;
  std::string private_key_passphrase;
  std::optional<std::string> known_hosts;
  std::optional<std::string> directory;
  std::optional<std::string> staging;
  bool mkdir = false;
  std::optional<std::string> archive_password;
  std::string extraction_root;
  bool no_tui = false;
  bool verbose = false;
  size_t buffer_mib = 64;
  unsigned retries = 3;  // Total connection/login and per-file upload attempts, including the first.
};

struct ParsedOptions {
  std::optional<Options> options;  // Empty: exit right away with `exit_code`.
  int exit_code = 0;
};

// Parses and validates UTF-8 arguments. Help, version and usage errors are
// printed here.
ParsedOptions parse_options(int argc, char** argv);

}  // namespace streamextract
