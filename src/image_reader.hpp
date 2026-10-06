#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace streamextract {

// Random access to a read-only image, including an image inside a compressed
// file. Implementations keep only bounded blocks of decoded data in memory.
class ImageReader {
 public:
  virtual ~ImageReader() = default;
  virtual uint64_t size() const = 0;
  virtual void read(uint64_t offset, uint8_t* data, size_t count) = 0;
};

std::shared_ptr<ImageReader> open_image(const std::string& path);
std::shared_ptr<ImageReader> image_slice(std::shared_ptr<ImageReader> source, uint64_t offset, uint64_t size);
std::shared_ptr<ImageReader> pfsc_image(std::shared_ptr<ImageReader> source, uint64_t logical_size);

// On-disk fields are decoded explicitly; no native struct layout or alignment.
inline uint16_t image_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t image_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t image_le64(const uint8_t* p) {
  return image_le32(p) | (static_cast<uint64_t>(image_le32(p + 4)) << 32);
}

}  // namespace streamextract
