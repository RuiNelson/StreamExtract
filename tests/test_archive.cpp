#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#ifdef _WIN32
#include <locale.h>
#endif

#include "archive.hpp"
#include "archive_fixtures.hpp"
#include "temp_dir.hpp"

using namespace streamextract;

namespace {

using test_support::TempDir;

struct Read {
  std::vector<ArchiveEntry> entries;
  std::vector<std::string> contents;  // Of every entry, in order.
  ArchiveFormat format = ArchiveFormat::Rar;
  ArchiveFlags flags;
};

// Lists the archive, then tests every entry in a second pass, like streamextract does.
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

// Tests only the `wanted` entries, skipping the others, like a re-run does.
std::map<std::string, std::string> extract_only(const std::string& path, const std::set<std::string>& wanted) {
  std::map<std::string, std::string> contents;
  std::string current;
  ArchiveCallbacks callbacks;
  callbacks.on_data = [&](const uint8_t* data, size_t size) {
    current.append(reinterpret_cast<const char*>(data), size);
    return true;
  };
  const std::unique_ptr<Archive> archive = open_archive(path, Archive::Mode::Extract, callbacks);
  ArchiveEntry entry;
  while (archive->next(entry)) {
    if (wanted.count(entry.name) == 0) {
      archive->skip();
      continue;
    }
    current.clear();
    archive->test();
    contents[entry.name] = current;
  }
  return contents;
}

}  // namespace

TEST_CASE("formats are recognized by their content, not their name") {
  const TempDir dir;
  CHECK(read_all(dir.write("rar.zip", fixtures::kTinyRar)).format == ArchiveFormat::Rar);
  CHECK(read_all(dir.write("zip.rar", fixtures::kTinyZip)).format == ArchiveFormat::Zip);
  CHECK(read_all(dir.write("7z.zip", fixtures::kTiny7z)).format == ArchiveFormat::SevenZip);
  CHECK(read_all(dir.write("tar.7z", fixtures::kTinyTar)).format == ArchiveFormat::Tar);
}

#ifdef _WIN32
TEST_CASE("opening archives preserves a Windows locale with a non-ASCII name") {
  // Restore with the wide API even if an assertion fails. The narrow API
  // interprets the saved name using whichever code page is active later.
  struct RestoreLocale {
    int mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    std::wstring name = _wsetlocale(LC_CTYPE, nullptr);
    ~RestoreLocale() {
      _wsetlocale(LC_CTYPE, name.c_str());
      _configthreadlocale(mode);
    }
  } restore;

  REQUIRE(_wsetlocale(LC_CTYPE, L"Norwegian Bokm\u00e5l_Norway.1252") != nullptr);
  const std::wstring expected = _wsetlocale(LC_CTYPE, nullptr);
  REQUIRE(expected.find(L'\u00e5') != std::wstring::npos);

  const TempDir dir;
  for (const std::string& path : {dir.write("tiny.rar", fixtures::kTinyRar),
                                  dir.write("tiny.7z", fixtures::kTiny7z),
                                  dir.write("tiny.zip", fixtures::kTinyZip),
                                  dir.write("tiny.tar", fixtures::kTinyTar)}) {
    CAPTURE(path);
    const Read read = read_all(path);
    REQUIRE(read.entries.size() == 1);
    CHECK(read.entries[0].name == "hello.txt");
    CHECK(read.contents == std::vector<std::string>{"hello\n"});
    CHECK(std::wstring(_wsetlocale(LC_CTYPE, nullptr)) == expected);
    CHECK(_configthreadlocale(0) == _ENABLE_PER_THREAD_LOCALE);
  }
}
#endif

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
  CHECK_FALSE(read.flags.solid);
  CHECK_FALSE(read.flags.skip_decompresses);
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries[0].name == "hello.txt");
  CHECK(read.entries[0].size == 6);
  CHECK(read.entries[0].mtime > 1767000000);  // 2026.
  CHECK_FALSE(read.entries[0].encrypted);
  CHECK(read.contents == std::vector<std::string>{"hello\n"});
}

