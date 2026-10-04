#include "sevenzip_archive.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <new>
#include <optional>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include "sevenzip_methods.hpp"
#include "util/text.hpp"

// 7-Zip's headers (LZMA SDK) come last: on Unix they define Windows names such
// as HRESULT, BSTR and S_OK.
#include "Common/Common.h"

#include "7zip/Archive/7z/7zDecode.h"
#include "7zip/Archive/7z/7zHeader.h"
#include "7zip/Archive/7z/7zIn.h"
#include "7zip/Common/CreateCoder.h"
#include "7zip/IPassword.h"
#include "7zip/IStream.h"

#include "../C/7zCrc.h"
#include "../C/7zVersion.h"

namespace streamextract {

void sevenzip_codecs_linked();  // sevenzip_codecs.cpp

namespace {

using NArchive::N7z::CDbEx;
using NArchive::N7z::CFileItem;

constexpr size_t kBufferSize = 1 << 20;
// Self-extracting archives: how far into the file the 7z signature is looked for.
constexpr UInt64 kSfxSearchLimit = 1 << 22;
// List mode: how much of the first encrypted file is decrypted to check the
// password. A file that is not larger is decrypted whole, and its CRC checked.
constexpr uint64_t kPasswordCheckBytes = 4 << 20;
constexpr uint32_t kNoFolder = NArchive::N7z::kNumNoIndex;

// FILE_ATTRIBUTE_UNIX_EXTENSION: the high 16 bits hold a Unix st_mode.
constexpr UInt32 kUnixExtension = 0x8000;
constexpr UInt32 kUnixTypeMask = 0170000;
constexpr UInt32 kUnixRegular = 0100000;
constexpr UInt32 kUnixDirectory = 0040000;
constexpr UInt32 kUnixSymlink = 0120000;

// Bad data found by a decoder or a checksum: a sign of a wrong password in an
// encrypted block.
class CorruptData : public ArchiveError {
 public:
  explicit CorruptData(bool encrypted)
      : ArchiveError(Kind::Other, encrypted ? "corrupt data or wrong password (checksum mismatch)"
                                            : "corrupt data (checksum mismatch)") {}
};

std::FILE* open_file(const std::string& path) {
#ifdef _WIN32
  return _wfopen(from_utf8(path).c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
#endif
}

int seek_file(std::FILE* file, uint64_t offset) {
#ifdef _WIN32
  return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET);
#else
  return fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::string file_name_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The archive, or the parts of a split one, as one seekable stream. Only one
// part is open at a time.
Z7_CLASS_IMP_IInStream(CPartsInStream)
  struct Part {
    std::string path;
    uint64_t start = 0;
    uint64_t size = 0;
  };

  std::vector<Part> parts_;
  uint64_t size_ = 0;
  uint64_t pos_ = 0;
  std::FILE* file_ = nullptr;
  size_t file_part_ = 0;   // The part open in file_.
  uint64_t file_pos_ = 0;  // Position of file_ in that part.

  void close_file() {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

 public:
  std::string error;  // Why the last read failed.

  ~CPartsInStream() { close_file(); }

  // Throws ArchiveError.
  void open(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
      std::error_code ec;
#ifdef _WIN32
      const auto size = std::filesystem::file_size(std::filesystem::path(from_utf8(path)), ec);
#else
      const auto size = std::filesystem::file_size(path, ec);
#endif
      if (ec) {
        throw ArchiveError(ArchiveError::Kind::Other,
                           fmt::format("cannot open {}: {}", file_name_of(path), ec.message()));
      }
      parts_.push_back(Part{path, size_, static_cast<uint64_t>(size)});
      size_ += static_cast<uint64_t>(size);
    }
  }

  uint64_t size() const { return size_; }
};

Z7_COM7F_IMF(CPartsInStream::Read(void* data, UInt32 size, UInt32* processedSize)) {
  if (processedSize != nullptr) {
    *processedSize = 0;
  }
  if (size == 0 || pos_ >= size_) {
    return S_OK;
  }
  // The last part starting at or before pos_ (empty parts are passed over).
  const auto after = std::upper_bound(parts_.begin(), parts_.end(), pos_,
                                      [](uint64_t pos, const Part& part) { return pos < part.start; });
  const size_t index = static_cast<size_t>(after - parts_.begin()) - 1;
  const Part& part = parts_[index];
  const uint64_t offset = pos_ - part.start;

  if (file_ == nullptr || file_part_ != index) {
    close_file();
    file_ = open_file(part.path);
    if (file_ == nullptr) {
      error = fmt::format("cannot open {}", file_name_of(part.path));
      return E_FAIL;
    }
    file_part_ = index;
    file_pos_ = 0;
  }
  if (file_pos_ != offset) {
    if (seek_file(file_, offset) != 0) {
      error = fmt::format("cannot read {}", file_name_of(part.path));
      return E_FAIL;
    }
    file_pos_ = offset;
  }
  const size_t wanted = static_cast<size_t>(std::min<uint64_t>(size, part.size - offset));
  const size_t got = std::fread(data, 1, wanted, file_);
  file_pos_ += got;
  pos_ += got;
  if (processedSize != nullptr) {
    *processedSize = static_cast<UInt32>(got);
  }
  if (got < wanted) {
    error = std::ferror(file_) != 0 ? fmt::format("cannot read {}", file_name_of(part.path))
                                    : fmt::format("{} is shorter than expected", file_name_of(part.path));
    return E_FAIL;
  }
  return S_OK;
}

Z7_COM7F_IMF(CPartsInStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64* newPosition)) {
  Int64 base = 0;
  switch (seekOrigin) {
    case STREAM_SEEK_SET:
      break;
    case STREAM_SEEK_CUR:
      base = static_cast<Int64>(pos_);
      break;
    case STREAM_SEEK_END:
      base = static_cast<Int64>(size_);
      break;
    default:
      return STG_E_INVALIDFUNCTION;
  }
  if (offset < -base) {
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  }
  pos_ = static_cast<uint64_t>(base + offset);
  if (newPosition != nullptr) {
    *newPosition = pos_;
  }
  return S_OK;
}

// Names of 7z methods nothing here decodes, for the error message.
std::string method_name(UInt64 id) {
  switch (id) {
    case 0x040109:
      return "Deflate64";
    case 0x4F71102:
      return "Brotli";
    case 0x4F71104:
      return "LZ4";
    case 0x4F71105:
      return "LZ5";
    case 0x4F71106:
      return "Lizard";
    default:
      return fmt::format("{:X}", id);
  }
}

}  // namespace

