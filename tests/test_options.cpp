#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "options.hpp"

using namespace streamextract;

namespace {
ParsedOptions parse(std::initializer_list<const char*> args) {
  std::vector<std::string> values{"sext", "--file", __FILE__, "--host", "example.com"};
  values.insert(values.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& value : values) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);
  return parse_options(static_cast<int>(values.size()), argv.data());
}
}  // namespace

TEST_CASE("CLI defaults to FTP on port 21") {
  const auto result = parse({});
  REQUIRE(result.options);
  CHECK(result.options->protocol == FtpProtocol::Ftp);
  CHECK(result.options->port == 21);
}

TEST_CASE("CLI selects explicit or implicit FTPS and their default ports") {
  const auto explicit_tls = parse({"--protocol", "FTPS"});
  REQUIRE(explicit_tls.options);
  CHECK(explicit_tls.options->protocol == FtpProtocol::Ftps);
  CHECK(explicit_tls.options->ftps_mode == FtpsMode::Explicit);
  CHECK(explicit_tls.options->port == 21);

  const auto implicit_tls = parse({"--protocol", "ftps", "--ftps-mode", "IMPLICIT"});
  REQUIRE(implicit_tls.options);
  CHECK(implicit_tls.options->ftps_mode == FtpsMode::Implicit);
  CHECK(implicit_tls.options->port == 990);

  for (const char* mode : {"explicit", "implicit"}) {
    const auto custom_port = parse({"--protocol", "ftps", "--ftps-mode", mode, "--port", "2121"});
    REQUIRE(custom_port.options);
    CHECK(custom_port.options->port == 2121);
  }
}

TEST_CASE("CLI rejects invalid protocol and TLS options used with FTP") {
  for (const auto& args : {std::initializer_list<const char*>{"--protocol", "sftp"},
                          {"--protocol", "ftps", "--ftps-mode", "invalid"},
                          {"--ftps-mode", "implicit"},
                          {"--protocol", "ftp", "--ftps-mode", "explicit"},
                          {"--cacert", __FILE__}}) {
    const auto result = parse(args);
    CHECK_FALSE(result.options);
    CHECK(result.exit_code == 2);
  }
}

TEST_CASE("CLI accepts a custom FTPS CA certificate file") {
  const auto result = parse({"--protocol", "ftps", "--cacert", __FILE__});
  REQUIRE(result.options);
  REQUIRE(result.options->ca_certificate);
  CHECK(*result.options->ca_certificate == __FILE__);
}
