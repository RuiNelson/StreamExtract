#pragma once

#include <memory>
#include <string>

#include "archive.hpp"

namespace streamextract {

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
  // In List mode, the first call on an encrypted file (data only: encrypted
  // headers are checked when opening) tests it with the password.
  void skip() override;
  bool aborted_by_callback() const override;

  struct Impl;

 private:
  // List mode: tests the first encrypted file so that a wrong password is
  // found while listing. Throws ArchiveError (BadPassword, MissingPassword).
  void check_password();

  std::unique_ptr<Impl> impl_;
};

}  // namespace streamextract