TEST_CASE("solid and non-solid 7z archives") {
  const TempDir dir;
  const std::string solid = dir.write("solid.7z", fixtures::kSolid7z);
  const Read read = read_all(solid);
  CHECK(read.flags.solid);
  CHECK(read.flags.skip_decompresses);
  REQUIRE(read.entries.size() == 5);
  CHECK(read.entries[0].name == "dir");
  CHECK(read.entries[0].kind == EntryKind::Directory);
  CHECK(read.entries[1].name == "empty.txt");
  CHECK(read.entries[1].kind == EntryKind::File);
  CHECK(read.entries[1].size == 0);
  CHECK(read.entries[4].name == "dir/c.txt");
  CHECK(read.contents == std::vector<std::string>{"", "", "first\n", "second\n", "third\n"});
  // Files skipped before the wanted ones are decompressed on the way.
  CHECK(extract_only(solid, {"b.txt", "dir/c.txt"}) ==
        std::map<std::string, std::string>{{"b.txt", "second\n"}, {"dir/c.txt", "third\n"}});
  CHECK(extract_only(solid, {"dir/c.txt"}) == std::map<std::string, std::string>{{"dir/c.txt", "third\n"}});

  const std::string blocks = dir.write("nonsolid.7z", fixtures::kNonSolid7z);
  const Read nonsolid = read_all(blocks);
  CHECK_FALSE(nonsolid.flags.solid);
  CHECK_FALSE(nonsolid.flags.skip_decompresses);
  CHECK(nonsolid.contents == std::vector<std::string>{"first\n", "second\n"});
  CHECK(extract_only(blocks, {"b.txt"}) == std::map<std::string, std::string>{{"b.txt", "second\n"}});
}

TEST_CASE("7z compression methods and filters") {
  const TempDir dir;
  struct Case {
    const char* name;
    const unsigned char* data;
    size_t size;
    const char* password;
  };
  // BZip2, Deflate and Zstandard are not in 7-Zip's SDK: streamextract's own decoders.
  for (const Case& c : {Case{"ppmd.7z", fixtures::kPpmd7z, sizeof(fixtures::kPpmd7z), nullptr},
                        Case{"delta.7z", fixtures::kDelta7z, sizeof(fixtures::kDelta7z), nullptr},
                        Case{"arm64.7z", fixtures::kArm647z, sizeof(fixtures::kArm647z), nullptr},
                        Case{"copy.7z", fixtures::kCopy7z, sizeof(fixtures::kCopy7z), nullptr},
                        Case{"bzip2.7z", fixtures::kBzip27z, sizeof(fixtures::kBzip27z), nullptr},
                        Case{"deflate.7z", fixtures::kDeflate7z, sizeof(fixtures::kDeflate7z), nullptr},
                        Case{"zstd.7z", fixtures::kZstd7z, sizeof(fixtures::kZstd7z), nullptr},
                        Case{"aes-bzip2.7z", fixtures::kAesBzip27z, sizeof(fixtures::kAesBzip27z), "secret"},
                        Case{"aes-deflate.7z", fixtures::kAesDeflate7z, sizeof(fixtures::kAesDeflate7z), "secret"}}) {
    CAPTURE(std::string(c.name));
    const std::optional<std::string> password =
        c.password != nullptr ? std::optional<std::string>(c.password) : std::nullopt;
    const Read read = read_all(dir.write(c.name, c.data, c.size), password);
    CHECK(read.format == ArchiveFormat::SevenZip);
    REQUIRE(read.entries.size() == 1);
    CHECK(read.entries[0].name == "hello.txt");
    CHECK(read.contents == std::vector<std::string>{"hello\n"});
  }

  const std::string zstd = dir.write("zstd-solid.7z", fixtures::kZstdSolid7z);
  const Read solid = read_all(zstd);
  CHECK(solid.flags.solid);
  CHECK(solid.contents == std::vector<std::string>{"first\n", "second\n", "third\n"});
  CHECK(extract_only(zstd, {"dir/c.txt"}) == std::map<std::string, std::string>{{"dir/c.txt", "third\n"}});

  // Found while reading the archive, before anything is uploaded.
  CHECK(error_text(dir.write("deflate64.7z", fixtures::kDeflate647z)) ==
        "unsupported 7z compression method: Deflate64");
}

