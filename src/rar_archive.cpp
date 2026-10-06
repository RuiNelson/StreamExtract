// Wrapper over the UnRAR DLL API.
//
// UnRAR source code may be used in any software to handle RAR archives
// without limitations free of charge, but cannot be used to develop RAR
// (WinRAR) compatible archiver and to re-create RAR compression algorithm,
// which is proprietary. Distribution of modified UnRAR source code in separate
// form or as a part of other software is permitted, provided that full text of
// this paragraph, starting from "UnRAR source code" words, is included in
// license, or in documentation if license is not available, and in source code
// comments of resulting package.

#include "rar_archive.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <vector>

#include <fmt/format.h>

#include "util/text.hpp"

// dll.hpp needs the platform macros below, and on Unix it defines generic
// names such as HANDLE, UINT and LONG, so it must come after every other
// header. This is the only file that includes it.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(_UNIX)
#define _UNIX
#endif
#include <dll.hpp>
#include <version.hpp>

namespace streamextract {

namespace {

// FILE_SYSTEM_REDIRECT values from UnRAR's headers.hpp.
constexpr unsigned kRedirUnixSymlink = 1;
constexpr unsigned kRedirWinSymlink = 2;
constexpr unsigned kRedirJunction = 3;
constexpr unsigned kRedirHardlink = 4;
constexpr unsigned kRedirFileCopy = 5;

// Wide characters available for a file name (RARHeaderDataEx::FileNameEx).
constexpr size_t kNameBufferSize = 32768;

}  // namespace

struct RarArchive::Impl {
  HANDLE handle = nullptr;
  ArchiveCallbacks callbacks;
  ArchiveFlags flags;
  RARHeaderDataEx header{};
  std::vector<wchar_t> name_buffer = std::vector<wchar_t>(kNameBufferSize);
  bool aborted = false;
  std::exception_ptr callback_error;

