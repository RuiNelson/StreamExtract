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

# --- mbed TLS (Apache-2.0) --------------------------------------------------
# Build the same static TLS backend on every platform.
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
  EXCLUDE_FROM_ALL)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mbedtls)
# TLS 1.3 uses shared PSA state; protect it when the engine uses multiple threads.
find_package(Threads REQUIRED)
target_compile_definitions(mbedcrypto PUBLIC MBEDTLS_THREADING_C)
if(WIN32)
  target_compile_definitions(mbedcrypto PUBLIC MBEDTLS_THREADING_ALT)
  target_include_directories(mbedcrypto PUBLIC "${PROJECT_SOURCE_DIR}/cmake/mbedtls")
else()
  target_compile_definitions(mbedcrypto PUBLIC MBEDTLS_THREADING_PTHREAD)
  target_link_libraries(mbedcrypto PUBLIC Threads::Threads)
endif()
# curl's finder accepts library targets, so no system TLS library is found.
set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include")
set(MBEDTLS_LIBRARY mbedtls)
set(MBEDX509_LIBRARY mbedx509)
set(MBEDCRYPTO_LIBRARY mbedcrypto)

# --- libcurl (curl license, MIT-like) ----------------------------------------
# Always build our own libcurl so protocol support does not depend on the system.
message(STATUS "streamextract: using bundled libcurl 8.22.0 (FTP and FTPS)")
FetchContent_Declare(curl
  URL https://github.com/curl/curl/releases/download/curl-8_22_0/curl-8.22.0.tar.xz
  URL_HASH SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
  EXCLUDE_FROM_ALL)

# Only FTP/FTPS: no compression or other protocols.
set(_curl_off
  BUILD_CURL_EXE BUILD_TESTING BUILD_EXAMPLES
  BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL
  CURL_USE_OPENSSL CURL_USE_GNUTLS CURL_USE_WOLFSSL CURL_USE_RUSTLS USE_APPLE_SECTRUST
  CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_LIBSSH USE_NGHTTP2 USE_LIBIDN2
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
set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(curl)

set(BUILD_SHARED_LIBS "${_saved_build_shared}")
if(NOT TARGET CURL::libcurl_static)
  message(FATAL_ERROR "bundled libcurl did not provide CURL::libcurl_static")
endif()
