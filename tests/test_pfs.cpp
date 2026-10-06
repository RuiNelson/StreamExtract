#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "archive.hpp"
#include "image_reader.hpp"
#include "logger.hpp"
#include "plan.hpp"

using namespace streamextract;

namespace {
std::string fixture(const std::string& name) { return std::string(STREAMEXTRACT_TEST_FIXTURES) + "/" + name; }

struct CopyImage {
  std::filesystem::path directory;
  std::string path;
  explicit CopyImage(const std::string& source = "raw.ffpfsc", const std::string& name = "image.ffpfsc") {
    directory = std::filesystem::temp_directory_path() /
                ("streamextract-pfs-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(directory);
    path = (directory / name).string();
    std::filesystem::copy_file(fixture(source), path);
  }
  ~CopyImage() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  void write(uint64_t offset, uint64_t value, size_t width) const {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(offset));
    for (size_t i = 0; i < width; ++i) file.put(static_cast<char>((value >> (i * 8)) & 255));
  }
};

std::map<std::string, std::string> contents(const std::string& path) {
  std::map<std::string, std::string> result;
  std::string data;
  ArchiveCallbacks cb;
  cb.on_data = [&](const uint8_t* bytes, size_t count) {
    data.append(reinterpret_cast<const char*>(bytes), count);
    return true;
  };
  const auto archive = open_archive(path, Archive::Mode::Extract, cb);
  ArchiveEntry entry;
  while (archive->next(entry)) {
    data.clear();
    archive->test();
    result[entry.name] = data;
  }
  return result;
}
void list(const std::string& path) {
  const auto archive = open_archive(path, Archive::Mode::List, {});
  ArchiveEntry entry;
  while (archive->next(entry)) archive->skip();
}
std::string repeated_bytes(size_t count) {
  std::string value(count, '\0');
  for (size_t i = 0; i < count; ++i) value[i] = static_cast<char>(i % 256);
  return value;
}

struct CompressedFile {
  uint64_t inode = 0;
  uint64_t payload = 0;
  uint64_t table = 0;
  uint64_t data = 0;
  uint64_t stored_size = 0;
};
CompressedFile first_compressed(const std::string& path) {
  const auto image = open_image(path);
  std::vector<uint8_t> header(1024);
  image->read(0, header.data(), header.size());
  const uint32_t block = image_le32(header.data() + 32);
  const uint64_t count = image_le64(header.data() + 48);
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t offset = (1 + i / (block / 168)) * block + i % (block / 168) * 168;
    image->read(offset, header.data(), 168);
    if ((image_le32(header.data() + 4) & 1) == 0) continue;
    const uint64_t start = static_cast<uint64_t>(image_le32(header.data() + 100)) * block;
    const uint64_t stored_size = image_le64(header.data() + 8);
    image->read(start, header.data(), 48);
    const uint64_t table = start + image_le64(header.data() + 24);
    const uint64_t data = start + image_le64(header.data() + 32);
    return {offset, start, table, data, stored_size};
  }
  FAIL("fixture must contain a PFSC file");
  return {};
}

uint64_t ufs1_inode_offset(const std::string& path, const std::string& name) {
  const auto source = open_image(path);
  std::array<uint8_t, 1376> sb{};
  source->read(8192, sb.data(), sb.size());
  const uint64_t fragment = image_le32(sb.data() + 52);
  const uint64_t inodes = image_le32(sb.data() + 16) * fragment;
  std::array<uint8_t, 128> root{};
  source->read(inodes + 2 * 128, root.data(), root.size());
  const uint64_t start = image_le32(root.data() + 40) * fragment;
  for (uint64_t offset = 0; offset < image_le64(root.data() + 8);) {
    std::array<uint8_t, 512> record{};
    source->read(start + offset, record.data(), 8);
    REQUIRE(image_le16(record.data() + 4) >= 12);
    source->read(start + offset + 8, record.data() + 8, record[7]);
    if (name == std::string(reinterpret_cast<const char*>(record.data() + 8), record[7]))
      return inodes + image_le32(record.data()) * 128;
    offset += image_le16(record.data() + 4);
  }
  FAIL("file must exist in the UFS fixture");
  return 0;
}
}  // namespace

