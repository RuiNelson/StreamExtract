#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "archive.hpp"

namespace rarftp {

// ZIP, 7z and tar archives, read with libarchive.
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

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// The format of `parts` (an archive, or the parts of a split one) according to
// libarchive's own detection, if it is ZIP, 7z or tar.
std::optional<ArchiveFormat> libarchive_format(const std::vector<std::string>& parts);

// The compression of a compressed tar archive (.tar.gz...), which is not
// supported: "gzip", "bzip2", "xz", "lzma" or "zstd".
std::optional<std::string> tar_compression(const std::vector<std::string>& parts);

// libarchive version, e.g. "3.8.9".
std::string libarchive_version();

}  // namespace rarftp
