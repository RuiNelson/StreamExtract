#pragma once

#include <memory>
#include <string>

#include "archive.hpp"

namespace streamextract {

// A single, uncompressed exFAT volume image, read through FatFs. Disk images
// with a partition table are rejected; no OS mount or temporary files are used.
class ExfatArchive final : public Archive {
 public:
  static bool recognizes(const std::string& path);
  ExfatArchive(const std::string& path, ArchiveCallbacks callbacks);
  ~ExfatArchive() override;
  ExfatArchive(const ExfatArchive&) = delete;
  ExfatArchive& operator=(const ExfatArchive&) = delete;

  ArchiveFormat format() const override { return ArchiveFormat::Exfat; }
  const ArchiveFlags& flags() const override;
  bool next(ArchiveEntry& entry) override;
  void test() override;
  void skip() override;
  bool aborted_by_callback() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace streamextract
