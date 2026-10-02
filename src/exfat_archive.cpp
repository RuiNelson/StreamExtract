#include "exfat_archive.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <ff.h>

#include <diskio.h>

#include "util/text.hpp"

namespace {

[[noreturn]] void exfat_error(const std::string& message) {
  throw rarftp::ArchiveError(rarftp::ArchiveError::Kind::Other, "exFAT: " + message);
}

uint32_t le32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t le64(const uint8_t* bytes) { return le32(bytes) | (static_cast<uint64_t>(le32(bytes + 4)) << 32); }

std::filesystem::path to_path(const std::string& path) {
#ifdef _WIN32
  return std::filesystem::path(rarftp::from_utf8(path));
#else
  return std::filesystem::path(path);
#endif
}

bool signature(const uint8_t* bytes) { return std::memcmp(bytes + 3, "EXFAT   ", 8) == 0; }

struct Image {
  std::ifstream file;
  uint64_t size = 0;
  uint64_t sectors = 0;
  uint16_t sector_size = 512;
  uint32_t clusters = 0;
  uint64_t cluster_bytes = 0;

  bool read(uint64_t offset, uint8_t* data, size_t count) {
    if (offset > size || count > size - offset ||
        offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
      return false;
    }
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(count));
    return file.good();
  }

  explicit Image(const std::string& path) : file(to_path(path), std::ios::binary) {
    if (!file) exfat_error("cannot open the image");
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    if (length < 0) exfat_error("cannot determine the image size");
    size = static_cast<uint64_t>(length);
    std::array<uint8_t, 512> boot{};
    if (!read(0, boot.data(), boot.size())) exfat_error("truncated boot sector");
    if (!signature(boot.data())) {
      exfat_error(
          "expected a single raw exFAT volume; disk images with a partition table, compressed "
          "or encrypted images are not supported");
    }
    // Check geometry before FatFs uses shifts and computes sector addresses.
    const unsigned sector_shift = boot[108];
    const unsigned cluster_shift = boot[109];
    if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 25 - sector_shift || boot[510] != 0x55 ||
        boot[511] != 0xaa) {
      exfat_error("invalid boot sector or sector/cluster size");
    }
    if (cluster_shift > 15 || boot[110] != 1) {
      exfat_error("unsupported volume: FatFs requires one FAT and at most 32768 sectors per cluster");
    }
    sector_size = static_cast<uint16_t>(1U << sector_shift);
    cluster_bytes = uint64_t{1} << (sector_shift + cluster_shift);
    sectors = le64(boot.data() + 72);
    clusters = le32(boot.data() + 92);
    const uint64_t fat_start = le32(boot.data() + 80);
    const uint64_t fat_sectors = le32(boot.data() + 84);
    const uint64_t heap_start = le32(boot.data() + 88);
    const uint32_t root = le32(boot.data() + 96);
    if (sectors < 24 || sectors > size / sector_size) exfat_error("truncated volume");
    if (clusters < 256 || clusters > 0x7ffffffd || fat_start < 24 || fat_start > heap_start ||
        fat_sectors > heap_start - fat_start ||
        fat_sectors * sector_size < (static_cast<uint64_t>(clusters) + 2) * 4 || heap_start > sectors ||
        clusters > (sectors - heap_start) / (uint64_t{1} << cluster_shift) || root < 2 || root - 2 >= clusters) {
      exfat_error("invalid volume geometry");
    }
    std::vector<uint8_t> region(static_cast<size_t>(sector_size) * 12);
    if (!read(0, region.data(), region.size())) exfat_error("truncated boot region");
    uint32_t checksum = 0;
    for (size_t i = 0; i < static_cast<size_t>(sector_size) * 11; ++i) {
      if (i != 106 && i != 107 && i != 112) {
        checksum = (checksum >> 1) | (checksum << 31);
        checksum += region[i];
      }
    }
    for (size_t i = static_cast<size_t>(sector_size) * 11; i < region.size(); i += 4) {
      if (le32(region.data() + i) != checksum) exfat_error("boot checksum mismatch");
    }
  }
};

// FatFs has process-global mount/name buffers, even in its reentrant mode.
// Serialize every API call, including mount/unmount, and reserve a distinct
// drive for each reader. Release the lock before calling on_data: the bounded
// pipe may block, and other jobs must still be able to read or close a volume.
std::mutex fatfs_mutex;
std::array<Image*, FF_VOLUMES> images{};

Image* image_for(BYTE drive) { return drive < images.size() ? images[drive] : nullptr; }

void check(FRESULT result, const std::string& operation) {
  if (result == FR_OK) return;
  const char* reason = "filesystem error";
  switch (result) {
    case FR_DISK_ERR:
      reason = "read error or truncated cluster chain";
      break;
    case FR_INT_ERR:
      reason = "corrupt filesystem metadata";
      break;
    case FR_NO_FILE:
    case FR_NO_PATH:
      reason = "file or directory not found";
      break;
    case FR_INVALID_NAME:
      reason = "invalid file name";
      break;
    case FR_NO_FILESYSTEM:
      reason = "invalid or unsupported exFAT filesystem";
      break;
    default:
      break;
  }
  exfat_error(fmt::format("{}: {} (FatFs {})", operation, reason, static_cast<int>(result)));
}

