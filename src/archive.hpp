#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace streamextract {

enum class ArchiveFormat { Rar, Zip, SevenZip, Tar, Exfat };

// "RAR", "ZIP", "7z", "tar", "exFAT".
const char* format_name(ArchiveFormat format);

enum class EntryKind {
  File,
  Directory,
  Symlink,   // Unix/Windows symlinks and junctions: nothing to upload.
  Hardlink,  // References to another archived file.
  FileCopy,  // Identical file stored as a reference (rar -oi).
  Special,   // Devices, FIFOs, sockets.
};

struct ArchiveEntry {
  std::string name;  // UTF-8. RAR: native separators; others: '/' (some tools write '\').
  EntryKind kind = EntryKind::File;
  uint64_t size = 0;  // Unpacked size.
  int64_t mtime = 0;  // Unix seconds, UTC; 0 when unknown.
  bool encrypted = false;
  bool split_before = false;  // RAR: continuation of a file started in the previous volume.
};

struct ArchiveFlags {
  bool volume = false;  // Part of a multi-volume set.
  bool first_volume = false;
  bool solid = false;
  bool encrypted_headers = false;
  // Skipping an entry still decompresses it (solid RAR and 7z).
  bool skip_decompresses = false;
  // Entries carry a checksum of their data, verified by test(). False for
  // tar (header checksums only) and exFAT (metadata checksums only).
  bool checksums = true;
  // Compressed tar: "gzip", "bzip2", "xz", "lzma", "zstd" or "lz4"; empty otherwise.
  std::string compression;
  // The entries can only be reached by decompressing everything before them
  // (compressed tar): streamextract reads such an archive once, planning each file
  // as it comes, instead of listing it first.
  bool stream_only = false;
  // Bytes of the archive (all its parts), to measure progress with
  // Archive::bytes_read() when the totals are not known up front.
  uint64_t size = 0;
  // Volumes known up front (split ZIP, 7z and tar: .001, .002, ...); 0 for RAR,
  // whose volumes are only met while reading.
  unsigned volume_count = 0;
};

class ArchiveError : public std::runtime_error {
 public:
  enum class Kind { Other, MissingPassword, BadPassword };

  ArchiveError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
  Kind kind() const { return kind_; }

 private:
  Kind kind_;
};

struct ArchiveCallbacks {
  // Decompressed data of the entry being tested. Return false to abort.
  std::function<bool(const uint8_t* data, size_t size)> on_data;
  // The archive needs a password; std::nullopt aborts.
  std::function<std::optional<std::string>()> on_password;
  // RAR: next volume opened.
  std::function<void(const std::string& volume)> on_volume;
  // RAR: next volume not found; the operation will fail.
  std::function<void(const std::string& volume)> on_missing_volume;
  // RAR: dictionary larger than UnRAR's default limit. Return true to allow it.
  std::function<bool(uint64_t dictionary, uint64_t limit)> on_large_dictionary;
};

// Sequential reader of an archive, whatever its format.
class Archive {
 public:
  enum class Mode { List, Extract };

  virtual ~Archive() = default;

  virtual ArchiveFormat format() const = 0;
  virtual const ArchiveFlags& flags() const = 0;

  // Reads the next entry header. Returns false at the end of the archive.
  // Throws ArchiveError.
  virtual bool next(ArchiveEntry& entry) = 0;
  // Decompresses and verifies the current entry (see ArchiveFlags::checksums),
  // feeding on_data. Throws ArchiveError, also when the checksum does not match.
  virtual void test() = 0;
  // Skips the current entry (which may still decompress it, see
  // ArchiveFlags::skip_decompresses). Throws ArchiveError.
  virtual void skip() = 0;

  // True if the last failure came from one of the callbacks returning "abort".
  virtual bool aborted_by_callback() const = 0;

  // Bytes of the archive file(s) read so far (compressed data for compressed
  // tar); 0 when not known.
  virtual uint64_t bytes_read() const { return 0; }
};

// Opens a RAR, ZIP, 7z or tar archive (also compressed: .tar.gz...) or a single
// exFAT volume image, recognized
// by its content. A split ZIP, 7z or tar archive (name.zip.001, name.zip.002,
// ...) is opened through its first part. Throws ArchiveError.
std::unique_ptr<Archive> open_archive(const std::string& path, Archive::Mode mode, ArchiveCallbacks callbacks);

// --- Exposed for the tests ---------------------------------------------------

// The parts of a split archive given its first part ("x.7z.001" -> x.7z.001,
// x.7z.002, ... as long as they exist). Any other name gives just itself.
// `exists` checks a path. Throws ArchiveError for a later part ("x.7z.002")
// when the first one exists, and for a gap in the numbering.
std::vector<std::string> split_archive_parts(const std::string& path,
                                             const std::function<bool(const std::string&)>& exists);

}  // namespace streamextract