TEST_CASE("PFS reads genuine MkPFS PS4/PS5 images and mixed PFSC blocks") {
  const auto decoded = contents(fixture("raw.ffpfsc"));
  CHECK(decoded == contents(fixture("raw-ps4.ffpfs")));
  CHECK(decoded.at("hello.txt") == "hello\n");
  CHECK(decoded.at("empty.txt").empty());
  CHECK(decoded.at("sub").empty());
  CHECK(decoded.at("large.bin") == repeated_bytes(1179648) + "end");
  CHECK(decoded.at("sub/raw.bin") == repeated_bytes(256));
  CHECK(decoded.at("mixed.bin").size() == 262148);
  CHECK(decoded.at("mixed.bin").substr(0, 65536) == std::string(65536, '\0'));
  CHECK(decoded.at("mixed.bin").substr(131072, 65536) == std::string(65536, 'a'));
  CHECK(decoded.at("mixed.bin").substr(262144) == "tail");
  CHECK(contents(fixture("single.ffpfs")).at("hello.txt") == "hello\n");
}

TEST_CASE("PFS unwraps compressed exFAT and nested PFS without materializing an image") {
  const auto expected = contents(fixture("volume.exfat"));
  CHECK(contents(fixture("exfat.ffpfsc")) == expected);
  CHECK(contents(fixture("nested.ffpfsc")) == expected);
}

TEST_CASE("UFS reads makefs UFS1/UFS2 volumes in both byte orders and wrapped PFSC") {
  const auto expected = contents(fixture("volume.ufs"));
  CHECK(expected.at("hello.txt") == "hello\n");
  CHECK(expected.at("large.bin") == repeated_bytes(1179648) + "end");
  CHECK(expected.at("double.bin") == std::string(10 * 1024 * 1024, '\0') + "last indirect block\n");
  CHECK(expected.at("link").empty());
  CHECK(expected.at("linked.txt").empty());
  for (const std::string name :
       {"volume2.ufs", "big-ufs1.ufs", "little-ufs2.ufs", "checksums.ufs", "ufs.ffpfsc", "ufs2.ffpfsc"}) {
    INFO(name);
    CHECK(contents(fixture(name)) == expected);
  }
}

TEST_CASE("PFS content detection, flags and planning work through the shared archive API") {
  const CopyImage file("raw.ffpfsc", "misleading.zip");
  Logger log;
  PasswordSource password(std::nullopt, {});
  const auto listing = list_archive(file.path, password, log);
  CHECK(listing.format == ArchiveFormat::Pfs);
  CHECK(std::string(format_name(listing.format)) == "PFS");
  CHECK(listing.flags.compression == "PFSC");
  CHECK_FALSE(listing.flags.checksums);
  CHECK_FALSE(listing.flags.skip_decompresses);
  CHECK_FALSE(listing.flags.stream_only);
  CHECK(listing.volumes == 1);
  CHECK(std::count_if(listing.entries.begin(), listing.entries.end(),
                      [](const ArchiveEntry& e) { return e.kind == EntryKind::File; }) == 5);
  REQUIRE(log.problems().size() == 1);
  CHECK(log.problems()[0].text.find("no checksum") != std::string::npos);
  const auto plan = build_plan(file.path, listing, "/upload", log);
  CHECK(plan.upload_files == 5);
  const auto hello = std::find_if(plan.entries.begin(), plan.entries.end(),
                                  [](const PlannedEntry& e) { return e.entry.name == "hello.txt"; });
  REQUIRE(hello != plan.entries.end());
  CHECK(hello->remote == "/upload/hello.txt");
  CHECK(hello->entry.size == 6);
  CHECK(hello->entry.mtime > 0);
}

TEST_CASE("PFS skipping, cancellation and concurrent readers preserve callback semantics") {
  for (const std::string name : {"raw.ffpfsc", "nested.ffpfsc", "ufs.ffpfsc"}) {
    size_t calls = 0;
    ArchiveCallbacks cb;
    cb.on_data = [&](const uint8_t*, size_t) {
      ++calls;
      CHECK_NOTHROW(list(fixture("raw.ffpfsc")));
      return false;
    };
    const auto archive = open_archive(fixture(name), Archive::Mode::Extract, cb);
    ArchiveEntry entry;
    bool found = false;
    while (archive->next(entry)) {
      if (entry.name != "large.bin") {
        archive->skip();
        continue;
      }
      found = true;
      CHECK_THROWS_AS(archive->test(), ArchiveError);
      CHECK(archive->aborted_by_callback());
      break;
    }
    CHECK(found);
    CHECK(calls == 1);
  }
  std::map<std::string, std::string> other;
  std::thread worker([&] { other = contents(fixture("ufs.ffpfsc")); });
  CHECK(contents(fixture("exfat.ffpfsc")).at("hello.txt") == "hello\n");
  worker.join();
  CHECK(other.at("hello.txt") == "hello\n");
}

