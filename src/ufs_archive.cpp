/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*-
 * SPDX-License-Identifier: (BSD-2-Clause AND BSD-3-Clause)
 *
 * Copyright (c) 2002 Networks Associates Technology, Inc.
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Marshall
 * Kirk McKusick and Network Associates Laboratories, the Security
 * Research Division of Network Associates, Inc. under DARPA/SPAWAR
 * contract N66001-01-C-8035 ("CBOSS"), as part of the DARPA CHATS
 * research program
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (c) 1982, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the authors may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// On-disk layouts and cylinder-group addressing adapted from FreeBSD's
// sys/ufs/ffs/fs.h, sys/ufs/ufs/dinode.h and sys/ufs/ufs/dir.h.
// Their copyright notices and licenses are retained above and in
// THIRD_PARTY_NOTICES.md.

#include "ufs_archive.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "util/text.hpp"

namespace streamextract {
namespace {
constexpr uint32_t kUfs1 = 0x00011954;
constexpr uint32_t kUfs2 = 0x19540119;
constexpr std::array<uint64_t, 2> kSuperblocks{65536, 8192};

[[noreturn]] void ufs_error(const std::string& text) {
  throw ArchiveError(ArchiveError::Kind::Other, "UFS: " + text);
}

uint64_t number(const uint8_t* p, size_t width, bool big) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value = (value << 8) | p[big ? i : width - 1 - i];
  return value;
}
bool magic(const uint8_t* p) {
  const auto little = image_le32(p);
  const auto big = number(p, 4, true);
  return little == kUfs1 || little == kUfs2 || big == kUfs1 || big == kUfs2;
}
bool power_of_two(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

// FreeBSD's metadata CRC32C uses an initial all-ones CRC, without a final XOR.
uint32_t crc32c(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
  }
  return crc;
}
}  // namespace

struct UfsArchive::Impl {
  struct Inode {
    uint16_t mode = 0;
    uint64_t size = 0;
    int64_t mtime = 0;
    std::array<uint64_t, 15> pointers{};
  };
  struct Directory {
    Inode inode;
    std::string name;
    uint64_t offset = 0;
  };
  std::shared_ptr<ImageReader> source;
  ArchiveCallbacks callbacks;
  ArchiveFlags flags;
  bool big = false;
  bool ufs2 = false;
  bool old_directories = false;
  bool aborted = false;
  bool has_current = false;
  bool pending_directory = false;
  uint32_t block_size = 0;
  uint32_t fragment_size = 0;
  uint32_t fragments = 0;
  uint32_t groups = 0;
  uint32_t inodes_per_group = 0;
  uint32_t inodes_per_block = 0;
  uint32_t fragments_per_group = 0;
  uint32_t inode_start = 0;
  uint32_t group_offset = 0;
  uint32_t group_mask = 0;
  uint32_t meta_checksums = 0;
  uint64_t volume_fragments = 0;
  Inode current;
  uint32_t current_number = 0;
  ArchiveEntry current_entry;
  std::vector<Directory> directories;
  std::unordered_set<uint32_t> seen_directories;
  std::unordered_set<uint32_t> seen_files;
  std::array<uint64_t, 3> cached_indirect{};
  std::array<std::vector<uint8_t>, 3> indirect_data;

  uint32_t u32(const uint8_t* p) const { return static_cast<uint32_t>(number(p, 4, big)); }
  uint64_t u64(const uint8_t* p) const { return number(p, 8, big); }

