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

add_executable(rarftp_tests
  test_main.cpp
  test_archive.cpp
  test_exfat.cpp
  test_json.cpp
  test_pipe.cpp
  test_plan.cpp
  test_progress.cpp
  test_remote_path.cpp
  test_summary.cpp
  test_text.cpp)
target_link_libraries(rarftp_tests PRIVATE rarftp_core doctest::doctest)
target_compile_definitions(rarftp_tests PRIVATE RARFTP_TEST_FIXTURES="${_test_fixtures}")

add_test(NAME unit COMMAND rarftp_tests)

if(TARGET rarftpcore)
  # C-ABI smoke test: only the public header and the shared library, exactly
  # what rarftp-gui sees.
  add_executable(rarftpcore_tests test_capi.cpp)
  target_include_directories(rarftpcore_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(rarftpcore_tests PRIVATE rarftpcore doctest::doctest)
  target_compile_definitions(rarftpcore_tests PRIVATE RARFTP_TEST_FIXTURES="${_test_fixtures}")
  if(WIN32)
    # No rpath on Windows: the DLL has to sit next to the executable.
    add_custom_command(TARGET rarftpcore_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rarftpcore> $<TARGET_FILE_DIR:rarftpcore_tests>)
  endif()
  add_test(NAME capi COMMAND rarftpcore_tests)

  # FTP resume, failures and cancellation against a local server, using only Python's stdlib.
  find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME ftp_faults COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/integration/ftp_faults.py"
      --rarftp $<TARGET_FILE:rarftp> --lib $<TARGET_FILE:rarftpcore>)
    set_tests_properties(ftp_faults PROPERTIES TIMEOUT 60 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endif()
endif()
