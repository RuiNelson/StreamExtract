#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "archive.hpp"
#include "logger.hpp"
#include "plan.hpp"

using namespace streamextract;

namespace {
const std::string kImage = std::string(STREAMEXTRACT_TEST_FIXTURES) + "/volume.exfat";

struct CopyImage {
  std::filesystem::path directory;
  std::string path;
  explicit CopyImage(const std::string& name = "image.exfat") {
    directory = std::filesystem::temp_directory_path() /
                ("streamextract-exfat-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(directory);
    path = (directory / name).string();
    std::filesystem::copy_file(kImage, path);
  }
  ~CopyImage() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

std::map<std::string, std::string> contents(const std::string& path) {
  std::map<std::string, std::string> result;
  std::string current;
  ArchiveCallbacks callbacks;
  callbacks.on_data = [&](const uint8_t* bytes, size_t size) {
    current.append(reinterpret_cast<const char*>(bytes), size);
    return true;
  };
  const auto archive = open_archive(path, Archive::Mode::Extract, callbacks);
  ArchiveEntry entry;
  while (archive->next(entry)) {
    current.clear();
    archive->test();
    result[entry.name] = current;
  }
  return result;
}

void list(const std::string& path) {
  const auto archive = open_archive(path, Archive::Mode::List, {});
  ArchiveEntry entry;
  while (archive->next(entry)) archive->skip();
}

std::string repeated_bytes(size_t count) {
  std::string data(count, '\0');
  for (size_t i = 0; i < count; ++i) data[i] = static_cast<char>(i % 256);
  return data;
}
}  // namespace

TEST_CASE("exFAT reads a real hdiutil volume, Unicode names and fragmented files") {
  const auto extracted = contents(kImage);
  CHECK(extracted.at("hello.txt") == "hello\n");
  CHECK(extracted.at("empty.txt").empty());
  CHECK(extracted.at("empty-dir").empty());
  CHECK(extracted.at("sub/ação 日本 😀.txt") == "unicode\n");
  CHECK(extracted.at("sub/é.txt") == "combining\n");
  std::string long_name = "sub/";
  for (int i = 0; i < 80; ++i) long_name += "日";
  CHECK(extracted.at(long_name + ".txt") == "long name\n");
  CHECK(extracted.at("large.bin") == repeated_bytes(1179648) + "end");
  CHECK(extracted.at("fragmented.bin") == repeated_bytes(5242880) + "fragmented end");
  CHECK(extracted.at("uninitialized.bin") == "written" + std::string(16993, '\0'));
}

TEST_CASE("exFAT keeps trailing dots and spaces and DEL in names") {
  // Real hdiutil/newfs_exfat volume: FatFs's path parser would strip them and open another entry (or none).
  const auto extracted = contents(std::string(STREAMEXTRACT_TEST_FIXTURES) + "/trailing.exfat");
  CHECK(extracted.at("Acme Inc.").empty());
  CHECK(extracted.at("Acme Inc./file.txt") == "inside dir\n");
  CHECK(extracted.at("Acme Inc") == "plain 4\n");
  CHECK(extracted.at("notes") == "plain 1\n");
  CHECK(extracted.at("notes.") == "dot one\n");  // Same size as "notes": the wrong entry would go unnoticed.
  CHECK(extracted.at("report") == "plain 2\n");
  CHECK(extracted.at("report ") == "space a\n");
  CHECK(extracted.at("end") == "plain 3\n");
  CHECK(extracted.at("end..") == "two dots\n");
  CHECK(extracted.at("trail ").empty());
  CHECK(extracted.at("trail /x.txt") == "in space dir\n");
  CHECK(extracted.at(std::string("a\x7f") + "b.txt") == "del\n");
}

TEST_CASE("exFAT is recognized by content and uses the existing planner") {
  const CopyImage file("misleading.zip");
  Logger log;
  PasswordSource passwords(std::nullopt, {});
  const auto listing = list_archive(file.path, passwords, log);
  CHECK(listing.format == ArchiveFormat::Exfat);
  CHECK(std::string(format_name(listing.format)) == "exFAT");
  CHECK_FALSE(listing.flags.checksums);
  CHECK_FALSE(listing.flags.stream_only);
  CHECK_FALSE(listing.flags.skip_decompresses);
  CHECK(listing.volumes == 1);
  REQUIRE(log.problems().size() == 1);
  CHECK(log.problems()[0].text.find("no checksum") != std::string::npos);
  const auto plan = build_plan(file.path, listing, "/upload", log);
  CHECK(plan.upload_files >= 8);
  const auto hello = std::find_if(plan.entries.begin(), plan.entries.end(),
                                  [](const PlannedEntry& entry) { return entry.entry.name == "hello.txt"; });
  REQUIRE(hello != plan.entries.end());
  CHECK(hello->remote == "/upload/hello.txt");
  CHECK(hello->entry.size == 6);
  CHECK(hello->entry.mtime == 1700000000);
}

TEST_CASE("exFAT cancellation and skipping do not interfere with other readers") {
  size_t calls = 0;
  ArchiveCallbacks callbacks;
  callbacks.on_data = [&](const uint8_t*, size_t) {
    ++calls;
    CHECK_NOTHROW(list(kImage));  // No FatFs lock is held while the callback runs.
    return false;
  };
  const auto archive = open_archive(kImage, Archive::Mode::Extract, callbacks);
  ArchiveEntry entry;
  bool found = false;
  while (archive->next(entry)) {
    if (entry.name == "large.bin") {
      found = true;
      CHECK_THROWS_AS(archive->test(), ArchiveError);
      CHECK(archive->aborted_by_callback());
      break;
    }
    archive->skip();
  }
  REQUIRE(found);
  CHECK(calls == 1);
  std::map<std::string, std::string> other;
  std::thread thread([&] { other = contents(kImage); });
  CHECK(contents(kImage).at("hello.txt") == "hello\n");
  thread.join();
  CHECK(other.at("fragmented.bin") == repeated_bytes(5242880) + "fragmented end");
}

TEST_CASE("exFAT refuses truncated and split images and releases failed mounts") {
  const CopyImage file;
  SUBCASE("truncated boot sector") { std::filesystem::resize_file(file.path, 100); }
  SUBCASE("truncated volume") { std::filesystem::resize_file(file.path, 1 << 20); }
  SUBCASE("split image") {
    const CopyImage first("image.exfat.001");
    const std::string second = first.path.substr(0, first.path.size() - 1) + "2";
    std::ofstream part(second, std::ios::binary);
    part.put('x');
    part.close();
    CHECK_THROWS_WITH_AS(list(first.path), "split exFAT images are not supported; use a single volume image",
                         ArchiveError);
    return;
  }
  CHECK_THROWS_AS(list(file.path), ArchiveError);
  CHECK_NOTHROW(list(kImage));
}

TEST_CASE("exFAT rejects a real hdiutil disk image with a partition table") {
  const std::string path = std::string(STREAMEXTRACT_TEST_FIXTURES) + "/partitioned.exfat";
  CHECK_THROWS_WITH_AS(list(path),
                       "exFAT: expected a single raw exFAT volume; disk images with a partition table, compressed "
                       "or encrypted images are not supported",
                       ArchiveError);
}
