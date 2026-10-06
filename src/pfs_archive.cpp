#include "pfs_archive.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "exfat_archive.hpp"
#include "ufs_archive.hpp"
#include "util/text.hpp"

namespace streamextract {
namespace {
constexpr uint64_t kMagic = 20130315;
constexpr uint32_t kInodeSize = 168;

[[noreturn]] void pfs_error(const std::string& text) {
  throw ArchiveError(ArchiveError::Kind::Other, "PFS: " + text);
}
}  // namespace

struct PfsArchive::Impl {
  struct Inode {
    uint16_t mode = 0;
    bool compressed = false;
    uint64_t stored_size = 0;
    uint64_t size = 0;
    uint64_t start = 0;
    int64_t mtime = 0;
  };
  struct Directory {
    std::shared_ptr<ImageReader> data;
    std::string name;
    uint64_t offset = 0;
  };
  struct Dirent {
    uint32_t inode = 0;
    uint32_t type = 0;
    std::string name;
  };
  std::shared_ptr<ImageReader> source;
  ArchiveCallbacks callbacks;
  ArchiveFlags flags;
  std::unique_ptr<Archive> inner;
  uint32_t block_size = 0;
  uint64_t inode_count = 0;
  uint64_t inode_blocks = 0;
  uint64_t blocks = 0;
  uint32_t root = 0;
  uint64_t cached_inode_block = std::numeric_limits<uint64_t>::max();
  std::vector<uint8_t> inode_data;
  uint32_t current_number = 0;
  Inode current;
  ArchiveEntry current_entry;
  bool has_current = false;
  bool pending_directory = false;
  bool aborted = false;
  std::vector<Directory> directories;
  std::unordered_set<uint32_t> seen_directories;
  std::unordered_set<uint32_t> seen_files;

  Impl(std::shared_ptr<ImageReader> image, ArchiveCallbacks cb, unsigned depth)
      : source(std::move(image)), callbacks(std::move(cb)) {
    if (depth >= 8) pfs_error("too many nested image wrappers");
    std::array<uint8_t, 1024> header{};
    source->read(0, header.data(), header.size());
    const uint64_t version = image_le64(header.data());
    if (image_le64(header.data() + 8) != kMagic || (version != 1 && version != 2))
      pfs_error("invalid header or unsupported PFS version");
    const uint16_t mode = image_le16(header.data() + 28);
    if (mode & 4) pfs_error("encrypted images are not supported");
    if (mode & 1) pfs_error("signed images are not supported");
    if (mode & 2) pfs_error("64-bit inodes are not supported; use MkPFS --inode-bits 32");
    if (mode & ~uint16_t{15}) pfs_error("unsupported image mode");
    block_size = image_le32(header.data() + 32);
    inode_count = image_le64(header.data() + 48);
    blocks = image_le64(header.data() + 56);
    inode_blocks = image_le64(header.data() + 64);
    if (block_size < 4096 || block_size > 65536 || (block_size & (block_size - 1)) != 0 ||
        image_le64(header.data() + 40) != 1 || inode_count < 3 || inode_count > 0xffffffff || inode_blocks == 0 ||
        blocks > source->size() / block_size || blocks < 2 || inode_blocks >= blocks - 1 ||
        inode_count > inode_blocks * (block_size / kInodeSize))
      pfs_error("invalid volume geometry or truncated image");
    flags.checksums = false;  // Unsigned PFS and raw PFSC blocks have no data checksum.
    flags.size = source->size();
    flags.volume_count = 1;
    inode_data.resize(block_size);
    // Validate allocations and find compression without storing the inode table.
    for (uint64_t i = 0; i < inode_count; ++i) {
      if (inode(static_cast<uint32_t>(i)).compressed) flags.compression = "PFSC";
    }
    Directory super{payload(inode(0)), "", 0};
    Dirent ent;
    bool found = false;
    while (dirent(super, ent)) {
      if (ent.name != "uroot") continue;
      if (found || ent.type != 3) pfs_error("invalid user root directory");
      found = true;
      root = ent.inode;
    }
    if (!found || root == 0) pfs_error("missing user root directory");
    current = inode(root);
    current_number = root;
    push_directory("");
    // MkPFS pack file / default pack folder contain a single inner volume.
    // Inspect its contents by signature, preserving a normal single-file PFS.
    Directory probe = directories.front();
    Dirent candidate;
    if (dirent(probe, candidate) && candidate.type == 2 && !dirent(probe, ent)) {
      const auto file = inode(candidate.inode);
      if ((file.mode & 0xf000) != 0x8000) return;
      auto view = payload(file);
      if (ExfatArchive::recognizes(*view)) {
        inner = std::make_unique<ExfatArchive>(view, callbacks);
      } else if (PfsArchive::recognizes(*view)) {
        inner = std::make_unique<PfsArchive>(view, callbacks, depth + 1);
      } else if (UfsArchive::recognizes(*view)) {
        inner = std::make_unique<UfsArchive>(view, callbacks);
      }
      if (inner) {
        directories.clear();
        if (flags.compression.empty()) flags.compression = inner->flags().compression;
      }
    }
  }

