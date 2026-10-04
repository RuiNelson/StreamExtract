#include "ftp_client.hpp"

#include <array>
#include <cctype>
#include <mutex>
#include <string_view>

#include <curl/curl.h>
#include <fmt/format.h>

#include "logger.hpp"
#include "util/remote_path.hpp"
#include "util/text.hpp"

namespace rarftp {

namespace {

constexpr long kConnectTimeout = 30;          // Seconds.
constexpr long kResponseTimeout = 180;        // Seconds; servers can be slow to confirm huge files.
constexpr long kAcceptTimeoutMs = 60 * 1000;  // Active mode: wait for the server's data connection.
constexpr long kKeepAliveIdle = 60;
constexpr long kKeepAliveInterval = 30;
constexpr long kLowSpeedTime = 300;  // Abort if less than 1 byte/s for this long.
constexpr long kUploadBufferSize = 1L << 20;

// RAII curl_slist.
class SList {
 public:
  SList() = default;
  SList(const SList&) = delete;
  SList& operator=(const SList&) = delete;
  ~SList() { curl_slist_free_all(list_); }
  void append(const std::string& item) { list_ = curl_slist_append(list_, item.c_str()); }
  curl_slist* get() const { return list_; }
  bool empty() const { return list_ == nullptr; }

 private:
  curl_slist* list_ = nullptr;
};

std::string_view trim_eol(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool is_reply_line(std::string_view line) {
  return line.size() >= 4 && std::isdigit(static_cast<unsigned char>(line[0])) &&
         std::isdigit(static_cast<unsigned char>(line[1])) && std::isdigit(static_cast<unsigned char>(line[2])) &&
         (line[3] == ' ' || line[3] == '-');
}

bool is_data_connection_error(CURLcode code) {
  switch (code) {
    case CURLE_FTP_ACCEPT_FAILED:
    case CURLE_FTP_ACCEPT_TIMEOUT:
    case CURLE_FTP_WEIRD_PASV_REPLY:
    case CURLE_FTP_CANT_GET_HOST:
    case CURLE_FTP_PORT_FAILED:
    case CURLE_COULDNT_CONNECT:
      return true;
    default:
      return false;
  }
}

}  // namespace

struct FtpClient::Impl {
  FtpConfig config;
  Logger& log;
  bool verbose = false;
  CURL* curl = nullptr;
  std::string base_url;
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  std::string last_reply;
  bool logged_in = false;

  enum class Timestamps { Unknown, Mfmt, Mdtm, Unsupported };
  Timestamps timestamps = Timestamps::Unknown;

  // State of the request in progress, used by the callbacks.
  const ReadFn* read = nullptr;
  const ProgressFn* progress = nullptr;
  std::function<bool()> cancel_check;
  std::string* listing = nullptr;
  bool source_aborted = false;
  bool upload_started = false;
  uint64_t sent = 0;

  Impl(FtpConfig c, Logger& l, bool v) : config(std::move(c)), log(l), verbose(v) {}

  static size_t on_header(char* buffer, size_t size, size_t nitems, void* user) {
    auto* self = static_cast<Impl*>(user);
    const std::string_view line = trim_eol(std::string_view(buffer, size * nitems));
    if (is_reply_line(line)) {
      self->last_reply.assign(line);
      if (self->read != nullptr && (line.substr(0, 3) == "125" || line.substr(0, 3) == "150")) {
        self->upload_started = true;
      }
    }
    return size * nitems;
  }

  static int on_debug(CURL*, curl_infotype type, char* data, size_t size, void* user) {
    auto* self = static_cast<Impl*>(user);
    const char* prefix = nullptr;
    switch (type) {
      case CURLINFO_TEXT:
        prefix = "*";
        break;
      case CURLINFO_HEADER_IN:
        prefix = "<";
        break;
      case CURLINFO_HEADER_OUT:
        prefix = ">";
        break;
      default:
        return 0;
    }
    std::string_view text = trim_eol(std::string_view(data, size));
    if (type == CURLINFO_HEADER_OUT && text.size() >= 5 && text.substr(0, 5) == "PASS ") {
      text = "PASS ****";
    }
    self->log.debug(fmt::format("{} {}", prefix, text));
    return 0;
  }

  static size_t on_read(char* buffer, size_t size, size_t nitems, void* user) {
    auto* self = static_cast<Impl*>(user);
    std::optional<size_t> n;
    try {
      n = (*self->read)(buffer, size * nitems);
    } catch (...) {
      n.reset();
    }
    if (!n) {
      self->source_aborted = true;
      curl_easy_setopt(self->curl, CURLOPT_FORBID_REUSE, 1L);
      return CURL_READFUNC_ABORT;
    }
    return *n;
  }

