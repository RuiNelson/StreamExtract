#include "sevenzip_methods.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// 7-Zip's headers (LZMA SDK) first: zlib's, with its prefixed names, would
// rename words in them (Byte, crc32...).
#include "Common/Common.h"

#include "Common/MyCom.h"
#include "7zip/Common/RegisterCodec.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/ICoder.h"
#include "7zip/IStream.h"

#define ZLIB_CONST
#include <bzlib.h>
#include <zlib.h>
#include <zstd.h>

namespace streamextract {

namespace {

constexpr size_t kInBufferSize = 1 << 16;
constexpr size_t kCodeBufferSize = 1 << 18;
// Zstandard: the largest window a 64-bit decoder accepts (2 GiB); zstd's
// default limit (128 MiB) would refuse archives made with long-range matching.
constexpr int kZstdWindowLogMax = 31;

// The streaming decompressor of a C library.
class Engine {
 public:
  enum class Status { Ok, End, Error };

  virtual ~Engine() = default;
  // Gets ready for a new stream. False when out of memory.
  virtual bool reset() = 0;
  // Decodes from `in` into `out`, advancing both. End: the stream is over.
  virtual Status decode(const uint8_t*& in, size_t& in_size, uint8_t*& out, size_t& out_size) = 0;
  // Whether another stream may follow (bzip2 streams, Zstandard frames).
  virtual bool concatenated() const = 0;
};

// Raw Deflate (no zlib header), as 7z stores it.
class DeflateEngine final : public Engine {
 public:
  ~DeflateEngine() override {
    if (initialized_) {
      inflateEnd(&stream_);
    }
  }

  bool reset() override {
    if (initialized_) {
      return inflateReset(&stream_) == Z_OK;
    }
    stream_ = z_stream{};
    initialized_ = inflateInit2(&stream_, -MAX_WBITS) == Z_OK;
    return initialized_;
  }

  Status decode(const uint8_t*& in, size_t& in_size, uint8_t*& out, size_t& out_size) override {
    const auto in_avail = static_cast<uInt>(std::min<size_t>(in_size, UINT_MAX));
    const auto out_avail = static_cast<uInt>(std::min<size_t>(out_size, UINT_MAX));
    stream_.next_in = in;
    stream_.avail_in = in_avail;
    stream_.next_out = out;
    stream_.avail_out = out_avail;
    const int result = inflate(&stream_, Z_NO_FLUSH);
    in += in_avail - stream_.avail_in;
    in_size -= in_avail - stream_.avail_in;
    out += out_avail - stream_.avail_out;
    out_size -= out_avail - stream_.avail_out;
    switch (result) {
      case Z_STREAM_END:
        return Status::End;
      case Z_OK:
      case Z_BUF_ERROR:  // No progress possible yet.
        return Status::Ok;
      default:
        return Status::Error;
    }
  }

  bool concatenated() const override { return false; }

 private:
  z_stream stream_{};
  bool initialized_ = false;
};

class Bzip2Engine final : public Engine {
 public:
  ~Bzip2Engine() override { end(); }

  bool reset() override {
    end();
    stream_ = bz_stream{};
    initialized_ = BZ2_bzDecompressInit(&stream_, 0, 0) == BZ_OK;
    return initialized_;
  }

  Status decode(const uint8_t*& in, size_t& in_size, uint8_t*& out, size_t& out_size) override {
    const auto in_avail = static_cast<unsigned>(std::min<size_t>(in_size, UINT_MAX));
    const auto out_avail = static_cast<unsigned>(std::min<size_t>(out_size, UINT_MAX));
    stream_.next_in = const_cast<char*>(reinterpret_cast<const char*>(in));
    stream_.avail_in = in_avail;
    stream_.next_out = reinterpret_cast<char*>(out);
    stream_.avail_out = out_avail;
    const int result = BZ2_bzDecompress(&stream_);
    in += in_avail - stream_.avail_in;
    in_size -= in_avail - stream_.avail_in;
    out += out_avail - stream_.avail_out;
    out_size -= out_avail - stream_.avail_out;
    switch (result) {
      case BZ_STREAM_END:
        return Status::End;
      case BZ_OK:
        return Status::Ok;
      default:
        return Status::Error;
    }
  }

  bool concatenated() const override { return true; }

 private:
  void end() {
    if (initialized_) {
      BZ2_bzDecompressEnd(&stream_);
      initialized_ = false;
    }
  }