TEST_CASE("encrypted 7z archives") {
  const TempDir dir;
  struct Case {
    const char* name;
    const unsigned char* data;
    size_t size;
    bool encrypted_headers;
  };
  for (const Case& c : {Case{"aes.7z", fixtures::kAes7z, sizeof(fixtures::kAes7z), false},
                        Case{"aes-headers.7z", fixtures::kAesHeaders7z, sizeof(fixtures::kAesHeaders7z), true}}) {
    CAPTURE(std::string(c.name));
    const std::string path = dir.write(c.name, c.data, c.size);

    int prompts = 0;
    const Read read = read_all(path, "secret", &prompts);
    CHECK(read.flags.encrypted_headers == c.encrypted_headers);
    REQUIRE(read.entries.size() == 1);
    CHECK(read.entries[0].name == "hello.txt");
    CHECK(read.entries[0].encrypted);
    CHECK(read.contents == std::vector<std::string>{"hello\n"});
    CHECK(prompts == 2);  // Once per archive object: listing, then extracting.

    // A wrong password shows up while listing: decrypting the names, or the
    // start of the first encrypted file.
    prompts = 0;
    ArchiveCallbacks callbacks;
    callbacks.on_password = [&]() -> std::optional<std::string> {
      ++prompts;
      return std::string("wrong");
    };
    try {
      const std::unique_ptr<Archive> archive = open_archive(path, Archive::Mode::List, callbacks);
      ArchiveEntry entry;
      while (archive->next(entry)) {
        archive->skip();
      }
      FAIL("a wrong password was accepted");
    } catch (const ArchiveError& error) {
      CHECK(error.kind() == ArchiveError::Kind::BadPassword);
    }
    CHECK(prompts == 1);

    CHECK(error_kind(path) == ArchiveError::Kind::MissingPassword);
  }

  // A block per file: every block has its own decoder.
  const std::string blocks = dir.write("aes-nonsolid.7z", fixtures::kAesNonSolid7z);
  CHECK(read_all(blocks, "secret").contents == std::vector<std::string>{"first\n", "second\n"});
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
  // Code page 437, the format's default.
  const Read cp437 = read_all(dir.write("cp437.zip", fixtures::kCp437Zip));
  REQUIRE(cp437.entries.size() == 1);
  CHECK(cp437.entries[0].name == "aço.txt");
  CHECK(cp437.contents == std::vector<std::string>{"hi\n"});
  // UTF-8 without the flag, as macOS writes them.
  const Read utf8 = read_all(dir.write("utf8.zip", fixtures::kUnflaggedUtf8Zip));
  REQUIRE(utf8.entries.size() == 1);
  CHECK(utf8.entries[0].name == "aço.txt");
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
    CAPTURE(std::string(c.name));
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

TEST_CASE("a tar whose last member is a ZIP is read as tar") {
  const TempDir dir;
  const Read read = read_all(dir.write("zip-last.tar", fixtures::kTarEndingWithZip));
  CHECK(read.format == ArchiveFormat::Tar);
  REQUIRE(read.entries.size() == 2);
  CHECK(read.entries[0].name == "dir/a-readme.txt");
  CHECK(read.entries[1].name == "dir/z-last.zip");
  CHECK(read.contents[0] == "hello\n");
  CHECK(read.contents[1].size() == 131);
  CHECK(read.contents[1].substr(0, 2) == "PK");
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
    CAPTURE(std::string(c.name));
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
  CHECK(error_text(dir.write("text.rar", text)) == "not a RAR, ZIP, 7z or tar archive, or an exFAT, PFS or UFS image");
  // A ZIP signature with nothing after it.
  const unsigned char truncated[] = {'P', 'K', 3, 4, 0, 0, 0, 0};
  CHECK(error_text(dir.write("truncated.zip", truncated)) == "not a valid ZIP archive");
}

TEST_CASE("format names") {
  CHECK(std::string(format_name(ArchiveFormat::Rar)) == "RAR");
  CHECK(std::string(format_name(ArchiveFormat::Zip)) == "ZIP");
  CHECK(std::string(format_name(ArchiveFormat::SevenZip)) == "7z");
  CHECK(std::string(format_name(ArchiveFormat::Tar)) == "tar");
  CHECK(std::string(format_name(ArchiveFormat::Exfat)) == "exFAT");
  CHECK(std::string(format_name(ArchiveFormat::Pfs)) == "PFS");
  CHECK(std::string(format_name(ArchiveFormat::Ufs)) == "UFS");
}

TEST_CASE("tar entry kinds, empty files and names without a declared charset") {
  const TempDir dir;
  const Read read = read_all(dir.write("kinds.tar", fixtures::kKindsTar));
  CHECK(read.format == ArchiveFormat::Tar);
  CHECK_FALSE(read.flags.checksums);
  REQUIRE(read.entries.size() == 8);
  const std::vector<std::pair<std::string, EntryKind>> expected = {
      {"d/", EntryKind::Directory},    {"d/f.txt", EntryKind::File},    {"d/empty", EntryKind::File},
      {"d/link", EntryKind::Symlink},  {"d/hard", EntryKind::Hardlink}, {"d/fifo", EntryKind::Special},
      {"d/ação.txt", EntryKind::File},  // Valid UTF-8 is kept.
      {"d/café.txt", EntryKind::File},  // Anything else is Latin-1.
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    CAPTURE(i);
    CHECK(read.entries[i].name == expected[i].first);
    CHECK(read.entries[i].kind == expected[i].second);
    CHECK(read.entries[i].mtime == 1700000000);
    CHECK_FALSE(read.entries[i].encrypted);
  }
  CHECK(read.entries[1].size == 5);
  CHECK(read.entries[2].size == 0);
  CHECK(read.contents == std::vector<std::string>{"", "data\n", "", "", "", "", "utf8\n", "latin1\n"});
}

TEST_CASE("ZIP directories, empty files and symbolic links") {
  const TempDir dir;
  const Read read = read_all(dir.write("kinds.zip", fixtures::kKindsZip));
  CHECK(read.format == ArchiveFormat::Zip);
  REQUIRE(read.entries.size() == 4);
  CHECK(read.entries[0].name == "d/");
  CHECK(read.entries[0].kind == EntryKind::Directory);
  CHECK(read.entries[1].name == "d/f.txt");
  CHECK(read.entries[1].kind == EntryKind::File);
  CHECK(read.entries[1].size == 5000);
  CHECK(read.entries[2].name == "d/empty");
  CHECK(read.entries[2].kind == EntryKind::File);
  CHECK(read.entries[2].size == 0);
  CHECK(read.entries[3].name == "d/link");
  CHECK(read.entries[3].kind == EntryKind::Symlink);
  REQUIRE(read.contents.size() == 4);
  std::string data;
  for (int i = 0; i < 1000; ++i) {
    data += "data\n";
  }
  CHECK(read.contents[1] == data);
  CHECK(read.contents[2].empty());
}

TEST_CASE("a callback returning false stops test() and is told apart from archive errors") {
  const TempDir dir;
  struct Case {
    const char* name;
    const unsigned char* data;
    size_t size;
  };
  for (const Case& c : {Case{"tiny.rar", fixtures::kTinyRar, sizeof(fixtures::kTinyRar)},
                        Case{"tiny.zip", fixtures::kTinyZip, sizeof(fixtures::kTinyZip)},
                        Case{"tiny.7z", fixtures::kTiny7z, sizeof(fixtures::kTiny7z)},
                        Case{"solid.7z", fixtures::kSolid7z, sizeof(fixtures::kSolid7z)},
                        Case{"tiny.tar", fixtures::kTinyTar, sizeof(fixtures::kTinyTar)},
                        Case{"tiny.tar.gz", fixtures::kTinyTarGz, sizeof(fixtures::kTinyTarGz)}}) {
    CAPTURE(std::string(c.name));
    int calls = 0;
    ArchiveCallbacks callbacks;
    callbacks.on_data = [&](const uint8_t*, size_t) {
      ++calls;
      return false;
    };
    const std::unique_ptr<Archive> archive =
        open_archive(dir.write(c.name, c.data, c.size), Archive::Mode::Extract, callbacks);
    ArchiveEntry entry;
    bool tested = false;
    while (archive->next(entry)) {
      if (entry.kind != EntryKind::File || entry.size == 0) {
        archive->skip();
        CHECK_FALSE(archive->aborted_by_callback());
        continue;
      }
      CHECK_THROWS_AS(archive->test(), ArchiveError);
      CHECK(archive->aborted_by_callback());
      tested = true;
      break;
    }
    CHECK(tested);
    CHECK(calls == 1);
  }

  // A checksum mismatch is the archive's fault, not the callback's.
  for (const Case& c : {Case{"bad.zip", fixtures::kBadCrcZip, sizeof(fixtures::kBadCrcZip)},
                        Case{"bad.7z", fixtures::kBadCrc7z, sizeof(fixtures::kBadCrc7z)}}) {
    CAPTURE(std::string(c.name));
    ArchiveCallbacks callbacks;
    callbacks.on_data = [](const uint8_t*, size_t) { return true; };
    const std::unique_ptr<Archive> archive =
        open_archive(dir.write(c.name, c.data, c.size), Archive::Mode::Extract, callbacks);
    ArchiveEntry entry;
    REQUIRE(archive->next(entry));
    CHECK_THROWS_AS(archive->test(), ArchiveError);
    CHECK_FALSE(archive->aborted_by_callback());
  }
}

TEST_CASE("RAR archives with encrypted headers") {
  const TempDir dir;
  const std::string path = dir.write("encrypted.rar", fixtures::kEncryptedRar);
  int prompts = 0;
  const Read read = read_all(path, "secret", &prompts);
  CHECK(read.format == ArchiveFormat::Rar);
  CHECK(read.flags.encrypted_headers);
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries[0].name == "hello.txt");
  CHECK(read.contents == std::vector<std::string>{"hello\n"});
  CHECK(prompts == 2);  // Once per archive object: listing, then extracting.

  CHECK(error_kind(path) == ArchiveError::Kind::MissingPassword);
  CHECK(error_kind(path, "wrong") == ArchiveError::Kind::BadPassword);
}

