# Builds 7-Zip's 7z decoder (static library) from the LZMA SDK in
# ${PROJECT_SOURCE_DIR}/lzmasdk. The sources are NOT part of this repository;
# see README.md for how to fetch them. LZMA SDK is placed in the public domain
# by Igor Pavlov.
#
# Only what reading 7z needs: the C++ archive code (headers and folder
# decoding, Archive/7z), the decoders the SDK has (LZMA, LZMA2, PPMd, the
# branch filters, BCJ2, Delta, Copy, byte swap) and 7zAES, decode-only. The
# blocks are decoded on the calling thread, but the SDK is built with thread
# support: it guards its global state (7zAES's key cache) with locks.

set(LZMASDK_SOURCE_DIR "${PROJECT_SOURCE_DIR}/lzmasdk")

if(NOT EXISTS "${LZMASDK_SOURCE_DIR}/CPP/7zip/Archive/7z/7zIn.cpp")
  message(FATAL_ERROR
    "LZMA SDK not found in ${LZMASDK_SOURCE_DIR}\n"
    "Download and extract it (it is not redistributed with this project); with 7-Zip or bsdtar:\n"
    "  curl -LO https://github.com/ip7z/7zip/releases/download/26.03/lzma2603.7z\n"
    "  mkdir lzmasdk\n"
    "  tar -xf lzma2603.7z -C lzmasdk      # or: 7z x -olzmasdk lzma2603.7z\n")
endif()

set(_lzmasdk_c_names
  7zCrc 7zCrcOpt 7zStream Aes AesOpt Alloc Bcj2 Bra Bra86 BraIA64 CpuArch Delta
  Lzma2Dec Lzma2DecMt LzmaDec MtDec Ppmd7 Ppmd7Dec Sha256 Sha256Opt SwapBytes Threads)
set(_lzmasdk_cpp_names
  Common/IntToString Common/MyString Common/MyVector Common/StringConvert Common/UTFConvert
  Windows/PropVariant Windows/Synchronization Windows/System
  7zip/Common/CreateCoder 7zip/Common/CWrappers 7zip/Common/FilterCoder 7zip/Common/InBuffer
  7zip/Common/LimitedStreams 7zip/Common/MethodId 7zip/Common/StreamBinder 7zip/Common/StreamObjects
  7zip/Common/StreamUtils 7zip/Common/VirtThread
  7zip/Archive/Common/CoderMixer2
  7zip/Archive/7z/7zDecode 7zip/Archive/7z/7zIn
  7zip/Compress/Bcj2Coder 7zip/Compress/BcjCoder 7zip/Compress/BranchMisc 7zip/Compress/CopyCoder
  7zip/Compress/Lzma2Decoder 7zip/Compress/LzmaDecoder 7zip/Compress/PpmdDecoder
  7zip/Crypto/7zAes 7zip/Crypto/MyAes)
if(NOT WIN32)
  list(APPEND _lzmasdk_cpp_names Common/MyWindows)  # BSTR and the other Windows types.
endif()

set(_lzmasdk_sources "")
foreach(_name IN LISTS _lzmasdk_c_names)
  list(APPEND _lzmasdk_sources "${LZMASDK_SOURCE_DIR}/C/${_name}.c")
endforeach()
foreach(_name IN LISTS _lzmasdk_cpp_names)
  list(APPEND _lzmasdk_sources "${LZMASDK_SOURCE_DIR}/CPP/${_name}.cpp")
endforeach()
# The codecs register themselves from static constructors, which a static
# library would leave out (nothing refers to them): this one file includes
# them all, and the 7z reader calls a function of it.
list(APPEND _lzmasdk_sources "${PROJECT_SOURCE_DIR}/src/sevenzip_codecs.cpp")

# Always a static library, like UnRAR.
add_library(lzmasdk STATIC ${_lzmasdk_sources})
add_library(rarftp::lzmasdk ALIAS lzmasdk)

# SYSTEM keeps the SDK's headers out of our warnings.
target_include_directories(lzmasdk SYSTEM PUBLIC "${LZMASDK_SOURCE_DIR}/CPP")
# Also needed by the code including the SDK's headers (same declarations).
target_compile_definitions(lzmasdk PUBLIC Z7_EXTRACT_ONLY)
set_target_properties(lzmasdk PROPERTIES
  CXX_STANDARD 17
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_definitions(lzmasdk PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(lzmasdk PRIVATE /W0 /EHsc)
else()
  # As 7-Zip's own makefiles (7zip_gcc.mak).
  target_compile_definitions(lzmasdk PRIVATE _REENTRANT _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
  # Third-party code: keep the build quiet.
  target_compile_options(lzmasdk PRIVATE -w)
endif()

find_package(Threads REQUIRED)
target_link_libraries(lzmasdk PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(lzmasdk PUBLIC oleaut32)  # SysAllocString & co.
endif()

# LZMA SDK version, for --version.
file(STRINGS "${LZMASDK_SOURCE_DIR}/C/7zVersion.h" _lzmasdk_version_line REGEX "#define MY_VERSION_NUMBERS ")
string(REGEX MATCH "\"([^\"]+)\"" _lzmasdk_version_match "${_lzmasdk_version_line}")
set(LZMASDK_VERSION_STRING "${CMAKE_MATCH_1}")
message(STATUS "rarftp: LZMA SDK ${LZMASDK_VERSION_STRING} from ${LZMASDK_SOURCE_DIR}")
