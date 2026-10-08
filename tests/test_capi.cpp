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

using fixtures::kEncryptedRar;
using fixtures::kTinyRar;

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

TEST_CASE("folder picker lists implicit and empty directories in ZIP and streamed tar") {
  for (const auto& format : {std::string("zip"), std::string("tar.gz")}) {
    const bool zip = format == "zip";
    const std::string name = "streamextract-root-browser." + format;
    const TempFile file(name.c_str(), zip ? fixtures::kExtractionRootZip : fixtures::kExtractionRootTarGz,
                        zip ? sizeof(fixtures::kExtractionRootZip) : sizeof(fixtures::kExtractionRootTarGz));
    char* raw = streamextract_archive_directories(file.path().c_str(), nullptr);
    REQUIRE(raw != nullptr);
    const std::string json(raw);
    streamextract_free(raw);
    CHECK(json ==
          "{\"directories\":[\"a\",\"a/x\",\"a/y\",\"b\",\"b/nested\",\"b2\"],"
          "\"password_required\":false,\"error\":null}");
  }
}

TEST_CASE("folder picker reports password requests, wrong passwords and read errors") {
  const TempFile file("streamextract-root-browser-encrypted.rar", kEncryptedRar, sizeof(kEncryptedRar));
  for (const char* password : {static_cast<const char*>(nullptr), "wrong", "secret"}) {
    char* raw = streamextract_archive_directories(file.path().c_str(), password);
    REQUIRE(raw != nullptr);
    const std::string json(raw);
    streamextract_free(raw);
    if (password == nullptr) {
      CHECK(json == "{\"directories\":[],\"password_required\":true,\"error\":null}");
    } else if (std::strcmp(password, "wrong") == 0) {
      CHECK(json == "{\"directories\":[],\"password_required\":true,\"error\":\"Wrong password\"}");
    } else {
      CHECK(json == "{\"directories\":[],\"password_required\":false,\"error\":null}");
    }
  }
  char* raw = streamextract_archive_directories("/nonexistent/streamextract-root.zip", nullptr);
  REQUIRE(raw != nullptr);
  const std::string json(raw);
  streamextract_free(raw);
  CHECK(contains(json, "\"password_required\":false"));
  CHECK_FALSE(contains(json, "\"error\":null"));
}

TEST_CASE("extraction root is validated before any server connection") {
  const TempFile file("streamextract-extraction-root.zip", fixtures::kExtractionRootZip,
                      sizeof(fixtures::kExtractionRootZip));
  const std::string path = file.path();
  const auto config = make_config(path, 1);
  for (const char* root : {"missing", "b/xyz.bin", "../b"}) {
    streamextract_job* job = streamextract_job_start_with_extraction_root(
        &config, 0, 1, STREAMEXTRACT_PROTOCOL_FTP, nullptr, nullptr, root);
    REQUIRE(job != nullptr);
    const std::string json = wait_for(job, "\"phase\":\"finished\"");
    streamextract_job_free(job);
    CHECK(contains(json, "\"status\":\"failed\""));
    CHECK(contains(json, "extraction root"));
    CHECK(contains(json, "\"target\":null"));
    CHECK_FALSE(contains(json, "Connecting to"));
  }
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
  CHECK(streamextract_job_start_with_staging(nullptr, 0, 3, STREAMEXTRACT_PROTOCOL_FTP, nullptr, nullptr,
                                          nullptr, "/stage") == nullptr);
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
    CAPTURE(std::string(c.name));
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

TEST_CASE("PFS and UFS images are reported through the GUI's JSON contract") {
  for (const std::string name : {"raw.ffpfsc", "exfat.ffpfsc", "ufs.ffpfsc", "volume.ufs"}) {
    const std::string path = std::string(STREAMEXTRACT_TEST_FIXTURES) + "/" + name;
    auto config = make_config(path, 1);
    streamextract_job* job = streamextract_job_start(&config);
    REQUIRE(job != nullptr);
    const std::string json = wait_for(job, "\"phase\":\"finished\"");
    CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));
    CHECK(contains(json, name == "volume.ufs" ? "\"format\":\"UFS\",\"compression\":null"
                                              : "\"format\":\"PFS\",\"compression\":\"PFSC\""));
    CHECK(contains(json, name == "exfat.ffpfsc" ? "\"files\":19" : "\"files\":5"));
    CHECK(contains(json, "no checksum of the file contents"));
    CHECK(contains(json, "\"prompt\":null"));
    streamextract_job_free(job);
  }
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

TEST_CASE("a wrong password for RAR file data is noticed while reading the archive") {
  const TempFile archive("streamextractcore-data.rar", fixtures::kEncryptedDataRar,
                         sizeof(fixtures::kEncryptedDataRar));
  const std::string path = archive.path();
  streamextract_job_config config = make_config(path, 1);
  streamextract_job* job = streamextract_job_start(&config);
  REQUIRE(job != nullptr);

  const char* first =
      "\"prompt\":{\"kind\":\"archive_password\",\"archive\":\"streamextractcore-data.rar\",\"error\":null}";
  const char* again =
      "\"prompt\":{\"kind\":\"archive_password\",\"archive\":\"streamextractcore-data.rar\",\"error\":\"Wrong "
      "password\"}";
  REQUIRE(contains(wait_for(job, first), first));
  streamextract_job_answer_password(job, "wrong");
  REQUIRE(contains(wait_for(job, again), again));
  streamextract_job_answer_password(job, "secret");
  const std::string json = wait_for(job, "\"phase\":\"finished\"");
  CHECK(contains(json, "\"format\":\"RAR\""));
  CHECK(contains(json, "\"encrypted\":true}"));
  CHECK(contains(json, "\"error\":\"cannot log in to 127.0.0.1:1"));  // Past the listing, at the FTP login.
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
