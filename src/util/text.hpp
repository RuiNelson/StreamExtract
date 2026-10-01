#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rarftp {

// UTF-8 <-> wchar_t strings (UTF-16 on Windows, UTF-32 elsewhere).
// Invalid input sequences are replaced by U+FFFD.
std::string to_utf8(std::wstring_view wide);
std::wstring from_utf8(std::string_view utf8);

// True if `text` is well-formed UTF-8 (no overlong forms or surrogates).
bool is_valid_utf8(std::string_view text);

// Decodes code page 437, the default charset of ZIP file names.
std::string cp437_to_utf8(std::string_view text);

// Decodes ISO 8859-1 (Latin-1), which maps every byte to a code point.
std::string latin1_to_utf8(std::string_view text);

// Human readable sizes using binary units: "512 B", "1.50 KiB", "3.25 GiB".
std::string format_bytes(uint64_t bytes);

// "85.3 MiB/s".
std::string format_speed(double bytes_per_second);

// "01:02:03" ("123:04:05" past 99 hours). Negative, NaN or infinite values
// mean "unknown" and give "--:--:--".
std::string format_duration(double seconds);

// Formats a Unix timestamp as "YYYYMMDDHHMMSS" in UTC (the MFMT/MDTM format).
std::string format_ftp_timestamp(int64_t unix_seconds);

// Converts a Windows FILETIME (100 ns ticks since 1601-01-01 UTC) to Unix
// seconds.
int64_t filetime_to_unix(uint64_t filetime);

}  // namespace rarftp