int64_t modification_time(const uint8_t* metadata) {
  const uint32_t stamp = le32(metadata + 12);
  if (stamp == 0) return 0;
  std::tm date{};
  date.tm_sec = static_cast<int>((stamp & 31) * 2 + metadata[21] / 100);
  date.tm_min = static_cast<int>((stamp >> 5) & 63);
  date.tm_hour = static_cast<int>((stamp >> 11) & 31);
  date.tm_mday = static_cast<int>((stamp >> 16) & 31);
  date.tm_mon = static_cast<int>((stamp >> 21) & 15) - 1;
  date.tm_year = static_cast<int>((stamp >> 25) & 127) + 80;
  if (date.tm_sec > 59 || date.tm_min > 59 || date.tm_hour > 23 || date.tm_mday < 1 || date.tm_mday > 31 ||
      date.tm_mon < 0 || date.tm_mon > 11 || metadata[21] > 199)
    return 0;
  const uint8_t zone = metadata[23];
  if ((zone & 0x80) == 0) {
    date.tm_isdst = -1;  // Without a stored UTC offset, exFAT uses local time.
    const std::time_t time = std::mktime(&date);
    return time == static_cast<std::time_t>(-1) ? 0 : static_cast<int64_t>(time);
  }
#ifdef _WIN32
  const int64_t time = _mkgmtime64(&date);
#else
  const int64_t time = static_cast<int64_t>(timegm(&date));
#endif
  const int offset = (zone & 0x40) ? static_cast<int>(zone & 0x7f) - 128 : zone & 0x7f;
  return time == -1 ? 0 : time - offset * 15 * 60;
}

struct OpenFatfsFile {
  FIL file{};
  bool opened = false;
  ~OpenFatfsFile() {
    if (opened) {
      std::lock_guard lock(fatfs_mutex);
      f_close(&file);
    }
  }
};

}  // namespace

// FatFs's media interface. Calls arrive with fatfs_mutex held. There is no
// writable disk implementation: both FatFs and the backing file are read-only.
extern "C" {
DSTATUS disk_status(BYTE drive) { return image_for(drive) ? STA_PROTECT : STA_NOINIT; }
DSTATUS disk_initialize(BYTE drive) { return disk_status(drive); }
DRESULT disk_read(BYTE drive, BYTE* data, LBA_t sector, UINT count) {
  Image* image = image_for(drive);
  if (!image) return RES_NOTRDY;
  if (!data || count == 0 || sector >= image->sectors || count > image->sectors - sector) return RES_PARERR;
  const uint64_t bytes = static_cast<uint64_t>(count) * image->sector_size;
  if (bytes > std::numeric_limits<size_t>::max()) return RES_PARERR;
  return image->read(sector * image->sector_size, data, static_cast<size_t>(bytes)) ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void* data) {
  Image* image = image_for(drive);
  if (!image) return RES_NOTRDY;
  if (!data) return RES_PARERR;
  if (command == GET_SECTOR_SIZE) {
    *static_cast<WORD*>(data) = image->sector_size;
    return RES_OK;
  }
  return RES_PARERR;
}
}

namespace rarftp {

struct ExfatArchive::Impl {
  struct Directory {
    DIR dir{};
    std::string name;
  };

  Image image;
  FATFS fs{};
  int drive = -1;
  std::string prefix;
  std::vector<std::unique_ptr<Directory>> directories;
  std::unordered_set<DWORD> seen_directories;
  ArchiveCallbacks callbacks;
  ArchiveFlags flags;
  ArchiveEntry current;
  bool has_current = false;
  bool pending_directory = false;
  bool aborted = false;

  Impl(const std::string& path, ArchiveCallbacks cb) : image(path), callbacks(std::move(cb)) {
    flags.checksums = false;
    flags.size = image.size;
    flags.volume_count = 1;
  }

  ~Impl() {
    std::lock_guard lock(fatfs_mutex);
    if (drive >= 0) {
      for (const auto& directory : directories) f_closedir(&directory->dir);
      f_mount(nullptr, prefix.c_str(), 0);
      images[static_cast<size_t>(drive)] = nullptr;
    }
  }

  // Called with fatfs_mutex held. A malformed directory may refer to itself or
  // a previously visited directory; reject it instead of descending forever.
  void push_directory(const std::string& name) {
    auto directory = std::make_unique<Directory>();
    directory->name = name;
    check(f_opendir(&directory->dir, (prefix + "/" + name).c_str()), "cannot open directory " + name);
    const DWORD cluster =
        directory->dir.obj.sclust == 0 ? static_cast<DWORD>(fs.dirbase) : directory->dir.obj.sclust;
    if (!seen_directories.insert(cluster).second) {
      f_closedir(&directory->dir);
      exfat_error("cyclic or cross-linked directory " + name);
    }
    directories.push_back(std::move(directory));
  }

