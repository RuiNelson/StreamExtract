#pragma once

#include "archive.hpp"
#include "image_reader.hpp"

namespace streamextract {

// Unsigned PlayStation PFS (MkPFS D32), with on-demand PFSC decompression.
// A single wrapped exFAT/UFS/PFS volume is opened without a temporary image.
class PfsArchive final : public Archive {
 public:
  static bool recognizes(ImageReader& source);
  PfsArchive(std::shared_ptr<ImageReader> source, ArchiveCallbacks callbacks, unsigned depth = 0);
  ~PfsArchive() override;
  ArchiveFormat format() const override { return ArchiveFormat::Pfs; }
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