  bz_stream stream_{};
  bool initialized_ = false;
};

// The method of 7-Zip ZS (and libarchive), whose properties (version, level)
// decoding does not need.
class ZstdEngine final : public Engine {
 public:
  ~ZstdEngine() override { ZSTD_freeDCtx(context_); }

  bool reset() override {
    if (context_ == nullptr) {
      context_ = ZSTD_createDCtx();
      if (context_ == nullptr) {
        return false;
      }
      ZSTD_DCtx_setParameter(context_, ZSTD_d_windowLogMax, kZstdWindowLogMax);
    }
    return !ZSTD_isError(ZSTD_DCtx_reset(context_, ZSTD_reset_session_only));
  }

  Status decode(const uint8_t*& in, size_t& in_size, uint8_t*& out, size_t& out_size) override {
    ZSTD_inBuffer input{in, in_size, 0};
    ZSTD_outBuffer output{out, out_size, 0};
    const size_t result = ZSTD_decompressStream(context_, &output, &input);
    in += input.pos;
    in_size -= input.pos;
    out += output.pos;
    out_size -= output.pos;
    if (ZSTD_isError(result)) {
      return Status::Error;
    }
    return result == 0 ? Status::End : Status::Ok;  // 0: the frame is complete and flushed.
  }

  bool concatenated() const override { return true; }

 private:
  ZSTD_DCtx* context_ = nullptr;
};

// A 7-Zip decoder over an Engine, in both of 7-Zip's ways of decoding: Code()
// writes everything to a stream, and with SetInStream() and SetOutStreamSize()
// the decoder is itself a stream to Read() from (7z blocks, see
// SevenZipArchive). Bad data is S_FALSE, as in 7-Zip's own decoders.
Z7_CLASS_IMP_COM_5(CLibraryDecoder, ICompressCoder, ICompressSetDecoderProperties2, ICompressSetInStream,
                   ICompressSetOutStreamSize, ISequentialInStream)
  std::unique_ptr<Engine> engine_;
  CMyComPtr<ISequentialInStream> in_stream_;
  std::vector<uint8_t> in_buffer_ = std::vector<uint8_t>(kInBufferSize);
  size_t in_pos_ = 0;
  size_t in_size_ = 0;
  bool in_end_ = false;
  UInt64 in_processed_ = 0;

  bool size_defined_ = false;
  UInt64 out_size_ = 0;
  UInt64 produced_ = 0;
  bool stream_ended_ = false;
  bool finished_ = false;
  HRESULT state_ = S_OK;  // The first failure, returned from then on.

  // Refills the input buffer once it has been used up.
  HRESULT fill() {
    if (in_pos_ < in_size_ || in_end_) {
      return S_OK;
    }
    if (!in_stream_) {
      return E_FAIL;
    }
    UInt32 got = 0;
    const HRESULT result = in_stream_->Read(in_buffer_.data(), static_cast<UInt32>(in_buffer_.size()), &got);
    in_pos_ = 0;
    in_size_ = got;
    in_end_ = got == 0;
    in_processed_ += got;
    return result;
  }

  // Decodes into [out, out + out_size); `progress`: whether anything happened.
  HRESULT step(uint8_t*& out, size_t& out_size, bool& progress) {
    const HRESULT filled = fill();
    if (filled != S_OK) {
      return filled;
    }
    const uint8_t* in = in_buffer_.data() + in_pos_;
    size_t in_left = in_size_ - in_pos_;
    const size_t in_before = in_left;
    const size_t out_before = out_size;
    const Engine::Status status = engine_->decode(in, in_left, out, out_size);
    in_pos_ += in_before - in_left;
    progress = in_left != in_before || out_size != out_before;
    if (status == Engine::Status::Error) {
      return S_FALSE;
    }
    if (status == Engine::Status::End) {
      stream_ended_ = true;
      progress = true;
    }
    return S_OK;
  }

  // Once all the expected output is there: the data must end right there.
  HRESULT finish() {
    uint8_t extra = 0;
    while (!stream_ended_) {
      uint8_t* out = &extra;
      size_t out_size = 1;
      bool progress = false;
      const HRESULT result = step(out, out_size, progress);
      if (result != S_OK) {
        return result;
      }
      if (out_size == 0 || !progress) {  // More data than expected, or truncated.
        return S_FALSE;
      }
    }
    return S_OK;
  }