  void validate_entry(const uint8_t* metadata, bool directory) const {
    const uint64_t valid = le64(metadata + 40);
    const uint64_t size = le64(metadata + 56);
    const uint32_t cluster = le32(metadata + 52);
    const uint64_t count = size / image.cluster_bytes + (size % image.cluster_bytes != 0 ? 1 : 0);
    if (valid > size || count > image.clusters || (size != 0 && (cluster < 2 || cluster - 2 >= image.clusters)) ||
        (size == 0 && cluster != 0) ||
        ((metadata[33] & 2) && size != 0 && count > image.clusters - (cluster - 2)) ||
        (directory && (valid != size || size == 0 || size % image.cluster_bytes != 0 || size > 0x10000000))) {
      exfat_error("invalid file or directory allocation");
    }
  }
};

bool ExfatArchive::recognizes(const std::string& path) {
  std::ifstream file(to_path(path), std::ios::binary);
  std::array<uint8_t, 11> header{};
  file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
  return file.good() && signature(header.data());
}

ExfatArchive::ExfatArchive(const std::string& path, ArchiveCallbacks callbacks)
    : impl_(std::make_unique<Impl>(path, std::move(callbacks))) {
  std::lock_guard lock(fatfs_mutex);
  Impl& m = *impl_;
  for (size_t i = 0; i < images.size(); ++i) {
    if (images[i] == nullptr) {
      m.drive = static_cast<int>(i);
      m.prefix = std::to_string(i) + ":";
      images[i] = &m.image;
      break;
    }
  }
  if (m.drive < 0) exfat_error("too many images open at once");
  check(f_mount(&m.fs, m.prefix.c_str(), 1), "cannot read the volume");
  if (m.fs.fs_type != FS_EXFAT || m.fs.volbase != 0) exfat_error("expected a single exFAT volume");
  m.push_directory("");
}

ExfatArchive::~ExfatArchive() = default;
const ArchiveFlags& ExfatArchive::flags() const { return impl_->flags; }
bool ExfatArchive::aborted_by_callback() const { return impl_->aborted; }

bool ExfatArchive::next(ArchiveEntry& entry) {
  std::lock_guard lock(fatfs_mutex);
  Impl& m = *impl_;
  if (m.pending_directory) {
    m.pending_directory = false;
    m.push_directory(m.current.name);
  }
  m.has_current = false;
  while (!m.directories.empty()) {
    Impl::Directory& directory = *m.directories.back();
    FILINFO info{};
    check(f_readdir(&directory.dir, &info), "cannot read directory " + directory.name);
    if (info.fname[0] == 0) {
      check(f_closedir(&directory.dir), "cannot close directory " + directory.name);
      m.directories.pop_back();
      continue;
    }
    const std::string name = info.fname;
    if (name == "?" || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos ||
        !is_valid_utf8(name))
      exfat_error("invalid or inaccessible file name");
    const bool is_directory = (info.fattrib & AM_DIR) != 0;
    // FF_USE_LFN=1 keeps this shared buffer alive after f_readdir. Capture its
    // fields while holding the lock, before any other FatFs API can reuse it.
    m.validate_entry(m.fs.dirbuf, is_directory);
    m.current = {};
    m.current.name = directory.name.empty() ? name : directory.name + "/" + name;
    m.current.kind = is_directory ? EntryKind::Directory : EntryKind::File;
    m.current.size = info.fsize;
    m.current.mtime = modification_time(m.fs.dirbuf);
    m.pending_directory = is_directory;
    m.has_current = true;
    entry = m.current;
    return true;
  }
  return false;
}

void ExfatArchive::test() {
  Impl& m = *impl_;
  if (!m.has_current || m.current.kind != EntryKind::File) return;
  OpenFatfsFile opened;
  uint64_t valid = 0;
  {
    std::lock_guard lock(fatfs_mutex);
    check(f_open(&opened.file, (m.prefix + "/" + m.current.name).c_str(), FA_READ),
          "cannot open " + m.current.name);
    opened.opened = true;
    m.validate_entry(m.fs.dirbuf, false);
    if (f_size(&opened.file) != m.current.size) exfat_error("the image changed while it was being read");
    valid = le64(m.fs.dirbuf + 40);
  }
  std::vector<uint8_t> buffer(1 << 20);
  uint64_t offset = 0;
  while (offset < m.current.size) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), m.current.size - offset));
    const UINT initialized = static_cast<UINT>(offset < valid ? std::min<uint64_t>(count, valid - offset) : 0);
    if (initialized != 0) {
      std::lock_guard lock(fatfs_mutex);
      UINT got = 0;
      check(f_read(&opened.file, buffer.data(), initialized, &got), "cannot read " + m.current.name);
      if (got != initialized) exfat_error("unexpected end of " + m.current.name);
    }
    // FatFs does not implement ValidDataLength; never expose uninitialized
    // clusters. The exFAT specification requires zeroes beyond this point.
    std::fill(buffer.begin() + initialized, buffer.begin() + count, uint8_t{0});
    if (m.callbacks.on_data && !m.callbacks.on_data(buffer.data(), count)) {
      m.aborted = true;
      exfat_error("aborted by callback");
    }
    offset += count;
  }
  m.has_current = false;
}

void ExfatArchive::skip() { impl_->has_current = false; }

}  // namespace rarftp
