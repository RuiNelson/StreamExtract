// Smoke test of the C API through the shared library, using only streamextract.h.

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

#include "archive_fixtures.hpp"
#include "capi/streamextract.h"

namespace {

using namespace std::chrono_literals;

using fixtures::kTinyRar;

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

streamextract_job_config make_config(const std::string& archive, int port = 21) {
  streamextract_job_config config{};
  config.archive = archive.c_str();
  config.host = "127.0.0.1";
  config.port = port;
  config.directory = "/upload";
  return config;
}

std::string poll(streamextract_job* job, uint64_t cursor = 0) {
  char* raw = streamextract_job_poll(job, cursor);
  REQUIRE(raw != nullptr);
  std::string json(raw);
  streamextract_free(raw);
  return json;
}

// Polls until `json` contains `needle`; returns the last state either way.
std::string wait_for(streamextract_job* job, const std::string& needle, std::chrono::milliseconds timeout = 10s) {
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
  const char* version = streamextract_version();
  REQUIRE(version != nullptr);
  CHECK(std::strlen(version) > 0);
  CHECK(std::strncmp(version, "StreamExtract ", 14) == 0);
  CHECK(streamextract_version() == version);  // Static storage.
}

TEST_CASE("protocol selection rejects invalid values before reading the archive") {
  CHECK(streamextract_job_start_with_protocol(nullptr, 0, 0, STREAMEXTRACT_PROTOCOL_FTP, nullptr) == nullptr);
  const std::string path = "/nonexistent/invalid-protocol.rar";
  streamextract_job_config config = make_config(path, 21);
  for (int protocol : {-1, 4}) {
    streamextract_job* job = streamextract_job_start_with_protocol(&config, 0, 0, protocol, nullptr);
    REQUIRE(job != nullptr);
    const std::string json = wait_for(job, "\"phase\":\"finished\"");
    CHECK(contains(json, "\"error\":\"invalid transfer protocol\""));
    streamextract_job_free(job);
  }
}

TEST_CASE("a missing config is refused and NULL handles are ignored") {
  CHECK(streamextract_job_start(nullptr) == nullptr);
  CHECK(streamextract_job_start_with_options(nullptr, 1, 5) == nullptr);
  CHECK(streamextract_job_poll(nullptr, 0) == nullptr);
  streamextract_job_answer_password(nullptr, "x");
  streamextract_job_cancel(nullptr);
  streamextract_job_free(nullptr);
  streamextract_free(nullptr);
}

TEST_CASE("a missing archive ends as failed") {
  const std::string archive = (std::filesystem::temp_directory_path() / "streamextractcore-no-such.rar").string();
  streamextract_job_config config = make_config(archive);
  streamextract_job* job = streamextract_job_start(&config);
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
  streamextract_job_free(job);
}

TEST_CASE("a refused connection ends as failed after reading the archive") {
  const TempFile archive("streamextractcore-tiny.rar", kTinyRar, sizeof(kTinyRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);  // Nobody listens on port 1.
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"status\":\"failed\""));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
  CHECK(contains(json,
                 "\"archive\":{\"name\":\"streamextractcore-tiny.rar\",\"format\":\"RAR\",\"compression\":null,"));
  CHECK(contains(json, "\"files\":1,\"bytes\":6,"));
  CHECK(contains(json, "\"bytes_text\":\"6 B\",\"volumes\":1,\"solid\":false,\"encrypted\":false}"));
  CHECK(contains(json, "\"target\":null"));
  CHECK(contains(json, "\"progress\":null"));
  CHECK(contains(json, "Connecting to 127.0.0.1:1 (passive mode)"));
  CHECK(contains(json, "after 3 attempt(s)"));
  CHECK(contains(json, "attempt 2/3"));
  CHECK(contains(json, "attempt 3/3"));
  streamextract_job_free(job);
}