TEST_CASE("PFS refuses invalid geometry, unsupported modes and split images") {
  const CopyImage file;
  SUBCASE("truncated header") { std::filesystem::resize_file(file.path, 16); }
  SUBCASE("truncated volume") { std::filesystem::resize_file(file.path, 70000); }
  SUBCASE("zero block size") { file.write(32, 0, 4); }
  SUBCASE("overflowing block count") { file.write(56, UINT64_MAX, 8); }
  SUBCASE("overflowing inode count") { file.write(48, UINT64_MAX, 8); }
  SUBCASE("out of range inode extent") { file.write(65536 + 100, UINT32_MAX, 4); }
  SUBCASE("unsupported version") { file.write(0, 3, 8); }
  SUBCASE("signed") {
    file.write(28, 9, 2);
    CHECK_THROWS_WITH_AS(list(file.path), "PFS: signed images are not supported", ArchiveError);
    return;
  }
  SUBCASE("encrypted") {
    file.write(28, 12, 2);
    CHECK_THROWS_WITH_AS(list(file.path), "PFS: encrypted images are not supported", ArchiveError);
    return;
  }
  SUBCASE("64-bit inodes") { file.write(28, 10, 2); }
  SUBCASE("split") {
    const CopyImage first("raw.ffpfsc", "volume.ffpfsc.001");
    const auto second = first.path.substr(0, first.path.size() - 1) + "2";
    std::ofstream(second, std::ios::binary).put('x');
    CHECK_THROWS_WITH_AS(list(first.path), "split PFS/UFS images are not supported; use a single image",
                         ArchiveError);
    return;
  }
  CHECK_THROWS_AS(list(file.path), ArchiveError);
}

TEST_CASE("PFSC rejects bad offsets, bad logical lengths and corrupt zlib data") {
  const CopyImage file;
  const auto compressed = first_compressed(file.path);
  SUBCASE("table overlaps data") { file.write(compressed.payload + 24, UINT64_MAX, 8); }
  SUBCASE("logical length disagrees with inode") { file.write(compressed.inode + 16, 1, 8); }
  SUBCASE("wrong block size") { file.write(compressed.payload + 12, 4096, 4); }
  SUBCASE("non-monotonic offset") { file.write(compressed.table + 8, 0, 8); }
  SUBCASE("offset outside payload") { file.write(compressed.table + 8, compressed.stored_size + 1, 8); }
  SUBCASE("corrupt compressed data") { file.write(compressed.data, 0, 2); }
  CHECK_THROWS_AS(contents(file.path), ArchiveError);
}

TEST_CASE("PFS refuses cyclic directories") {
  const CopyImage file;
  const auto image = open_image(file.path);
  std::array<uint8_t, 168> inode{};
  image->read(65536 + 2 * 168, inode.data(), inode.size());
  const uint64_t root = static_cast<uint64_t>(image_le32(inode.data() + 100)) * 65536;
  // Replace the first ordinary directory entry with a reference to uroot.
  uint64_t offset = root;
  std::array<uint8_t, 16> entry{};
  while (true) {
    image->read(offset, entry.data(), entry.size());
    if (image_le32(entry.data() + 4) == 3) break;
    REQUIRE(image_le32(entry.data() + 12) != 0);
    offset += image_le32(entry.data() + 12);
  }
  file.write(offset, 2, 4);
  CHECK_THROWS_AS(list(file.path), ArchiveError);
}

