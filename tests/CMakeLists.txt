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
  test_pipe.cpp
  test_progress.cpp
  test_remote_path.cpp
  test_text.cpp)
target_link_libraries(rarftp_tests PRIVATE rarftp_core doctest::doctest)

add_test(NAME unit COMMAND rarftp_tests)