TEST_CASE("ZIP, 7z and tar archives are read too") {
  const TempFile zip("streamextractcore-tiny.zip", fixtures::kTinyZip, sizeof(fixtures::kTinyZip));
  const TempFile seven_zip("streamextractcore-tiny.7z", fixtures::kTiny7z, sizeof(fixtures::kTiny7z));
  const TempFile tar("streamextractcore-tiny.tar", fixtures::kTinyTar, sizeof(fixtures::kTinyTar));
  struct Case {
    std::string path;
    std::string name;
    std::string format;
  };
  for (const Case& c : {Case{zip.path(), "streamextractcore-tiny.zip", "ZIP"},
                        Case{seven_zip.path(), "streamextractcore-tiny.7z", "7z"},
                        Case{tar.path(), "streamextractcore-tiny.tar", "tar"}}) {
    CAPTURE(c.name);
    streamextract_job_config config = make_config(c.path, 1);
    streamextract_job* job = streamextract_job_start(&config);
    REQUIRE(job != nullptr);
    const std::string json = wait_for(job, "\"phase\":\"finished\"");
    CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));  // Past the listing, at the FTP login.
    const std::string archive = "\"archive\":{\"name\":\"" + c.name + "\",\"format\":\"" + c.format + "\",";
    CHECK(contains(json, archive.c_str()));
    CHECK(contains(json, "\"files\":1,\"bytes\":6,"));
    streamextract_job_free(job);
  }
}

TEST_CASE("a real exFAT image is reported through the GUI's JSON contract") {
  const std::string path = std::string(STREAMEXTRACT_TEST_FIXTURES) + "/volume.exfat";
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
  CHECK(contains(json, "\"archive\":{\"name\":\"volume.exfat\",\"format\":\"exFAT\",\"compression\":null,"));
  CHECK(contains(json, "\"volumes\":1,\"solid\":false,\"encrypted\":false"));
  CHECK(contains(json, "no checksum of the file contents"));
  CHECK(contains(json, "\"prompt\":null"));
  streamextract_job_free(job);
}

TEST_CASE("a compressed tar archive is read as it is uploaded") {
  const TempFile archive("streamextractcore-tiny.tar.gz", fixtures::kTinyTarGz, sizeof(fixtures::kTinyTarGz));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
  // Not listed first: its contents are only known once it has been read.
  CHECK(contains(json,
                 "\"format\":\"tar\",\"compression\":\"gzip\",\"files\":null,\"bytes\":null,"
                 "\"bytes_text\":null,"));
  CHECK(contains(json, "read as it is uploaded"));
  streamextract_job_free(job);
}

TEST_CASE("a wrong ZIP password is noticed while reading the archive") {
  const TempFile archive("streamextractcore-aes.zip", fixtures::kAesZip, sizeof(fixtures::kAesZip));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  const char* first =
      "\"prompt\":{\"kind\":\"archive_password\",\"archive\":\"streamextractcore-aes.zip\",\"error\":null}";
  const char* again =
      "\"prompt\":{\"kind\":\"archive_password\",\"archive\":\"streamextractcore-aes.zip\",\"error\":\"Wrong "
      "password\"}";
  REQUIRE(contains(wait_for(job, first), first));
  streamextract_job_answer_password(job, "wrong");
  REQUIRE(contains(wait_for(job, again), again));
  streamextract_job_answer_password(job, "secret");
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"format\":\"ZIP\""));
  CHECK(contains(json, "\"encrypted\":true}"));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
  streamextract_job_free(job);
}

TEST_CASE("the password prompt is asked again until it is right") {
  const TempFile archive("streamextractcore-enc.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  const char* first =
      "\"prompt\":{\"kind\":\"archive_password\",\"archive\":\"streamextractcore-enc.rar\",\"error\":null}";
  const char* again =
      "\"prompt\":{\"kind\":\"archive_password\",\"archive\":\"streamextractcore-enc.rar\",\"error\":\"Wrong "
      "password\"}";
  std::string json = wait_for(job, first);
  REQUIRE(contains(json, first));
  CHECK(contains(json, "\"phase\":\"reading\""));

  streamextract_job_answer_password(job, "wrong");
  json = wait_for(job, again);
  REQUIRE(contains(json, again));

  streamextract_job_answer_password(job, "secret");
  json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"prompt\":null"));
  CHECK(contains(json, "\"encrypted\":true}"));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));  // Past the listing, at the FTP login.

  streamextract_job_answer_password(job, "ignored");  // No prompt pending any more.
  streamextract_job_free(job);
}

