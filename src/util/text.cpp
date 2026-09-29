#include "util/text.hpp"

#include <array>
#include <cmath>

#include <fmt/format.h>

namespace rarftp {

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

std::string format_bytes(uint64_t bytes) {
  if (bytes < 1024) {
    return fmt::format("{} B", bytes);
  }
  static constexpr std::array<const char*, 6> kUnits = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
  double value = static_cast<double>(bytes) / 1024.0;
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }
  if (value < 10.0) {
    return fmt::format("{:.2f} {}", value, kUnits[unit]);
  }
  if (value < 100.0) {
    return fmt::format("{:.1f} {}", value, kUnits[unit]);
  }
  return fmt::format("{:.0f} {}", value, kUnits[unit]);
}

std::string format_speed(double bytes_per_second) {
  if (!std::isfinite(bytes_per_second) || bytes_per_second < 0.5) {
    return "0 B/s";
  }
  return format_bytes(static_cast<uint64_t>(bytes_per_second)) + "/s";
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

}  // namespace rarftp
