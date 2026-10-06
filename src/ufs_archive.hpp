#pragma once

#include "archive.hpp"
#include "image_reader.hpp"

namespace streamextract {

// A raw UFS1/UFS2 volume, also used for the inner image of a PFS wrapper.
class UfsArchive final : public Archive {
 public:
  static bool recognizes(ImageReader& source);
  UfsArchive(std::shared_ptr<ImageReader> source, ArchiveCallbacks callbacks);
  ~UfsArchive() override;
  ArchiveFormat format() const override { return ArchiveFormat::Ufs; }
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
