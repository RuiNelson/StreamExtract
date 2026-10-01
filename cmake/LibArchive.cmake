# libarchive (ZIP, 7z and tar reading) and the compression libraries it uses, all
# built from pinned sources (URL + SHA-256) as static libraries.
#
# libarchive's configure step checks its dependencies by compiling and linking
# test programs against them, so they cannot be FetchContent subprojects (their
# libraries do not exist yet at configure time). Each one is an ExternalProject
# instead, built and installed into ${RARFTP_ARCHIVE_DEPS} before libarchive is
# configured, always in Release and with the toolchain settings of this build.
#
# Defines the imported target rarftp::libarchive.

include(ExternalProject)

set(RARFTP_ARCHIVE_DEPS "${CMAKE_BINARY_DIR}/archive-deps")
set(_ad "${RARFTP_ARCHIVE_DEPS}")
file(MAKE_DIRECTORY "${_ad}/include")

if(MSVC)
  set(_lib_prefix "")
  set(_lib_suffix ".lib")
else()
  set(_lib_prefix "lib")
  set(_lib_suffix ".a")
endif()
macro(_rarftp_static_lib var name)
  set(${var} "${_ad}/lib/${_lib_prefix}${name}${_lib_suffix}")
endmacro()

if(WIN32)
  _rarftp_static_lib(_zlib_lib zs)
else()
  _rarftp_static_lib(_zlib_lib z)
endif()
_rarftp_static_lib(_bzip2_lib bz2)
_rarftp_static_lib(_lzma_lib lzma)
if(MSVC)
  _rarftp_static_lib(_zstd_lib zstd_static)
else()
  _rarftp_static_lib(_zstd_lib zstd)
endif()
_rarftp_static_lib(_lz4_lib lz4)
_rarftp_static_lib(_mbedcrypto_lib mbedcrypto)
_rarftp_static_lib(_mbedx509_lib mbedx509)
_rarftp_static_lib(_mbedtls_lib mbedtls)
_rarftp_static_lib(_archive_lib archive)

# AES for encrypted ZIP files: libarchive uses CommonCrypto on macOS and CNG on
# Windows; elsewhere it needs a crypto library, mbed TLS here.
if(APPLE OR WIN32)
  set(_use_mbedtls OFF)
else()
  set(_use_mbedtls ON)
endif()

# Settings shared by every dependency, as an initial cache file: values such as
# CMAKE_OSX_ARCHITECTURES are lists, which do not survive a command line.
set(_cache "${CMAKE_BINARY_DIR}/archive-deps-cache.cmake")
set(_cache_text "")
macro(_rarftp_cache name type value)
  string(APPEND _cache_text "set(${name} [==[${value}]==] CACHE ${type} \"\" FORCE)\n")
endmacro()
_rarftp_cache(CMAKE_BUILD_TYPE STRING Release)
_rarftp_cache(CMAKE_INSTALL_PREFIX PATH "${_ad}")
_rarftp_cache(CMAKE_INSTALL_LIBDIR PATH lib)
_rarftp_cache(CMAKE_INSTALL_INCLUDEDIR PATH include)
_rarftp_cache(CMAKE_PREFIX_PATH PATH "${_ad}")
_rarftp_cache(CMAKE_POSITION_INDEPENDENT_CODE BOOL ON)
_rarftp_cache(BUILD_SHARED_LIBS BOOL OFF)
_rarftp_cache(BUILD_TESTING BOOL OFF)
_rarftp_cache(CMAKE_POLICY_DEFAULT_CMP0091 STRING NEW)
foreach(_var IN ITEMS CMAKE_C_COMPILER CMAKE_C_COMPILER_LAUNCHER CMAKE_C_FLAGS CMAKE_MAKE_PROGRAM
                      CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET
                      CMAKE_OSX_SYSROOT CMAKE_MSVC_RUNTIME_LIBRARY)
  if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
    _rarftp_cache(${_var} STRING "${${_var}}")
  endif()
endforeach()
file(CONFIGURE OUTPUT "${_cache}" CONTENT "${_cache_text}" @ONLY)

