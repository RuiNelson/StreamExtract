#include "libarchive_reader.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <utility>

#include <archive.h>
#include <archive_entry.h>
#include <fmt/format.h>

#include "util/text.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <locale.h>

namespace rarftp {

namespace {

constexpr size_t kReadBlockSize = 1 << 20;

// libarchive converts names to the charset of the current locale (7z stores
// them as UTF-16), and a GUI app often runs with the "C" locale. Every call
// into libarchive therefore runs with a UTF-8 LC_CTYPE on the calling thread.
#ifndef _WIN32
class Utf8Locale {
 public:
  Utf8Locale() {
    if (const locale_t utf8 = utf8_locale(); utf8 != locale_t(nullptr)) {
      previous_ = uselocale(utf8);
    }
  }
  ~Utf8Locale() {
    if (previous_ != locale_t(nullptr)) {
      uselocale(previous_);
    }
  }
  Utf8Locale(const Utf8Locale&) = delete;
  Utf8Locale& operator=(const Utf8Locale&) = delete;

 private:
  static locale_t utf8_locale() {
    static const locale_t locale = [] {
      for (const char* name : {"C.UTF-8", "C.utf8", "en_US.UTF-8", "UTF-8"}) {
        // Kept for the lifetime of the process.
        if (const locale_t found = newlocale(LC_CTYPE_MASK, name, locale_t(nullptr)); found != locale_t(nullptr)) {
          return found;
        }
      }
      return locale_t(nullptr);
    }();
    return locale;
  }

  locale_t previous_ = locale_t(nullptr);
};
#else
// Windows: the C runtime's locale, made per-thread (libarchive reads the code
// page from it; ".UTF8" needs Windows 10 1803 or later).
class Utf8Locale {
 public:
  Utf8Locale() : mode_(_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)) {
    if (const char* current = setlocale(LC_CTYPE, nullptr)) {
      previous_ = current;
    }
    setlocale(LC_CTYPE, ".UTF8");
  }
  ~Utf8Locale() {
    if (!previous_.empty()) {
      setlocale(LC_CTYPE, previous_.c_str());
    }
    _configthreadlocale(mode_);
  }
  Utf8Locale(const Utf8Locale&) = delete;
  Utf8Locale& operator=(const Utf8Locale&) = delete;