struct SevenZipArchive::Impl {
  Mode mode = Mode::List;
  ArchiveCallbacks callbacks;
  ArchiveFlags flags;

  CPartsInStream* parts = nullptr;  // Owned by `stream`.
  CMyComPtr<IInStream> stream;
  CMyComPtr<ICryptoGetTextPassword> password_getter;
  CDbEx db;
  // A new one for each block: once used, 7-Zip's single-threaded mixer (the
  // decoded block as a stream read on demand) cannot read another block whose
  // coders are chained (AES and LZMA2, a filter and LZMA2...).
  std::unique_ptr<NArchive::N7z::CDecoder> decoder;
  std::vector<bool> folder_encrypted;

  bool password_asked = false;
  bool password_declined = false;
  std::optional<std::string> password;
  bool password_checked = false;

  // The entry last returned by next(), and where its data starts in its block.
  unsigned next_index = 0;
  unsigned current = 0;
  bool has_current = false;
  uint32_t current_folder = kNoFolder;
  uint64_t current_offset = 0;
  // Where the next file of the same block starts.
  uint32_t last_folder = kNoFolder;
  uint64_t next_offset = 0;

  // The block being decompressed.
  CMyComPtr<ISequentialInStream> folder_stream;
  uint32_t open_folder = kNoFolder;
  uint64_t folder_pos = 0;

  std::vector<uint8_t> buffer;
  bool aborted = false;
  std::exception_ptr callback_error;

