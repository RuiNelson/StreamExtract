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

add_executable(rarftp_tests
  test_main.cpp
  test_archive.cpp
  test_json.cpp
  test_pipe.cpp
  test_plan.cpp
  test_progress.cpp
  test_remote_path.cpp
  test_summary.cpp
  test_text.cpp)
target_link_libraries(rarftp_tests PRIVATE rarftp_core doctest::doctest)

add_test(NAME unit COMMAND rarftp_tests)

if(TARGET rarftpcore)
  # C-ABI smoke test: only the public header and the shared library, exactly
  # what rarftp-gui sees.
  add_executable(rarftpcore_tests test_capi.cpp)
  target_include_directories(rarftpcore_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(rarftpcore_tests PRIVATE rarftpcore doctest::doctest)
  if(WIN32)
    # No rpath on Windows: the DLL has to sit next to the executable.
    add_custom_command(TARGET rarftpcore_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rarftpcore> $<TARGET_FILE_DIR:rarftpcore_tests>)
  endif()
  add_test(NAME capi COMMAND rarftpcore_tests)
endif()
