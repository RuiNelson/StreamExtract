# Minimal static build of bzip2 1.0.8 (which has no CMake build of its own),
# copied into the bzip2 sources by cmake/LibArchive.cmake.

cmake_minimum_required(VERSION 3.21)
project(bzip2 VERSION 1.0.8 LANGUAGES C)

add_library(bz2 STATIC
  blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c)
# On Windows bzlib.h declares the API as pointers to DLL functions unless
# BZ_EXPORT is defined.
target_compile_definitions(bz2 PRIVATE BZ_EXPORT $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

# The installed header always declares plain functions, as a static library
# needs, without its users having to define anything.
file(READ "${PROJECT_SOURCE_DIR}/bzlib.h" _bzlib_h)
string(REPLACE "#   ifdef BZ_EXPORT" "#   if 1 /* static library */" _bzlib_h "${_bzlib_h}")
file(WRITE "${PROJECT_BINARY_DIR}/include/bzlib.h" "${_bzlib_h}")

include(GNUInstallDirs)
install(TARGETS bz2 ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${PROJECT_BINARY_DIR}/include/bzlib.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