  void rethrow_callback_error() {
    if (callback_error) {
      std::exception_ptr error = callback_error;
      callback_error = nullptr;
      std::rethrow_exception(error);
    }
  }
};

namespace {

int CALLBACK rar_callback(UINT msg, LPARAM user_data, LPARAM p1, LPARAM p2) {
  auto* impl = reinterpret_cast<RarArchive::Impl*>(user_data);
  try {
    switch (msg) {
      case UCM_CHANGEVOLUMEW: {
        const auto* name = reinterpret_cast<const wchar_t*>(p1);
        const std::string volume = name != nullptr ? to_utf8(name) : std::string();
        if (p2 == RAR_VOL_ASK) {  // The next volume does not exist.
          impl->aborted = true;
          if (impl->callbacks.on_missing_volume) {
            impl->callbacks.on_missing_volume(volume);
          }
          return -1;
        }
        if (impl->callbacks.on_volume) {
          impl->callbacks.on_volume(volume);
        }
        return 1;
      }
      case UCM_CHANGEVOLUME:  // ANSI variant, already handled above.
        return p2 == RAR_VOL_ASK ? -1 : 1;
      case UCM_PROCESSDATA: {
        const auto* data = reinterpret_cast<const uint8_t*>(p1);
        const auto size = static_cast<size_t>(p2);
        if (impl->callbacks.on_data && !impl->callbacks.on_data(data, size)) {
          impl->aborted = true;
          return -1;
        }
        return 1;
      }
      case UCM_NEEDPASSWORDW: {
        auto* buffer = reinterpret_cast<wchar_t*>(p1);
        const auto capacity = static_cast<size_t>(p2);
        std::optional<std::string> password;
        if (impl->callbacks.on_password) {
          password = impl->callbacks.on_password();
        }
        if (!password || buffer == nullptr || capacity == 0) {
          impl->aborted = true;
          return -1;
        }
        std::wstring wide = from_utf8(*password);
        const size_t n = std::min(wide.size(), capacity - 1);
        std::copy_n(wide.data(), n, buffer);
        buffer[n] = 0;
        std::fill(wide.begin(), wide.end(), L'\0');
        return 1;
      }
      case UCM_NEEDPASSWORD:  // ANSI fallback: only reached if the above failed.
        return -1;
      case UCM_LARGEDICT: {
        const uint64_t dictionary = static_cast<uint64_t>(p1) * 1024;
        const uint64_t limit = static_cast<uint64_t>(p2) * 1024;
        const bool allow =
            impl->callbacks.on_large_dictionary && impl->callbacks.on_large_dictionary(dictionary, limit);
        return allow ? 1 : 0;
      }
      default:
        return 0;
    }
  } catch (...) {
    impl->callback_error = std::current_exception();
    impl->aborted = true;
    return -1;
  }
}

std::string rar_error_message(int code) {
  switch (code) {
    case ERAR_SUCCESS:
      return "success";
    case ERAR_END_ARCHIVE:
      return "unexpected end of archive";
    case ERAR_NO_MEMORY:
      return "not enough memory";
    case ERAR_BAD_DATA:
      return "corrupt data (checksum mismatch)";
    case ERAR_BAD_ARCHIVE:
      return "not a RAR, ZIP, 7z or tar archive, or an exFAT, PFS or UFS image";
    case ERAR_UNKNOWN_FORMAT:
      return "unsupported archive format or compression method";
    case ERAR_EOPEN:
      return "cannot open the archive or one of its volumes";
    case ERAR_ECREATE:
      return "cannot create file";
    case ERAR_ECLOSE:
      return "cannot close the archive";
    case ERAR_EREAD:
      return "read error";
    case ERAR_EWRITE:
      return "write error";
    case ERAR_SMALL_BUF:
      return "buffer too small";
    case ERAR_UNKNOWN:
      return "unknown error";
    case ERAR_MISSING_PASSWORD:
      return "password required";
    case ERAR_EREFERENCE:
      return "cannot resolve a file reference";
    case ERAR_BAD_PASSWORD:
      return "wrong password";
    case ERAR_LARGE_DICT:
      return "dictionary size exceeds the allowed limit";
    default:
      return fmt::format("UnRAR error {}", code);
  }
}

ArchiveError rar_error(int code) {
  switch (code) {
    case ERAR_BAD_PASSWORD:
      return ArchiveError(ArchiveError::Kind::BadPassword, rar_error_message(code));
    case ERAR_MISSING_PASSWORD:
      return ArchiveError(ArchiveError::Kind::MissingPassword, rar_error_message(code));
    default:
      return ArchiveError(ArchiveError::Kind::Other, rar_error_message(code));
  }
}

}  // namespace

std::string unrar_version() {
  std::string version = fmt::format("{}.{:02}", RARVER_MAJOR, RARVER_MINOR);
  if constexpr (RARVER_BETA != 0) {
    version += fmt::format(" beta {}", RARVER_BETA);
  }
  return version;
}

RarArchive::RarArchive(const std::string& path, Mode mode, ArchiveCallbacks callbacks)
    : impl_(std::make_unique<Impl>()) {
  impl_->callbacks = std::move(callbacks);

  RAROpenArchiveDataEx data{};
#ifdef _WIN32
  std::wstring wide_path = from_utf8(path);
  data.ArcNameW = wide_path.data();
#else
  // On Unix pass the name as raw bytes: UnRAR maps bytes it cannot convert
  // with the current locale, so any file name round-trips.
  std::string narrow_path = path;
  data.ArcName = narrow_path.data();
#endif
  data.OpenMode = mode == Mode::List ? RAR_OM_LIST : RAR_OM_EXTRACT;
  data.Callback = rar_callback;
  data.UserData = reinterpret_cast<LPARAM>(impl_.get());

  impl_->handle = RAROpenArchiveEx(&data);
  if (impl_->handle == nullptr || data.OpenResult != ERAR_SUCCESS) {
    if (impl_->handle != nullptr) {
      RARCloseArchive(impl_->handle);
      impl_->handle = nullptr;
    }
    impl_->rethrow_callback_error();
    throw rar_error(data.OpenResult != ERAR_SUCCESS ? static_cast<int>(data.OpenResult) : ERAR_UNKNOWN);
  }

  impl_->flags.volume = (data.Flags & ROADF_VOLUME) != 0;
  impl_->flags.first_volume = (data.Flags & ROADF_FIRSTVOLUME) != 0;
  impl_->flags.solid = (data.Flags & ROADF_SOLID) != 0;
  impl_->flags.encrypted_headers = (data.Flags & ROADF_ENCHEADERS) != 0;
  impl_->flags.skip_decompresses = impl_->flags.solid;
}

RarArchive::~RarArchive() {
  if (impl_ && impl_->handle != nullptr) {
    RARCloseArchive(impl_->handle);
  }
}

const ArchiveFlags& RarArchive::flags() const { return impl_->flags; }

bool RarArchive::next(ArchiveEntry& entry) {
  RARHeaderDataEx& h = impl_->header;
  std::memset(&h, 0, sizeof(h));
  h.FileNameEx = impl_->name_buffer.data();
  h.FileNameExSize = static_cast<unsigned int>(impl_->name_buffer.size());
  impl_->aborted = false;

  const int code = RARReadHeaderEx(impl_->handle, &h);
  if (code == ERAR_END_ARCHIVE) {
    return false;
  }
  if (code != ERAR_SUCCESS) {
    impl_->rethrow_callback_error();
    throw rar_error(code);
  }

  entry = ArchiveEntry{};
  entry.name = to_utf8(h.FileNameEx[0] != 0 ? h.FileNameEx : h.FileNameW);
  entry.size = (static_cast<uint64_t>(h.UnpSizeHigh) << 32) | h.UnpSize;
  const uint64_t mtime = (static_cast<uint64_t>(h.MtimeHigh) << 32) | h.MtimeLow;
  entry.mtime = mtime != 0 ? filetime_to_unix(mtime) : 0;
  entry.encrypted = (h.Flags & RHDF_ENCRYPTED) != 0;
  entry.split_before = (h.Flags & RHDF_SPLITBEFORE) != 0;
  switch (h.RedirType) {
    case kRedirUnixSymlink:
    case kRedirWinSymlink:
    case kRedirJunction:
      entry.kind = EntryKind::Symlink;
      break;
    case kRedirHardlink:
      entry.kind = EntryKind::Hardlink;
      break;
    case kRedirFileCopy:
      entry.kind = EntryKind::FileCopy;
      break;
    default:
      entry.kind = (h.Flags & RHDF_DIRECTORY) != 0 ? EntryKind::Directory : EntryKind::File;
      break;
  }
  return true;
}

void RarArchive::test() {
  impl_->aborted = false;
  const int code = RARProcessFileW(impl_->handle, RAR_TEST, nullptr, nullptr);
  impl_->rethrow_callback_error();
  if (code != ERAR_SUCCESS) {
    throw rar_error(code);
  }
}

void RarArchive::skip() {
  impl_->aborted = false;
  const int code = RARProcessFileW(impl_->handle, RAR_SKIP, nullptr, nullptr);
  impl_->rethrow_callback_error();
  if (code != ERAR_SUCCESS) {
    throw rar_error(code);
  }
}

bool RarArchive::aborted_by_callback() const { return impl_->aborted; }

}  // namespace streamextract
