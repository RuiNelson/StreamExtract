#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "archive.hpp"
#include "archive_fixtures.hpp"

using namespace rarftp;

namespace {

// A file in a temporary directory of its own, removed with it.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("rarftp-test-archive-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" +
             std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string write(const std::string& name, const unsigned char* data, size_t size) const {
    const std::filesystem::path file = path_ / name;
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.string();
  }
  template <size_t N>
  std::string write(const std::string& name, const unsigned char (&data)[N]) const {
    return write(name, data, N);
  }
  std::string write(const std::string& name, const std::string& data) const {
    return write(name, reinterpret_cast<const unsigned char*>(data.data()), data.size());
  }

 private:
  std::filesystem::path path_;
};

struct Read {
  std::vector<ArchiveEntry> entries;
  std::vector<std::string> contents;  // Of every entry, in order.
  ArchiveFormat format = ArchiveFormat::Rar;
  ArchiveFlags flags;
};

// Lists the archive, then tests every entry in a second pass, like rarftp does.
Read read_all(const std::string& path, std::optional<std::string> password = std::nullopt,
              int* prompts = nullptr) {
  ArchiveCallbacks callbacks;
  callbacks.on_password = [&]() -> std::optional<std::string> {
    if (prompts != nullptr) {
      ++*prompts;
    }
    return password;
  };
  Read read;
  {
    const std::unique_ptr<Archive> archive = open_archive(path, Archive::Mode::List, callbacks);
    read.format = archive->format();
    read.flags = archive->flags();
    ArchiveEntry entry;
    while (archive->next(entry)) {
      read.entries.push_back(entry);
      archive->skip();
    }
  }
  std::string current;
  callbacks.on_data = [&](const uint8_t* data, size_t size) {
    current.append(reinterpret_cast<const char*>(data), size);
    return true;
  };
  const std::unique_ptr<Archive> archive = open_archive(path, Archive::Mode::Extract, callbacks);
  ArchiveEntry entry;
  while (archive->next(entry)) {
    current.clear();
    archive->test();
    read.contents.push_back(current);
  }
  return read;
}

ArchiveError::Kind error_kind(const std::string& path, std::optional<std::string> password = std::nullopt) {
  try {
    read_all(path, std::move(password));
  } catch (const ArchiveError& error) {
    return error.kind();
  }
  FAIL("no error");
  return ArchiveError::Kind::Other;
}

std::string error_text(const std::string& path) {
  try {
    read_all(path);
  } catch (const ArchiveError& error) {
    return error.what();
  }
  return "no error";
}

}  // namespace

TEST_CASE("formats are recognized by their content, not their name") {
  const TempDir dir;
  CHECK(read_all(dir.write("zip.rar", fixtures::kTinyZip)).format == ArchiveFormat::Zip);
  CHECK(read_all(dir.write("7z.zip", fixtures::kTiny7z)).format == ArchiveFormat::SevenZip);
  CHECK(read_all(dir.write("tar.7z", fixtures::kTinyTar)).format == ArchiveFormat::Tar);
}

TEST_CASE("the parts of a split archive") {
  std::set<std::string> files;
  const auto exists = [&](const std::string& path) { return files.count(path) != 0; };

  files = {"/d/a.zip.001", "/d/a.zip.002", "/d/a.zip.003"};
  CHECK(split_archive_parts("/d/a.zip.001", exists) ==
        std::vector<std::string>{"/d/a.zip.001", "/d/a.zip.002", "/d/a.zip.003"});
  CHECK(split_archive_parts("/d/a.zip", exists) == std::vector<std::string>{"/d/a.zip"});
  CHECK_THROWS_WITH_AS(split_archive_parts("/d/a.zip.002", exists),
                       "a.zip.002 is not the first part of a split archive; pass a.zip.001", ArchiveError);

  files = {"/d/a.7z.001", "/d/a.7z.003"};
  CHECK_THROWS_WITH_AS(split_archive_parts("/d/a.7z.001", exists), "volume not found: /d/a.7z.002", ArchiveError);

  files = {"/d/a.7z.0001", "/d/a.7z.0002"};
  CHECK(split_archive_parts("/d/a.7z.0001", exists) == std::vector<std::string>{"/d/a.7z.0001", "/d/a.7z.0002"});

  // Not numbered parts: fewer than three digits, a directory named like one.
  files = {"/d/a.zip.01", "/d/a.zip.02"};
  CHECK(split_archive_parts("/d/a.zip.01", exists) == std::vector<std::string>{"/d/a.zip.01"});
  CHECK(split_archive_parts("/d/x.001/a.zip", exists) == std::vector<std::string>{"/d/x.001/a.zip"});
  // A later part whose first part is missing is read as it is.
  files = {"/d/b.zip.002"};
  CHECK(split_archive_parts("/d/b.zip.002", exists) == std::vector<std::string>{"/d/b.zip.002"});
}

