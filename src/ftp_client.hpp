#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rarftp {

class Logger;

enum class FtpMode { Passive, Active };

struct FtpConfig {
  std::string host;
  int port = 21;
  FtpMode mode = FtpMode::Passive;
  std::string user;  // Empty: anonymous login.
  std::string password;
  bool mention_flags = true;  // Hints in error messages name command-line options ("--mode active").
};

class FtpError : public std::runtime_error {
 public:
  FtpError(const std::string& message, int curl_code) : std::runtime_error(message), curl_code_(curl_code) {}
  int curl_code() const { return curl_code_; }

 private:
  int curl_code_;
};

struct RemoteFile {
  bool exists = false;
  std::optional<uint64_t> size;  // Unknown if the server does not support SIZE.
};

struct UploadResult {
  bool ok = false;
  bool remote_started = false;     // The server accepted STOR/APPE: a partial file may need deleting.
  bool aborted_by_source = false;  // The read function aborted the transfer.
  bool timestamp_failed = false;   // Uploaded, but the modification time was not set.
  uint64_t bytes_sent = 0;
  int curl_code = 0;
  std::string error;
};

// Blocking FTP client over one libcurl easy handle, so the control connection
// is reused across operations. All paths are absolute remote paths.
class FtpClient {
 public:
  // Fills `buffer`; returns the byte count (0 at the end of the file) or
  // std::nullopt to abort the upload.
  using ReadFn = std::function<std::optional<size_t>(char* buffer, size_t capacity)>;
  // Reports bytes sent so far; return false to abort.
  using ProgressFn = std::function<bool(uint64_t sent)>;

  FtpClient(FtpConfig config, Logger& log, bool verbose);
  ~FtpClient();
  FtpClient(const FtpClient&) = delete;
  FtpClient& operator=(const FtpClient&) = delete;

  // Polled about once a second while a request waits for the network (and at
  // every progress update of an upload); returning true aborts the request.
  // Not set by default, and not thread-safe: set it while no request runs.
  void set_cancel_check(std::function<bool()> check);

  // Logs in and returns the login directory reported by PWD. Connection/login
  // failures get up to `attempts` total attempts, immediately; cancellation
  // stops without retrying. This limit is separate from each file's uploads.
  std::string connect(unsigned attempts = 1);

  bool directory_exists(const std::string& dir);
  // One MKD, not recursive.
  void make_directory(const std::string& dir);
  // Creates `dir` and any missing parent.
  void ensure_directory(const std::string& dir);
  // File names in `dir` (NLST); std::nullopt if the listing failed.
  std::optional<std::vector<std::string>> list_names(const std::string& dir);
  RemoteFile stat_file(const std::string& path);
  bool delete_file(const std::string& path);

  // Uploads `size` bytes pulled from `read` to `path`, creating missing
  // directories. If `mtime` (Unix seconds) is non-zero the remote modification
  // time is set afterwards (MFMT, or MDTM as used by vsftpd). With `append`,
  // `read` supplies only the remaining bytes, which are appended with APPE.
  UploadResult upload(const std::string& path, uint64_t size, int64_t mtime, const ReadFn& read,
                      const ProgressFn& progress, bool append = false);

  // True once the server is known not to support setting file times.
  bool timestamps_unsupported() const;

  std::string url_for(const std::string& path) const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// libcurl version string, for --version.
std::string curl_version_string();

}  // namespace rarftp
