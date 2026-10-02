# FatFs R0.16, downloaded by hand into fatfs/ (see README), like UnRAR and
# the LZMA SDK. Build a private copy with our read-only configuration and the
# author's official patches, leaving the downloaded sources untouched.
set(FATFS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/fatfs/source")
if(NOT EXISTS "${FATFS_SOURCE_DIR}/ff.c" OR NOT EXISTS "${FATFS_SOURCE_DIR}/ffunicode.c")
  message(FATAL_ERROR
    "FatFs not found in ${FATFS_SOURCE_DIR}\n"
    "Download and extract FatFs R0.16 (not redistributed with this project):\n"
    "  curl -LO https://elm-chan.org/fsw/ff/arc/ff16.zip\n"
    "  mkdir fatfs\n"
    "  unzip ff16.zip -d fatfs       # or: 7z x -ofatfs ff16.zip\n")
endif()

set(_fatfs_dir "${CMAKE_CURRENT_BINARY_DIR}/fatfs")
file(MAKE_DIRECTORY "${_fatfs_dir}")
foreach(_file ff.c ff.h diskio.h ffunicode.c)
  configure_file("${FATFS_SOURCE_DIR}/${_file}" "${_fatfs_dir}/${_file}" COPYONLY)
endforeach()
configure_file("${PROJECT_SOURCE_DIR}/cmake/fatfs/ffconf.h" "${_fatfs_dir}/ffconf.h" COPYONLY)
include("${PROJECT_SOURCE_DIR}/cmake/fatfs/patches.cmake")
_fatfs_patch("${_fatfs_dir}/ff.c")

add_library(fatfs STATIC "${_fatfs_dir}/ff.c" "${_fatfs_dir}/ffunicode.c")
add_library(rarftp::fatfs ALIAS fatfs)
target_include_directories(fatfs SYSTEM PUBLIC "${_fatfs_dir}")
set_target_properties(fatfs PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_definitions(fatfs PUBLIC NOMINMAX PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(fatfs PRIVATE /W0)
else()
  target_compile_options(fatfs PRIVATE -w)
endif()
message(STATUS "rarftp: FatFs R0.16 (official patches 1 + 2) from ${FATFS_SOURCE_DIR}")