  static int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t ulnow) {
    auto* self = static_cast<Impl*>(user);
    self->sent = static_cast<uint64_t>(ulnow);
    if (self->progress != nullptr && !(*self->progress)(self->sent)) {
      // A pending FTP reply (e.g. 226) must not be mistaken for the reply to
      // the cleanup command on the next request.
      curl_easy_setopt(self->curl, CURLOPT_FORBID_REUSE, 1L);
      return 1;  // CURLE_ABORTED_BY_CALLBACK.
    }
    if (self->cancel_check && self->cancel_check()) {
      curl_easy_setopt(self->curl, CURLOPT_FORBID_REUSE, 1L);
      return 1;
    }
    return 0;
  }

  static size_t on_write(char* buffer, size_t size, size_t nitems, void* user) {
    auto* self = static_cast<Impl*>(user);
    if (self->listing != nullptr) {
      self->listing->append(buffer, size * nitems);
    }
    return size * nitems;
  }

  // Resets the handle (the connection stays cached) and applies common options.
  void prepare(const std::string& url) {
    curl_easy_reset(curl);
    error_buffer[0] = '\0';
    last_reply.clear();
    read = nullptr;
    progress = nullptr;
    listing = nullptr;
    source_aborted = false;
    upload_started = false;
    sent = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.data());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (config.user.empty()) {
      curl_easy_setopt(curl, CURLOPT_USERNAME, "anonymous");
      curl_easy_setopt(curl, CURLOPT_PASSWORD, "anonymous@");
    } else {
      curl_easy_setopt(curl, CURLOPT_USERNAME, config.user.c_str());
      curl_easy_setopt(curl, CURLOPT_PASSWORD, config.password.c_str());
    }
    if (config.mode == FtpMode::Active) {
      curl_easy_setopt(curl, CURLOPT_FTPPORT, "-");
    }
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPRT, 1L);
    curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    // One CWD for the whole directory instead of one per path component.
    // The latter adds a round trip per level whenever small files alternate
    // between directories. perform() retains it as a compatibility fallback.
    curl_easy_setopt(curl, CURLOPT_FTP_FILEMETHOD, static_cast<long>(CURLFTPMETHOD_SINGLECWD));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeout);
    curl_easy_setopt(curl, CURLOPT_SERVER_RESPONSE_TIMEOUT, kResponseTimeout);
    curl_easy_setopt(curl, CURLOPT_ACCEPTTIMEOUT_MS, kAcceptTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, kKeepAliveIdle);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, kKeepAliveInterval);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kLowSpeedTime);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &Impl::on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Impl::on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    if (cancel_check) {
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Impl::on_progress);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    }
    if (verbose) {
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
      curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, &Impl::on_debug);
      curl_easy_setopt(curl, CURLOPT_DEBUGDATA, this);
    }
  }

  CURLcode perform(bool retry_cwd = true) {
    CURLcode code = curl_easy_perform(curl);
    if (retry_cwd && code == CURLE_REMOTE_ACCESS_DENIED) {
      // CWD failed before any data was transferred. Walking the components
      // supports servers that reject a full path and creates missing parents
      // one at a time when FTP_CREATE_MISSING_DIRS is enabled.
      curl_easy_setopt(curl, CURLOPT_FTP_FILEMETHOD, static_cast<long>(CURLFTPMETHOD_MULTICWD));
      code = curl_easy_perform(curl);
    }
    if (code == CURLE_OK) {
      logged_in = true;
    }
    return code;
  }

  std::string describe(CURLcode code) const {
    std::string message = error_buffer[0] != '\0' ? std::string(error_buffer.data()) : curl_easy_strerror(code);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
      message.pop_back();
    }
    if (!last_reply.empty() && message.find(last_reply) == std::string::npos) {
      message += fmt::format(" (server said: {})", last_reply);
    }
    if (logged_in && is_data_connection_error(code)) {
      const char* other = config.mode == FtpMode::Passive ? "active" : "passive";
      message += config.mention_flags ? fmt::format(" - the data connection failed; try --mode {}", other)
                                      : fmt::format(" - the data connection failed; try {} mode", other);
    }
    return message;
  }

  [[noreturn]] void fail(const std::string& what, CURLcode code) const {
    throw FtpError(fmt::format("{}: {}", what, describe(code)), static_cast<int>(code));
  }

  // Runs raw commands (absolute paths only) on the control connection.
  CURLcode quote(const std::vector<std::string>& commands) {
    prepare(base_url + "/");
    SList list;
    for (const auto& command : commands) {
      list.append(command);
    }
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    // Commands already contain absolute paths; do not return to the login
    // directory afterwards (e.g. when falling back from MFMT to MDTM).
    curl_easy_setopt(curl, CURLOPT_FTP_FILEMETHOD, static_cast<long>(CURLFTPMETHOD_NOCWD));
    curl_easy_setopt(curl, CURLOPT_QUOTE, list.get());
    return perform(false);
  }
};

