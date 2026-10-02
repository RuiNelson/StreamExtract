cmake_minimum_required(VERSION 3.21)

# Extracted files get the extraction time (rebuilds when a pinned URL changes).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

project(rarftp
  VERSION 2.2.0
  DESCRIPTION "Uploads archives and exFAT volume images to an FTP server without extracting them to disk"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RARFTP_BUNDLED_CURL "Build libcurl from source instead of using the system one" OFF)
option(RARFTP_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RARFTP_BUILD_LIBRARY "Build the rarftpcore shared library used by rarftp-gui" OFF)

if(RARFTP_BUILD_LIBRARY)
  # Everything that ends up inside the shared library must be position independent
  # (UnRAR already is); this has to be set before the dependencies are added.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(UnRAR)
include(LzmaSdk)
include(FatFs)
include(Dependencies)
include(LibArchive)

# The engine: everything but the command line and its front-ends. It is linked
# into the rarftp executable, the tests and the rarftpcore shared library.
add_library(rarftp_core STATIC
  src/app_version.cpp
  src/archive.cpp
  src/exfat_archive.cpp
  src/ftp_client.cpp
  src/libarchive_reader.cpp
  src/logger.cpp
  src/plan.cpp
  src/progress.cpp
  src/rar_archive.cpp
  src/sevenzip_archive.cpp
  src/sevenzip_methods.cpp
  src/summary.cpp
  src/transfer.cpp
  src/util/json.cpp
  src/util/remote_path.cpp
  src/util/terminal.cpp
  src/util/text.cpp)
target_include_directories(rarftp_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(rarftp_core PRIVATE RARFTP_VERSION="${PROJECT_VERSION}")
target_link_libraries(rarftp_core
  PUBLIC fmt::fmt
  PRIVATE unrar::unrar rarftp::lzmasdk rarftp::fatfs rarftp::libarchive CURL::libcurl Threads::Threads)
if(APPLE)
  target_link_libraries(rarftp_core PRIVATE "-framework CoreFoundation")  # Unicode normalization of names.
endif()

add_executable(rarftp
  src/main.cpp
  src/options.cpp
  src/ui_plain.cpp
  src/ui_tui.cpp)
target_link_libraries(rarftp PRIVATE rarftp_core CLI11::CLI11 ftxui::component ftxui::dom ftxui::screen Threads::Threads)

set(_rarftp_targets rarftp_core rarftp)

if(RARFTP_BUILD_LIBRARY)
  # C API for rarftp-gui. UnRAR, libarchive, libcurl and fmt are linked
  # statically into it and only the rarftp_* functions are exported.
  add_library(rarftpcore SHARED src/capi/capi.cpp src/capi/job.cpp)
  target_link_libraries(rarftpcore PRIVATE rarftp_core Threads::Threads)
  target_compile_definitions(rarftpcore PRIVATE RARFTP_BUILDING_LIBRARY)
  set_target_properties(rarftpcore PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  if(APPLE)
    target_link_options(rarftpcore PRIVATE "LINKER:-exported_symbol,_rarftp_*")
  elseif(NOT WIN32)
    # ELF: a version script, since visibility only covers our own objects.
    set(_rarftp_version_script "${CMAKE_BINARY_DIR}/rarftpcore.map")
    file(WRITE "${_rarftp_version_script}" "{ global: rarftp_*; local: *; };\n")
    target_link_options(rarftpcore PRIVATE
      "LINKER:--version-script=${_rarftp_version_script}" "LINKER:--exclude-libs,ALL")
    set_property(TARGET rarftpcore APPEND PROPERTY LINK_DEPENDS "${_rarftp_version_script}")
  endif()
  list(APPEND _rarftp_targets rarftpcore)
endif()

foreach(_target IN LISTS _rarftp_targets)
  if(MSVC)
    target_compile_options(${_target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${_target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${_target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
endforeach()

install(TARGETS rarftp RUNTIME DESTINATION bin)

if(RARFTP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
