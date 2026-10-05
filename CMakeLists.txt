cmake_minimum_required(VERSION 3.21)

# Extracted files get the extraction time (rebuilds when a pinned URL changes).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

project(StreamExtract
  VERSION 2.4.1
  DESCRIPTION "Uploads archives and exFAT volume images to an FTP server without extracting them to disk"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(STREAMEXTRACT_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(STREAMEXTRACT_BUILD_LIBRARY "Build the streamextractcore shared library used by StreamExtract" OFF)

if(STREAMEXTRACT_BUILD_LIBRARY)
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
# into the sext executable, the tests and the streamextractcore shared library.
add_library(streamextract_core STATIC
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
target_include_directories(streamextract_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(streamextract_core PRIVATE STREAMEXTRACT_VERSION="${PROJECT_VERSION}")
target_link_libraries(streamextract_core
  PUBLIC fmt::fmt
  PRIVATE unrar::unrar streamextract::lzmasdk streamextract::fatfs streamextract::libarchive CURL::libcurl_static Threads::Threads)
if(APPLE)
  target_link_libraries(streamextract_core PRIVATE "-framework CoreFoundation")  # Unicode normalization of names.
endif()

add_executable(sext
  src/main.cpp
  src/options.cpp
  src/ui_plain.cpp
  src/ui_tui.cpp)
target_link_libraries(sext PRIVATE streamextract_core CLI11::CLI11 ftxui::component ftxui::dom ftxui::screen Threads::Threads)

set(_streamextract_targets streamextract_core sext)

if(STREAMEXTRACT_BUILD_LIBRARY)
  # C API for StreamExtract. UnRAR, libarchive, libcurl and fmt are linked
  # statically into it and only the streamextract_* functions are exported.
  add_library(streamextractcore SHARED src/capi/capi.cpp src/capi/job.cpp)
  target_link_libraries(streamextractcore PRIVATE streamextract_core Threads::Threads)
  target_compile_definitions(streamextractcore PRIVATE STREAMEXTRACT_BUILDING_LIBRARY)
  set_target_properties(streamextractcore PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  if(APPLE)
    target_link_options(streamextractcore PRIVATE "LINKER:-exported_symbol,_streamextract_*")
  elseif(NOT WIN32)
    # ELF: a version script, since visibility only covers our own objects.
    set(_streamextract_version_script "${CMAKE_BINARY_DIR}/streamextractcore.map")
    file(WRITE "${_streamextract_version_script}" "{ global: streamextract_*; local: *; };\n")
    target_link_options(streamextractcore PRIVATE
      "LINKER:--version-script=${_streamextract_version_script}" "LINKER:--exclude-libs,ALL")
    set_property(TARGET streamextractcore APPEND PROPERTY LINK_DEPENDS "${_streamextract_version_script}")
  endif()
  list(APPEND _streamextract_targets streamextractcore)
endif()

foreach(_target IN LISTS _streamextract_targets)
  if(MSVC)
    target_compile_options(${_target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${_target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${_target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
endforeach()

install(TARGETS sext RUNTIME DESTINATION bin)

if(STREAMEXTRACT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