  ~Impl() {
    folder_stream.Release();  // Before the decoder it reads from.
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

  // Asked by the decoder of each encrypted block (and of encrypted headers).
  HRESULT get_password(BSTR* out) {
    if (!password_asked) {
      password_asked = true;
      try {
        if (callbacks.on_password) {
          password = callbacks.on_password();
        }
      } catch (...) {
        callback_error = std::current_exception();
        aborted = true;
        return E_ABORT;
      }
    }
    if (!password) {
      password_declined = true;
      aborted = true;
      return E_ABORT;
    }
    // One UTF-16 code unit per wchar_t: 7-Zip makes two bytes (UTF-16LE) of
    // each, also where wchar_t is wider.
    std::u16string utf16 = utf8_to_utf16(*password);
    std::wstring wide(utf16.begin(), utf16.end());
    *out = ::SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
    std::fill(utf16.begin(), utf16.end(), u'\0');
    std::fill(wide.begin(), wide.end(), L'\0');
    return *out != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  // Throws the error of a failed call into 7-Zip's code.
  [[noreturn]] void fail(HRESULT result, bool encrypted) {
    rethrow_callback_error();
    switch (result) {
      case S_FALSE:
        throw CorruptData(encrypted);
      case E_ABORT:
        if (password_declined) {
          throw ArchiveError(ArchiveError::Kind::MissingPassword, "password required");
        }
        throw ArchiveError(ArchiveError::Kind::Other, "aborted");
      case E_NOTIMPL:
        throw ArchiveError(ArchiveError::Kind::Other, "unsupported compression method");
      case E_OUTOFMEMORY:
        throw ArchiveError(ArchiveError::Kind::Other, "not enough memory");
      default:
        break;
    }
    if (!parts->error.empty()) {
      throw ArchiveError(ArchiveError::Kind::Other, std::exchange(parts->error, std::string()));
    }
    throw ArchiveError(ArchiveError::Kind::Other,
                       fmt::format("7-Zip error 0x{:08X}", static_cast<uint32_t>(result)));
  }

  // Calls into 7-Zip's code, which reports failures with HRESULTs but may also
  // throw (memory allocation).
  template <typename Call>
  HRESULT call(Call&& function) {
    try {
      return function();
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }

  std::string name_of(unsigned index) const {
    if (!db.NameOffsets || !db.NamesBuf) {
      return {};
    }
    const size_t start = db.NameOffsets[index];
    const size_t end = db.NameOffsets[index + 1];
    if (end <= start) {
      return {};
    }
    // UTF-16LE, with a terminating zero.
    const Byte* bytes = static_cast<const Byte*>(db.NamesBuf) + start * 2;
    std::u16string name(end - start - 1, u'\0');
    for (size_t i = 0; i < name.size(); ++i) {
      name[i] = static_cast<char16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    }
    return utf16_to_utf8(name);
  }

  EntryKind kind_of(unsigned index) const {
    if (db.IsItemAnti(index)) {  // Deletion marker of an update archive.
      return EntryKind::Special;
    }
    UInt32 attributes = 0;
    if (db.Attrib.GetItem(index, attributes) && (attributes & kUnixExtension) != 0) {
      switch ((attributes >> 16) & kUnixTypeMask) {
        case 0:
        case kUnixRegular:
          break;
        case kUnixDirectory:
          return EntryKind::Directory;
        case kUnixSymlink:
          return EntryKind::Symlink;
        default:
          return EntryKind::Special;
      }
    }
    return db.Files[index].IsDir ? EntryKind::Directory : EntryKind::File;
  }

  void open_block(uint32_t folder) {
    folder_stream.Release();
    open_folder = kNoFolder;
    bool data_after_end = false;
    bool encrypted = false;
    bool password_defined = false;
    UString_Wipe password_copy;
    const HRESULT result = call([&] {
      decoder = std::make_unique<NArchive::N7z::CDecoder>(false);  // Single-threaded mixer.
      return decoder->Decode(stream, db.ArcInfo.DataStartPosition, db, folder,
                             nullptr,  // The whole block.
                             nullptr, nullptr, &folder_stream, data_after_end, password_getter, encrypted,
                             password_defined, password_copy,
                             false, 1, 0);  // No multithreading (it needs the other mixer).
    });
    if (result != S_OK || !folder_stream) {
      folder_stream.Release();
      fail(result != S_OK ? result : E_FAIL, folder_encrypted[folder]);
    }
    open_folder = folder;
    folder_pos = 0;
  }

  // Reads the next bytes of the open block into `buffer`; 0 at its end.
  size_t read_block(size_t size) {
    UInt32 got = 0;
    const HRESULT result =
        call([&] { return folder_stream->Read(buffer.data(), static_cast<UInt32>(size), &got); });
    if (result != S_OK) {
      const bool encrypted = folder_encrypted[open_folder];
      folder_stream.Release();
      open_folder = kNoFolder;
      fail(result, encrypted);
    }
    folder_pos += got;
    return got;
  }

  size_t read_block_or_fail(size_t size) {
    const size_t got = read_block(size);
    if (got == 0) {
      const bool encrypted = folder_encrypted[open_folder];
      folder_stream.Release();
      open_folder = kNoFolder;
      throw CorruptData(encrypted);
    }
    return got;
  }

  // Decompresses up to `limit` bytes of the current file, feeding on_data if
  // `feed`, and checks its CRC once it has been read whole. Returns whether it
  // was.
  bool read_current(uint64_t limit, bool feed) {
    const CFileItem& file = db.Files[current];
    if (current_folder == kNoFolder || file.Size == 0) {
      return true;
    }
    if (open_folder != current_folder || folder_pos > current_offset) {
      open_block(current_folder);
    }
    while (folder_pos < current_offset) {  // Files of the block skipped before this one.
      read_block_or_fail(static_cast<size_t>(std::min<uint64_t>(kBufferSize, current_offset - folder_pos)));
    }

    const bool encrypted = folder_encrypted[current_folder];
    const uint64_t wanted = std::min<uint64_t>(file.Size, limit);
    uint64_t done = 0;
    UInt32 crc = CRC_INIT_VAL;
    while (done < wanted) {
      const size_t n = read_block_or_fail(static_cast<size_t>(std::min<uint64_t>(kBufferSize, wanted - done)));
      crc = CrcUpdate(crc, buffer.data(), n);
      done += n;
      if (feed && callbacks.on_data && !callbacks.on_data(buffer.data(), n)) {
        aborted = true;
        throw ArchiveError(ArchiveError::Kind::Other, "aborted");
      }
    }
    if (done < file.Size) {
      return false;
    }
    if (file.CrcDefined && CRC_GET_DIGEST(crc) != file.Crc) {
      throw CorruptData(encrypted);
    }
    if (folder_pos == db.GetFolderUnpackSize(current_folder)) {
      // The end of the block: one more read lets the decoders check how their
      // data ends.
      if (read_block(1) != 0) {
        throw CorruptData(encrypted);
      }
      folder_stream.Release();
      open_folder = kNoFolder;
    }
    return true;
  }
};

namespace {

Z7_CLASS_IMP_COM_1(CPasswordGetter, ICryptoGetTextPassword)
 public:
  SevenZipArchive::Impl* impl = nullptr;
};

Z7_COM7F_IMF(CPasswordGetter::CryptoGetTextPassword(BSTR* password)) {
  *password = nullptr;
  return impl->get_password(password);
}

}  // namespace

std::unique_ptr<SevenZipArchive> SevenZipArchive::open(const std::vector<std::string>& parts, Mode mode,
                                                       const ArchiveCallbacks& callbacks) {
  sevenzip_codecs_linked();
  register_sevenzip_methods();
  auto impl = std::make_unique<Impl>();
  Impl& m = *impl;
  m.mode = mode;
  m.callbacks = callbacks;
  m.buffer.resize(kBufferSize);

  m.parts = new CPartsInStream;
  m.stream = m.parts;
  m.parts->open(parts);
  auto* getter = new CPasswordGetter;
  getter->impl = impl.get();
  m.password_getter = getter;

  bool headers_encrypted = false;
  bool password_defined = false;
  NArchive::N7z::CInArchive reader(false);
  const HRESULT opened = m.call([&] {
    const UInt64 limit = kSfxSearchLimit;
    return reader.Open(m.stream, &limit);
  });
  if (opened != S_OK) {
    m.rethrow_callback_error();
    if (opened == S_FALSE) {
      throw ArchiveError(ArchiveError::Kind::Other, "not a valid 7z archive");
    }
    m.fail(opened, false);
  }
  {
    UString_Wipe password_copy;
    const HRESULT result = m.call([&] {
      return reader.ReadDatabase(m.db, m.password_getter, headers_encrypted, password_defined, password_copy);
    });
    if (result != S_OK) {
      m.rethrow_callback_error();
      if (headers_encrypted && password_defined && result != E_ABORT) {
        throw ArchiveError(ArchiveError::Kind::BadPassword, "wrong password");
      }
      if (result == S_FALSE) {
        throw ArchiveError(ArchiveError::Kind::Other, m.db.UnexpectedEnd ? "unexpected end of archive"
                                                                         : "corrupt archive headers");
      }
      m.fail(result, headers_encrypted);
    }
  }
  // Decrypting the headers proved the password.
  m.password_checked = headers_encrypted;

  std::optional<UInt64> unsupported;
  m.folder_encrypted.assign(m.db.NumFolders, false);
  const HRESULT parsed = m.call([&] {
    for (uint32_t i = 0; i < m.db.NumFolders; ++i) {
      NArchive::N7z::CFolder folder;
      m.db.ParseFolderInfo(i, folder);
      for (unsigned c = 0; c < folder.Coders.Size(); ++c) {
        const UInt64 method = folder.Coders[c].MethodID;
        if (method == NArchive::N7z::k_AES) {
          m.folder_encrypted[i] = true;
        }
        AString name;
        if (!FindMethod(method, name)) {
          unsupported = method;
        }
      }
    }
    return S_OK;
  });
  if (parsed != S_OK) {
    throw ArchiveError(ArchiveError::Kind::Other, "corrupt archive headers");
  }
  if (unsupported) {  // Reported now, before anything is uploaded.
    throw ArchiveError(ArchiveError::Kind::Other,
                       fmt::format("unsupported 7z compression method: {}", method_name(*unsupported)));
  }

  m.flags.volume = parts.size() > 1;
  m.flags.first_volume = true;
  m.flags.volume_count = static_cast<unsigned>(parts.size());
  m.flags.solid = m.db.IsSolid();
  m.flags.encrypted_headers = headers_encrypted;
  m.flags.skip_decompresses = m.flags.solid;
  m.flags.size = m.parts->size();
  return std::unique_ptr<SevenZipArchive>(new SevenZipArchive(std::move(impl)));
}

SevenZipArchive::SevenZipArchive(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SevenZipArchive::~SevenZipArchive() = default;

const ArchiveFlags& SevenZipArchive::flags() const { return impl_->flags; }

bool SevenZipArchive::next(ArchiveEntry& entry) {
  Impl& m = *impl_;
  m.aborted = false;
  if (m.next_index >= m.db.Files.Size()) {
    m.has_current = false;
    return false;
  }
  const unsigned index = m.next_index++;
  const CFileItem& file = m.db.Files[index];
  m.current = index;
  m.has_current = true;
  m.current_folder = file.HasStream ? m.db.FileIndexToFolderIndexMap[index] : kNoFolder;
  if (m.current_folder != kNoFolder) {
    if (m.current_folder != m.last_folder) {
      m.last_folder = m.current_folder;
      m.next_offset = 0;
    }
    m.current_offset = m.next_offset;
    m.next_offset += file.Size;
  }

  entry = ArchiveEntry{};
  entry.name = m.name_of(index);
  entry.kind = m.kind_of(index);
  entry.size = file.Size;
  UInt64 mtime = 0;
  if (m.db.MTime.GetItem(index, mtime)) {
    entry.mtime = filetime_to_unix(mtime);
  }
  entry.encrypted = m.current_folder != kNoFolder && m.folder_encrypted[m.current_folder];
  return true;
}

void SevenZipArchive::test() {
  Impl& m = *impl_;
  m.aborted = false;
  if (m.has_current) {
    m.read_current(UINT64_MAX, true);
  }
}

void SevenZipArchive::skip() {
  Impl& m = *impl_;
  m.aborted = false;
  if (m.mode == Mode::List && m.has_current && m.current_folder != kNoFolder &&
      m.folder_encrypted[m.current_folder] && !m.password_checked) {
    try {
      m.read_current(kPasswordCheckBytes, false);
    } catch (const CorruptData&) {
      throw ArchiveError(ArchiveError::Kind::BadPassword, "wrong password");
    }
    m.password_checked = true;
  }
}

bool SevenZipArchive::aborted_by_callback() const { return impl_->aborted; }

std::string lzma_sdk_version() { return MY_VERSION_NUMBERS; }

}  // namespace streamextract
