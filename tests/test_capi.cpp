// Smoke test of the C API through the shared library, using only rarftp.h.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "capi/rarftp.h"

namespace {

using namespace std::chrono_literals;

// A RAR5 archive holding one stored file, "hello.txt" ("hello\n").
constexpr unsigned char kTinyRar[] = {0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00, 0xc5, 0x1a, 0x33, 0x32,
                                      0x03, 0x01, 0x00, 0x00, 0xd2, 0xf7, 0x70, 0x6c, 0x17, 0x02, 0x02, 0x06,
                                      0x04, 0x06, 0xa4, 0x03, 0x20, 0x30, 0x3a, 0x36, 0x00, 0x01, 0x09, 0x68,
                                      0x65, 0x6c, 0x6c, 0x6f, 0x2e, 0x74, 0x78, 0x74, 0x68, 0x65, 0x6c, 0x6c,
                                      0x6f, 0x0a, 0x19, 0xb2, 0x3a, 0x35, 0x03, 0x05, 0x00, 0x00};

// The same file in a RAR5 archive whose headers are encrypted (password "secret").
constexpr unsigned char kEncryptedRar[] = {
    0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00, 0xe2, 0x5d, 0x07, 0xb0, 0x21, 0x04, 0x00, 0x00, 0x01,
    0x04, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x04, 0x1d, 0x34, 0x11, 0x97, 0x86, 0x2d, 0xf8, 0x8b, 0x0b, 0x73, 0x32, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
    0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xed, 0xc8, 0x78, 0xf8, 0x88, 0x80,
    0x86, 0x64, 0x4d, 0x0f, 0xab, 0xea, 0x82, 0x04, 0xbe, 0xb3, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xdd, 0x0b, 0xb7, 0x4e, 0x82, 0x3a, 0xd1, 0xe5,
    0x53, 0xe3, 0x93, 0x9c, 0x47, 0xdb, 0x67, 0xb9, 0x62, 0x75, 0x50, 0x7b, 0x56, 0xcf, 0x4a, 0xff, 0x75,
    0x4e, 0x19, 0x5d, 0xd8, 0xa9, 0xe4, 0x57, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x0a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xfb, 0xba, 0x81, 0x7c, 0xf2,
    0x50, 0x25, 0xfe, 0x2b, 0xc7, 0x7b, 0xe8, 0xc9, 0xdc, 0x96, 0x5f};

// Writes `bytes` to a temporary file that is removed on destruction.
class TempFile {
 public:
  TempFile(const char* name, const unsigned char* bytes, size_t size)
      : path_(std::filesystem::temp_directory_path() / name) {
    std::ofstream out(path_, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  }
  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

rarftp_job_config make_config(const std::string& archive, int port = 21) {
  rarftp_job_config config{};
  config.archive = archive.c_str();
  config.host = "127.0.0.1";
  config.port = port;
  config.directory = "/upload";
  return config;
}

std::string poll(rarftp_job* job, uint64_t cursor = 0) {
  char* raw = rarftp_job_poll(job, cursor);
  REQUIRE(raw != nullptr);
  std::string json(raw);
  rarftp_free(raw);
  return json;
}

// Polls until `json` contains `needle`; returns the last state either way.
std::string wait_for(rarftp_job* job, const std::string& needle, std::chrono::milliseconds timeout = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string json = poll(job);
  while (json.find(needle) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
    json = poll(job);
  }
  return json;
}

bool contains(const std::string& json, const char* text) { return json.find(text) != std::string::npos; }

// The "seq" numbers of the log lines, in order (the "result" section repeats
// the problems, so stop there).
std::vector<uint64_t> log_seqs(const std::string& json) {
  std::vector<uint64_t> seqs;
  const size_t end = json.find("\"result\":");
  size_t pos = json.find("\"log\":");
  while ((pos = json.find("\"seq\":", pos)) != std::string::npos && pos < end) {
    pos += 6;
    seqs.push_back(std::strtoull(json.c_str() + pos, nullptr, 10));
  }
  return seqs;
}

uint64_t log_next(const std::string& json) {
  const size_t pos = json.find("\"next\":");
  REQUIRE(pos != std::string::npos);
  return std::strtoull(json.c_str() + pos + 7, nullptr, 10);
}

}  // namespace

TEST_CASE("version string") {
  const char* version = rarftp_version();
  REQUIRE(version != nullptr);
  CHECK(std::strlen(version) > 0);
  CHECK(std::strncmp(version, "rarftp ", 7) == 0);
  CHECK(rarftp_version() == version);  // Static storage.
}

TEST_CASE("a missing config is refused and NULL handles are ignored") {
  CHECK(rarftp_job_start(nullptr) == nullptr);
  CHECK(rarftp_job_poll(nullptr, 0) == nullptr);
  rarftp_job_answer_password(nullptr, "x");
  rarftp_job_cancel(nullptr);
  rarftp_job_free(nullptr);
  rarftp_free(nullptr);
}

TEST_CASE("a missing archive ends as failed") {
  const std::string archive = (std::filesystem::temp_directory_path() / "rarftpcore-no-such.rar").string();
  rarftp_job_config config = make_config(archive);
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);

  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"phase\":\"finished\""));
  CHECK(contains(json, "\"status\":\"failed\""));
  CHECK(contains(json, "\"summary\":[\"FAILED: cannot read "));
  CHECK(contains(json, "\"prompt\":null"));
  CHECK(contains(json, "\"archive\":null"));
  CHECK(contains(json, "\"progress\":null"));
  CHECK(contains(json, "\"mode\":\"passive\""));
  CHECK(contains(json, "\"user\":\"anonymous\""));
  CHECK(contains(json, "\"cancelling\":false"));

  // The log is numbered and `next` works as a cursor.
  const std::vector<uint64_t> seqs = log_seqs(json);
  REQUIRE(seqs.size() >= 2);
  for (size_t i = 0; i < seqs.size(); ++i) {
    CHECK(seqs[i] == i);
  }
  CHECK(log_next(json) == seqs.size());
  CHECK(contains(json, "\"level\":\"info\""));
  CHECK(contains(json, "\"level\":\"error\""));

  const std::string rest = poll(job, log_next(json));
  CHECK(contains(rest, "\"lines\":[]"));
  CHECK(log_next(rest) == seqs.size());

  const std::string tail = poll(job, 1);
  const std::vector<uint64_t> tail_seqs = log_seqs(tail);
  REQUIRE(tail_seqs.size() == seqs.size() - 1);
  CHECK(tail_seqs.front() == 1);

  CHECK(contains(poll(job, 1000000), "\"lines\":[]"));  // A cursor from the future.
  rarftp_job_free(job);
}

