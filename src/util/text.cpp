#include "util/text.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

namespace streamextract {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool is_surrogate(char32_t cp) { return cp >= 0xD800 && cp <= 0xDFFF; }

void append_utf8(std::string& out, char32_t cp) {
  if (cp > 0x10FFFF || is_surrogate(cp)) {
    cp = kReplacement;
  }
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

void append_wide(std::wstring& out, char32_t cp) {
  if (cp > 0x10FFFF || is_surrogate(cp)) {
    cp = kReplacement;
  }
  if constexpr (sizeof(wchar_t) == 2) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += static_cast<wchar_t>(0xD800 + (cp >> 10));
      out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
      return;
    }
  }
  out += static_cast<wchar_t>(cp);
}

// Decodes one UTF-8 sequence starting at `i`; advances `i`. Invalid or
// truncated sequences consume one byte and yield U+FFFD.
char32_t decode_utf8(std::string_view s, size_t& i) {
  const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char b0 = byte(i);
  if (b0 < 0x80) {
    ++i;
    return b0;
  }
  size_t len = 0;
  char32_t cp = 0;
  char32_t min = 0;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
    min = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
    min = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
    min = 0x10000;
  } else {
    ++i;
    return kReplacement;
  }
  if (i + len > s.size()) {
    ++i;
    return kReplacement;
  }
  for (size_t k = 1; k < len; ++k) {
    const unsigned char b = byte(i + k);
    if ((b & 0xC0) != 0x80) {
      ++i;
      return kReplacement;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || is_surrogate(cp)) {
    ++i;
    return kReplacement;
  }
  i += len;
  return cp;
}

}  // namespace

std::string to_utf8(std::wstring_view wide) {
  std::string out;
  out.reserve(wide.size());
  for (size_t i = 0; i < wide.size(); ++i) {
    char32_t cp = static_cast<char32_t>(wide[i]);
    if constexpr (sizeof(wchar_t) == 2) {
      cp &= 0xFFFF;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size()) {
        const char32_t low = static_cast<char32_t>(wide[i + 1]) & 0xFFFF;
        if (low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          ++i;
        }
      }
    }
    append_utf8(out, cp);
  }
  return out;
}

std::wstring from_utf8(std::string_view utf8) {
  std::wstring out;
  out.reserve(utf8.size());
  size_t i = 0;
  while (i < utf8.size()) {
    append_wide(out, decode_utf8(utf8, i));
  }
  return out;
}

std::string utf16_to_utf8(std::u16string_view utf16) {
  std::string out;
  out.reserve(utf16.size());
  for (size_t i = 0; i < utf16.size(); ++i) {
    char32_t cp = utf16[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (utf16[i + 1] - 0xDC00);
      ++i;
    }
    append_utf8(out, cp);
  }
  return out;
}

std::u16string utf8_to_utf16(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());
  size_t i = 0;
  while (i < utf8.size()) {
    char32_t cp = decode_utf8(utf8, i);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += static_cast<char16_t>(0xD800 + (cp >> 10));
      out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    } else {
      out += static_cast<char16_t>(cp);
    }
  }
  return out;
}

bool is_valid_utf8(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    const size_t start = i;
    // A real U+FFFD takes three bytes; a decoding error consumes one.
    if (decode_utf8(text, i) == kReplacement && i - start == 1) {
      return false;
    }
  }
  return true;
}

std::string cp437_to_utf8(std::string_view text) {
  static constexpr char32_t kHigh[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
  };
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    append_utf8(out, byte < 0x80 ? byte : kHigh[byte - 0x80]);
  }
  return out;
}

std::string latin1_to_utf8(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    append_utf8(out, static_cast<unsigned char>(c));
  }
  return out;
}

std::string format_bytes(uint64_t bytes, ByteUnits units) {
  const uint64_t base = units == ByteUnits::Si ? 1000 : 1024;
  if (bytes < base) {
    return fmt::format("{} B", bytes);
  }
  static constexpr std::array<const char*, 6> kBinary = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
  static constexpr std::array<const char*, 6> kSi = {"kB", "MB", "GB", "TB", "PB", "EB"};
  const auto& names = units == ByteUnits::Si ? kSi : kBinary;
  const double divisor = static_cast<double>(base);
  double value = static_cast<double>(bytes) / divisor;
  size_t unit = 0;
  while (value >= divisor && unit + 1 < names.size()) {
    value /= divisor;
    ++unit;
  }
  // Three significant digits. Rounding can reach the next magnitude ("9.999"
  // as "10.00", 1023.9 KiB as "1024"): print that magnitude instead.
  int decimals = value < 10.0 ? 2 : value < 100.0 ? 1 : 0;
  std::string number = fmt::format("{:.{}f}", value, decimals);
  while (decimals > 0 && number.find('.') > static_cast<size_t>(3 - decimals)) {
    --decimals;
    number = fmt::format("{:.{}f}", value, decimals);
  }
  if (decimals == 0 && std::strtoull(number.c_str(), nullptr, 10) >= base && unit + 1 < names.size()) {
    value /= divisor;
    ++unit;
    number = fmt::format("{:.2f}", value);
  }
  return fmt::format("{} {}", number, names[unit]);
}

std::string format_speed(double bytes_per_second, ByteUnits units) {
  if (!std::isfinite(bytes_per_second) || bytes_per_second < 0.5) {
    return "0 B/s";
  }
  return format_bytes(static_cast<uint64_t>(bytes_per_second), units) + "/s";
}

std::string format_duration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0 || seconds >= 1000.0 * 3600.0) {
    return "--:--:--";
  }
  const auto total = static_cast<uint64_t>(std::llround(seconds));
  return fmt::format("{:02}:{:02}:{:02}", total / 3600, (total / 60) % 60, total % 60);
}

std::string format_ftp_timestamp(int64_t unix_seconds) {
  if (unix_seconds < 0) {
    unix_seconds = 0;
  }
  const int64_t days = unix_seconds / 86400;
  const int64_t secs = unix_seconds % 86400;

  // Civil-from-days, H. Hinnant's algorithm (days since 1970-01-01).
  const int64_t z = days + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);

  return fmt::format("{:04}{:02}{:02}{:02}{:02}{:02}", year, month, day, secs / 3600, (secs / 60) % 60, secs % 60);
}

int64_t filetime_to_unix(uint64_t filetime) {
  constexpr int64_t kEpochDelta = 11644473600;  // Seconds from 1601 to 1970.
  return static_cast<int64_t>(filetime / 10000000) - kEpochDelta;
}

}  // namespace streamextract
