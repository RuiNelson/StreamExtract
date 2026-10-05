#include "util/ssh_settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <curl/curl.h>
#ifdef _WIN32
#include <windows.h>

#include <Lmcons.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace streamextract {
std::string current_username() {
#ifdef _WIN32
  wchar_t name[UNLEN + 1];
  DWORD size = UNLEN + 1;
  if (GetUserNameW(name, &size)) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
      std::string result(static_cast<size_t>(bytes), '\0');
      WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), bytes, nullptr, nullptr);
      result.pop_back();
      return result;
    }
  }
#else
  const passwd* account = getpwuid(geteuid());
  if (account && account->pw_name) return account->pw_name;
#endif
  throw FtpError("cannot determine the current local username", CURLE_LOGIN_DENIED);
}

std::string user_home() {
#ifdef _WIN32
  const wchar_t* home = _wgetenv(L"USERPROFILE");
  if (home && *home) return std::filesystem::path(home).u8string();
#else
  const char* home = std::getenv("HOME");
  if (home && *home) return home;
#endif
#ifndef _WIN32
  const passwd* account = getpwuid(geteuid());
  if (account && account->pw_dir) return account->pw_dir;
#endif
  throw FtpError("cannot determine the current user's home directory", CURLE_LOGIN_DENIED);
}

std::vector<std::string> prepare_ssh_settings(FtpConfig& config) {
  if (config.user.empty()) config.user = current_username();
  if (config.known_hosts && config.known_hosts->empty()) {
    config.known_hosts = (std::filesystem::u8path(user_home()) / ".ssh" / "known_hosts").u8string();
  }
  if (config.known_hosts) {
    std::ifstream file(std::filesystem::u8path(*config.known_hosts));
    if (!file)
      throw FtpError("cannot read known hosts file: " + *config.known_hosts, CURLE_PEER_FAILED_VERIFICATION);
  }
  if (config.private_key) return {*config.private_key};
  if (config.password_supplied || !config.password.empty()) return {};
  const auto directory = std::filesystem::u8path(user_home()) / ".ssh";
  std::vector<std::string> keys;
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (!error) {
    for (const auto& item : iterator) {
      if (!item.is_regular_file(error) || error) continue;
      std::ifstream file(item.path(), std::ios::binary);
      char line[80]{};
      file.getline(line, sizeof(line));
      std::string header(line);
      if (!header.empty() && header.back() == '\r') header.pop_back();
      if (header == "-----BEGIN OPENSSH PRIVATE KEY-----" || header == "-----BEGIN RSA PRIVATE KEY-----" ||
          header == "-----BEGIN EC PRIVATE KEY-----" || header == "-----BEGIN PRIVATE KEY-----" ||
          header == "-----BEGIN ENCRYPTED PRIVATE KEY-----")
        keys.push_back(item.path().u8string());
    }
  }
  std::sort(keys.begin(), keys.end());
  if (keys.empty())
    throw FtpError("no private keys found in " + directory.u8string() + "; supply a password or a private key",
                   CURLE_LOGIN_DENIED);
  return keys;
}
}  // namespace streamextract