TEST_CASE("a ZIP archive") {
  const TempDir dir;
  const Read read = read_all(dir.write("tiny.zip", fixtures::kTinyZip));
  CHECK(read.format == ArchiveFormat::Zip);
  CHECK_FALSE(read.flags.volume);
  CHECK_FALSE(read.flags.skip_decompresses);
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries[0].name == "hello.txt");
  CHECK(read.entries[0].kind == EntryKind::File);
  CHECK(read.entries[0].size == 6);
  CHECK(read.entries[0].mtime > 1767000000);  // 2026.
  CHECK_FALSE(read.entries[0].encrypted);
  CHECK(read.contents == std::vector<std::string>{"hello\n"});
}

TEST_CASE("a 7z archive") {
  const TempDir dir;
  const Read read = read_all(dir.write("tiny.7z", fixtures::kTiny7z));
  CHECK(read.format == ArchiveFormat::SevenZip);
  CHECK(read.flags.skip_decompresses);
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries[0].name == "hello.txt");
  CHECK(read.entries[0].size == 6);
  CHECK(read.contents == std::vector<std::string>{"hello\n"});
}

TEST_CASE("7z names are UTF-8 whatever the locale") {
  const TempDir dir;
  const Read read = read_all(dir.write("unicode.7z", fixtures::kUnicode7z));
  REQUIRE(!read.entries.empty());
  CHECK(read.entries.back().name == "ação/日本.txt");
  CHECK(read.contents.back() == "hi\n");
}

TEST_CASE("ZIP names without the UTF-8 flag") {
  const TempDir dir;
  // Code page 437, the format's default (and the OEM code page of US Windows).
  const Read cp437 = read_all(dir.write("cp437.zip", fixtures::kCp437Zip));
  REQUIRE(cp437.entries.size() == 1);
  CHECK(cp437.entries[0].name == "aço.txt");
  CHECK(cp437.contents == std::vector<std::string>{"hi\n"});
#ifndef _WIN32
  // UTF-8 without the flag, as macOS writes them (Windows decodes them with the OEM code page).
  const Read utf8 = read_all(dir.write("utf8.zip", fixtures::kUnflaggedUtf8Zip));
  REQUIRE(utf8.entries.size() == 1);
  CHECK(utf8.entries[0].name == "aço.txt");
#endif
}

TEST_CASE("an AES-encrypted ZIP archive") {
  const TempDir dir;
  const std::string path = dir.write("aes.zip", fixtures::kAesZip);

  int prompts = 0;
  const Read read = read_all(path, "secret", &prompts);
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries[0].encrypted);
  CHECK(read.contents == std::vector<std::string>{"hello\n"});
  CHECK(prompts == 2);  // Once per archive object: listing, then extracting.

  // The listing already decrypts the start of the first encrypted file.
  prompts = 0;
  ArchiveCallbacks callbacks;
  callbacks.on_password = [&]() -> std::optional<std::string> {
    ++prompts;
    return std::string("wrong");
  };
  const std::unique_ptr<Archive> archive = open_archive(path, Archive::Mode::List, callbacks);
  ArchiveEntry entry;
  REQUIRE(archive->next(entry));
  try {
    archive->skip();
    FAIL("a wrong password was accepted");
  } catch (const ArchiveError& error) {
    CHECK(error.kind() == ArchiveError::Kind::BadPassword);
  }
  CHECK(prompts == 1);  // Asked once, not again and again.

  CHECK(error_kind(path) == ArchiveError::Kind::MissingPassword);
}

