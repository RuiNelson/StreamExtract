#include <doctest/doctest.h>

#include "util/remote_path.hpp"

using namespace streamextract;

TEST_CASE("sanitize keeps ordinary relative paths") {
  const SanitizedPath r = sanitize_archive_path("dir/sub/file.txt", false);
  CHECK(r.path == "dir/sub/file.txt");
  CHECK_FALSE(r.traversal);
  CHECK_FALSE(r.control_chars);
  CHECK(sanitize_archive_path("ação/ç.txt", false).path == "ação/ç.txt");
  CHECK(sanitize_archive_path(".hidden/..rc", false).path == ".hidden/..rc");
}

TEST_CASE("sanitize removes traversal the way UnRAR does") {
  CHECK(sanitize_archive_path("../../etc/passwd", false).path == "etc/passwd");
  CHECK(sanitize_archive_path("../../etc/passwd", false).traversal);
  CHECK(sanitize_archive_path("a/../b", false).path == "b");
  CHECK(sanitize_archive_path("a/b/..", false).path.empty());
  CHECK(sanitize_archive_path("/abs/file", false).path == "abs/file");
  CHECK(sanitize_archive_path("/abs/file", false).traversal);
  CHECK(sanitize_archive_path("//server/share/dir/f", false).path == "dir/f");
  CHECK(sanitize_archive_path("a/.../b", false).path == "a/b");
}

TEST_CASE("sanitize drops empty and dot components") {
  CHECK(sanitize_archive_path("./a/./b", false).path == "a/b");
  CHECK(sanitize_archive_path("a//b/", false).path == "a/b");
  CHECK_FALSE(sanitize_archive_path("a//b/", false).traversal);
  CHECK(sanitize_archive_path(".", false).path.empty());
  CHECK(sanitize_archive_path("", false).path.empty());
}

TEST_CASE("sanitize handles Windows separators and drives") {
  CHECK(sanitize_archive_path("C:\\dir\\file.txt", true).path == "dir/file.txt");
  CHECK(sanitize_archive_path("C:\\dir\\file.txt", true).traversal);
  CHECK(sanitize_archive_path("dir\\file.txt", true).path == "dir/file.txt");
  // On Unix a backslash is an ordinary file name character.
  CHECK(sanitize_archive_path("dir\\file.txt", false).path == "dir\\file.txt");
  CHECK(sanitize_archive_path("C:/x", false).path == "C:/x");
}

TEST_CASE("sanitize replaces control characters") {
  const SanitizedPath r = sanitize_archive_path("bad\r\nname\x7f.txt", false);
  CHECK(r.path == "bad__name_.txt");
  CHECK(r.control_chars);
}

TEST_CASE("normalize_remote_path") {
  CHECK(normalize_remote_path("/") == "/");
  CHECK(normalize_remote_path("") == "/");
  CHECK(normalize_remote_path("/a/./b/../c/") == "/a/c");
  CHECK(normalize_remote_path("a/b") == "/a/b");
  CHECK(normalize_remote_path("/..") == "/");
  CHECK(normalize_remote_path("//a///b") == "/a/b");
}

TEST_CASE("resolve_remote_path") {
  CHECK(resolve_remote_path("/home/bob", "uploads") == "/home/bob/uploads");
  CHECK(resolve_remote_path("/home/bob", "/abs/dir/") == "/abs/dir");
  CHECK(resolve_remote_path("/home/bob", "../x") == "/home/x");
  CHECK(resolve_remote_path("/", ".") == "/");
}

TEST_CASE("join, parent and basename") {
  CHECK(join_remote_path("/", "a/b") == "/a/b");
  CHECK(join_remote_path("/dir", "a") == "/dir/a");
  CHECK(join_remote_path("/dir/", "a") == "/dir/a");
  CHECK(join_remote_path("/dir", "") == "/dir");
  CHECK(remote_parent("/a/b") == "/a");
  CHECK(remote_parent("/a") == "/");
  CHECK(remote_parent("/") == "/");
  CHECK(remote_basename("/a/b.txt") == "b.txt");
  CHECK(remote_basename("/a/") == "a");
}

TEST_CASE("url encoding and FTP URLs") {
  CHECK(url_encode("a b#?%;=.txt") == "a%20b%23%3F%25%3B%3D.txt");
  CHECK(url_encode("ç") == "%C3%A7");
  CHECK(url_encode("A-z_0.9~") == "A-z_0.9~");
  CHECK(ftp_base_url("example.com", 21) == "ftp://example.com:21");
  CHECK(ftp_base_url("::1", 2121) == "ftp://[::1]:2121");
  CHECK(ftp_base_url("[::1]", 21) == "ftp://[::1]:21");
  CHECK(ftp_url("ftp://h:21", "/", true) == "ftp://h:21/%2F/");
  CHECK(ftp_url("ftp://h:21", "/a b/c", true) == "ftp://h:21/%2Fa%20b/c/");
  CHECK(ftp_url("ftp://h:21", "/f.txt", false) == "ftp://h:21/%2Ff.txt");
  // ";type=" would switch libcurl to ASCII mode if it were not encoded.
  CHECK(ftp_url("ftp://h:21", "/a/f;type=a", false) == "ftp://h:21/%2Fa/f%3Btype%3Da");
}
