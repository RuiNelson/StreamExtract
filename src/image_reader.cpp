#include "image_reader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

#include <zlib.h>

#include "archive.hpp"
#include "util/text.hpp"

namespace streamextract {
namespace {

[[noreturn]] void image_error(const std::string& text) { throw ArchiveError(ArchiveError::Kind::Other, text); }

void bounds(uint64_t size, uint64_t offset, size_t count) {
  if (offset > size || count > size - offset) image_error("image: read outside the image or truncated data");
}

class FileImage final : public ImageReader {
 public:
  explicit FileImage(const std::string& path) {
#ifdef _WIN32
    file_.open(std::filesystem::path(from_utf8(path)), std::ios::binary);
#else
    file_.open(std::filesystem::path(path), std::ios::binary);
#endif
    if (!file_) image_error("image: cannot open " + path);
    file_.seekg(0, std::ios::end);
    const std::streamoff length = file_.tellg();
    if (length < 0) image_error("image: cannot determine the image size");
    size_ = static_cast<uint64_t>(length);
    position_ = size_;
  }
  uint64_t size() const override { return size_; }
  void read(uint64_t offset, uint8_t* data, size_t count) override {
    bounds(size_, offset, count);
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
      image_error("image: read is too large");
    if (count == 0) return;
    file_.clear();
    if (position_ != offset) file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(count));
    if (!file_) {
      position_ = std::numeric_limits<uint64_t>::max();
      image_error("image: read error or truncated image");
    }
    position_ = offset + count;
  }

 private:
  std::ifstream file_;
  uint64_t size_ = 0;
  uint64_t position_ = std::numeric_limits<uint64_t>::max();
};

class SliceImage final : public ImageReader {
 public:
  SliceImage(std::shared_ptr<ImageReader> source, uint64_t offset, uint64_t size)
      : source_(std::move(source)), offset_(offset), size_(size) {
    if (offset_ > source_->size() || size_ > source_->size() - offset_) image_error("image: invalid file extent");
  }
  uint64_t size() const override { return size_; }
  void read(uint64_t offset, uint8_t* data, size_t count) override {
    bounds(size_, offset, count);
    source_->read(offset_ + offset, data, count);
  }

 private:
  std::shared_ptr<ImageReader> source_;
  uint64_t offset_;
  uint64_t size_;
};

class PfscImage final : public ImageReader {
 public:
  PfscImage(std::shared_ptr<ImageReader> source, uint64_t size) : source_(std::move(source)), size_(size) {
    std::array<uint8_t, 48> header{};
    source_->read(0, header.data(), header.size());
    const uint64_t padded = image_le64(header.data() + 40);
    table_ = image_le64(header.data() + 24);
    data_ = image_le64(header.data() + 32);
    if (std::memcmp(header.data(), "PFSC", 4) != 0 || image_le32(header.data() + 4) != 0 ||
        image_le32(header.data() + 8) != 6 || image_le32(header.data() + 12) != kBlockSize ||
        image_le64(header.data() + 16) != kBlockSize || padded % kBlockSize != 0 || padded < size_ ||
        padded - size_ >= kBlockSize || table_ < header.size() || data_ > source_->size() || table_ > data_ ||
        padded / kBlockSize >= (data_ - table_) / 8)
      image_error("PFSC: invalid header or block offset table");
    blocks_ = padded / kBlockSize;
    // Validate the complete table without retaining an image-sized index.
    uint64_t previous = data_;
    uint64_t index = 0;
    while (index <= blocks_) {
      load_offsets(index);
      for (size_t i = 0; i < offset_entries_; ++i) {
        const uint64_t value = image_le64(offsets_.data() + i * 8);
        if (value > source_->size() || (index + i == 0 ? value != data_ : value <= previous) ||
            value - previous > kBlockSize)
          image_error("PFSC: invalid block offsets");
        previous = value;
      }
      index += offset_entries_;
    }
  }
  ~PfscImage() override {
    if (stream_initialized_) inflateEnd(&stream_);
  }
  uint64_t size() const override { return size_; }
  void read(uint64_t offset, uint8_t* data, size_t count) override {
    bounds(size_, offset, count);
    while (count != 0) {
      const uint64_t block = offset / kBlockSize;
      const size_t within = static_cast<size_t>(offset % kBlockSize);
      const size_t take = std::min<size_t>(count, kBlockSize - within);
      std::memcpy(data, decode(block) + within, take);
      offset += take;
      data += take;
      count -= take;
    }
  }

