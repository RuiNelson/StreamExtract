include(FetchContent)

# --- doctest (MIT) -----------------------------------------------------------
FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
  URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
  EXCLUDE_FROM_ALL)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

# Real exFAT volume formatted with hdiutil; compressed only for repository size.
# CMake's archive reader works on all platforms without a test-time formatter.
set(_test_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exfat.zip")
file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exfat.zip" DESTINATION "${_test_fixtures}")

add_executable(streamextract_tests
  test_main.cpp
  test_options.cpp
  ../src/options.cpp
  test_archive.cpp
  test_exfat.cpp
  test_json.cpp
  test_pipe.cpp
  test_plan.cpp
  test_progress.cpp
  test_remote_path.cpp
  test_summary.cpp
  test_text.cpp)
target_link_libraries(streamextract_tests PRIVATE streamextract_core doctest::doctest CLI11::CLI11)
target_compile_definitions(streamextract_tests PRIVATE STREAMEXTRACT_TEST_FIXTURES="${_test_fixtures}")

add_test(NAME unit COMMAND streamextract_tests)

# CLI FTP/FTPS tests run without the GUI library, Docker or external archivers.
find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME ftps COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/ftps.py" --sext $<TARGET_FILE:sext>)
  set_tests_properties(ftps PROPERTIES TIMEOUT 60 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

if(TARGET streamextractcore)
  # C-ABI smoke test: only the public header and the shared library, exactly
  # what StreamExtract sees.
  add_executable(streamextractcore_tests test_capi.cpp)
  target_include_directories(streamextractcore_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(streamextractcore_tests PRIVATE streamextractcore doctest::doctest)
  target_compile_definitions(streamextractcore_tests PRIVATE STREAMEXTRACT_TEST_FIXTURES="${_test_fixtures}")
  if(WIN32)
    # No rpath on Windows: the DLL has to sit next to the executable.
    add_custom_command(TARGET streamextractcore_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:streamextractcore> $<TARGET_FILE_DIR:streamextractcore_tests>)
  endif()
  add_test(NAME capi COMMAND streamextractcore_tests)

  # FTP resume, failures and cancellation against a local server, using only Python's stdlib.
  find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME ftp_faults COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/integration/ftp_faults.py"
      --sext $<TARGET_FILE:sext> --lib $<TARGET_FILE:streamextractcore>)
    set_tests_properties(ftp_faults PROPERTIES TIMEOUT 60 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endif()
endif()