TEST_CASE("checksum errors") {
  const TempDir dir;
  CHECK(error_text(dir.write("bad.zip", fixtures::kBadCrcZip)) == "corrupt data (checksum mismatch)");
  CHECK(error_text(dir.write("bad.7z", fixtures::kBadCrc7z)) == "corrupt data (checksum mismatch)");
}

TEST_CASE("split ZIP, 7z and tar archives") {
  const TempDir dir;
  struct Case {
    const char* name;
    const unsigned char* data;
    size_t size;
  };
  for (const Case& c : {Case{"a.zip", fixtures::kTinyZip, sizeof(fixtures::kTinyZip)},
                        Case{"a.7z", fixtures::kTiny7z, sizeof(fixtures::kTiny7z)},
                        Case{"a.tar", fixtures::kTinyTar, sizeof(fixtures::kTinyTar)}}) {
    CAPTURE(c.name);
    const unsigned char* data = c.data;
    const size_t size = c.size;
    const std::string base = std::string(c.name) + ".";
    const size_t third = size / 3;
    const std::string first = dir.write(base + "001", data, third);
    dir.write(base + "002", data + third, third);
    const std::string last = dir.write(base + "003", data + 2 * third, size - 2 * third);

    const Read read = read_all(first);
    CHECK(read.flags.volume);
    CHECK(read.flags.volume_count == 3);
    REQUIRE(read.entries.size() == 1);
    CHECK(read.contents == std::vector<std::string>{"hello\n"});

    CHECK(error_text(last) == base + "003 is not the first part of a split archive; pass " + base + "001");
  }
}

TEST_CASE("a tar archive") {
  const TempDir dir;
  const Read read = read_all(dir.write("tiny.tar", fixtures::kTinyTar));
  CHECK(read.format == ArchiveFormat::Tar);
  CHECK_FALSE(read.flags.checksums);
  CHECK_FALSE(read.flags.skip_decompresses);
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries[0].name == "hello.txt");
  CHECK(read.entries[0].kind == EntryKind::File);
  CHECK(read.entries[0].size == 6);
  CHECK(read.entries[0].mtime > 1767000000);  // 2026.
  CHECK(read.contents == std::vector<std::string>{"hello\n"});
  CHECK(read.flags.compression.empty());
  CHECK_FALSE(read.flags.stream_only);
}

TEST_CASE("compressed tar archives") {
  const TempDir dir;
  struct Case {
    const char* name;
    const unsigned char* data;
    size_t size;
    const char* compression;
  };
  for (const Case& c : {Case{"a.tar.gz", fixtures::kTinyTarGz, sizeof(fixtures::kTinyTarGz), "gzip"},
                        Case{"a.tar.bz2", fixtures::kTinyTarBz2, sizeof(fixtures::kTinyTarBz2), "bzip2"},
                        Case{"a.tar.xz", fixtures::kTinyTarXz, sizeof(fixtures::kTinyTarXz), "xz"},
                        Case{"a.tar.lzma", fixtures::kTinyTarLzma, sizeof(fixtures::kTinyTarLzma), "lzma"},
                        Case{"a.tar.zst", fixtures::kTinyTarZst, sizeof(fixtures::kTinyTarZst), "zstd"},
                        Case{"a.tar.lz4", fixtures::kTinyTarLz4, sizeof(fixtures::kTinyTarLz4), "lz4"}}) {
    CAPTURE(c.name);
    const Read read = read_all(dir.write(c.name, c.data, c.size));
    CHECK(read.format == ArchiveFormat::Tar);
    CHECK(read.flags.compression == c.compression);
    CHECK(read.flags.stream_only);
    CHECK(read.flags.skip_decompresses);
    CHECK(read.flags.size == c.size);
    REQUIRE(read.entries.size() == 1);
    CHECK(read.entries[0].name == "hello.txt");
    CHECK(read.contents == std::vector<std::string>{"hello\n"});
  }
}

TEST_CASE("files that are not archives") {
  const TempDir dir;
  const unsigned char text[] = "just some text, long enough to look like a file";
  CHECK(error_text(dir.write("text.rar", text)) == "not a RAR, ZIP, 7z or tar archive");
  // A ZIP signature with nothing after it.
  const unsigned char truncated[] = {'P', 'K', 3, 4, 0, 0, 0, 0};
  CHECK(error_text(dir.write("truncated.zip", truncated)) == "not a valid ZIP archive");
}
