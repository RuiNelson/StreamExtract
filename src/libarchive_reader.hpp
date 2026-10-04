#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "archive.hpp"

namespace streamextract {

// ZIP and tar archives, read with libarchive. (7z archives are recognized with
// libarchive_format() but read by SevenZipArchive.)
class LibArchiveReader final : public Archive {
 public:
  // `parts`: the archive, or every part of a split one in order. Throws
  // ArchiveError.
  LibArchiveReader(std::vector<std::string> parts, ArchiveFormat format, Mode mode, ArchiveCallbacks callbacks);
  ~LibArchiveReader() override;
  LibArchiveReader(const LibArchiveReader&) = delete;
  LibArchiveReader& operator=(const LibArchiveReader&) = delete;

  ArchiveFormat format() const override;
  const ArchiveFlags& flags() const override;
  bool next(ArchiveEntry& entry) override;
  void test() override;
  void skip() override;
  bool aborted_by_callback() const override;
  uint64_t bytes_read() const override;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// The format of `parts` (an archive, or the parts of a split one) according to
// libarchive's own detection, if it is ZIP, 7z or tar (compressed or not).
std::optional<ArchiveFormat> libarchive_format(const std::vector<std::string>& parts);

// libarchive version, e.g. "3.8.9".
std::string libarchive_version();

}  // namespace streamextract