TEST_CASE("UFS refuses truncated volumes, invalid geometry and directory cycles") {
  const CopyImage file("volume.ufs");
  SUBCASE("truncated") { std::filesystem::resize_file(file.path, 70000); }
  SUBCASE("bad fragment size") { file.write(8192 + 52, 0, 4); }
  SUBCASE("bad inode count") { file.write(8192 + 184, 0, 4); }
  SUBCASE("cycle") {
    const auto image = open_image(file.path);
    std::array<uint8_t, 1376> sb{};
    image->read(8192, sb.data(), sb.size());
    const uint64_t fragment = image_le32(sb.data() + 52);
    std::array<uint8_t, 128> inode{};
    image->read(image_le32(sb.data() + 16) * fragment + 2 * 128, inode.data(), inode.size());
    const uint64_t root = image_le32(inode.data() + 40) * fragment;
    uint64_t offset = root;
    std::array<uint8_t, 8> record{};
    while (true) {
      image->read(offset, record.data(), record.size());
      if (record[6] == 4 && record[7] > 2) break;
      REQUIRE(image_le16(record.data() + 4) != 0);
      offset += image_le16(record.data() + 4);
    }
    file.write(offset, 2, 4);
  }
  CHECK_THROWS_AS(list(file.path), ArchiveError);
}

TEST_CASE("UFS verifies FreeBSD superblock and inode CRC32C values") {
  const CopyImage file("checksums.ufs");
  SUBCASE("superblock") {
    file.write(8192 + 200, UINT32_MAX, 4);
    CHECK_THROWS_WITH_AS(list(file.path), "UFS: superblock checksum mismatch", ArchiveError);
  }
  SUBCASE("inode") {
    const auto image = open_image(file.path);
    std::array<uint8_t, 1376> sb{};
    image->read(8192, sb.data(), sb.size());
    const uint64_t inode =
        static_cast<uint64_t>(image_le32(sb.data() + 16)) * image_le32(sb.data() + 52) + 2 * 256;
    file.write(inode + 4, 1, 4);
    CHECK_THROWS_WITH_AS(list(file.path), "UFS: inode checksum mismatch", ArchiveError);
  }
}

TEST_CASE("UFS reads sparse holes and rejects out-of-volume data pointers") {
  const CopyImage file("volume.ufs");
  const uint64_t inode = ufs1_inode_offset(file.path, "large.bin");
  SUBCASE("sparse direct block") {
    file.write(inode + 40, 0, 4);
    const auto data = contents(file.path).at("large.bin");
    CHECK(data == std::string(8192, '\0') + repeated_bytes(1179648 - 8192) + "end");
  }
  SUBCASE("sparse indirect subtree") {
    file.write(inode + 88, 0, 4);
    const auto data = contents(file.path).at("large.bin");
    CHECK(data == repeated_bytes(12 * 8192) + std::string(1179651 - 12 * 8192, '\0'));
  }
  SUBCASE("invalid direct block") {
    file.write(inode + 40, UINT32_MAX, 4);
    CHECK_THROWS_AS(contents(file.path), ArchiveError);
  }
  SUBCASE("invalid indirect block") {
    file.write(inode + 88, UINT32_MAX, 4);
    CHECK_THROWS_AS(contents(file.path), ArchiveError);
  }
}

TEST_CASE("PFS preserves empty directories") {
  const CopyImage file;
  const auto image = open_image(file.path);
  std::array<uint8_t, 168> inode{};
  std::array<uint8_t, 1024> header{};
  image->read(0, header.data(), header.size());
  const uint64_t count = image_le64(header.data() + 48);
  // MkPFS omits empty source directories. Make its existing subdirectory empty
  // by removing the child record, retaining the formatter's directory inode.
  for (uint64_t index = 3; index < count; ++index) {
    image->read(65536 + index * 168, inode.data(), inode.size());
    if ((image_le16(inode.data()) & 0xf000) != 0x4000) continue;
    const uint64_t start = static_cast<uint64_t>(image_le32(inode.data() + 100)) * 65536;
    uint64_t offset = start;
    std::array<uint8_t, 16> record{};
    for (;;) {
      image->read(offset, record.data(), record.size());
      const auto type = image_le32(record.data() + 4);
      if (type == 2) {
        file.write(offset, 0, 8);
        file.write(offset + 8, 0, 8);
        const auto result = contents(file.path);
        CHECK(result.count("sub") == 1);
        CHECK(result.count("sub/raw.bin") == 0);
        return;
      }
      REQUIRE(image_le32(record.data() + 12) != 0);
      offset += image_le32(record.data() + 12);
    }
  }
  FAIL("fixture must have a subdirectory");
}
