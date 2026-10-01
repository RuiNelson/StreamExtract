#include "archive.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include "libarchive_reader.hpp"
#include "rar_archive.hpp"
#include "util/text.hpp"

namespace rarftp {

namespace {

std::filesystem::path to_path(const std::string& utf8) {
#ifdef _WIN32
  return std::filesystem::path(from_utf8(utf8));
#else
  return std::filesystem::path(utf8);
#endif
}

bool file_exists(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::is_regular_file(to_path(path), ignored);
}

std::string file_name_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

const char* format_name(ArchiveFormat format) {
  switch (format) {
    case ArchiveFormat::Rar:
      return "RAR";
    case ArchiveFormat::Zip:
      return "ZIP";
    case ArchiveFormat::SevenZip:
      return "7z";
    case ArchiveFormat::Tar:
      return "tar";
  }
  return "RAR";
}

std::vector<std::string> split_archive_parts(const std::string& path,
                                             const std::function<bool(const std::string&)>& exists) {
  // "<stem>.<digits>", with three or more digits, as 7-Zip names them.
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  const size_t width = dot == std::string::npos ? 0 : path.size() - dot - 1;
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || width < 3 || width > 9 ||
      path.find_first_not_of("0123456789", dot + 1) != std::string::npos) {
    return {path};
  }
  const std::string stem = path.substr(0, dot + 1);
  const auto part = [&](unsigned long long number) { return fmt::format("{}{:0{}}", stem, number, width); };

  const unsigned long long number = std::stoull(path.substr(dot + 1));
  if (number != 1) {
    if (exists(part(1))) {
      throw ArchiveError(ArchiveError::Kind::Other,
                         fmt::format("{} is not the first part of a split archive; pass {}", file_name_of(path),
                                     file_name_of(part(1))));
    }
    return {path};
  }

  std::vector<std::string> parts{path};
  while (exists(part(parts.size() + 1))) {
    parts.push_back(part(parts.size() + 1));
  }
  if (exists(part(parts.size() + 2))) {
    throw ArchiveError(ArchiveError::Kind::Other, fmt::format("volume not found: {}", part(parts.size() + 1)));
  }
  return parts;
}

std::unique_ptr<Archive> open_archive(const std::string& path, Archive::Mode mode, ArchiveCallbacks callbacks) {
  std::vector<std::string> parts = split_archive_parts(path, file_exists);

  // The formats libarchive reads are recognized by libarchive itself. Anything
  // else goes to UnRAR, which also finds RAR archives inside self-extracting
  // executables, and reports the file as unreadable or not an archive.
  if (const std::optional<ArchiveFormat> format = libarchive_format(parts)) {
    return std::make_unique<LibArchiveReader>(std::move(parts), *format, mode, std::move(callbacks));
  }
  if (parts.size() > 1) {
    throw ArchiveError(ArchiveError::Kind::Other,
                       fmt::format("{} is a numbered part of a split archive; only split ZIP, 7z and tar archives "
                                   "are supported",
                                   file_name_of(path)));
  }
  return std::make_unique<RarArchive>(path, mode, std::move(callbacks));
}

}  // namespace rarftp
