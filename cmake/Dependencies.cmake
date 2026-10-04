# Third-party dependencies. Everything is pinned (tag + SHA-256) and fetched at
# configure time, except libcurl, which comes from the system when available.

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

# --- libcurl (curl license, MIT-like) ----------------------------------------
if(NOT STREAMEXTRACT_BUNDLED_CURL)
  find_package(CURL 7.73 QUIET)
  if(CURL_FOUND)
    message(STATUS "streamextract: using system libcurl ${CURL_VERSION_STRING}")
  else()
    message(STATUS "streamextract: system libcurl not found, building the bundled one")
  endif()
endif()

if(STREAMEXTRACT_BUNDLED_CURL OR NOT CURL_FOUND)
  message(STATUS "streamextract: using bundled libcurl 8.22.0 (FTP only)")
  FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_22_0/curl-8.22.0.tar.xz
    URL_HASH SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
    EXCLUDE_FROM_ALL)

  # Only what an FTP client needs: no TLS, no compression, no other protocols.
  set(_curl_off
    BUILD_CURL_EXE BUILD_SHARED_LIBS BUILD_TESTING BUILD_EXAMPLES
    BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL CURL_ENABLE_SSL
    CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_LIBSSH USE_NGHTTP2 USE_LIBIDN2
    CURL_BROTLI CURL_ZSTD CURL_ZLIB PICKY_COMPILER CURL_ENABLE_EXPORT_TARGET
    CURL_DISABLE_FTP)
  set(_curl_on
    BUILD_STATIC_LIBS CURL_DISABLE_INSTALL CURL_DISABLE_HTTP CURL_DISABLE_DICT
    CURL_DISABLE_FILE CURL_DISABLE_GOPHER CURL_DISABLE_IMAP CURL_DISABLE_LDAP
    CURL_DISABLE_LDAPS CURL_DISABLE_MQTT CURL_DISABLE_POP3 CURL_DISABLE_RTSP
    CURL_DISABLE_SMTP CURL_DISABLE_TELNET CURL_DISABLE_TFTP CURL_DISABLE_IPFS
    CURL_DISABLE_WEBSOCKETS CURL_DISABLE_ALTSVC CURL_DISABLE_COOKIES
    CURL_DISABLE_HSTS CURL_DISABLE_DOH CURL_DISABLE_NETRC CURL_DISABLE_AWS)
  # BUILD_SHARED_LIBS is global: remember it so it can be restored.
  if(DEFINED BUILD_SHARED_LIBS)
    set(_saved_build_shared "${BUILD_SHARED_LIBS}")
  endif()
  foreach(_opt IN LISTS _curl_off)
    set(${_opt} OFF CACHE INTERNAL "")
  endforeach()
  foreach(_opt IN LISTS _curl_on)
    set(${_opt} ON CACHE INTERNAL "")
  endforeach()
  set(CURL_ZLIB OFF CACHE STRING "" FORCE)
  set(CURL_BROTLI OFF CACHE STRING "" FORCE)
  set(CURL_ZSTD OFF CACHE STRING "" FORCE)

  FetchContent_MakeAvailable(curl)

  if(DEFINED _saved_build_shared)
    set(BUILD_SHARED_LIBS "${_saved_build_shared}" CACHE BOOL "" FORCE)
  else()
    unset(BUILD_SHARED_LIBS CACHE)
  endif()
  if(NOT TARGET CURL::libcurl)
    message(FATAL_ERROR "bundled libcurl did not provide CURL::libcurl")
  endif()
endif()