  Impl(std::shared_ptr<ImageReader> image, ArchiveCallbacks cb)
      : source(std::move(image)), callbacks(std::move(cb)) {
    std::array<uint8_t, 8192> sb{};
    bool found = false;
    for (uint64_t offset : kSuperblocks) {
      if (offset > source->size() || sb.size() > source->size() - offset) continue;
      source->read(offset, sb.data(), sb.size());
      if (magic(sb.data() + 1372)) {
        found = true;
        break;
      }
    }
    if (!found) ufs_error("expected a raw UFS1 or UFS2 volume at byte zero");
    big = image_le32(sb.data() + 1372) != kUfs1 && image_le32(sb.data() + 1372) != kUfs2;
    ufs2 = u32(sb.data() + 1372) == kUfs2;
    inode_start = u32(sb.data() + 16);
    group_offset = u32(sb.data() + 24);
    group_mask = u32(sb.data() + 28);
    groups = u32(sb.data() + 44);
    block_size = u32(sb.data() + 48);
    fragment_size = u32(sb.data() + 52);
    fragments = u32(sb.data() + 56);
    inodes_per_block = u32(sb.data() + 120);
    inodes_per_group = u32(sb.data() + 184);
    fragments_per_group = u32(sb.data() + 188);
    volume_fragments = ufs2 ? u64(sb.data() + 1080) : u32(sb.data() + 36);
    meta_checksums = (u32(sb.data() + 1312) & 0x200) ? u32(sb.data() + 1308) : 0;
    old_directories = !ufs2 && u32(sb.data() + 1320) == 0;
    const uint32_t inode_size = ufs2 ? 256 : 128;
    const uint32_t pointer_size = ufs2 ? 8 : 4;
    if (!power_of_two(block_size) || block_size < 4096 || block_size > 65536 || !power_of_two(fragment_size) ||
        fragment_size < 512 || fragment_size > block_size || fragments != block_size / fragment_size ||
        fragments > 8 || groups == 0 || inodes_per_group < 3 || inodes_per_block != block_size / inode_size ||
        inodes_per_group % inodes_per_block != 0 || u32(sb.data() + 116) != block_size / pointer_size ||
        fragments_per_group == 0 || inode_start == 0 || inode_start >= fragments_per_group ||
        volume_fragments == 0 || volume_fragments > source->size() / fragment_size ||
        static_cast<uint64_t>(groups - 1) * fragments_per_group >= volume_fragments)
      ufs_error("invalid volume geometry or truncated image");
    const uint32_t sb_size = u32(sb.data() + 104);
    if (sb_size < 1376 || sb_size > sb.size()) ufs_error("invalid superblock size");
    if ((meta_checksums & 1) != 0) {
      const uint32_t expected = u32(sb.data() + 1304);
      std::fill_n(sb.data() + 1304, 4, uint8_t{0});
      if (crc32c(sb.data(), sb_size) != expected) ufs_error("superblock checksum mismatch");
    }
    flags.checksums = false;
    flags.size = source->size();
    flags.volume_count = 1;
    for (auto& data : indirect_data) data.resize(block_size);
    current = inode(2);
    current_number = 2;
    push_directory("");
  }

  Inode inode(uint32_t index) {
    if (index < 2 || index / inodes_per_group >= groups) ufs_error("invalid inode number");
    const uint64_t group = index / inodes_per_group;
    uint64_t start = group * fragments_per_group;
    if (!ufs2) start += static_cast<uint64_t>(group_offset) * (group & ~group_mask);
    const uint64_t fragment =
        start + inode_start + (index % inodes_per_group / inodes_per_block) * static_cast<uint64_t>(fragments);
    const size_t length = ufs2 ? 256 : 128;
    std::array<uint8_t, 256> bytes{};
    const uint64_t within = (index % inodes_per_block) * length;
    if (fragment >= volume_fragments || within + length > (volume_fragments - fragment) * fragment_size)
      ufs_error("inode outside the volume");
    source->read(fragment * fragment_size + within, bytes.data(), length);
    Inode result;
    result.mode = static_cast<uint16_t>(number(bytes.data(), 2, big));
    if (result.mode == 0) ufs_error("directory refers to an unallocated inode");
    if (ufs2 && (meta_checksums & 4) != 0) {
      const uint32_t expected = u32(bytes.data() + 244);
      std::fill_n(bytes.data() + 244, 4, uint8_t{0});
      if (crc32c(bytes.data(), length) != expected) ufs_error("inode checksum mismatch");
    }
    result.size = u64(bytes.data() + (ufs2 ? 16 : 8));
    result.mtime = ufs2 ? static_cast<int64_t>(u64(bytes.data() + 40)) : u32(bytes.data() + 24);
    const size_t width = ufs2 ? 8 : 4;
    for (size_t i = 0; i < result.pointers.size(); ++i)
      result.pointers[i] = number(bytes.data() + (ufs2 ? 112 : 40) + i * width, width, big);
    const uint64_t n = block_size / width;
    if (result.size > (12 + n + n * n + n * n * n) * block_size)
      ufs_error("file size exceeds the inode's addressing capacity");
    return result;
  }

  void push_directory(const std::string& name) {
    if ((current.mode & 0xf000) != 0x4000 || current.size == 0 || current.size % 512 != 0 ||
        !seen_directories.insert(current_number).second)
      ufs_error("invalid, cyclic or cross-linked directory " + name);
    directories.push_back({current, name, 0});
  }

  uint64_t block(const Inode& file, uint64_t logical) {
    if (logical < 12) return file.pointers[static_cast<size_t>(logical)];
    logical -= 12;
    const uint64_t n = block_size / (ufs2 ? 8 : 4);
    uint64_t capacity = n;
    for (size_t level = 1; level <= 3; ++level) {
      if (logical < capacity) {
        uint64_t pointer = file.pointers[11 + level];
        for (size_t depth = level; depth != 0; --depth) {
          if (pointer == 0) return 0;  // Sparse indirect subtree.
          if (pointer >= volume_fragments || fragments > volume_fragments - pointer)
            ufs_error("indirect block outside the volume");
          capacity /= n;
          const uint64_t index = logical / capacity;
          logical %= capacity;
          const size_t width = ufs2 ? 8 : 4;
          const size_t slot = depth - 1;
          if (cached_indirect[slot] != pointer) {
            source->read(pointer * fragment_size, indirect_data[slot].data(), block_size);
            cached_indirect[slot] = pointer;
          }
          pointer = number(indirect_data[slot].data() + index * width, width, big);
        }
        return pointer;
      }
      logical -= capacity;
      capacity *= n;
    }
    ufs_error("invalid logical block number");
  }

