#include "util/json.hpp"

#include <cmath>

#include <fmt/format.h>

#include "util/text.hpp"

namespace streamextract {

namespace {

void append_escaped(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += fmt::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

bool is_ascii(std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      return false;
    }
  }
  return true;
}

}  // namespace

void JsonWriter::begin_value() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!has_items_.empty()) {
    if (has_items_.back()) {
      out_ += ',';
    }
    has_items_.back() = true;
  }
}

void JsonWriter::begin_object() {
  begin_value();
  out_ += '{';
  has_items_.push_back(false);
}

void JsonWriter::end_object() {
  out_ += '}';
  has_items_.pop_back();
}

void JsonWriter::begin_array() {
  begin_value();
  out_ += '[';
  has_items_.push_back(false);
}

void JsonWriter::end_array() {
  out_ += ']';
  has_items_.pop_back();
}

void JsonWriter::key(std::string_view name) {
  begin_value();
  append_escaped(out_, is_ascii(name) ? std::string(name) : to_utf8(from_utf8(name)));
  out_ += ':';
  after_key_ = true;
}

void JsonWriter::value(std::string_view text) {
  begin_value();
  if (is_ascii(text)) {
    append_escaped(out_, text);
  } else {
    append_escaped(out_, to_utf8(from_utf8(text)));  // Invalid sequences become U+FFFD.
  }
}

void JsonWriter::value(bool flag) {
  begin_value();
  out_ += flag ? "true" : "false";
}

void JsonWriter::value(double number) {
  begin_value();
  if (!std::isfinite(number)) {
    out_ += "null";
    return;
  }
  std::string text = fmt::format("{}", number);
  if (text.find_first_of(".eE") == std::string::npos) {
    text += ".0";  // Keep it a float for strict consumers.
  }
  out_ += text;
}

void JsonWriter::null() {
  begin_value();
  out_ += "null";
}

std::string json_quote(std::string_view text) {
  JsonWriter writer;
  writer.value(text);
  return writer.str();
}

}  // namespace streamextract
