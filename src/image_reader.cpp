#include "image_reader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

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
  }
  uint64_t size() const override { return size_; }
  void read(uint64_t offset, uint8_t* data, size_t count) override {
    bounds(size_, offset, count);
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
      image_error("image: read is too large");
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(count));
    if (!file_) image_error("image: read error or truncated image");
  }

 private:
  std::ifstream file_;
  uint64_t size_ = 0;
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
    std::array<uint8_t, 8192> page{};
    uint64_t previous = data_;
    uint64_t index = 0;
    while (index <= blocks_) {
      const size_t entries = static_cast<size_t>(std::min<uint64_t>(page.size() / 8, blocks_ + 1 - index));
      source_->read(table_ + index * 8, page.data(), entries * 8);
      for (size_t i = 0; i < entries; ++i) {
        const uint64_t value = image_le64(page.data() + i * 8);
        if (value > source_->size() || (index + i == 0 ? value != data_ : value <= previous) ||
            value - previous > kBlockSize)
          image_error("PFSC: invalid block offsets");
        previous = value;
      }
      index += entries;
    }
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
  struct CachedBlock {
    uint64_t index = std::numeric_limits<uint64_t>::max();
    uint64_t used = 0;
    std::array<uint8_t, kBlockSize> bytes{};
  };
  std::array<CachedBlock, 16> cache_{};
  uint64_t clock_ = 0;
  std::array<uint8_t, kBlockSize> stored_{};

  const uint8_t* decode(uint64_t block) {
    ++clock_;
    for (auto& slot : cache_) {
      if (slot.index != block) continue;
      slot.used = clock_;
      return slot.bytes.data();
    }
    auto& slot = *std::min_element(cache_.begin(), cache_.end(),
                                   [](const CachedBlock& a, const CachedBlock& b) { return a.used < b.used; });
    // A failed decode must never leave the previous cached block marked valid.
    slot.index = std::numeric_limits<uint64_t>::max();
    std::array<uint8_t, 16> pair{};
    source_->read(table_ + block * 8, pair.data(), pair.size());
    const uint64_t start = image_le64(pair.data());
    const uint64_t end = image_le64(pair.data() + 8);
    if (start < data_ || end <= start || end > source_->size() || end - start > kBlockSize)
      image_error("PFSC: invalid block offsets");
    const size_t length = static_cast<size_t>(end - start);
    source_->read(start, stored_.data(), length);
    if (length == kBlockSize) {
      std::memcpy(slot.bytes.data(), stored_.data(), length);
    } else {
      z_stream stream{};
      stream.next_in = stored_.data();
      stream.avail_in = static_cast<uInt>(length);
      stream.next_out = slot.bytes.data();
      stream.avail_out = kBlockSize;
      if (inflateInit(&stream) != Z_OK) image_error("PFSC: cannot initialize decompression");
      const int result = inflate(&stream, Z_FINISH);
      const bool valid = result == Z_STREAM_END && stream.total_out == kBlockSize && stream.total_in == length;
      inflateEnd(&stream);
      if (!valid) image_error("PFSC: corrupt compressed block or checksum mismatch");
    }
    slot.index = block;
    slot.used = clock_;
    return slot.bytes.data();
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
