#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ftp_client.hpp"
#include "temp_dir.hpp"
#include "util/ssh_settings.hpp"

using namespace streamextract;
using test_support::TempDir;

namespace {

#ifdef _WIN32
constexpr wchar_t kHomeVariable[] = L"USERPROFILE";
#else
constexpr char kHomeVariable[] = "HOME";
#endif

// Points the home directory (HOME, USERPROFILE on Windows) at `home` for one test.
class ScopedHome {
 public:
  explicit ScopedHome(const std::filesystem::path& home) {
#ifdef _WIN32
    if (const wchar_t* old = _wgetenv(kHomeVariable)) old_ = std::wstring(old);
    _wputenv_s(kHomeVariable, home.wstring().c_str());
#else
    if (const char* old = std::getenv(kHomeVariable)) old_ = std::string(old);
    setenv(kHomeVariable, home.string().c_str(), 1);
#endif
  }
  ~ScopedHome() {
#ifdef _WIN32
    _wputenv_s(kHomeVariable, old_ ? old_->c_str() : L"");
#else
    if (old_) {
      setenv(kHomeVariable, old_->c_str(), 1);
    } else {
      unsetenv(kHomeVariable);
    }
#endif
  }
  ScopedHome(const ScopedHome&) = delete;
  ScopedHome& operator=(const ScopedHome&) = delete;

 private:
#ifdef _WIN32
  std::optional<std::wstring> old_;
#else
  std::optional<std::string> old_;
#endif
};

FtpConfig sftp_config() {
  FtpConfig config;
  config.protocol = FtpProtocol::Sftp;
  config.user = "someone";
  return config;
}

std::string ssh_path(const TempDir& home, const std::string& name) {
  return (home.path() / ".ssh" / name).u8string();
}

}  // namespace

TEST_CASE("SSH settings use the given private key, or none with a password") {
  const TempDir home;  // Without a ~/.ssh directory.
  const ScopedHome scoped(home.path());

  FtpConfig key = sftp_config();
  key.private_key = "/keys/id_test";
  CHECK(prepare_ssh_settings(key) == std::vector<std::string>{"/keys/id_test"});
  CHECK(key.user == "someone");

  FtpConfig password = sftp_config();
  password.password = "secret";
  CHECK(prepare_ssh_settings(password).empty());

  FtpConfig empty_password = sftp_config();
  empty_password.password_supplied = true;  // An explicit empty password is still a password.
  CHECK(prepare_ssh_settings(empty_password).empty());

  FtpConfig nothing = sftp_config();
  CHECK_THROWS_AS(prepare_ssh_settings(nothing), FtpError);  // No ~/.ssh keys to try.
}

TEST_CASE("SSH settings fill in the local user name") {
  FtpConfig config = sftp_config();
  config.user.clear();
  config.password = "secret";
  prepare_ssh_settings(config);
  CHECK_FALSE(config.user.empty());
  CHECK(config.user == current_username());
}

TEST_CASE("SSH known hosts: an empty path means ~/.ssh/known_hosts, and the file must exist") {
  const TempDir home;
  const ScopedHome scoped(home.path());
  CHECK(std::filesystem::u8path(user_home()) == home.path());

  FtpConfig config = sftp_config();
  config.password = "secret";
  config.known_hosts = "";
  CHECK_THROWS_AS(prepare_ssh_settings(config), FtpError);  // Not there yet.

  home.write(".ssh/known_hosts", std::string("example.com ssh-ed25519 AAAA\n"));
  config.known_hosts = "";
  prepare_ssh_settings(config);
  CHECK(config.known_hosts == ssh_path(home, "known_hosts"));

  FtpConfig missing = sftp_config();
  missing.password = "secret";
  missing.known_hosts = (home.path() / "no-such-file").u8string();
  CHECK_THROWS_AS(prepare_ssh_settings(missing), FtpError);

  FtpConfig omitted = sftp_config();  // Omitted: any host key is accepted.
  omitted.password = "secret";
  prepare_ssh_settings(omitted);
  CHECK_FALSE(omitted.known_hosts);
}

TEST_CASE("SSH settings find private keys in ~/.ssh by their first line, in name order") {
  const TempDir home;
  const ScopedHome scoped(home.path());
  home.write(".ssh/id_ed25519", std::string("-----BEGIN OPENSSH PRIVATE KEY-----\nAAAA\n"));
  home.write(".ssh/id_rsa", std::string("-----BEGIN RSA PRIVATE KEY-----\r\nAAAA\r\n"));  // CRLF.
  home.write(".ssh/work", std::string("-----BEGIN PRIVATE KEY-----\nAAAA\n"));
  home.write(".ssh/a_encrypted", std::string("-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n"));
  home.write(".ssh/ec", std::string("-----BEGIN EC PRIVATE KEY-----\nAAAA\n"));
  home.write(".ssh/id_rsa.pub", std::string("ssh-rsa AAAA user@host\n"));
  home.write(".ssh/known_hosts", std::string("example.com ssh-ed25519 AAAA\n"));
  home.write(".ssh/config", std::string("Host *\n"));
  home.write(".ssh/empty", std::string());
  home.write(".ssh/keys.d/nested", std::string("-----BEGIN OPENSSH PRIVATE KEY-----\n"));  // Not searched.

  FtpConfig config = sftp_config();
  CHECK(prepare_ssh_settings(config) ==
        std::vector<std::string>{ssh_path(home, "a_encrypted"), ssh_path(home, "ec"), ssh_path(home, "id_ed25519"),
                                 ssh_path(home, "id_rsa"), ssh_path(home, "work")});
}

TEST_CASE("SSH settings without a password or any private key cannot log in") {
  const TempDir home;
  const ScopedHome scoped(home.path());
  home.write(".ssh/id_rsa.pub", std::string("ssh-rsa AAAA user@host\n"));
  FtpConfig config = sftp_config();
  CHECK_THROWS_AS(prepare_ssh_settings(config), FtpError);
}
