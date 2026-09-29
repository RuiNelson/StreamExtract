#include <limits>
#include <string>

#include <doctest/doctest.h>

#include "util/text.hpp"

using namespace rarftp;

TEST_CASE("UTF-8 round trip through wide strings") {
  const std::string text = "ação – 日本語 😀";
  CHECK(to_utf8(from_utf8(text)) == text);

  const std::wstring emoji = from_utf8("😀");
  CHECK(emoji.size() == (sizeof(wchar_t) == 2 ? 2u : 1u));
  CHECK(to_utf8(emoji) == "😀");
}

TEST_CASE("invalid UTF-8 becomes U+FFFD") {
  const std::wstring replacement(1, static_cast<wchar_t>(0xFFFD));
  CHECK(from_utf8("\xff") == replacement);
  CHECK(from_utf8("\xc0\xaf") == replacement + replacement);  // Overlong '/'.
  CHECK(from_utf8("\xed\xa0\x80").size() == 3);               // Encoded surrogate.
  CHECK(from_utf8("ab\xe2\x82") == L"ab" + replacement + replacement);
}

TEST_CASE("format_bytes") {
  CHECK(format_bytes(0) == "0 B");
  CHECK(format_bytes(1023) == "1023 B");
  CHECK(format_bytes(1024) == "1.00 KiB");
  CHECK(format_bytes(1536) == "1.50 KiB");
  CHECK(format_bytes(10ull << 20) == "10.0 MiB");
  CHECK(format_bytes(500ull << 30) == "500 GiB");
  CHECK(format_bytes(5ull << 40) == "5.00 TiB");
}

TEST_CASE("format_speed and format_duration") {
  CHECK(format_speed(0.0) == "0 B/s");
  CHECK(format_speed(2.5 * 1024 * 1024) == "2.50 MiB/s");
  CHECK(format_duration(0) == "00:00:00");
  CHECK(format_duration(3723) == "01:02:03");
  CHECK(format_duration(100 * 3600) == "100:00:00");
  CHECK(format_duration(-1) == "--:--:--");
  CHECK(format_duration(std::numeric_limits<double>::infinity()) == "--:--:--");
  CHECK(format_duration(std::numeric_limits<double>::quiet_NaN()) == "--:--:--");
}

TEST_CASE("FTP timestamps") {
  CHECK(format_ftp_timestamp(0) == "19700101000000");
  CHECK(format_ftp_timestamp(951782400) == "20000229000000");  // Leap day.
  CHECK(format_ftp_timestamp(1700000000) == "20231114221320");
  CHECK(format_ftp_timestamp(-5) == "19700101000000");
  CHECK(filetime_to_unix(116444736000000000ull) == 0);
  CHECK(filetime_to_unix(116444736000000000ull + 17000000000000000ull) == 1700000000);
}
