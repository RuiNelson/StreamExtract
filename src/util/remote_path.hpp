#pragma once

#include <string>
#include <string_view>

namespace streamextract {

struct SanitizedPath {
  std::string path;            // Relative, '/'-separated. Empty: nothing usable left.
  bool traversal = false;      // '..', absolute or drive prefixes were removed.
  bool control_chars = false;  // Control characters were replaced by '_'.
};

// Turns an archive entry name into a safe path relative to the destination
// directory. Mirrors UnRAR's own ConvertPath(): everything up to the last ".."
// component is dropped, as are leading separators, "." components and (with
// `windows_paths`) drive letters. `windows_paths` also treats '\' as a
// separator, which is what UnRAR produces on Windows builds.
SanitizedPath sanitize_archive_path(std::string_view name, bool windows_paths);

// True when compiled for Windows, i.e. UnRAR gives '\'-separated names.
constexpr bool kNativeWindowsPaths =
#ifdef _WIN32
    true;
#else
    false;
#endif

// Lexical normalization of a remote path: "a/./b/../c/" -> "/a/c". The result
// is always absolute; the root is "/".
std::string normalize_remote_path(std::string_view path);

// Resolves `path` (absolute or relative) against the absolute `base`.
std::string resolve_remote_path(std::string_view base, std::string_view path);

// "/dir" + "a/b" -> "/dir/a/b"; "/" + "a" -> "/a".
std::string join_remote_path(std::string_view dir, std::string_view relative);

// "/a/b" -> "/a"; "/a" -> "/"; "/" -> "/".
std::string remote_parent(std::string_view path);

// "/a/b" -> "b".
std::string remote_basename(std::string_view path);

// Percent-encodes everything except RFC 3986 unreserved characters.
std::string url_encode(std::string_view text);

// "ftp://host:21", with IPv6 literals in brackets.
std::string ftp_base_url(std::string_view host, int port);

// URL for an absolute remote path. libcurl decodes "%2F" to a leading "/", so
// the path is independent of the login directory. Directories get a trailing
// '/', which makes libcurl CWD into them instead of treating the last
// component as a file.
std::string ftp_url(std::string_view base_url, std::string_view absolute_path, bool directory);

}  // namespace streamextract