 public:
  explicit CLibraryDecoder(std::unique_ptr<Engine> engine) : engine_(std::move(engine)) {}
};

Z7_COM7F_IMF(CLibraryDecoder::SetDecoderProperties2(const Byte* /*data*/, UInt32 /*size*/)) { return S_OK; }

Z7_COM7F_IMF(CLibraryDecoder::SetInStream(ISequentialInStream* inStream)) {
  in_stream_ = inStream;
  return S_OK;
}

Z7_COM7F_IMF(CLibraryDecoder::ReleaseInStream()) {
  in_stream_.Release();
  return S_OK;
}

Z7_COM7F_IMF(CLibraryDecoder::SetOutStreamSize(const UInt64* outSize)) {
  size_defined_ = outSize != nullptr;
  out_size_ = size_defined_ ? *outSize : 0;
  produced_ = 0;
  in_pos_ = 0;
  in_size_ = 0;
  in_end_ = false;
  in_processed_ = 0;
  stream_ended_ = false;
  finished_ = false;
  state_ = engine_->reset() ? S_OK : E_OUTOFMEMORY;
  return state_;
}

Z7_COM7F_IMF(CLibraryDecoder::Read(void* data, UInt32 size, UInt32* processedSize)) {
  if (processedSize != nullptr) {
    *processedSize = 0;
  }
  if (state_ != S_OK || finished_) {
    return state_;
  }
  if (size_defined_ && produced_ >= out_size_) {
    finished_ = true;
    state_ = finish();
    return state_;
  }
  size_t wanted = size;
  if (size_defined_) {
    wanted = static_cast<size_t>(std::min<UInt64>(wanted, out_size_ - produced_));
  }
  auto* out = static_cast<uint8_t*>(data);
  size_t out_left = wanted;
  while (out_left == wanted && wanted > 0) {
    if (stream_ended_) {
      // Before the expected size: only another stream can follow.
      const HRESULT filled = fill();
      if (filled != S_OK) {
        state_ = filled;
        break;
      }
      if (!engine_->concatenated() || in_pos_ == in_size_) {
        if (size_defined_) {
          state_ = S_FALSE;  // Shorter than the 7z headers say.
        } else {
          finished_ = true;
        }
        break;
      }
      if (!engine_->reset()) {
        state_ = E_OUTOFMEMORY;
        break;
      }
      stream_ended_ = false;
    }
    bool progress = false;
    const HRESULT result = step(out, out_left, progress);
    if (result != S_OK) {
      state_ = result;
      break;
    }
    if (!progress) {  // Truncated data.
      state_ = S_FALSE;
      break;
    }
  }
  const size_t got = wanted - out_left;
  produced_ += got;
  if (processedSize != nullptr) {
    *processedSize = static_cast<UInt32>(got);
  }
  // Data already decoded goes first; the failure comes with the next call.
  return got > 0 ? S_OK : state_;
}

Z7_COM7F_IMF(CLibraryDecoder::Code(ISequentialInStream* inStream, ISequentialOutStream* outStream,
                                   const UInt64* /*inSize*/, const UInt64* outSize,
                                   ICompressProgressInfo* progress)) {
  SetInStream(inStream);
  HRESULT result = SetOutStreamSize(outSize);
  std::vector<uint8_t> buffer(kCodeBufferSize);
  while (result == S_OK) {
    UInt32 got = 0;
    result = Read(buffer.data(), static_cast<UInt32>(buffer.size()), &got);
    if (got == 0) {
      break;
    }
    const HRESULT written = WriteStream(outStream, buffer.data(), got);
    if (written != S_OK) {
      result = written;
    } else if (progress != nullptr) {
      result = progress->SetRatioInfo(&in_processed_, &produced_);
    }
  }
  ReleaseInStream();
  return result;
}

template <typename E>
void* create_decoder() {
  return static_cast<ICompressCoder*>(new CLibraryDecoder(std::make_unique<E>()));
}

// IDs from 7-Zip's DOC/Methods.txt.
const CCodecInfo kMethods[] = {
    {create_decoder<DeflateEngine>, nullptr, 0x040108, "Deflate", 1, false},
    {create_decoder<Bzip2Engine>, nullptr, 0x040202, "BZip2", 1, false},
    {create_decoder<ZstdEngine>, nullptr, 0x4F71101, "ZSTD", 1, false},
};

}  // namespace

void register_sevenzip_methods() {
  static std::once_flag once;
  std::call_once(once, [] {
    for (const CCodecInfo& method : kMethods) {
      RegisterCodec(&method);
    }
  });
}

}  // namespace streamextract
