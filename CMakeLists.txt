cmake_minimum_required(VERSION 3.21)

# Extracted files get the extraction time (rebuilds when a pinned URL changes).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

project(rarftp
  VERSION 1.0.0
  DESCRIPTION "Uploads the contents of a RAR archive to an FTP server without extracting it to disk"
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

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(UnRAR)
include(Dependencies)

# Everything but main() lives in a library so the tests can link it.
add_library(rarftp_core STATIC
  src/ftp_client.cpp
  src/logger.cpp
  src/options.cpp
  src/plan.cpp
  src/progress.cpp
  src/rar_archive.cpp
  src/transfer.cpp
  src/ui_plain.cpp
  src/ui_tui.cpp
  src/util/remote_path.cpp
  src/util/terminal.cpp
  src/util/text.cpp)
target_include_directories(rarftp_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(rarftp_core PRIVATE RARFTP_VERSION="${PROJECT_VERSION}")
target_link_libraries(rarftp_core
  PUBLIC fmt::fmt
  PRIVATE unrar::unrar CURL::libcurl CLI11::CLI11 ftxui::component ftxui::dom ftxui::screen Threads::Threads)

add_executable(rarftp src/main.cpp)
target_link_libraries(rarftp PRIVATE rarftp_core)

foreach(_target rarftp_core rarftp)
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