 private:
  int mode_;
  std::string previous_;
};
#endif

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

#ifdef __APPLE__
// libarchive normalizes the Unicode names it converts to NFD on macOS (to
// match HFS+) and to NFC elsewhere. NFC everywhere: the same remote names
// whatever system uploads them.
std::string to_nfc(const std::string& utf8) {
  const CFStringRef text =
      CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(utf8.data()),
                              static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
  if (text == nullptr) {
    return utf8;
  }
  const CFMutableStringRef normalized = CFStringCreateMutableCopy(nullptr, 0, text);
  CFRelease(text);
  if (normalized == nullptr) {
    return utf8;
  }
  CFStringNormalize(normalized, kCFStringNormalizationFormC);
  const CFRange range = CFRangeMake(0, CFStringGetLength(normalized));
  const CFIndex capacity = CFStringGetMaximumSizeForEncoding(range.length, kCFStringEncodingUTF8);
  std::string out(static_cast<size_t>(capacity), '\0');
  CFIndex used = 0;
  CFStringGetBytes(normalized, range, kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8*>(out.data()),
                   capacity, &used);
  CFRelease(normalized);
  out.resize(static_cast<size_t>(used));
  return out;
}
#endif

std::string entry_name(archive_entry* entry, ArchiveFormat format) {
  // With the UTF-8 locale, names in a known charset (7z, flagged ZIP, pax)
  // come as UTF-8. The others arrive as the raw bytes (compat-2x, set when
  // opening, on every system): UTF-8 (what macOS and today's systems write) or
  // else the format's traditional charset, CP437 for ZIP and Latin-1 for tar.
  const char* raw = archive_entry_pathname(entry);
  if (raw == nullptr) {
    return {};
  }
  if (!is_valid_utf8(raw)) {
    return format == ArchiveFormat::Tar ? latin1_to_utf8(raw) : cp437_to_utf8(raw);
  }
#ifdef __APPLE__
  return to_nfc(raw);
#else
  return raw;
#endif
}

EntryKind entry_kind(archive_entry* entry) {
  if (archive_entry_hardlink(entry) != nullptr) {
    return EntryKind::Hardlink;
  }
  switch (archive_entry_filetype(entry)) {
    case 0:  // No type stored.
    case AE_IFREG:
      return EntryKind::File;
    case AE_IFDIR:
      return EntryKind::Directory;
    case AE_IFLNK:
      return EntryKind::Symlink;
    default:
      return EntryKind::Special;
  }
}

// Opens `parts` as one stream. The open runs libarchive's format detection, so
// it fails unless one of the registered readers recognizes the data.
int open_parts(struct archive* handle, const std::vector<std::string>& parts) {
#ifdef _WIN32
  std::vector<std::wstring> wide;
  for (const std::string& part : parts) {
    wide.push_back(from_utf8(part));
  }
  std::vector<const wchar_t*> names;
  for (const std::wstring& name : wide) {
    names.push_back(name.c_str());
  }
  names.push_back(nullptr);
  return archive_read_open_filenames_w(handle, names.data(), kReadBlockSize);
#else
  std::vector<const char*> names;
  for (const std::string& part : parts) {
    names.push_back(part.c_str());
  }
  names.push_back(nullptr);
  return archive_read_open_filenames(handle, names.data(), kReadBlockSize);
#endif
}

// The tar reader, with the decompression filters for compressed tar. Only
// filters built on the libraries linked in: libarchive would otherwise run an
// external program, and says so with ARCHIVE_WARN.
int support_tar(struct archive* handle) {
  for (const auto support : {archive_read_support_filter_gzip, archive_read_support_filter_bzip2,
                             archive_read_support_filter_xz, archive_read_support_filter_lzma,
                             archive_read_support_filter_zstd, archive_read_support_filter_lz4}) {
    if (support(handle) != ARCHIVE_OK) {
      return ARCHIVE_FATAL;
    }
  }
  return archive_read_support_format_tar(handle);
}

using Handle = std::unique_ptr<struct archive, int (*)(struct archive*)>;

// `parts` opened with only what `setup` registers; empty if libarchive does not
// recognize them.
template <typename Setup>
Handle probe(const std::vector<std::string>& parts, Setup setup) {
  Handle handle(archive_read_new(), archive_read_free);
  if (!handle || setup(handle.get()) != ARCHIVE_OK || open_parts(handle.get(), parts) != ARCHIVE_OK) {
    return Handle(nullptr, archive_read_free);
  }
  return handle;
}

}  // namespace

std::optional<ArchiveFormat> libarchive_format(const std::vector<std::string>& parts) {
  const Utf8Locale utf8;
  // One format at a time, so that the open says whether that one recognizes
  // the data. Both ZIP readers bid: the streaming one also knows a ZIP whose
  // end (the central directory the reader below needs) is missing.
  if (probe(parts, archive_read_support_format_zip)) {
    return ArchiveFormat::Zip;
  }
  if (probe(parts, archive_read_support_format_7zip)) {
    return ArchiveFormat::SevenZip;
  }
  if (probe(parts, support_tar)) {
    return ArchiveFormat::Tar;
  }
  return std::nullopt;
}

struct LibArchiveReader::Impl {
  struct archive* handle = nullptr;
  ArchiveFormat format = ArchiveFormat::Zip;
  Mode mode = Mode::List;
  ArchiveCallbacks callbacks;
  ArchiveFlags flags;
  bool entry_encrypted = false;
  bool password_asked = false;
  bool password_checked = false;
  std::optional<std::string> password;  // libarchive copies it when we hand it over.
  bool aborted = false;
  std::exception_ptr callback_error;

  ~Impl() {
    if (handle != nullptr) {
      archive_read_free(handle);
    }
    if (password) {
      std::fill(password->begin(), password->end(), '\0');
    }
  }

  void rethrow_callback_error() {
    if (callback_error) {
      std::exception_ptr error = callback_error;
      callback_error = nullptr;
      std::rethrow_exception(error);
    }
  }