# ExternalProject_Add with the common settings. Every dependency is built in
# Release, whatever the configuration of this build.
function(_rarftp_dependency name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;SHA256;SOURCE_SUBDIR" "ARGS;DEPENDS;BYPRODUCTS;PATCH_COMMAND")
  if(NOT arg_PATCH_COMMAND)
    set(arg_PATCH_COMMAND "")
  endif()
  ExternalProject_Add(${name}
    URL "${arg_URL}"
    URL_HASH SHA256=${arg_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${CMAKE_BINARY_DIR}/_ext/${name}"
    SOURCE_SUBDIR "${arg_SOURCE_SUBDIR}"
    PATCH_COMMAND ${arg_PATCH_COMMAND}
    CMAKE_ARGS -C "${_cache}" ${arg_ARGS}
    # Built and installed in one step: the libraries this build links appear
    # in the install directory, and must be byproducts of the step that writes
    # them, or Ninja would not relink after they change.
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target install
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${arg_BYPRODUCTS}
    DEPENDS ${arg_DEPENDS}
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON
    EXCLUDE_FROM_ALL ON)
endfunction()

# --- zlib (zlib license) -----------------------------------------------------
# Prefixed symbols (z_inflate...): no clash with a system zlib that a system
# libcurl may load.
_rarftp_dependency(rarftp_zlib
  URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz
  SHA256 d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
  ARGS -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF -DZLIB_PREFIX=ON
  BYPRODUCTS "${_zlib_lib}")

# --- bzip2 (bzip2 license, BSD-like) -----------------------------------------
_rarftp_dependency(rarftp_bzip2
  URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
  SHA256 ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
  PATCH_COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_LIST_DIR}/bzip2/CMakeLists.txt" <SOURCE_DIR>/
  BYPRODUCTS "${_bzip2_lib}")

# --- xz / liblzma (0BSD) -----------------------------------------------------
_rarftp_dependency(rarftp_xz
  URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.xz
  SHA256 4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b
  ARGS -DXZ_THREADS=no -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF
       -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF
  BYPRODUCTS "${_lzma_lib}")

# --- zstd (BSD-3-Clause) -----------------------------------------------------
_rarftp_dependency(rarftp_zstd
  URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  SOURCE_SUBDIR build/cmake
  ARGS -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
       -DZSTD_BUILD_CONTRIB=OFF -DZSTD_LEGACY_SUPPORT=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF
  BYPRODUCTS "${_zstd_lib}")

# --- LZ4 (BSD-2-Clause for the library) --------------------------------------
_rarftp_dependency(rarftp_lz4
  URL https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz
  SHA256 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
  SOURCE_SUBDIR build/cmake
  ARGS -DBUILD_STATIC_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
  BYPRODUCTS "${_lz4_lib}")

set(_archive_depends rarftp_zlib rarftp_bzip2 rarftp_xz rarftp_zstd rarftp_lz4)
set(_archive_crypto_args -DENABLE_MBEDTLS=OFF)

# --- mbed TLS (Apache-2.0), only its crypto library is used ------------------
if(_use_mbedtls)
  _rarftp_dependency(rarftp_mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    SHA256 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    ARGS -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF -DGEN_FILES=OFF
         -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
    BYPRODUCTS "${_mbedcrypto_lib}" "${_mbedx509_lib}" "${_mbedtls_lib}")
  list(APPEND _archive_depends rarftp_mbedtls)
  set(_archive_crypto_args
    -DENABLE_MBEDTLS=ON
    "-DMBEDTLS_INCLUDE_DIRS=${_ad}/include"
    "-DMBEDTLS_LIBRARY=${_mbedtls_lib}"
    "-DMBEDX509_LIBRARY=${_mbedx509_lib}"
    "-DMBEDCRYPTO_LIBRARY=${_mbedcrypto_lib}")
endif()

# --- libarchive (BSD-2-Clause) -----------------------------------------------
# Only the readers are used. Everything optional that the ZIP, 7z and tar readers
# do not need is off, so nothing is picked up from the system by accident.
_rarftp_dependency(rarftp_libarchive
  URL https://github.com/libarchive/libarchive/releases/download/v3.8.9/libarchive-3.8.9.tar.xz
  SHA256 888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde
  PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/libarchive-patches.cmake"
  ARGS
    -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF
    -DENABLE_INSTALL=ON -DENABLE_WERROR=OFF -DENABLE_COVERAGE=OFF
    -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=ON
    -DENABLE_LZO=OFF -DENABLE_LIBB2=OFF -DENABLE_MD=OFF
    -DENABLE_OPENSSL=OFF -DENABLE_NETTLE=OFF -DENABLE_CNG=ON
    -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_WIN32_XMLLITE=OFF -DENABLE_BSDXML=OFF
    -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF
    -DPOSIX_REGEX_LIB=NONE  # Only bsdtar uses regular expressions (Windows has none: it would want libgcc).
    -DENABLE_XATTR=OFF -DENABLE_ACL=OFF -DENABLE_ICONV=ON
    "-DZLIB_INCLUDE_DIR=${_ad}/include" "-DZLIB_LIBRARY=${_zlib_lib}"
    "-DBZIP2_INCLUDE_DIR=${_ad}/include" "-DBZIP2_LIBRARY_RELEASE=${_bzip2_lib}"
    "-DLIBLZMA_INCLUDE_DIR=${_ad}/include" "-DLIBLZMA_LIBRARY_RELEASE=${_lzma_lib}"
    "-DZSTD_INCLUDE_DIR=${_ad}/include" "-DZSTD_LIBRARY=${_zstd_lib}"
    "-DLZ4_INCLUDE_DIR=${_ad}/include" "-DLZ4_LIBRARY=${_lz4_lib}"
    ${_archive_crypto_args}
  DEPENDS ${_archive_depends}
  BYPRODUCTS "${_archive_lib}")
# A change to the fixes extracts the sources again before applying them.
ExternalProject_Add_StepDependencies(rarftp_libarchive download "${CMAKE_CURRENT_LIST_DIR}/libarchive-patches.cmake")

# Link order matters for static libraries: libarchive first, then what it uses.
set(_archive_link "${_zstd_lib}" "${_lz4_lib}" "${_lzma_lib}" "${_bzip2_lib}" "${_zlib_lib}")
if(_use_mbedtls)
  list(APPEND _archive_link "${_mbedcrypto_lib}")
endif()
if(APPLE)
  list(APPEND _archive_link iconv)
elseif(WIN32)
  list(APPEND _archive_link bcrypt)
endif()

add_library(rarftp::libarchive STATIC IMPORTED GLOBAL)
set_target_properties(rarftp::libarchive PROPERTIES
  IMPORTED_LOCATION "${_archive_lib}"
  INTERFACE_INCLUDE_DIRECTORIES "${_ad}/include"
  INTERFACE_COMPILE_DEFINITIONS LIBARCHIVE_STATIC
  INTERFACE_LINK_LIBRARIES "${_archive_link}")
add_dependencies(rarftp::libarchive rarftp_libarchive)