namespace {
std::once_flag g_curl_init;
}

FtpClient::FtpClient(FtpConfig config, Logger& log, bool verbose)
    : impl_(std::make_unique<Impl>(std::move(config), log, verbose)) {
  std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  impl_->curl = curl_easy_init();
  if (impl_->curl == nullptr) {
    throw FtpError("cannot initialize libcurl", CURLE_FAILED_INIT);
  }
  impl_->base_url = ftp_base_url(impl_->config.host, impl_->config.port);
}

FtpClient::~FtpClient() {
  if (impl_ && impl_->curl != nullptr) {
    curl_easy_cleanup(impl_->curl);
  }
}

void FtpClient::set_cancel_check(std::function<bool()> check) { impl_->cancel_check = std::move(check); }

std::string FtpClient::connect(unsigned attempts) {
  if (attempts == 0) {
    attempts = 1;
  }
  for (unsigned attempt = 1;; ++attempt) {
    if (attempt > 1) {
      // Check before preparing another request so a cancellation after the
      // previous failure cannot start another login.
      if (impl_->cancel_check && impl_->cancel_check()) {
        impl_->fail("connection cancelled", CURLE_ABORTED_BY_CALLBACK);
      }
      impl_->log.info("Retrying connection to {}:{} (attempt {}/{})", impl_->config.host, impl_->config.port,
                      attempt, attempts);
    }
    impl_->prepare(impl_->base_url + "/");
    SList quote;
    quote.append("*OPTS UTF8 ON");  // '*': ignore servers that do not know it.
    curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_QUOTE, quote.get());
    const CURLcode code = impl_->perform();
    if (code == CURLE_OK) {
      const char* entry = nullptr;
      curl_easy_getinfo(impl_->curl, CURLINFO_FTP_ENTRY_PATH, &entry);
      if (entry == nullptr || *entry == '\0') {
        impl_->log.warn("the server did not report its current directory (PWD); assuming \"/\"");
        return "/";
      }
      return entry;
    }
    const std::string error = fmt::format("cannot log in to {}:{}: {}", impl_->config.host, impl_->config.port,
                                          impl_->describe(code));
    if (code == CURLE_ABORTED_BY_CALLBACK || (impl_->cancel_check && impl_->cancel_check())) {
      throw FtpError(error, static_cast<int>(code));
    }
    if (attempt == attempts) {
      throw FtpError(fmt::format("{} (after {} attempt(s))", error, attempt), static_cast<int>(code));
    }
    impl_->log.warn("connection failed (attempt {}/{}): {}; retrying immediately", attempt, attempts, error);
  }
}

bool FtpClient::directory_exists(const std::string& dir) {
  impl_->prepare(ftp_url(impl_->base_url, dir, true));
  curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
  const CURLcode code = impl_->perform();
  if (code == CURLE_OK) {
    return true;
  }
  if (code == CURLE_REMOTE_ACCESS_DENIED) {  // CWD failed.
    return false;
  }
  impl_->fail(fmt::format("cannot check remote directory {}", dir), code);
}

void FtpClient::make_directory(const std::string& dir) {
  const CURLcode code = impl_->quote({"MKD " + dir});
  if (code != CURLE_OK) {
    impl_->fail(fmt::format("cannot create remote directory {}", dir), code);
  }
}

void FtpClient::ensure_directory(const std::string& dir) {
  impl_->prepare(ftp_url(impl_->base_url, dir, true));
  curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(impl_->curl, CURLOPT_FTP_CREATE_MISSING_DIRS, static_cast<long>(CURLFTP_CREATE_DIR_RETRY));
  const CURLcode code = impl_->perform();
  if (code != CURLE_OK) {
    impl_->fail(fmt::format("cannot create remote directory {}", dir), code);
  }
}