  Inode inode(uint32_t number) {
    if (number >= inode_count) pfs_error("invalid inode number");
    const uint32_t per_block = block_size / kInodeSize;
    const uint64_t table_block = 1 + static_cast<uint64_t>(number / per_block);
    if (cached_inode_block != table_block) {
      source->read(table_block * block_size, inode_data.data(), inode_data.size());
      cached_inode_block = table_block;
    }
    const uint8_t* bytes = inode_data.data() + (number % per_block) * kInodeSize;
    Inode result;
    result.mode = image_le16(bytes);
    result.compressed = (image_le32(bytes + 4) & 1) != 0;
    const uint64_t size = image_le64(bytes + 8);
    const uint64_t size_compressed = image_le64(bytes + 16);
    result.stored_size = result.compressed ? size : size_compressed;
    result.size = result.compressed ? size_compressed : size;
    result.mtime = static_cast<int64_t>(image_le64(bytes + 32));
    const uint32_t count = image_le32(bytes + 96);
    result.start = image_le32(bytes + 100);
    if (count == 0 || result.start < 1 + inode_blocks || result.start >= blocks || count > blocks - result.start ||
        result.stored_size > static_cast<uint64_t>(count) * block_size ||
        result.size > static_cast<uint64_t>(INT64_MAX) || (!result.compressed && result.size > result.stored_size))
      pfs_error("invalid inode allocation or file size");
    return result;
  }

  std::shared_ptr<ImageReader> payload(const Inode& file) {
    auto view = image_slice(source, file.start * block_size, file.stored_size);
    return file.compressed ? pfsc_image(std::move(view), file.size) : image_slice(std::move(view), 0, file.size);
  }

  bool dirent(Directory& directory, Dirent& result) {
    while (directory.offset < directory.data->size()) {
      if (directory.data->size() - directory.offset < 16) pfs_error("truncated directory record");
      std::array<uint8_t, 16> header{};
      directory.data->read(directory.offset, header.data(), header.size());
      const uint32_t ino = image_le32(header.data());
      const uint32_t type = image_le32(header.data() + 4);
      const uint32_t length = image_le32(header.data() + 8);
      const uint32_t record = image_le32(header.data() + 12);
      if (ino == 0 && type == 0 && length == 0 && record == 0) return false;
      if (record < 24 || record % 8 != 0 || record > directory.data->size() - directory.offset || length == 0 ||
          length > 255 || length >= record - 16 || ino >= inode_count || type < 2 || type > 5)
        pfs_error("invalid directory record");
      std::array<uint8_t, 256> name{};
      directory.data->read(directory.offset + 16, name.data(), length + 1);
      directory.offset += record;
      result = {ino, type, std::string(reinterpret_cast<const char*>(name.data()), length)};
      if (name[length] != 0 || result.name.find('\0') != std::string::npos ||
          result.name.find('/') != std::string::npos || !is_valid_utf8(result.name))
        pfs_error("invalid file name");
      if (type == 4 || type == 5) {
        if (result.name != (type == 4 ? "." : "..")) pfs_error("invalid dot directory entry");
        continue;
      }
      if (result.name == "." || result.name == "..") pfs_error("invalid directory name");
      return true;
    }
    return false;
  }