TEST_CASE("RAR archives with encrypted file data and clear headers") {
  const TempDir dir;
  const std::string path = dir.write("encrypted-data.rar", fixtures::kEncryptedDataRar);
  int prompts = 0;
  const Read read = read_all(path, "secret", &prompts);
  CHECK_FALSE(read.flags.encrypted_headers);
  REQUIRE(read.entries.size() == 1);
  CHECK(read.entries[0].encrypted);
  CHECK(read.contents == std::vector<std::string>{"hello\n"});
  CHECK(prompts == 2);  // Once per archive object: listing, then extracting.

  // The listing UnRAR does with clear headers never asks for a password, but
  // ours decrypts the start of the first encrypted file, as for ZIP and 7z.
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
  CHECK(prompts == 1);

  // The right one passes, and the listing goes on.
  callbacks.on_password = []() -> std::optional<std::string> { return std::string("secret"); };
  const std::unique_ptr<Archive> good = open_archive(path, Archive::Mode::List, callbacks);
  REQUIRE(good->next(entry));
  CHECK_NOTHROW(good->skip());
  CHECK_FALSE(good->next(entry));

  // No answer at all.
  callbacks.on_password = []() -> std::optional<std::string> { return std::nullopt; };
  const std::unique_ptr<Archive> none = open_archive(path, Archive::Mode::List, callbacks);
  REQUIRE(none->next(entry));
  try {
    none->skip();
    FAIL("a missing password was accepted");
  } catch (const ArchiveError& error) {
    CHECK(error.kind() == ArchiveError::Kind::MissingPassword);
  }
  CHECK(error_kind(path) == ArchiveError::Kind::MissingPassword);
  CHECK(error_kind(path, "wrong") == ArchiveError::Kind::BadPassword);
}