  // The error libarchive reported last, in our terms.
  ArchiveError error() const {
    const char* text = archive_error_string(handle);
    const std::string message = text != nullptr ? text : "unknown error";
    if (contains(message, "Incorrect passphrase")) {
      return ArchiveError(ArchiveError::Kind::BadPassword, "wrong password");
    }
    if (contains(message, "Passphrase required")) {
      return ArchiveError(ArchiveError::Kind::MissingPassword, "password required");
    }
    if (contains(message, "bad CRC") || contains(message, "bad Authentication code")) {
      return ArchiveError(ArchiveError::Kind::Other, "corrupt data (checksum mismatch)");
    }
    if (contains(message, "Unrecognized archive format")) {
      return ArchiveError(ArchiveError::Kind::Other, fmt::format("not a valid {} archive", format_name(format)));
    }
    return ArchiveError(ArchiveError::Kind::Other, message);
  }

  bool feed(const uint8_t* data, size_t size) {
    if (callbacks.on_data && !callbacks.on_data(data, size)) {
      aborted = true;
      return false;
    }
    return true;
  }
};

namespace {

// Asked for the password of an encrypted entry. After a wrong answer libarchive
// asks again, until it gets nullptr; the answer would not change.
const char* passphrase_callback(struct archive*, void* data) {
  auto* impl = static_cast<LibArchiveReader::Impl*>(data);
  if (impl->password_asked) {
    return nullptr;
  }
  impl->password_asked = true;
  try {
    if (impl->callbacks.on_password) {
      impl->password = impl->callbacks.on_password();
    }
  } catch (...) {
    impl->callback_error = std::current_exception();
    impl->aborted = true;
    return nullptr;
  }
  return impl->password ? impl->password->c_str() : nullptr;
}

}  // namespace

LibArchiveReader::LibArchiveReader(std::vector<std::string> parts, ArchiveFormat format, Mode mode,
                                   ArchiveCallbacks callbacks)
    : impl_(std::make_unique<Impl>()) {
  const Utf8Locale utf8;
  Impl& m = *impl_;
  m.format = format;
  m.mode = mode;
  m.callbacks = std::move(callbacks);
  m.flags.volume = parts.size() > 1;
  m.flags.first_volume = true;
  m.flags.volume_count = static_cast<unsigned>(parts.size());
  m.flags.checksums = format != ArchiveFormat::Tar;

  m.handle = archive_read_new();
  if (m.handle == nullptr) {
    throw ArchiveError(ArchiveError::Kind::Other, "not enough memory");
  }
  int supported = ARCHIVE_FATAL;
  switch (format) {
    case ArchiveFormat::Zip:  // The seekable reader trusts the central directory, like other tools.
      supported = archive_read_support_format_zip_seekable(m.handle);
      if (supported == ARCHIVE_OK) {
        supported = archive_read_set_format_option(m.handle, "zip", "compat-2x", "1");
      }
      break;
    case ArchiveFormat::Tar:
      supported = support_tar(m.handle);
      if (supported == ARCHIVE_OK) {
        supported = archive_read_set_format_option(m.handle, "tar", "compat-2x", "1");
      }
      break;
    case ArchiveFormat::SevenZip:  // SevenZipArchive.
    case ArchiveFormat::Exfat:     // ExfatArchive.
    case ArchiveFormat::Rar:
      break;
  }
  if (supported != ARCHIVE_OK) {
    throw m.error();
  }
  archive_read_set_passphrase_callback(m.handle, &m, passphrase_callback);

  const int opened = open_parts(m.handle, parts);
  if (opened != ARCHIVE_OK) {
    throw m.error();
  }
  if (archive_filter_code(m.handle, 0) != ARCHIVE_FILTER_NONE) {
    m.flags.compression = archive_filter_name(m.handle, 0);
    m.flags.skip_decompresses = true;
    m.flags.stream_only = true;
  }
  for (const std::string& part : parts) {
    std::error_code ignored;
#ifdef _WIN32
    const auto size = std::filesystem::file_size(std::filesystem::path(from_utf8(part)), ignored);
#else
    const auto size = std::filesystem::file_size(part, ignored);
#endif
    m.flags.size += size != static_cast<std::uintmax_t>(-1) ? static_cast<uint64_t>(size) : 0;
  }
}