 private:
  static constexpr uint32_t kBlockSize = 65536;
  std::shared_ptr<ImageReader> source_;
  uint64_t size_;
  uint64_t table_ = 0;
  uint64_t data_ = 0;
  uint64_t blocks_ = 0;
  // Keep one offset-table page, including adjacent offsets for each decoded
  // block. The complete table is still validated when the reader is opened.
  std::array<uint8_t, 8192> offsets_;
  uint64_t offset_first_ = 0;
  size_t offset_entries_ = 0;
  struct CachedBlock {
    uint64_t index = std::numeric_limits<uint64_t>::max();
    uint64_t used = 0;
    // Tiny compressed files need only one decoded block, not a 1 MiB cache.
    std::unique_ptr<std::array<uint8_t, kBlockSize>> bytes;
  };
  std::array<CachedBlock, 16> cache_{};
  uint64_t clock_ = 0;
  std::unique_ptr<uint8_t[]> stored_;
  size_t stored_capacity_ = 0;
  z_stream stream_{};
  bool stream_initialized_ = false;

  void load_offsets(uint64_t first) {
    const size_t entries = static_cast<size_t>(std::min<uint64_t>(offsets_.size() / 8, blocks_ + 1 - first));
    // Publish only after a successful read so a failed source cannot leave a
    // partially overwritten offset page marked valid.
    offset_entries_ = 0;
    source_->read(table_ + first * 8, offsets_.data(), entries * 8);
    offset_first_ = first;
    offset_entries_ = entries;
  }

  std::pair<uint64_t, uint64_t> block_offsets(uint64_t block) {
    if (block < offset_first_ || block - offset_first_ + 1 >= offset_entries_) {
      // Adjacent pages overlap by one entry, keeping boundary pairs together.
      constexpr uint64_t kPageBlocks = 8192 / 8 - 1;
      load_offsets(block / kPageBlocks * kPageBlocks);
    }
    const size_t within = static_cast<size_t>(block - offset_first_) * 8;
    return {image_le64(offsets_.data() + within), image_le64(offsets_.data() + within + 8)};
  }

  const uint8_t* decode(uint64_t block) {
    ++clock_;
    for (auto& slot : cache_) {
      if (slot.index != block) continue;
      slot.used = clock_;
      return slot.bytes->data();
    }
    auto& slot = *std::min_element(cache_.begin(), cache_.end(),
                                   [](const CachedBlock& a, const CachedBlock& b) { return a.used < b.used; });
    // A failed decode must never leave the previous cached block marked valid.
    slot.index = std::numeric_limits<uint64_t>::max();
    const auto [start, end] = block_offsets(block);
    if (start < data_ || end <= start || end > source_->size() || end - start > kBlockSize)
      image_error("PFSC: invalid block offsets");
    const size_t length = static_cast<size_t>(end - start);
    if (!slot.bytes) slot.bytes = std::make_unique<std::array<uint8_t, kBlockSize>>();
    if (length == kBlockSize) {
      source_->read(start, slot.bytes->data(), length);
    } else {
      if (stored_capacity_ < length) {
        stored_ = std::unique_ptr<uint8_t[]>(new uint8_t[length]);
        stored_capacity_ = length;
      }
      source_->read(start, stored_.get(), length);
      if (stream_initialized_) {
        if (inflateReset(&stream_) != Z_OK) image_error("PFSC: cannot reset decompression");
      } else {
        if (inflateInit(&stream_) != Z_OK) image_error("PFSC: cannot initialize decompression");
        stream_initialized_ = true;
      }
      stream_.next_in = stored_.get();
      stream_.avail_in = static_cast<uInt>(length);
      stream_.next_out = slot.bytes->data();
      stream_.avail_out = kBlockSize;
      const int result = inflate(&stream_, Z_FINISH);
      const bool valid = result == Z_STREAM_END && stream_.total_out == kBlockSize && stream_.total_in == length;
      if (!valid) image_error("PFSC: corrupt compressed block or checksum mismatch");
    }
    slot.index = block;
    slot.used = clock_;
    return slot.bytes->data();
  }
};

}  // namespace

std::shared_ptr<ImageReader> open_image(const std::string& path) { return std::make_shared<FileImage>(path); }
std::shared_ptr<ImageReader> image_slice(std::shared_ptr<ImageReader> source, uint64_t offset, uint64_t size) {
  return std::make_shared<SliceImage>(std::move(source), offset, size);
}
std::shared_ptr<ImageReader> pfsc_image(std::shared_ptr<ImageReader> source, uint64_t logical_size) {
  return std::make_shared<PfscImage>(std::move(source), logical_size);
}

}  // namespace streamextract