TEST_CASE("archives cut inside file data are errors, not shorter files") {
  const TempDir dir;
  struct Case {
    const char* name;
    const unsigned char* data;
    size_t cut;  // Bytes kept.
  };
  for (const Case& c : {Case{"cut.rar", fixtures::kTinyRar, 46},
                        Case{"cut.zip", fixtures::kKindsZip, 85},  // Inside the deflated "d/f.txt".
                        Case{"cut.7z", fixtures::kSolid7z, sizeof(fixtures::kSolid7z) * 2 / 3},
                        Case{"cut.tar", fixtures::kKindsTar, 8 * 512 + 2},  // Inside "d/ação.txt".
                        Case{"cut.tar.gz", fixtures::kTinyTarGz, sizeof(fixtures::kTinyTarGz) * 2 / 3}}) {
    CAPTURE(std::string(c.name));
    CHECK_THROWS_AS(read_all(dir.write(c.name, c.data, c.cut)), ArchiveError);
  }
}

TEST_CASE("compressed tar reports how much of the archive was read") {
  const TempDir dir;
  ArchiveCallbacks callbacks;
  callbacks.on_data = [](const uint8_t*, size_t) { return true; };
  const std::unique_ptr<Archive> archive =
      open_archive(dir.write("a.tar.gz", fixtures::kTinyTarGz), Archive::Mode::Extract, callbacks);
  const uint64_t size = archive->flags().size;
  CHECK(size == sizeof(fixtures::kTinyTarGz));
  ArchiveEntry entry;
  uint64_t previous = archive->bytes_read();
  while (archive->next(entry)) {
    archive->test();
    CHECK(archive->bytes_read() >= previous);
    previous = archive->bytes_read();
  }
  CHECK(previous > 0);
  CHECK(previous <= size);
}

