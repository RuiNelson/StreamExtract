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

TEST_CASE("CLI staging is optional and requires a remote path") {
  REQUIRE(parse({}).options);
  CHECK_FALSE(parse({}).options->staging);
  const auto result = parse({"--staging", "../incoming", "--directory", "/published"});
  REQUIRE(result.options);
  CHECK(result.options->staging == "../incoming");
  CHECK(result.options->directory == "/published");
  CHECK_FALSE(parse({"--staging", ""}).options);
  CHECK_FALSE(parse({"--staging"}).options);
}

TEST_CASE("CLI extraction root is optional and independent of the destination") {
  REQUIRE(parse({}).options);
  CHECK(parse({}).options->extraction_root.empty());
  const auto result = parse({"--extraction-root", "b/nested", "--directory", "/remote/dest"});
  REQUIRE(result.options);
  CHECK(result.options->extraction_root == "b/nested");
  CHECK(result.options->directory == "/remote/dest");
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
  for (const auto& args : {std::initializer_list<const char*>{"--protocol", "scp"},
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

TEST_CASE("SFTP defaults to SSH port and accepts a password without a user") {
  const auto result = parse({"--protocol", "SFTP", "--password", "secret"});
  REQUIRE(result.options);
  CHECK(result.options->protocol == FtpProtocol::Sftp);
  CHECK(result.options->port == 22);
  CHECK_FALSE(result.options->user);
  CHECK(result.options->password == "secret");
  const auto custom = parse({"--protocol", "sftp", "--port", "2222"});
  REQUIRE(custom.options);
  CHECK(custom.options->port == 2222);
}

TEST_CASE("SFTP distinguishes omitted, empty and explicit known hosts paths") {
  const auto omitted = parse({"--protocol", "sftp"});
  REQUIRE(omitted.options);
  CHECK_FALSE(omitted.options->known_hosts);
  const auto empty = parse({"--protocol", "sftp", "--known-hosts", ""});
  REQUIRE(empty.options);
  REQUIRE(empty.options->known_hosts);
  CHECK(empty.options->known_hosts->empty());
  const auto explicit_path = parse({"--protocol", "sftp", "--known-hosts", __FILE__, "--private-key", __FILE__,
                                    "--private-key-passphrase", "secret"});
  REQUIRE(explicit_path.options);
  CHECK(explicit_path.options->private_key == __FILE__);
  CHECK(explicit_path.options->known_hosts == __FILE__);
  CHECK(explicit_path.options->private_key_passphrase == "secret");
  const auto alias = parse({"--protocol", "sftp", "--private-key-passphare", "secret"});
  REQUIRE(alias.options);
  CHECK(alias.options->private_key_passphrase == "secret");
}

TEST_CASE("CLI rejects SSH arguments for FTP and FTPS arguments for SFTP") {
  for (const auto& args : {std::initializer_list<const char*>{"--private-key", __FILE__},
                           {"--protocol", "ftps", "--known-hosts", ""},
                           {"--private-key-passphrase", "secret"},
                           {"--protocol", "sftp", "--ftps-mode", "implicit"},
                           {"--protocol", "sftp", "--cacert", __FILE__}}) {
    const auto result = parse(args);
    CHECK_FALSE(result.options);
    CHECK(result.exit_code == 2);
  }
}

namespace {
// Without the default --file/--host of parse().
ParsedOptions parse_only(std::initializer_list<const char*> args) {
  std::vector<std::string> values{"sext"};
  values.insert(values.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& value : values) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);
  return parse_options(static_cast<int>(values.size()), argv.data());
}
}  // namespace

TEST_CASE("CLI defaults") {
  const auto result = parse({});
  REQUIRE(result.options);
  const Options& o = *result.options;
  CHECK(o.file == __FILE__);
  CHECK(o.host == "example.com");
  CHECK(o.mode == FtpMode::Passive);
  CHECK(o.retries == 3);
  CHECK(o.buffer_mib == 64);
  CHECK_FALSE(o.user);
  CHECK_FALSE(o.password);
  CHECK_FALSE(o.directory);
  CHECK_FALSE(o.archive_password);
  CHECK_FALSE(o.mkdir);
  CHECK_FALSE(o.no_tui);
  CHECK_FALSE(o.verbose);
}

TEST_CASE("CLI archive password and its hidden former name") {
  CHECK(parse({"--archive-password", "secret"}).options->archive_password == "secret");
  CHECK(parse({"--rar-password", "secret"}).options->archive_password == "secret");
  CHECK(parse({"--archive-password", ""}).options->archive_password == "");  // Given, even if empty.
  const auto both = parse({"--archive-password", "a", "--rar-password", "b"});
  CHECK_FALSE(both.options);
  CHECK(both.exit_code == 2);
}

TEST_CASE("CLI attempts and buffer size are bounded") {
  CHECK(parse({"--retries", "1"}).options->retries == 1);
  CHECK(parse({"--buffer", "1"}).options->buffer_mib == 1);
  CHECK(parse({"--buffer", "4096"}).options->buffer_mib == 4096);
  for (const auto& args : {std::initializer_list<const char*>{"--retries", "0"},
                           {"--retries", "-1"},
                           {"--buffer", "0"},
                           {"--buffer", "4097"},
                           {"--port", "0"},
                           {"--port", "65536"}}) {
    const auto result = parse(args);
    CHECK_FALSE(result.options);
    CHECK(result.exit_code == 2);
  }
}

TEST_CASE("CLI login: an FTP password needs a user, and an empty user is anonymous") {
  const auto no_user = parse({"--password", "secret"});
  CHECK_FALSE(no_user.options);
  CHECK(no_user.exit_code == 2);
  const auto login = parse({"--user", "bob", "--password", "secret", "--mode", "ACTIVE", "--mkdir"});
  REQUIRE(login.options);
  CHECK(login.options->user == "bob");
  CHECK(login.options->password == "secret");
  CHECK(login.options->mode == FtpMode::Active);
  CHECK(login.options->mkdir);
  const auto anonymous = parse({"--user", ""});
  REQUIRE(anonymous.options);
  CHECK_FALSE(anonymous.options->user);
}

TEST_CASE("CLI requires an existing archive and a plain host name") {
  for (const auto& args : {std::initializer_list<const char*>{"--host", "example.com"},
                           {"--file", __FILE__},
                           {"--file", "/no/such/archive.rar", "--host", "example.com"},
                           {"--file", __FILE__, "--host", "ftp://example.com"},
                           {"--file", __FILE__, "--host", "example.com/dir"},
                           {"--file", __FILE__, "--host", "two words"},
                           {"--file", __FILE__, "--host", ""},
                           {"--file", __FILE__, "--host", "example.com", "--directory", ""},
                           {"--file", __FILE__, "--host", "example.com", "--no-such-option"}}) {
    const auto result = parse_only(args);
    CHECK_FALSE(result.options);
    CHECK(result.exit_code == 2);
  }
  const auto ipv6 = parse_only({"--file", __FILE__, "--host", "::1"});
  REQUIRE(ipv6.options);
  CHECK(ipv6.options->host == "::1");
}