  void read(const Inode& file, uint64_t offset, uint8_t* data, size_t count) {
    if (offset > file.size || count > file.size - offset) ufs_error("read outside a file");
    while (count != 0) {
      const uint64_t pointer = block(file, offset / block_size);
      const size_t within = static_cast<size_t>(offset % block_size);
      const size_t take = std::min<size_t>(count, block_size - within);
      if (pointer == 0) {
        std::fill_n(data, take, uint8_t{0});
      } else {
        if (pointer >= volume_fragments || within + take > (volume_fragments - pointer) * fragment_size)
          ufs_error("data block outside the volume");
        source->read(pointer * fragment_size + within, data, take);
      }
      offset += take;
      data += take;
      count -= take;
    }
  }
};

bool UfsArchive::recognizes(ImageReader& source) {
  for (uint64_t offset : kSuperblocks) {
    if (offset + 1376 > source.size()) continue;
    std::array<uint8_t, 4> bytes{};
    source.read(offset + 1372, bytes.data(), bytes.size());
    if (magic(bytes.data())) return true;
  }
  return false;
}
UfsArchive::UfsArchive(std::shared_ptr<ImageReader> source, ArchiveCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(source), std::move(callbacks))) {}
UfsArchive::~UfsArchive() = default;
const ArchiveFlags& UfsArchive::flags() const { return impl_->flags; }
bool UfsArchive::aborted_by_callback() const { return impl_->aborted; }

bool UfsArchive::next(ArchiveEntry& entry) {
  Impl& m = *impl_;
  if (m.pending_directory) {
    m.pending_directory = false;
    m.push_directory(m.current_entry.name);
  }
  m.has_current = false;
  while (!m.directories.empty()) {
    auto& directory = m.directories.back();
    if (directory.offset == directory.inode.size) {
      m.directories.pop_back();
      continue;
    }
    std::array<uint8_t, 512> bytes{};
    m.read(directory.inode, directory.offset, bytes.data(), 8);
    const uint32_t ino = m.u32(bytes.data());
    const auto record = static_cast<size_t>(number(bytes.data() + 4, 2, m.big));
    const auto length = static_cast<size_t>(m.old_directories ? number(bytes.data() + 6, 2, m.big) : bytes[7]);
    if (record < 8 || record % 4 != 0 || record > 512 - directory.offset % 512 ||
        record > directory.inode.size - directory.offset || length > 255 || length + 9 > record)
      ufs_error("invalid directory record");
    if (ino == 0 || (!m.old_directories && bytes[6] == 14)) {
      directory.offset += record;
      continue;
    }
    m.read(directory.inode, directory.offset + 8, bytes.data() + 8, length + 1);
    directory.offset += record;
    const std::string name(reinterpret_cast<const char*>(bytes.data() + 8), length);
    if (bytes[8 + length] != 0 || name.empty() || name.find('\0') != std::string::npos ||
        name.find('/') != std::string::npos || !is_valid_utf8(name))
      ufs_error("invalid file name");
    if (name == "." || name == "..") continue;
    m.current = m.inode(ino);
    m.current_number = ino;
    m.current_entry = {};
    m.current_entry.name = directory.name.empty() ? name : directory.name + "/" + name;
    const auto type = m.current.mode & 0xf000;
    m.current_entry.kind = type == 0x4000   ? EntryKind::Directory
                           : type == 0x8000 ? EntryKind::File
                           : type == 0xa000 ? EntryKind::Symlink
                                            : EntryKind::Special;
    if (m.current_entry.kind == EntryKind::File && !m.seen_files.insert(ino).second)
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
void UfsArchive::test() {
  Impl& m = *impl_;
  if (!m.has_current || m.current_entry.kind != EntryKind::File) return;
  std::vector<uint8_t> buffer(1 << 20);
  for (uint64_t offset = 0; offset < m.current.size;) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), m.current.size - offset));
    m.read(m.current, offset, buffer.data(), count);
    if (m.callbacks.on_data && !m.callbacks.on_data(buffer.data(), count)) {
      m.aborted = true;
      ufs_error("aborted by callback");
    }
    offset += count;
  }
  m.has_current = false;
}
void UfsArchive::skip() { impl_->has_current = false; }

}  // namespace streamextract