TEST_CASE("missing files, directories and empty files cannot be opened") {
  const TempDir dir;
  const std::string empty = dir.write("empty.zip", std::string());
  for (const std::string& path : {(dir.path() / "missing.rar").string(), dir.path().string(), empty}) {
    CAPTURE(path);
    CHECK_THROWS_AS(read_all(path), ArchiveError);
  }
}

TEST_CASE("RAR archives cut inside a header are truncated, not shorter (UnRAR patch)") {
  const TempDir dir;
  constexpr const char* kTruncated = "unexpected end of archive: the file is truncated";
  // Inside the main header, the file header, the file data and the end-of-archive block.
  for (const size_t cut : {10u, 20u, 38u, 47u, 54u}) {
    CAPTURE(cut);
    const std::string path = dir.write("cut.rar", fixtures::kTinyRar, cut);
    CHECK_THROWS_WITH_AS(read_all(path), kTruncated, ArchiveError);  // Already while listing.
  }

  // The file before the cut was complete, but the archive is not: test() fails too.
  ArchiveCallbacks callbacks;
  callbacks.on_data = [](const uint8_t*, size_t) { return true; };
  const auto archive =
      open_archive(dir.write("end.rar", fixtures::kTinyRar, 54), Archive::Mode::Extract, callbacks);
  ArchiveEntry entry;
  REQUIRE(archive->next(entry));
  CHECK_THROWS_WITH_AS(archive->test(), kTruncated, ArchiveError);
  CHECK_FALSE(archive->aborted_by_callback());

  // Ending exactly at a block boundary without the end-of-archive block is how RAR 1.5 and
  // "rar -en" archives end: UnRAR accepts it, and so do we.
  const Read boundary = read_all(dir.write("boundary.rar", fixtures::kTinyRar, 50));
  CHECK(boundary.contents == std::vector<std::string>{"hello\n"});
}
