#pragma once

#include <memory>
#include <string>
#include <vector>

#include "archive.hpp"

namespace streamextract {

// 7z archives, read with 7-Zip's own code (LZMA SDK) and, for the methods the
// SDK lacks, the decoders of sevenzip_methods.hpp.
class SevenZipArchive final : public Archive {
 public:
  // `parts`: the archive, or every part of a split one in order. Throws
  // ArchiveError, also for a compression method nothing here decodes.
  static std::unique_ptr<SevenZipArchive> open(const std::vector<std::string>& parts, Mode mode,
                                               const ArchiveCallbacks& callbacks);
  ~SevenZipArchive() override;
  SevenZipArchive(const SevenZipArchive&) = delete;
  SevenZipArchive& operator=(const SevenZipArchive&) = delete;

  ArchiveFormat format() const override { return ArchiveFormat::SevenZip; }
  const ArchiveFlags& flags() const override;
  bool next(ArchiveEntry& entry) override;
  void test() override;
  // Decompresses nothing: a later test() of the same block decompresses what
  // it has to go through. In List mode, the start of the first encrypted file
  // is decrypted so that a wrong password shows up while listing.
  void skip() override;
  bool aborted_by_callback() const override;

  struct Impl;

 private:
  explicit SevenZipArchive(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

// LZMA SDK version, e.g. "26.03".
std::string lzma_sdk_version();

}  // namespace streamextract