  void push_directory(const std::string& name) {
    if ((current.mode & 0xf000) != 0x4000 || !seen_directories.insert(current_number).second)
      pfs_error("invalid, cyclic or cross-linked directory " + name);
    directories.push_back({payload(current), name, 0});
  }
};

bool PfsArchive::recognizes(ImageReader& source) {
  if (source.size() < 16) return false;
  std::array<uint8_t, 16> bytes{};
  source.read(0, bytes.data(), bytes.size());
  return image_le64(bytes.data() + 8) == kMagic;
}
PfsArchive::PfsArchive(std::shared_ptr<ImageReader> source, ArchiveCallbacks callbacks, unsigned depth)
    : impl_(std::make_unique<Impl>(std::move(source), std::move(callbacks), depth)) {}
PfsArchive::~PfsArchive() = default;
const ArchiveFlags& PfsArchive::flags() const { return impl_->flags; }
bool PfsArchive::aborted_by_callback() const {
  return impl_->aborted || (impl_->inner && impl_->inner->aborted_by_callback());
}

bool PfsArchive::next(ArchiveEntry& entry) {
  Impl& m = *impl_;
  if (m.inner) return m.inner->next(entry);
  if (m.pending_directory) {
    m.pending_directory = false;
    m.push_directory(m.current_entry.name);
  }
  m.has_current = false;
  while (!m.directories.empty()) {
    auto& directory = m.directories.back();
    Impl::Dirent ent;
    if (!m.dirent(directory, ent)) {
      m.directories.pop_back();
      continue;
    }
    m.current = m.inode(ent.inode);
    m.current_number = ent.inode;
    const uint16_t type = m.current.mode & 0xf000;
    if ((ent.type == 3 && type != 0x4000) || (ent.type == 2 && type != 0x8000 && type != 0xa000))
      pfs_error("directory entry and inode type disagree");
    m.current_entry = {};
    m.current_entry.name = directory.name.empty() ? ent.name : directory.name + "/" + ent.name;
    m.current_entry.kind = type == 0x4000   ? EntryKind::Directory
                           : type == 0xa000 ? EntryKind::Symlink
                                            : EntryKind::File;
    if (m.current_entry.kind == EntryKind::File && !m.seen_files.insert(ent.inode).second)
      m.current_entry.kind = EntryKind::Hardlink;
    m.current_entry.size = m.current.size;
    m.current_entry.mtime = m.current.mtime;
    m.pending_directory = m.current_entry.kind == EntryKind::Directory;
    m.has_current = true;
    entry = m.current_entry;
    return true;
  }
  return false;
}
void PfsArchive::test() {
  Impl& m = *impl_;
  if (m.inner) return m.inner->test();
  if (!m.has_current || m.current_entry.kind != EntryKind::File) return;
  const auto view = m.payload(m.current);
  std::vector<uint8_t> buffer(1 << 20);
  for (uint64_t offset = 0; offset < view->size();) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), view->size() - offset));
    view->read(offset, buffer.data(), count);
    if (m.callbacks.on_data && !m.callbacks.on_data(buffer.data(), count)) {
      m.aborted = true;
      pfs_error("aborted by callback");
    }
    offset += count;
  }
  m.has_current = false;
}
void PfsArchive::skip() {
  if (impl_->inner) impl_->inner->skip();
  impl_->has_current = false;
}

}  // namespace streamextract