TEST_CASE("declining the password prompt fails the job") {
  const TempFile archive("streamextractcore-enc-declined.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  REQUIRE(contains(wait_for(job, "\"prompt\":{"), "\"kind\":\"archive_password\""));
  streamextract_job_answer_password(job, nullptr);
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"status\":\"failed\""));
  CHECK(contains(json, "\"error\":\"the archive is encrypted and no password was entered\""));
  CHECK(contains(json, "\"archive\":null"));
  streamextract_job_free(job);
}

TEST_CASE("a configured password is used without asking") {
  const TempFile archive("streamextractcore-enc-given.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  config.archive_password = "secret";
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"prompt\":null"));
  CHECK(contains(json, "\"encrypted\":true}"));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
  streamextract_job_free(job);
}

TEST_CASE("cancelling while the password prompt is open") {
  const TempFile archive("streamextractcore-enc-cancel.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  REQUIRE(contains(wait_for(job, "\"prompt\":{"), "\"kind\":\"archive_password\""));
  streamextract_job_cancel(job);
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"status\":\"cancelled\""));
  CHECK(contains(json, "\"prompt\":null"));
  streamextract_job_free(job);
}

TEST_CASE("freeing a job that waits for a password does not hang") {
  const TempFile archive("streamextractcore-enc-free.rar", kEncryptedRar, sizeof(kEncryptedRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);
  REQUIRE(contains(wait_for(job, "\"prompt\":{"), "\"kind\":\"archive_password\""));
  streamextract_job_free(job);
}

TEST_CASE("cancelling a running job and freeing it does not hang") {
  const TempFile archive("streamextractcore-tiny-cancel.rar", kTinyRar, sizeof(kTinyRar));
  const std::string path = archive.path();
  for (int i = 0; i < 20; ++i) {
    streamextract_job_config config = make_config(path, 1);
    streamextract_job* job = streamextract_job_start(&config);
    REQUIRE(job != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(i));
    streamextract_job_cancel(job);
    streamextract_job_cancel(job);
    streamextract_job_free(job);  // Must return: it waits for the controller thread.
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

  const TempFile archive("streamextractcore-tiny-silent.rar", kTinyRar, sizeof(kTinyRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, port);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  std::string json = wait_for(job, "Connecting to 127.0.0.1:");
  REQUIRE(contains(json, "\"phase\":\"connecting\""));
  std::this_thread::sleep_for(300ms);
  streamextract_job_cancel(job);
  CHECK(contains(poll(job), "\"cancelling\":true"));

  json = wait_for(job, "\"phase\":\"finished\"", 15s);  // libcurl polls our check about once a second.
  CHECK(contains(json, "\"phase\":\"finished\""));
  CHECK(contains(json, "\"status\":\"cancelled\""));
  CHECK(contains(json, "\"summary\":[\"Cancelled: 0 file(s), 0 B uploaded.\"]"));
  CHECK(contains(json, "\"cancelling\":false"));
  streamextract_job_free(job);
  ::close(listener);
}
#endif

TEST_CASE("display units are selected per job while the original C API stays binary") {
  CHECK(streamextract_job_start_with_units(nullptr, 1) == nullptr);
  const std::string path = std::string(STREAMEXTRACT_TEST_FIXTURES) + "/volume.exfat";
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* si = streamextract_job_start_with_units(&config, 1);
  streamextract_job* binary = streamextract_job_start(&config);
  streamextract_job* explicit_binary = streamextract_job_start_with_units(&config, 0);
  REQUIRE(si != nullptr);
  REQUIRE(binary != nullptr);
  REQUIRE(explicit_binary != nullptr);
  const std::string si_json = wait_for(si, "\"phase\":\"finished\"");
  const std::string binary_json = wait_for(binary, "\"phase\":\"finished\"");
  const std::string explicit_json = wait_for(explicit_binary, "\"phase\":\"finished\"");
  CHECK(contains(si_json, "\"bytes\":6484635,\"bytes_text\":\"6.48 MB\""));
  CHECK(contains(si_json, "Archive: exFAT, 19 file(s), 6.48 MB"));
  CHECK(contains(binary_json, "\"bytes\":6484635,\"bytes_text\":\"6.18 MiB\""));
  CHECK(contains(binary_json, "Archive: exFAT, 19 file(s), 6.18 MiB"));
  CHECK(contains(explicit_json, "Archive: exFAT, 19 file(s), 6.18 MiB"));
  streamextract_job_free(si);
  streamextract_job_free(binary);
  streamextract_job_free(explicit_binary);
}
