#include "util/remote_path.hpp"

#include <stdexcept>
#include <vector>

namespace streamextract {

namespace {

std::vector<std::string> split(std::string_view text, bool backslash_too) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == '/' || (backslash_too && c == '\\')) {
      parts.push_back(std::move(current));
      current.clear();
    } else {
      current += c;
    }
  }
  parts.push_back(std::move(current));
  return parts;
}

bool is_separator(char c, bool backslash_too) { return c == '/' || (backslash_too && c == '\\'); }

bool only_dots(const std::string& s) { return !s.empty() && s.find_first_not_of('.') == std::string::npos; }

bool is_drive(const std::string& s) {
  return s.size() == 2 && s[1] == ':' && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'));
}

std::string join(const std::vector<std::string>& parts, bool leading_slash) {
  std::string out;
  for (const auto& part : parts) {
    if (leading_slash || !out.empty()) {
      out += '/';
    }
    out += part;
  }
  if (leading_slash && out.empty()) {
    out = "/";
  }
  return out;
}

}  // namespace

SanitizedPath sanitize_archive_path(std::string_view name, bool windows_paths) {
  SanitizedPath result;
  std::vector<std::string> parts = split(name, windows_paths);

  // UNC prefix "//server/share/": drop server and share, like UnRAR does.
  size_t first = 0;
  if (name.size() >= 2 && is_separator(name[0], windows_paths) && is_separator(name[1], windows_paths)) {
    size_t dropped = 0;
    for (first = 0; first < parts.size() && dropped < 2; ++first) {
      if (!parts[first].empty()) {
        ++dropped;
      }
    }
    result.traversal = true;
  }

  // Everything up to (and including) the last ".." is discarded.
  for (size_t i = first; i < parts.size(); ++i) {
    if (parts[i] == "..") {
      first = i + 1;
      result.traversal = true;
    }
  }

  std::vector<std::string> kept;
  for (size_t i = first; i < parts.size(); ++i) {
    std::string& part = parts[i];
    if (part.empty() || part == ".") {
      if (kept.empty() && i == 0 && part.empty() && parts.size() > 1) {
        result.traversal = true;  // Absolute path.
      }
      continue;
    }
    if (only_dots(part)) {
      result.traversal = true;
      continue;
    }
    if (windows_paths && kept.empty() && is_drive(part)) {
      result.traversal = true;
      continue;
    }
    for (char& c : part) {
      const auto u = static_cast<unsigned char>(c);
      if (u < 0x20 || u == 0x7F) {
        c = '_';
        result.control_chars = true;
      }
    }
    kept.push_back(std::move(part));
  }
  result.path = join(kept, false);
  return result;
}

std::string normalize_remote_path(std::string_view path) {
  std::vector<std::string> kept;
  for (auto& part : split(path, false)) {
    if (part.empty() || part == ".") {
      continue;
    }
    if (part == "..") {
      if (!kept.empty()) {
        kept.pop_back();
      }
      continue;
    }
    kept.push_back(std::move(part));
  }
  return join(kept, true);
}

std::string resolve_remote_path(std::string_view base, std::string_view path) {
  if (!path.empty() && path.front() == '/') {
    return normalize_remote_path(path);
  }
  std::string combined(base);
  combined += '/';
  combined += path;
  return normalize_remote_path(combined);
}

std::string join_remote_path(std::string_view dir, std::string_view relative) {
  std::string out(dir);
  if (relative.empty()) {
    return out.empty() ? "/" : out;
  }
  if (out.empty() || out.back() != '/') {
    out += '/';
  }
  out += relative;
  return out;
}

std::string remote_parent(std::string_view path) {
  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos || slash == 0) {
    return "/";
  }
  return std::string(path.substr(0, slash));
}

std::string remote_basename(std::string_view path) {
  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }
  const size_t slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string url_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    const bool unreserved = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
                            u == '-' || u == '.' || u == '_' || u == '~';
    if (unreserved) {
      out += c;
    } else {
      out += '%';
      out += kHex[u >> 4];
      out += kHex[u & 0x0F];
    }
  }
  return out;
}

std::string ftp_base_url(std::string_view host, int port) {
  std::string url = "ftp://";
  const bool ipv6 = host.find(':') != std::string_view::npos && !host.empty() && host.front() != '[';
  if (ipv6) {
    url += '[';
  }
  url += host;
  if (ipv6) {
    url += ']';
  }
  url += ':';
  url += std::to_string(port);
  return url;
}

std::string ftp_url(std::string_view base_url, std::string_view absolute_path, bool directory) {
  std::string url(base_url);
  url += "/%2F";
  bool any = false;
  for (const auto& part : split(absolute_path, false)) {
    if (part.empty()) {
      continue;
    }
    if (any) {
      url += '/';
    }
    url += url_encode(part);
    any = true;
  }
  if (directory) {
    url += '/';
  }
  return url;
}

void validate_staging_path(const std::string& staging, const std::string& destination) {
  for (const unsigned char c : staging) {
    if (c < 32 || c == 127) throw std::runtime_error("the staging directory contains control characters");
  }
  if (staging == destination || staging == "/" || destination.compare(0, staging.size() + 1, staging + "/") == 0) {
    throw std::runtime_error("the staging directory must differ from the destination and cannot contain it");
  }
}

}  // namespace streamextract
