# Builds the UnRAR library (DLL API, statically linked) from the sources in
# ${PROJECT_SOURCE_DIR}/unrarsrc. The sources are NOT part of this repository
# because of their license; see README.md for how to fetch them.
#
# UnRAR source code may be used in any software to handle RAR archives without
# limitations free of charge, but cannot be used to develop RAR (WinRAR)
# compatible archiver and to re-create RAR compression algorithm, which is
# proprietary.

set(UNRAR_SOURCE_DIR "${PROJECT_SOURCE_DIR}/unrarsrc")
# Accept both the flattened layout (unrarsrc/dll.hpp) and the tarball layout
# extracted as-is (unrarsrc/unrar/dll.hpp).
if(NOT EXISTS "${UNRAR_SOURCE_DIR}/dll.hpp" AND EXISTS "${UNRAR_SOURCE_DIR}/unrar/dll.hpp")
  set(UNRAR_SOURCE_DIR "${UNRAR_SOURCE_DIR}/unrar")
endif()

if(NOT EXISTS "${UNRAR_SOURCE_DIR}/dll.hpp")
  message(FATAL_ERROR
    "UnRAR sources not found in ${PROJECT_SOURCE_DIR}/unrarsrc\n"
    "Download and extract them (they are not redistributed with this project):\n"
    "  curl -LO https://www.rarlab.com/rar/unrarsrc-7.3.1.tar.gz\n"
    "  mkdir unrarsrc\n"
    "  tar -xzf unrarsrc-7.3.1.tar.gz -C unrarsrc --strip-components=1\n")
endif()

# Same object list as the 'lib' target of the official makefile.
set(_unrar_names
  rar strlist strfn pathfn smallfn global file filefn filcreat archive arcread
  unicode system crypt crc rawread encname resource match timefn rdwrfn consio
  options errhnd rarvm secpassword rijndael getbits sha1 sha256 blake2s hash
  extinfo extract volume list find unpack headers threadpool rs16 cmddata ui
  largepage filestr scantree dll qopen)
if(WIN32)
  # Extra units listed in UnRARDll.vcxproj.
  list(APPEND _unrar_names isnt motw rs)
endif()

set(_unrar_sources "")
foreach(_name IN LISTS _unrar_names)
  list(APPEND _unrar_sources "${UNRAR_SOURCE_DIR}/${_name}.cpp")
endforeach()

# Always a static library, whatever BUILD_SHARED_LIBS says: UnRAR is linked
# into the streamextract executable, which never loads an UnRAR shared library.
add_library(unrar STATIC ${_unrar_sources})
add_library(unrar::unrar ALIAS unrar)

# Our code only includes dll.hpp; SYSTEM keeps UnRAR's headers out of our
# warnings.
target_include_directories(unrar SYSTEM INTERFACE "${UNRAR_SOURCE_DIR}")
target_compile_definitions(unrar PRIVATE RARDLL UNRAR SILENT RAR_SMP
  _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
set_target_properties(unrar PROPERTIES
  CXX_STANDARD 17
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_definitions(unrar PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(unrar PRIVATE /W0 /EHsc)
else()
  # Third-party code: keep the build quiet.
  target_compile_options(unrar PRIVATE -w)
endif()

find_package(Threads REQUIRED)
target_link_libraries(unrar PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(unrar PUBLIC shlwapi powrprof psapi wbemuuid ole32 oleaut32 advapi32 shell32)
endif()

# UnRAR version, for --version.
file(STRINGS "${UNRAR_SOURCE_DIR}/version.hpp" _unrar_ver_lines REGEX "#define RARVER_(MAJOR|MINOR|BETA)")
foreach(_line IN LISTS _unrar_ver_lines)
  if(_line MATCHES "RARVER_(MAJOR|MINOR|BETA)[ \t]+([0-9]+)")
    set(_unrar_ver_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()
set(UNRAR_VERSION_STRING "${_unrar_ver_MAJOR}.${_unrar_ver_MINOR}")
if(_unrar_ver_BETA AND NOT _unrar_ver_BETA EQUAL 0)
  string(APPEND UNRAR_VERSION_STRING " beta ${_unrar_ver_BETA}")
endif()
message(STATUS "streamextract: UnRAR ${UNRAR_VERSION_STRING} from ${UNRAR_SOURCE_DIR}")
