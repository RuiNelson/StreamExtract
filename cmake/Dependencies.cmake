# Third-party dependencies. Everything is pinned (tag + SHA-256) and fetched at
# configure time and built as static libraries.

include(FetchContent)

# --- fmt (MIT) ---------------------------------------------------------------
FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz
  URL_HASH SHA256=8b852bb5aa6e7d8564f9e81394055395dd1d1936d38dfd3a17792a02bebd7af0
  EXCLUDE_FROM_ALL)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)

# --- CLI11 (BSD-3-Clause) ----------------------------------------------------
FetchContent_Declare(CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
  URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
  EXCLUDE_FROM_ALL)
set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)

# --- FTXUI (MIT) -------------------------------------------------------------
FetchContent_Declare(ftxui
  URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
  URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
  EXCLUDE_FROM_ALL)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET ON CACHE BOOL "" FORCE)

# These libraries are only used by streamextract: link them statically, like UnRAR,
# even when the parent project builds shared libraries.
set(_streamextract_build_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(fmt CLI11 ftxui)
set(BUILD_SHARED_LIBS "${_streamextract_build_shared}")

# --- OpenSSL and libssh2 (static TLS and SSH) --------------------------------
find_package(Threads REQUIRED)
include(SSH)

# --- libcurl (curl license, MIT-like) ----------------------------------------
# Always build our own libcurl so protocol support does not depend on the system.
# Our fork of curl 8.23.0 (RuiNelson/curl-streamextract) adds
# FTPMC, FTP block mode (CURLOPT_FTP_TRANSFER_MODE, CURLINFO_FTP_TRANSFER_MODE) and the
# SSH passphrase, SFTP rename and SFTP zero-size fixes. To update it, pin the new
# commit and the SHA-256 of its GitHub archive.
message(STATUS "streamextract: using bundled libcurl 8.23.0 with FTPMC and FTP block mode (FTP, FTPS and SFTP)")
FetchContent_Declare(curl
  URL https://github.com/RuiNelson/curl-streamextract/archive/bc901172f8602f1d004eead19e50f6cf0b7256e2.tar.gz
  URL_HASH SHA256=287fcc075225ed770c45f82ae9a990fa0b8fbc57f5be4c44af6fbbeb30d994cf
  EXCLUDE_FROM_ALL)

# Enable FTP/FTPS and SSH; the application exposes SFTP only. No compression.
set(_curl_off
  BUILD_CURL_EXE BUILD_TESTING BUILD_EXAMPLES
  BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL
  CURL_USE_GNUTLS CURL_USE_WOLFSSL CURL_USE_RUSTLS
  CURL_USE_LIBPSL CURL_USE_LIBSSH USE_NGHTTP2 USE_LIBIDN2
  CURL_BROTLI CURL_ZSTD CURL_ZLIB PICKY_COMPILER CURL_ENABLE_EXPORT_TARGET
  CURL_DISABLE_FTP)
set(_curl_on
  BUILD_STATIC_LIBS CURL_ENABLE_SSL CURL_DISABLE_INSTALL CURL_DISABLE_HTTP CURL_DISABLE_DICT
  CURL_DISABLE_FILE CURL_DISABLE_GOPHER CURL_DISABLE_IMAP CURL_DISABLE_LDAP
  CURL_DISABLE_LDAPS CURL_DISABLE_MQTT CURL_DISABLE_POP3 CURL_DISABLE_RTSP
  CURL_DISABLE_SMTP CURL_DISABLE_TELNET CURL_DISABLE_TFTP CURL_DISABLE_IPFS
  CURL_DISABLE_WEBSOCKETS CURL_DISABLE_ALTSVC CURL_DISABLE_COOKIES
  CURL_DISABLE_HSTS CURL_DISABLE_DOH CURL_DISABLE_NETRC CURL_DISABLE_AWS)
# BUILD_SHARED_LIBS is global: keep the parent project's setting.
set(_saved_build_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
foreach(_opt IN LISTS _curl_off)
  set(${_opt} OFF CACHE INTERNAL "")
endforeach()
foreach(_opt IN LISTS _curl_on)
  set(${_opt} ON CACHE INTERNAL "")
endforeach()
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE STRING "" FORCE)
set(CURL_ZSTD OFF CACHE STRING "" FORCE)
set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(USE_APPLE_SECTRUST ${APPLE} CACHE BOOL "" FORCE)
set(CURL_CA_NATIVE ${WIN32} CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(curl)

set(BUILD_SHARED_LIBS "${_saved_build_shared}")
if(NOT TARGET CURL::libcurl_static)
  message(FATAL_ERROR "bundled libcurl did not provide CURL::libcurl_static")
endif()
