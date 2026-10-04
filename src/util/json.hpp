#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace streamextract {

// Minimal streaming JSON writer. Commas are inserted automatically; inside an
// object every value is preceded by key(). Strings are escaped as RFC 8259
// requires, and invalid UTF-8 is replaced by U+FFFD so the output is always
// valid UTF-8.
class JsonWriter {
 public:
  void begin_object();
  void end_object();
  void begin_array();
  void end_array();
  void key(std::string_view name);

  void value(std::string_view text);
  void value(const char* text) { value(std::string_view(text)); }
  void value(const std::string& text) { value(std::string_view(text)); }
  void value(bool flag);
  // Non-finite numbers have no JSON form: written as null.
  void value(double number);
  template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  void value(T number) {
    begin_value();
    out_ += std::to_string(number);
  }
  void null();

  // key() + value() in one call.
  template <typename T>
  void member(std::string_view name, const T& v) {
    key(name);
    value(v);
  }
  void member_null(std::string_view name) {
    key(name);
    null();
  }

  const std::string& str() const { return out_; }

 private:
  // Writes the separator due before a value (or before a key in an object).
  void begin_value();

  std::string out_;
  std::vector<bool> has_items_;  // One entry per open object/array.
  bool after_key_ = false;
};

// `text` as a quoted JSON string.
std::string json_quote(std::string_view text);

}  // namespace streamextract
