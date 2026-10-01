#pragma once

#include <memory>
#include <string>

#include "archive.hpp"

namespace rarftp {

// UnRAR library version, e.g. "7.30 beta 1" (from version.hpp at build time).
std::string unrar_version();

// RAR archives, through the UnRAR DLL API (sequential access only).
class RarArchive final : public Archive {
 public:
  // Throws ArchiveError.
  RarArchive(const std::string& path, Mode mode, ArchiveCallbacks callbacks);
  ~RarArchive() override;
  RarArchive(const RarArchive&) = delete;
  RarArchive& operator=(const RarArchive&) = delete;

  ArchiveFormat format() const override { return ArchiveFormat::Rar; }
  const ArchiveFlags& flags() const override;
  bool next(ArchiveEntry& entry) override;
  // RAR_TEST: decompresses and verifies CRC/BLAKE2 without creating files.
  void test() override;
  // In solid archives this still decompresses the entry, without feeding on_data.
  void skip() override;
  bool aborted_by_callback() const override;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace rarftp
