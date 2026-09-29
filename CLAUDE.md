# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`rarftp` streams the contents of a RAR archive to an FTP server without extracting to disk (C++17, CMake). See `README.md` for options and behaviour.

## Build and test

UnRAR sources are **not in the repo** (license) and must exist in `unrarsrc/` (gitignored) or CMake fails at configure time; the exact `curl` + `tar` commands are in `README.md`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # add -DRARFTP_BUNDLED_CURL=ON to build FTP-only libcurl from source (what CI does)
cmake --build build                                        # produces build/rarftp
ctest --test-dir build --output-on-failure                 # unit tests (doctest)
build/tests/rarftp_tests -tc="*pipe*"                      # single unit test case, doctest filter
python3 tests/integration/run.py --rarftp build/rarftp --rar /path/to/rar [-k NAME] [--big]   # e2e
```

- Integration tests need Docker (vsftpd via `delfer/alpine-ftp-server`), RARLAB's `rar`, and Python 3.9+ stdlib only. `-k` filters tests by name substring. Active-mode tests only work on Linux.
- Formatting: `.clang-format` (Google style, 115 columns, left pointer alignment). Warnings are strict (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`; `/W4` on MSVC).
- Dependencies (fmt, CLI11, FTXUI, doctest, optionally curl) are fetched by `FetchContent` with pinned URL + SHA-256 in `cmake/Dependencies.cmake`; bump the hash together with the version. UnRAR is built from `unrarsrc/` by `cmake/UnRAR.cmake` as a static library using the DLL API (`RAR_TEST` mode).

## Architecture

Everything except `main()` is in the static library `rarftp_core` (so `tests/` can link it); `src/main.cpp` is the only file in the `rarftp` executable.

Pipeline (`src/transfer.cpp`), two threads joined by a bounded `Pipe` (`src/pipe.hpp`):

1. **Extractor thread**: UnRAR in `RAR_TEST` mode (decompresses and verifies CRC/BLAKE2 without creating files) delivers data through a callback, which pushes `PipeMessage`s (`EnsureDir`, `FileBegin`, `Data`, `FileEnd`, `End`).
2. **Uploader thread**: pops messages and streams `Data` to a single libcurl FTP `STOR` on one control connection (`src/ftp_client.cpp`).

Key invariants:
- The `Pipe` capacity counts only `Data` bytes; that is what couples decompression speed to upload speed (`--buffer`, default 64 MiB). Oversized messages are accepted when the queue is empty.
- A file counts as uploaded only after the extractor reports `FileEnd` with `ok` (checksum verified). On any error the pipeline is **fail-fast**: both threads stop and the partial remote file is deleted.
- Cancellation (`Transfer::cancel`) is thread-safe and callable from the UI thread or the signal watcher.

Before transfer (`main.cpp` orchestration): `rar_archive` lists all headers of all volumes → `plan.cpp` sanitizes names (UnRAR-style: no `..`, absolute paths or control chars), detects duplicates/case collisions, ignores links, and builds a `TransferPlan` → `probe_remote` marks files already on the server with the same size as `Skip` (this is what makes re-runs resumable at file granularity).

UI: `ui.hpp` declares two front-ends, `run_tui` (FTXUI full-screen, `ui_tui.cpp`) and `run_plain` (`ui_plain.cpp`, for `--no-tui` or non-terminals). Both observe `Progress` (thread-safe counters/speeds/ETAs) and `Logger` (log lines plus a bounded list of warnings/errors reprinted in the final summary). Platform/text helpers live in `src/util/`.

Exit codes: `0` ok, `1` error, `2` usage, `130` cancelled.

## CI

`.github/workflows/ci.yml` builds Linux/macOS/Windows with bundled static libcurl, downloads UnRAR sources (pinned SHA-256), runs the unit tests only (the Docker integration tests are local-only), and publishes a `.rar` per platform as an artifact (RARLAB's `rar` is used only to create those archives).
