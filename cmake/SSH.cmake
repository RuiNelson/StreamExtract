# Bundled static OpenSSL for FTPS and libssh2 for SFTP.
include(ExternalProject)
find_package(Perl REQUIRED)
if(MSVC)
  find_program(_ssh_make NAMES nmake REQUIRED)
  set(_ssh_crypto_name libcrypto.lib)
  set(_ssh_ssl_name libssl.lib)
  set(_ssh_library_name libssh2.lib)
else()
  find_program(_ssh_make NAMES gmake make REQUIRED)
  set(_ssh_crypto_name libcrypto.a)
  set(_ssh_ssl_name libssl.a)
  set(_ssh_library_name libssh2.a)
endif()

set(_ssh_deps "${CMAKE_BINARY_DIR}/ssh-deps")
file(MAKE_DIRECTORY "${_ssh_deps}/include")
set(_openssl_url https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz)
set(_openssl_hash 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)

if(APPLE)
  set(_ssh_arches "${CMAKE_OSX_ARCHITECTURES}")
  if(NOT _ssh_arches)
    set(_ssh_arches "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
else()
  set(_ssh_arches "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(_ssh_crypto_libraries "")
set(_ssh_ssl_libraries "")
set(_ssh_openssl_projects "")
foreach(_arch IN LISTS _ssh_arches)
  if(APPLE AND _arch STREQUAL "arm64")
    set(_openssl_target darwin64-arm64-cc)
  elseif(APPLE AND _arch STREQUAL "x86_64")
    set(_openssl_target darwin64-x86_64-cc)
  elseif(MSVC AND _arch MATCHES "^(ARM64|arm64|aarch64)$")
    set(_openssl_target VC-WIN64-ARM)
  elseif(MSVC)
    set(_openssl_target VC-WIN64A)
  elseif(_arch MATCHES "^(aarch64|arm64)$")
    set(_openssl_target linux-aarch64)
  elseif(_arch MATCHES "^(x86_64|AMD64|amd64)$")
    set(_openssl_target linux-x86_64)
  else()
    message(FATAL_ERROR "Unsupported SSH crypto build platform: ${CMAKE_SYSTEM_NAME}/${_arch}")
  endif()
  set(_openssl_name "streamextract_openssl_${_arch}")
  set(_openssl_prefix "${_ssh_deps}/${_arch}")
  set(_openssl_flags "${CMAKE_C_FLAGS}")
  if(NOT MSVC)
    string(APPEND _openssl_flags " -fPIC")
  endif()
  if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    string(APPEND _openssl_flags " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  if(APPLE AND CMAKE_OSX_SYSROOT)
    string(APPEND _openssl_flags " -isysroot ${CMAKE_OSX_SYSROOT}")
  elseif(CMAKE_SYSROOT)
    string(APPEND _openssl_flags " --sysroot=${CMAKE_SYSROOT}")
  endif()
  if(MSVC)
    set(_openssl_build "${_ssh_make}")
  else()
    set(_openssl_build "${_ssh_make}" -j8)
  endif()
  ExternalProject_Add(${_openssl_name}
    URL "${_openssl_url}" URL_HASH SHA256=${_openssl_hash}
    PREFIX "${CMAKE_BINARY_DIR}/_ext/${_openssl_name}"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "CFLAGS=${_openssl_flags}"
      "${PERL_EXECUTABLE}" <SOURCE_DIR>/Configure ${_openssl_target}
      no-shared no-asm no-tests no-apps no-docs no-module no-dso
      "--prefix=${_openssl_prefix}" --libdir=lib
    BUILD_COMMAND ${_openssl_build} build_libs
      COMMAND "${_ssh_make}" install_dev
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${_openssl_prefix}/lib/${_ssh_crypto_name}" "${_openssl_prefix}/lib/${_ssh_ssl_name}"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_OUTPUT_ON_FAILURE ON EXCLUDE_FROM_ALL ON)
  list(APPEND _ssh_openssl_projects ${_openssl_name})
  list(APPEND _ssh_crypto_libraries "${_openssl_prefix}/lib/${_ssh_crypto_name}")
  list(APPEND _ssh_ssl_libraries "${_openssl_prefix}/lib/${_ssh_ssl_name}")
  if(NOT _ssh_openssl_headers)
    set(_ssh_openssl_headers "${_openssl_prefix}/include")
  endif()
endforeach()
list(LENGTH _ssh_crypto_libraries _ssh_arch_count)
if(_ssh_arch_count GREATER 1)
  find_program(_ssh_lipo lipo REQUIRED)
  set(_ssh_crypto_library "${_ssh_deps}/lib/${_ssh_crypto_name}")
  add_custom_command(OUTPUT "${_ssh_crypto_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ssh_deps}/lib"
    COMMAND "${_ssh_lipo}" -create ${_ssh_crypto_libraries} -output "${_ssh_crypto_library}"
    DEPENDS ${_ssh_openssl_projects} ${_ssh_crypto_libraries} VERBATIM)
  set(_ssh_ssl_library "${_ssh_deps}/lib/${_ssh_ssl_name}")
  add_custom_command(OUTPUT "${_ssh_ssl_library}"
    COMMAND "${_ssh_lipo}" -create ${_ssh_ssl_libraries} -output "${_ssh_ssl_library}"
    DEPENDS ${_ssh_openssl_projects} ${_ssh_ssl_libraries} VERBATIM)
  add_custom_target(streamextract_ssh_crypto_build DEPENDS "${_ssh_crypto_library}" "${_ssh_ssl_library}")
else()
  list(GET _ssh_crypto_libraries 0 _ssh_crypto_library)
  list(GET _ssh_ssl_libraries 0 _ssh_ssl_library)
  add_custom_target(streamextract_ssh_crypto_build DEPENDS ${_ssh_openssl_projects})
endif()
add_library(streamextract::ssh_crypto STATIC IMPORTED GLOBAL)
set_target_properties(streamextract::ssh_crypto PROPERTIES IMPORTED_LOCATION "${_ssh_crypto_library}")
add_dependencies(streamextract::ssh_crypto streamextract_ssh_crypto_build)
if(WIN32)
  set_property(TARGET streamextract::ssh_crypto PROPERTY INTERFACE_LINK_LIBRARIES "crypt32;bcrypt;ws2_32")
else()
  set_property(TARGET streamextract::ssh_crypto PROPERTY INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
endif()

add_library(streamextract::tls STATIC IMPORTED GLOBAL)
set_target_properties(streamextract::tls PROPERTIES IMPORTED_LOCATION "${_ssh_ssl_library}"
  INTERFACE_LINK_LIBRARIES streamextract::ssh_crypto)
add_dependencies(streamextract::tls streamextract_ssh_crypto_build)
# Targets let curl configure before OpenSSL has been built; libarchive needs the
# installed filenames because it links dependency checks in a separate project.
set(OPENSSL_INCLUDE_DIR "${_ssh_openssl_headers}" CACHE PATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY streamextract::ssh_crypto CACHE STRING "" FORCE)
set(OPENSSL_SSL_LIBRARY streamextract::tls CACHE STRING "" FORCE)
set(OPENSSL_USE_STATIC_LIBS ON)
# FindOpenSSL reads version headers at configure time. The pinned source version
# is known even before ExternalProject generates those headers.
set(OPENSSL_VERSION "3.5.9")
set(HAVE_AWSLC OFF CACHE INTERNAL "")
set(HAVE_BORINGSSL OFF CACHE INTERNAL "")
set(HAVE_LIBRESSL OFF CACHE INTERNAL "")
set(HAVE_DES_ECB_ENCRYPT ON CACHE INTERNAL "")
set(HAVE_SSL_SET0_WBIO ON CACHE INTERNAL "")
file(MAKE_DIRECTORY "${_ssh_openssl_headers}")
set_property(TARGET streamextract::ssh_crypto PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_ssh_openssl_headers}")
set_property(TARGET streamextract::tls PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_ssh_openssl_headers}")
add_library(OpenSSL::Crypto ALIAS streamextract::ssh_crypto)
add_library(OpenSSL::SSL ALIAS streamextract::tls)

# Populate headers at configure time so curl's finder can read libssh2's version.
# Build it separately after OpenSSL has generated and installed its headers.
FetchContent_Declare(libssh2
  URL https://libssh2.org/download/libssh2-1.11.1.tar.gz
  URL_HASH SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
  SOURCE_SUBDIR streamextract-download-only EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libssh2)
set(_ssh_cache "${CMAKE_BINARY_DIR}/ssh-deps-cache.cmake")
set(_ssh_cache_text "")
foreach(_var IN ITEMS CMAKE_C_COMPILER CMAKE_C_COMPILER_LAUNCHER CMAKE_C_FLAGS CMAKE_MAKE_PROGRAM
                      CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET
                      CMAKE_OSX_SYSROOT CMAKE_MSVC_RUNTIME_LIBRARY)
  if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
    string(APPEND _ssh_cache_text "set(${_var} [==[${${_var}}]==] CACHE STRING \"\" FORCE)\n")
  endif()
endforeach()
file(WRITE "${_ssh_cache}" "${_ssh_cache_text}")
ExternalProject_Add(streamextract_libssh2
  SOURCE_DIR "${libssh2_SOURCE_DIR}"
  PREFIX "${CMAKE_BINARY_DIR}/_ext/streamextract_libssh2"
  CMAKE_ARGS -C "${_ssh_cache}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${_ssh_deps}
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF
    -DCRYPTO_BACKEND=OpenSSL -DOPENSSL_USE_STATIC_LIBS=ON
    -DOPENSSL_INCLUDE_DIR=${_ssh_openssl_headers} -DOPENSSL_CRYPTO_LIBRARY=${_ssh_crypto_library}
    -DENABLE_ZLIB_COMPRESSION=OFF -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON
    -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target install --parallel 8
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${_ssh_deps}/lib/${_ssh_library_name}"
  DEPENDS streamextract_ssh_crypto_build
  LOG_CONFIGURE ON LOG_BUILD ON LOG_OUTPUT_ON_FAILURE ON EXCLUDE_FROM_ALL ON)
add_library(streamextract::ssh STATIC IMPORTED GLOBAL)
set_target_properties(streamextract::ssh PROPERTIES
  IMPORTED_LOCATION "${_ssh_deps}/lib/${_ssh_library_name}"
  INTERFACE_INCLUDE_DIRECTORIES "${libssh2_SOURCE_DIR}/include"
  INTERFACE_LINK_LIBRARIES streamextract::ssh_crypto)
add_dependencies(streamextract::ssh streamextract_libssh2)
set(LIBSSH2_INCLUDE_DIR "${libssh2_SOURCE_DIR}/include")
set(LIBSSH2_LIBRARY streamextract::ssh)
set(LIBSSH2_USE_STATIC_LIBS ON)