std::optional<std::vector<std::string>> FtpClient::list_names(const std::string& dir) {
  std::string listing;
  impl_->prepare(ftp_url(impl_->base_url, dir, true));
  impl_->listing = &listing;
  curl_easy_setopt(impl_->curl, CURLOPT_DIRLISTONLY, 1L);
  const CURLcode code = impl_->perform();
  impl_->listing = nullptr;
  if (code != CURLE_OK) {
    impl_->log.debug(fmt::format("NLST {} failed: {}", dir, impl_->describe(code)));
    return std::nullopt;
  }
  std::vector<std::string> names;
  size_t start = 0;
  while (start < listing.size()) {
    size_t end = listing.find('\n', start);
    if (end == std::string::npos) {
      end = listing.size();
    }
    std::string name(trim_eol(std::string_view(listing).substr(start, end - start)));
    // Some servers answer with paths; keep the last component.
    const size_t slash = name.rfind('/');
    if (slash != std::string::npos) {
      name.erase(0, slash + 1);
    }
    if (!name.empty() && name != "." && name != "..") {
      names.push_back(std::move(name));
    }
    start = end + 1;
  }
  return names;
}

RemoteFile FtpClient::stat_file(const std::string& path) {
  impl_->prepare(ftp_url(impl_->base_url, path, false));
  curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
  const CURLcode code = impl_->perform();
  RemoteFile file;
  if (code == CURLE_REMOTE_FILE_NOT_FOUND || code == CURLE_REMOTE_ACCESS_DENIED) {
    return file;
  }
  if (code != CURLE_OK) {
    impl_->fail(fmt::format("cannot query remote file {}", path), code);
  }
  file.exists = true;
  curl_off_t length = -1;
  if (curl_easy_getinfo(impl_->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK && length >= 0) {
    file.size = static_cast<uint64_t>(length);
  }
  return file;
}

bool FtpClient::delete_file(const std::string& path) {
  const CURLcode code = impl_->quote({"DELE " + path});
  if (code != CURLE_OK) {
    impl_->log.debug(fmt::format("DELE {} failed: {}", path, impl_->describe(code)));
  }
  return code == CURLE_OK;
}

UploadResult FtpClient::upload(const std::string& path, uint64_t size, int64_t mtime, const ReadFn& read,
                               const ProgressFn& progress, bool append) {
  using Timestamps = Impl::Timestamps;
  Impl& d = *impl_;
  d.prepare(ftp_url(d.base_url, path, false));
  d.read = &read;
  d.progress = &progress;

  CURL* curl = d.curl;
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_APPEND, append ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, &Impl::on_read);
  curl_easy_setopt(curl, CURLOPT_READDATA, &d);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
  curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, kUploadBufferSize);
  curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, static_cast<long>(CURLFTP_CREATE_DIR_RETRY));
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Impl::on_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &d);

  // Setting the time rides on the upload's control connection: libcurl only
  // sends POSTQUOTE commands after the server confirmed the complete file, so
  // CURLE_QUOTE_ERROR below means "uploaded, but the time was not set".
  const bool want_time = mtime > 0 && d.timestamps != Timestamps::Unsupported;
  const std::string stamp = want_time ? format_ftp_timestamp(mtime) : std::string();
  SList postquote;
  if (want_time) {
    postquote.append(fmt::format("{} {} {}", d.timestamps == Timestamps::Mdtm ? "MDTM" : "MFMT", stamp, path));
    curl_easy_setopt(curl, CURLOPT_POSTQUOTE, postquote.get());
  }

  const CURLcode code = d.perform();
  UploadResult result;
  result.remote_started = d.upload_started || code == CURLE_OK || (code == CURLE_QUOTE_ERROR && want_time);
  result.bytes_sent = d.sent;
  result.curl_code = static_cast<int>(code);
  const bool source_aborted = d.source_aborted;
  d.read = nullptr;
  d.progress = nullptr;

  if (code == CURLE_OK) {
    result.ok = true;
    result.bytes_sent = size;  // libcurl checked the count against INFILESIZE.
    if (want_time && d.timestamps == Timestamps::Unknown) {
      d.timestamps = Timestamps::Mfmt;
    }
    return result;
  }
  if (code == CURLE_QUOTE_ERROR && want_time) {
    result.ok = true;
    result.bytes_sent = size;
    if (d.timestamps == Timestamps::Unknown) {
      // No MFMT: try the vsftpd flavour, "MDTM <time> <path>".
      if (d.quote({fmt::format("MDTM {} {}", stamp, path)}) == CURLE_OK) {
        d.timestamps = Timestamps::Mdtm;
        return result;
      }
      d.timestamps = Timestamps::Unsupported;
    }
    result.timestamp_failed = true;
    return result;
  }
  result.aborted_by_source = source_aborted;
  result.error = d.describe(code);
  return result;
}

bool FtpClient::timestamps_unsupported() const { return impl_->timestamps == Impl::Timestamps::Unsupported; }

std::string FtpClient::url_for(const std::string& path) const { return impl_->base_url + path; }

std::string curl_version_string() {
  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  return info != nullptr && info->version != nullptr ? info->version : "unknown";
}

}  // namespace rarftp