LibArchiveReader::~LibArchiveReader() {
  const Utf8Locale utf8;
  impl_.reset();
}

ArchiveFormat LibArchiveReader::format() const { return impl_->format; }

const ArchiveFlags& LibArchiveReader::flags() const { return impl_->flags; }

bool LibArchiveReader::next(ArchiveEntry& entry) {
  const Utf8Locale utf8;
  Impl& m = *impl_;
  m.aborted = false;
  m.entry_encrypted = false;
  archive_entry* header = nullptr;
  const int result = archive_read_next_header(m.handle, &header);
  m.rethrow_callback_error();
  if (result == ARCHIVE_EOF) {
    return false;
  }
  if (result < ARCHIVE_WARN) {  // Warnings (inconsistent local headers...) are not fatal.
    throw m.error();
  }

  entry = ArchiveEntry{};
  entry.name = entry_name(header, m.format);
  entry.kind = entry_kind(header);
  const la_int64_t size = archive_entry_size_is_set(header) ? archive_entry_size(header) : 0;
  entry.size = size > 0 ? static_cast<uint64_t>(size) : 0;
  entry.mtime = archive_entry_mtime_is_set(header) ? static_cast<int64_t>(archive_entry_mtime(header)) : 0;
  entry.encrypted = archive_entry_is_encrypted(header) != 0;
  m.entry_encrypted = entry.encrypted;
  return true;
}

void LibArchiveReader::test() {
  const Utf8Locale utf8;
  Impl& m = *impl_;
  m.aborted = false;
  uint64_t position = 0;
  while (true) {
    const void* buffer = nullptr;
    size_t size = 0;
    la_int64_t offset = 0;
    const int result = archive_read_data_block(m.handle, &buffer, &size, &offset);
    m.rethrow_callback_error();
    // Warnings too: 7z reports a checksum mismatch as one, on the last block.
    if (result != ARCHIVE_OK && result != ARCHIVE_EOF) {
      throw m.error();
    }
    // Holes of sparse tar entries: each block says where it goes, and the end
    // of the entry (where the last hole ends) comes with ARCHIVE_EOF.
    static constexpr std::array<uint8_t, 64 * 1024> kZeros{};
    const uint64_t start = offset > 0 ? static_cast<uint64_t>(offset) : 0;
    while (position < start) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(kZeros.size(), start - position));
      if (!m.feed(kZeros.data(), n)) {
        throw ArchiveError(ArchiveError::Kind::Other, "aborted");
      }
      position += n;
    }
    if (result == ARCHIVE_EOF) {
      return;
    }
    if (size > 0 && !m.feed(static_cast<const uint8_t*>(buffer), size)) {
      throw ArchiveError(ArchiveError::Kind::Other, "aborted");
    }
    position += size;
  }
}

void LibArchiveReader::skip() {
  const Utf8Locale utf8;
  Impl& m = *impl_;
  m.aborted = false;
  if (m.mode == Mode::List && m.entry_encrypted && !m.password_checked) {
    // Decrypt the start of the first encrypted file: a wrong password then
    // shows up while listing, instead of once the upload has started.
    char first = 0;
    const la_ssize_t n = archive_read_data(m.handle, &first, 1);
    m.rethrow_callback_error();
    if (n < 0) {
      throw m.error();
    }
    m.password_checked = n > 0;
  }
  const int result = archive_read_data_skip(m.handle);
  m.rethrow_callback_error();
  if (result < ARCHIVE_WARN) {
    throw m.error();
  }
}

bool LibArchiveReader::aborted_by_callback() const { return impl_->aborted; }

uint64_t LibArchiveReader::bytes_read() const {
  // The last filter is the one reading the files.
  const la_int64_t read = archive_filter_bytes(impl_->handle, -1);
  return read > 0 ? static_cast<uint64_t>(read) : 0;
}

std::string libarchive_version() {
  const int number = archive_version_number();  // 3008009 for 3.8.9.
  return fmt::format("{}.{}.{}", number / 1000000, number / 1000 % 1000, number % 1000);
}

}  // namespace rarftp