TEST_CASE("a refused connection ends as failed after reading the archive") {
  const TempFile archive("rarftpcore-tiny.rar", kTinyRar, sizeof(kTinyRar));
  const std::string path = archive.path();
  rarftp_job_config config = make_config(path, 1);  // Nobody listens on port 1.
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);

  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"status\":\"failed\""));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
  CHECK(contains(json, "\"archive\":{\"name\":\"rarftpcore-tiny.rar\",\"files\":1,\"bytes\":6,"));
  CHECK(contains(json, "\"bytes_text\":\"6 B\",\"volumes\":1,\"solid\":false,\"encrypted\":false}"));
  CHECK(contains(json, "\"target\":null"));
  CHECK(contains(json, "\"progress\":null"));
  CHECK(contains(json, "Connecting to 127.0.0.1:1 (passive mode)"));
  rarftp_job_free(job);
}

TEST_CASE("the password prompt is asked again until it is right") {
  const TempFile archive("rarftpcore-enc.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  rarftp_job_config config = make_config(path, 1);
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);

  const char* first = "\"prompt\":{\"kind\":\"rar_password\",\"archive\":\"rarftpcore-enc.rar\",\"error\":null}";
  const char* again =
      "\"prompt\":{\"kind\":\"rar_password\",\"archive\":\"rarftpcore-enc.rar\",\"error\":\"Wrong password\"}";
  std::string json = wait_for(job, first);
  REQUIRE(contains(json, first));
  CHECK(contains(json, "\"phase\":\"reading\""));

  rarftp_job_answer_password(job, "wrong");
  json = wait_for(job, again);
  REQUIRE(contains(json, again));

  rarftp_job_answer_password(job, "secret");
  json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"prompt\":null"));
  CHECK(contains(json, "\"encrypted\":true}"));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));  // Past the listing, at the FTP login.

  rarftp_job_answer_password(job, "ignored");  // No prompt pending any more.
  rarftp_job_free(job);
}

