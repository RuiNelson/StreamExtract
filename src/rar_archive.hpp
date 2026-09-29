#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace rarftp {

enum class EntryKind {
  File,
  Directory,
  Symlink,   // Unix/Windows symlinks and junctions: nothing to upload.
  Hardlink,  // References to another archived file.
  FileCopy,  // Identical file stored as a reference (rar -oi).
};

struct ArchiveEntry {
  std::string name;  // UTF-8, as reported by UnRAR (native separators).
  EntryKind kind = EntryKind::File;
  uint64_t size = 0;  // Unpacked size.
  int64_t mtime = 0;  // Unix seconds, UTC; 0 when unknown.
  bool encrypted = false;
  bool split_before = false;
};

struct ArchiveFlags {
  bool volume = false;
  bool first_volume = false;
  bool solid = false;
  bool encrypted_headers = false;
};

// Error from the UnRAR library, carrying the ERAR_* code.
class RarError : public std::runtime_error {
 public:
  RarError(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
  int code() const { return code_; }

 private:
  int code_;
};

std::string rar_error_message(int code);
bool rar_is_bad_password(int code);
bool rar_is_missing_password(int code);

// UnRAR library version, e.g. "7.30 beta 1" (from version.hpp at build time).
std::string unrar_version();

struct RarCallbacks {
  // Decompressed data of the entry being tested. Return false to abort.
  std::function<bool(const uint8_t* data, size_t size)> on_data;
  // UnRAR needs a password; std::nullopt aborts.
  std::function<std::optional<std::string>()> on_password;
  // Next volume opened.
  std::function<void(const std::string& volume)> on_volume;
  // Next volume not found; the operation will fail.
  std::function<void(const std::string& volume)> on_missing_volume;
  // Dictionary larger than UnRAR's default limit. Return true to allow it.
  std::function<bool(uint64_t dictionary, uint64_t limit)> on_large_dictionary;
};

// Thin RAII wrapper over the UnRAR DLL API (sequential access only).
class RarArchive {
 public:
  enum class Mode { List, Extract };

  // Throws RarError.
  RarArchive(const std::string& path, Mode mode, RarCallbacks callbacks);
  ~RarArchive();
  RarArchive(const RarArchive&) = delete;
  RarArchive& operator=(const RarArchive&) = delete;

  const ArchiveFlags& flags() const;

  // Reads the next entry header. Returns false at the end of the archive.
  // Throws RarError.
  bool next(ArchiveEntry& entry);

  // Decompresses and verifies the current entry, feeding on_data. Returns the
  // ERAR_* code (0 on success).
  int test();
  // Skips the current entry (in solid archives this still decompresses it,
  // without feeding on_data). Returns the ERAR_* code.
  int skip();

  // True if the last failure came from one of our callbacks returning "abort".
  bool aborted_by_callback() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace rarftp
