#include "ftp_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <mutex>
#include <string_view>

#include <curl/curl.h>
#include <fmt/format.h>

#include "logger.hpp"
#include "util/remote_path.hpp"
#include "util/ssh_settings.hpp"
#include "util/text.hpp"

namespace streamextract {

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

std::optional<uint64_t> size_reply(std::string_view reply) {
  if (reply.size() <= 4 || reply.substr(0, 4) != "213 ") return std::nullopt;
  reply.remove_prefix(4);
  uint64_t size = 0;
  const auto parsed = std::from_chars(reply.data(), reply.data() + reply.size(), size);
  if (parsed.ec != std::errc() || parsed.ptr != reply.data() + reply.size() ||
      size > static_cast<uint64_t>(std::numeric_limits<curl_off_t>::max())) {
    return std::nullopt;
  }
  return size;
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

// SFTP quote commands are parsed by curl, with backslash escaping inside double quotes.
std::string sftp_quote_path(const std::string& path) {
  std::string quoted = "\"";
  for (char c : path) {
    if (c == '\\' || c == '\"') quoted += '\\';
    quoted += c;
  }
  return quoted + '\"';
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
  std::vector<std::string> private_keys;
  size_t private_key_index = 0;

  enum class Timestamps { Unknown, Mfmt, Mdtm, Unsupported };
  Timestamps timestamps = Timestamps::Unknown;

  // ftpsrv (PS4/PS5) answers SIZE for a SELF file with the size of the ELF
  // inside it, while APPE appends at the stored size. Its SELF command toggles
  // that conversion for one control connection; each new connection starts on.
  enum class Ftpsrv { No, Yes, WithoutSelf };
  Ftpsrv ftpsrv = Ftpsrv::No;
  bool self_conversion = false;
  bool self_conversion_warned = false;
  // A native file HEAD establishes binary TYPE in both curl's connection
  // cache and the server. Later SIZE-only requests may then omit REST 0.
  bool binary_size_ready = false;
  bool size_reply_compatible = true;

  // With mode_b: the transfer mode of the current control connection was
  // logged. Each new connection (seen by its 220 greeting) logs it again.
  bool transfer_mode_reported = false;
  bool mode_b_refusal_warned = false;
  std::optional<long> mc_connections_reported;
  bool mc_refusal_warned = false;

  // State of the request in progress, used by the callbacks.
  const ReadFn* read = nullptr;
  const ProgressFn* progress = nullptr;
  std::function<bool()> cancel_check;
  std::string* listing = nullptr;
  bool source_aborted = false;
  bool upload_started = false;
  uint64_t sent = 0;

  Impl(FtpConfig c, Logger& l, bool v) : config(std::move(c)), log(l), verbose(v) {}

  std::string path_url(const std::string& path, bool directory) const {
    std::string url = ftp_url(base_url, path, directory);
    if (is_ssh(config.protocol)) url.replace(base_url.size(), 4, "/");
    return url;
  }

  static size_t on_header(char* buffer, size_t size, size_t nitems, void* user) {
    auto* self = static_cast<Impl*>(user);
    const std::string_view line = trim_eol(std::string_view(buffer, size * nitems));
    if (is_reply_line(line)) {
      self->last_reply.assign(line);
      if (self->read != nullptr && (line.substr(0, 3) == "125" || line.substr(0, 3) == "150")) {
        self->upload_started = true;
      }
      if (line.substr(0, 3) == "220") {  // Greeting of a new control connection.
        if (self->ftpsrv == Ftpsrv::No && line.find("ftpsrv") != std::string_view::npos) {
          self->ftpsrv = Ftpsrv::Yes;
        }
        self->self_conversion = self->ftpsrv == Ftpsrv::Yes;
        self->binary_size_ready = false;
        self->size_reply_compatible = true;
        self->transfer_mode_reported = false;
        self->mc_connections_reported.reset();
      } else if (line.find("SELF transfer mode disabled") != std::string_view::npos) {
        self->self_conversion = false;
      } else if (line.find("SELF transfer mode enabled") != std::string_view::npos) {
        self->self_conversion = true;
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
    if (is_ssh(self->config.protocol)) self->upload_started = true;
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

  // FTP block mode (MODE B), from our curl fork (CURLOPT_FTP_TRANSFER_MODE and
  // CURLINFO_FTP_TRANSFER_MODE).

  // Called by prepare() for every request. With mode_b, curl negotiates MODE B
  // once per control connection, right after login, and stays in stream mode
  // if the server refuses it. Without it, MODE is never sent.
  void request_transfer_mode() {
    if (is_ssh(config.protocol)) return;
    curl_easy_setopt(curl, CURLOPT_FTP_TRANSFER_MODE, config.mode_b ? CURLFTPMODE_BLOCK : CURLFTPMODE_STREAM);
  }

  // Whether the control connection of the last successful request uses block
  // mode; std::nullopt if curl cannot tell. CURLINFO_FTP_TRANSFER_MODE is 0
  // both for stream mode and for "unknown": after a request that logged in, 0
  // means stream mode.
  std::optional<bool> block_mode_in_effect() const {
    long mode = CURLFTPMODE_STREAM;
    if (curl_easy_getinfo(curl, CURLINFO_FTP_TRANSFER_MODE, &mode) != CURLE_OK) return std::nullopt;
    return mode == CURLFTPMODE_BLOCK;
  }

  // With mode_b, logs the transfer mode once per control connection, after its
  // first successful request. A refusal is a warning the first time only.
  void report_transfer_mode() {
    if (!config.mode_b || is_ssh(config.protocol) || transfer_mode_reported) return;
    const std::optional<bool> block = block_mode_in_effect();
    if (!block) return;
    transfer_mode_reported = true;
    if (*block) {
      log.info("Transfer mode: block (MODE B); the data connection stays open between files");
    } else if (!mode_b_refusal_warned) {
      mode_b_refusal_warned = true;
      log.warn("the server refused block mode (MODE B); using stream mode (MODE S)");
    } else {
      log.info("Transfer mode: stream (MODE S); the server refused block mode (MODE B)");
    }
  }

  // Data requests alone report FTPMC: NOBODY requests return 0 without negotiating it.
  // Report again after a reconnect or when a server changes the connection count.
  void report_ftp_mc() {
    if (!config.ftp_mc_connections || is_ssh(config.protocol) || config.mode == FtpMode::Active ||
        (read == nullptr && listing == nullptr))
      return;
    long count = 0;
    if (curl_easy_getinfo(curl, CURLINFO_FTP_MC_CONNECTIONS, &count) != CURLE_OK ||
        mc_connections_reported == count)
      return;
    mc_connections_reported = count;
    if (count > 0) {
      log.info("FTPMC: {} parallel data connection(s) per file", count);
    } else if (!mc_refusal_warned) {
      mc_refusal_warned = true;
      log.warn("the server did not accept FTPMC; using one data connection");
    } else {
      log.info("FTPMC: using one data connection; the server did not accept FTPMC");
    }
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
    if (config.protocol == FtpProtocol::Ftps) {
      // Explicit FTPS upgrades ftp:// with AUTH TLS; ftps:// starts TLS immediately.
      // Require encrypted control and data channels on every operation, including retries.
      curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
      curl_easy_setopt(curl, CURLOPT_FTPSSLAUTH, static_cast<long>(CURLFTPAUTH_TLS));
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
      if (config.ca_certificate) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, config.ca_certificate->c_str());
      }
    }
    request_transfer_mode();
    if (!is_ssh(config.protocol) && config.mode == FtpMode::Passive) {
      curl_easy_setopt(curl, CURLOPT_FTP_MC_CONNECTIONS, static_cast<long>(config.ftp_mc_connections));
      curl_easy_setopt(curl, CURLOPT_FTP_MC_STRIPE, static_cast<long>(config.ftp_mc_stripe));
    }
    if (is_ssh(config.protocol)) {
      curl_easy_setopt(curl, CURLOPT_USERNAME, config.user.c_str());
      curl_easy_setopt(curl, CURLOPT_PASSWORD, config.password.c_str());
      long auth = (config.password_supplied || !config.password.empty()) ? CURLSSH_AUTH_PASSWORD : 0L;
      if (!private_keys.empty()) {
        auth |= CURLSSH_AUTH_PUBLICKEY;
        curl_easy_setopt(curl, CURLOPT_SSH_PRIVATE_KEYFILE, private_keys[private_key_index].c_str());
        curl_easy_setopt(curl, CURLOPT_SSH_PUBLIC_KEYFILE, "");  // Derive the public key from the private key.
        curl_easy_setopt(curl, CURLOPT_KEYPASSWD, config.private_key_passphrase.c_str());
      }
      curl_easy_setopt(curl, CURLOPT_SSH_AUTH_TYPES, auth);
      if (config.known_hosts) curl_easy_setopt(curl, CURLOPT_SSH_KNOWNHOSTS, config.known_hosts->c_str());
    } else if (config.user.empty()) {
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

  // `report`: log the transfer mode of a new control connection (connect()
  // leaves it to the next request, after "Logged in as").
  CURLcode perform(bool retry_cwd = true, bool report = true) {
    CURLcode code = curl_easy_perform(curl);
    if (!is_ssh(config.protocol) && retry_cwd && code == CURLE_REMOTE_ACCESS_DENIED) {
      // CWD failed before any data was transferred. Walking the components
      // supports servers that reject a full path and creates missing parents
      // one at a time when FTP_CREATE_MISSING_DIRS is enabled.
      curl_easy_setopt(curl, CURLOPT_FTP_FILEMETHOD, static_cast<long>(CURLFTPMETHOD_MULTICWD));
      code = curl_easy_perform(curl);
    }
    if (code == CURLE_OK) {
      logged_in = true;
      if (report) report_transfer_mode();
      report_ftp_mc();
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
    if (!is_ssh(config.protocol) && logged_in && is_data_connection_error(code)) {
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

  // Turns ftpsrv's SELF conversion off on the current control connection. A
  // connection error is left to the next request; a reconnection during the
  // command is seen in the greeting and answered by this same command.
  void disable_self_conversion() {
    for (int i = 0; i < 2 && self_conversion; ++i) {  // A toggle: check the reply.
      const CURLcode code = quote({"SELF"});
      if (code == CURLE_QUOTE_ERROR) {
        // Versions without the command do not convert either.
        log.debug(fmt::format("the server rejected SELF ({}); assuming it reports stored sizes", last_reply));
        ftpsrv = Ftpsrv::WithoutSelf;
        self_conversion = false;
      } else if (code != CURLE_OK) {
        return;
      } else if (!self_conversion) {
        log.debug("turned off the server's SELF to ELF conversion for this connection");
      }
    }
  }
};

namespace {
std::once_flag g_curl_init;
}

FtpClient::FtpClient(FtpConfig config, Logger& log, bool verbose)
    : impl_(std::make_unique<Impl>(std::move(config), log, verbose)) {
  std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (impl_->config.ftp_mc_connections > kMaxFtpMcConnections || impl_->config.ftp_mc_stripe > kMaxFtpMcStripe) {
    throw FtpError("invalid FTPMC connection count or stripe size", CURLE_BAD_FUNCTION_ARGUMENT);
  }
  if (is_ssh(impl_->config.protocol)) impl_->private_keys = prepare_ssh_settings(impl_->config);
  impl_->curl = curl_easy_init();
  if (impl_->curl == nullptr) {
    throw FtpError("cannot initialize libcurl", CURLE_FAILED_INIT);
  }
  impl_->base_url = ftp_base_url(impl_->config.host, impl_->config.port);
  if (is_ssh(impl_->config.protocol)) impl_->base_url.replace(0, 3, "sftp");
  if (impl_->config.protocol == FtpProtocol::Ftps && impl_->config.ftps_mode == FtpsMode::Implicit) {
    impl_->base_url.replace(0, 3, "ftps");
  }
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
    if (!is_ssh(impl_->config.protocol)) quote.append("*OPTS UTF8 ON");
    curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_QUOTE, quote.get());
    CURLcode code = impl_->perform(true, false);
    while (code == CURLE_LOGIN_DENIED && impl_->private_key_index + 1 < impl_->private_keys.size()) {
      ++impl_->private_key_index;
      impl_->prepare(impl_->base_url + "/");
      curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
      code = impl_->perform(true, false);
    }
    if (code == CURLE_OK) {
      const char* entry = nullptr;
      curl_easy_getinfo(impl_->curl, CURLINFO_FTP_ENTRY_PATH, &entry);
      if (entry == nullptr || *entry == '\0') {
        impl_->log.warn("the server did not report its current directory (PWD); assuming \"/\"");
        return "/";
      }
      return entry;
    }
    const std::string error =
        fmt::format("cannot log in to {}:{}: {}", impl_->config.host, impl_->config.port, impl_->describe(code));
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
  impl_->prepare(impl_->path_url(dir, true));
  if (is_ssh(impl_->config.protocol))
    curl_easy_setopt(impl_->curl, CURLOPT_DIRLISTONLY, 1L);
  else
    curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
  const CURLcode code = impl_->perform();
  if (code == CURLE_OK) {
    return true;
  }
  if (code == CURLE_REMOTE_ACCESS_DENIED || code == CURLE_REMOTE_FILE_NOT_FOUND) {
    return false;
  }
  impl_->fail(fmt::format("cannot check remote directory {}", dir), code);
}

void FtpClient::make_directory(const std::string& dir) {
  const CURLcode code =
      impl_->quote({is_ssh(impl_->config.protocol) ? "mkdir " + sftp_quote_path(dir) : "MKD " + dir});
  if (code != CURLE_OK) {
    impl_->fail(fmt::format("cannot create remote directory {}", dir), code);
  }
}

void FtpClient::ensure_directory(const std::string& dir) {
  if (is_ssh(impl_->config.protocol)) {
    if (directory_exists(dir)) return;
    const auto parent = remote_parent(dir);
    if (parent != dir) ensure_directory(parent);
    try {
      make_directory(dir);
    } catch (const FtpError&) {
      if (!directory_exists(dir)) throw;
    }
    return;
  }
  impl_->prepare(impl_->path_url(dir, true));
  curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(impl_->curl, CURLOPT_FTP_CREATE_MISSING_DIRS, static_cast<long>(CURLFTP_CREATE_DIR_RETRY));
  const CURLcode code = impl_->perform();
  if (code != CURLE_OK) {
    impl_->fail(fmt::format("cannot create remote directory {}", dir), code);
  }
}

std::optional<std::vector<std::string>> FtpClient::list_names(const std::string& dir, bool include_hidden) {
  // curl requests ASCII TYPE for a listing. Establish binary TYPE with a
  // native file request again before using SIZE-only queries.
  impl_->binary_size_ready = false;
  std::string listing;
  impl_->prepare(impl_->path_url(dir, true));
  impl_->listing = &listing;
  curl_easy_setopt(impl_->curl, CURLOPT_DIRLISTONLY, 1L);
  if (include_hidden && !is_ssh(impl_->config.protocol)) {
    curl_easy_setopt(impl_->curl, CURLOPT_CUSTOMREQUEST, "NLST -a");
  }
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
  CURLcode code = CURLE_OK;
  const std::string name = remote_basename(path);
  // Keep curl's URL validation for invalid paths instead of inserting control
  // characters into a quoted command, even for direct callers of this method.
  const bool valid_name = !path.empty() && path.back() != '/' && !name.empty() &&
                          std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20; });
  if (!is_ssh(impl_->config.protocol) && impl_->binary_size_ready && valid_name) {
    impl_->disable_self_conversion();
    if (impl_->binary_size_ready) {
      // The parent-directory URL preserves curl's CWD handling and fallback.
      // With no filename, NOBODY sends neither SIZE nor REST; POSTQUOTE asks
      // for the exact binary size without another range-support round trip.
      impl_->prepare(impl_->path_url(remote_parent(path), true));
      SList postquote;
      postquote.append("*SIZE " + name);
      curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
      curl_easy_setopt(impl_->curl, CURLOPT_POSTQUOTE, postquote.get());
      code = impl_->perform();
      if (code == CURLE_OK && impl_->binary_size_ready && !impl_->self_conversion) {
        const std::string_view reply = impl_->last_reply;
        if (reply.size() >= 4 && reply.substr(0, 4) == "550 ") return {};
        if (reply.size() >= 4 && reply.substr(0, 4) == "213 ") {
          if (const auto size = size_reply(reply)) return {true, size};
          // curl accepts some non-standard SIZE replies. Let its native
          // parser handle those, and keep using it on this connection.
          impl_->size_reply_compatible = false;
        } else {
          return {true, std::nullopt};  // SIZE is unavailable, as with a native HEAD.
        }
      } else if (code != CURLE_OK) {
        impl_->binary_size_ready = false;
        if (code == CURLE_REMOTE_FILE_NOT_FOUND || code == CURLE_REMOTE_ACCESS_DENIED) return {};
        // POSTQUOTE runs after curl's normal reconnect/retry phase. A closed
        // cached connection can therefore fail here instead of being retried
        // by curl; the native read-only query safely reestablishes the session.
        if (code != CURLE_SEND_ERROR && code != CURLE_RECV_ERROR && code != CURLE_GOT_NOTHING)
          impl_->fail(fmt::format("cannot query remote file {}", path), code);
      }
      // A reconnect invalidates binary TYPE and may enable ftpsrv SELF again.
      // Ignore that reply and perform the same native query as the first file.
    }
  }
  impl_->binary_size_ready = false;
  // A request that reconnects reaches a new ftpsrv session, where the SELF
  // conversion is on again: turn it off and ask once more.
  for (int pass = 0; pass < 2; ++pass) {
    impl_->disable_self_conversion();
    impl_->prepare(impl_->path_url(path, false));
    curl_easy_setopt(impl_->curl, CURLOPT_NOBODY, 1L);
    code = impl_->perform();
    if (!impl_->self_conversion) break;
  }
  RemoteFile file;
  if (code == CURLE_REMOTE_FILE_NOT_FOUND || code == CURLE_REMOTE_ACCESS_DENIED) {
    return file;
  }
  if (code != CURLE_OK) {
    impl_->fail(fmt::format("cannot query remote file {}", path), code);
  }
  file.exists = true;
  if (impl_->self_conversion) {
    // The size may be that of a converted SELF: unknown sizes are uploaded from the beginning.
    if (!impl_->self_conversion_warned) {
      impl_->self_conversion_warned = true;
      impl_->log.warn(
          "cannot turn off the server's SELF to ELF conversion; existing files will be uploaded again from the "
          "beginning");
    }
    return file;
  }
  if (!is_ssh(impl_->config.protocol)) impl_->binary_size_ready = valid_name && impl_->size_reply_compatible;
  curl_off_t length = -1;
  if (curl_easy_getinfo(impl_->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK && length >= 0) {
    file.size = static_cast<uint64_t>(length);
  }
  return file;
}

bool FtpClient::delete_file(const std::string& path) {
  const CURLcode code =
      impl_->quote({is_ssh(impl_->config.protocol) ? "rm " + sftp_quote_path(path) : "DELE " + path});
  if (code != CURLE_OK) {
    impl_->log.debug(fmt::format("cannot delete {}: {}", path, impl_->describe(code)));
  }
  return code == CURLE_OK;
}

void FtpClient::move_path(const std::string& source, const std::string& destination) {
  const CURLcode code = impl_->quote(
      is_ssh(impl_->config.protocol)
          ? std::vector<std::string>{"rename " + sftp_quote_path(source) + " " + sftp_quote_path(destination)}
          : std::vector<std::string>{"RNFR " + source, "RNTO " + destination});
  if (code != CURLE_OK) {
    impl_->fail(fmt::format("cannot move staged path {} to {}", source, destination), code);
  }
}

bool FtpClient::remove_directory(const std::string& dir) {
  const CURLcode code =
      impl_->quote({is_ssh(impl_->config.protocol) ? "rmdir " + sftp_quote_path(dir) : "RMD " + dir});
  if (code != CURLE_OK) {
    impl_->log.debug(fmt::format("cannot remove staging directory {}: {}", dir, impl_->describe(code)));
  }
  return code == CURLE_OK;
}

UploadResult FtpClient::upload(const std::string& path, uint64_t size, int64_t mtime, const ReadFn& read,
                               const ProgressFn& progress, bool append) {
  using Timestamps = Impl::Timestamps;
  Impl& d = *impl_;
  d.prepare(d.path_url(path, false));
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
    if (is_ssh(d.config.protocol)) {
      const auto date = fmt::format(
          "{} {} {} {}:{}:{} GMT", stamp.substr(6, 2),
          std::array<const char*, 12>{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov",
                                      "Dec"}[static_cast<size_t>(std::stoi(stamp.substr(4, 2)) - 1)],
          stamp.substr(0, 4), stamp.substr(8, 2), stamp.substr(10, 2), stamp.substr(12, 2));
      postquote.append("mtime " + sftp_quote_path(date) + " " + sftp_quote_path(path));
    } else {
      postquote.append(fmt::format("{} {} {}", d.timestamps == Timestamps::Mdtm ? "MDTM" : "MFMT", stamp, path));
    }
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
    if (is_ssh(d.config.protocol)) {
      d.timestamps = Timestamps::Unsupported;
    } else if (d.timestamps == Timestamps::Unknown) {
      // No MFMT: try the vsftpd flavor, "MDTM <time> <path>".
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

}  // namespace streamextract