TEST_CASE("declining the password prompt fails the job") {
  const TempFile archive("rarftpcore-enc-declined.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  rarftp_job_config config = make_config(path, 1);
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);

  REQUIRE(contains(wait_for(job, "\"prompt\":{"), "\"kind\":\"rar_password\""));
  rarftp_job_answer_password(job, nullptr);
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"status\":\"failed\""));
  CHECK(contains(json, "\"error\":\"the archive is encrypted and no password was entered\""));
  CHECK(contains(json, "\"archive\":null"));
  rarftp_job_free(job);
}

TEST_CASE("a configured password is used without asking") {
  const TempFile archive("rarftpcore-enc-given.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  rarftp_job_config config = make_config(path, 1);
  config.rar_password = "secret";
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);

  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"prompt\":null"));
  CHECK(contains(json, "\"encrypted\":true}"));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
  rarftp_job_free(job);
}

TEST_CASE("cancelling while the password prompt is open") {
  const TempFile archive("rarftpcore-enc-cancel.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  rarftp_job_config config = make_config(path, 1);
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);

  REQUIRE(contains(wait_for(job, "\"prompt\":{"), "\"kind\":\"rar_password\""));
  rarftp_job_cancel(job);
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"status\":\"cancelled\""));
  CHECK(contains(json, "\"prompt\":null"));
  rarftp_job_free(job);
}

TEST_CASE("freeing a job that waits for a password does not hang") {
  const TempFile archive("rarftpcore-enc-free.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  rarftp_job_config config = make_config(path, 1);
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);
  REQUIRE(contains(wait_for(job, "\"prompt\":{"), "\"kind\":\"rar_password\""));
  rarftp_job_free(job);
}

TEST_CASE("cancelling a running job and freeing it does not hang") {
  const TempFile archive("rarftpcore-tiny-cancel.rar", kTinyRar, sizeof(kTinyRar));
  const std::string path = archive.path();
  for (int i = 0; i < 20; ++i) {
    rarftp_job_config config = make_config(path, 1);
    rarftp_job* job = rarftp_job_start(&config);
    REQUIRE(job != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(i));
    rarftp_job_cancel(job);
    rarftp_job_cancel(job);
    rarftp_job_free(job);  // Must return: it waits for the controller thread.
  }
}

#ifndef _WIN32
TEST_CASE("cancelling while the server stays silent") {
  // A server that accepts the connection and never sends its greeting.
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(listener >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  REQUIRE(::listen(listener, 4) == 0);
  socklen_t length = sizeof(address);
  REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
  const int port = ntohs(address.sin_port);

  const TempFile archive("rarftpcore-tiny-silent.rar", kTinyRar, sizeof(kTinyRar));
  const std::string path = archive.path();
  rarftp_job_config config = make_config(path, port);
  rarftp_job* job = rarftp_job_start(&config);
  REQUIRE(job != nullptr);

  std::string json = wait_for(job, "Connecting to 127.0.0.1:");
  REQUIRE(contains(json, "\"phase\":\"connecting\""));
  std::this_thread::sleep_for(300ms);
  rarftp_job_cancel(job);
  CHECK(contains(poll(job), "\"cancelling\":true"));

  json = wait_for(job, "\"phase\":\"finished\"", 15s);  // libcurl polls our check about once a second.
  CHECK(contains(json, "\"phase\":\"finished\""));
  CHECK(contains(json, "\"status\":\"cancelled\""));
  CHECK(contains(json, "\"summary\":[\"Cancelled: 0 file(s), 0 B uploaded.\"]"));
  CHECK(contains(json, "\"cancelling\":false"));
  rarftp_job_free(job);
  ::close(listener);
}
#endif
