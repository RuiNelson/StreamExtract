#include <cmath>
#include <limits>
#include <string>

#include <doctest/doctest.h>

#include "util/json.hpp"

using namespace rarftp;

TEST_CASE("json strings are escaped") {
  CHECK(json_quote("plain") == "\"plain\"");
  CHECK(json_quote("say \"hi\"") == "\"say \\\"hi\\\"\"");
  CHECK(json_quote("C:\\dir\\file") == "\"C:\\\\dir\\\\file\"");
  CHECK(json_quote("a\nb\r\tc") == "\"a\\nb\\r\\tc\"");
  CHECK(json_quote("\b\f") == "\"\\b\\f\"");
  CHECK(json_quote(std::string("\x01\x1f", 2)) == "\"\\u0001\\u001f\"");
  CHECK(json_quote(std::string("a\0b", 3)) == "\"a\\u0000b\"");
  CHECK(json_quote("/") == "\"/\"");
  CHECK(json_quote("") == "\"\"");
}

TEST_CASE("json passes UTF-8 through and repairs invalid input") {
  const std::string text = "ação – 日本語 😀";
  CHECK(json_quote(text) == "\"" + text + "\"");
  CHECK(json_quote("\xff") == "\"\xEF\xBF\xBD\"");
  CHECK(json_quote("ab\xe2\x82") == "\"ab\xEF\xBF\xBD\xEF\xBF\xBD\"");
}

TEST_CASE("json writer separates values") {
  JsonWriter json;
  json.begin_object();
  json.member("name", "x\"y");
  json.member("count", 3);
  json.member("big", uint64_t{18446744073709551615ull});
  json.member("negative", int64_t{-5});
  json.member("flag", true);
  json.member("no", false);
  json.member_null("nothing");
  json.key("list");
  json.begin_array();
  json.value(1);
  json.value("two");
  json.begin_object();
  json.end_object();
  json.begin_array();
  json.end_array();
  json.end_array();
  json.key("nested");
  json.begin_object();
  json.member(std::string("key\n"), std::string("v"));
  json.end_object();
  json.end_object();
  CHECK(json.str() ==
        "{\"name\":\"x\\\"y\",\"count\":3,\"big\":18446744073709551615,\"negative\":-5,\"flag\":true,"
        "\"no\":false,\"nothing\":null,\"list\":[1,\"two\",{},[]],\"nested\":{\"key\\n\":\"v\"}}");
}

TEST_CASE("json numbers") {
  const auto number = [](double v) {
    JsonWriter json;
    json.value(v);
    return json.str();
  };
  CHECK(number(0.0) == "0.0");
  CHECK(number(-1.0) == "-1.0");
  CHECK(number(0.5) == "0.5");
  CHECK(number(1234567.0) == "1234567.0");
  CHECK(number(std::numeric_limits<double>::infinity()) == "null");
  CHECK(number(-std::numeric_limits<double>::infinity()) == "null");
  CHECK(number(std::nan("")) == "null");
  // Whatever the format, it must read back as the same number.
  CHECK(std::stod(number(0.1)) == 0.1);
  CHECK(std::stod(number(1e21)) == 1e21);
}
