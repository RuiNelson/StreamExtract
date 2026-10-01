# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`rarftp` streams the contents of a RAR archive to an FTP server without extracting to disk (C++17, CMake). See `README.md` for options and behaviour. `rarftp-gui` is a desktop front-end for the same engine (Tauri 2 + vanilla HTML/CSS/JS in `gui/`) that talks to it through the `rarftpcore` shared library.

## Build and test

UnRAR sources are **not in the repo** (license) and must exist in `unrarsrc/` (gitignored) or CMake fails at configure time; the exact `curl` + `tar` commands are in `README.md`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # add -DRARFTP_BUNDLED_CURL=ON to build FTP-only libcurl from source (what CI does)
                                                           # add -DRARFTP_BUILD_LIBRARY=ON for the shared library the GUI links
cmake --build build                                        # produces build/rarftp (and build/librarftpcore.{dylib,so} / rarftpcore.dll)
ctest --test-dir build --output-on-failure                 # unit tests (doctest); with the library also the C API smoke test (tests/test_capi.cpp)
build/tests/rarftp_tests -tc="*pipe*"                      # single unit test case, doctest filter
python3 tests/integration/run.py --rarftp build/rarftp --rar /path/to/rar [-k NAME] [--big] [--lib build/librarftpcore.dylib]   # e2e
cd gui && cargo tauri build                                # GUI bundle (macOS: gui/src-tauri/target/release/bundle/macos/rarftp-gui.app); `cargo tauri dev` runs it
cargo test --manifest-path gui/src-tauri/Cargo.toml        # Rust tests (memory file, FFI wrapper)
```

- Integration tests need Docker (vsftpd via `delfer/alpine-ftp-server`), RARLAB's `rar`, and Python 3.9+ stdlib only. `-k` filters tests by name substring. Active-mode tests only work on Linux. `--lib` adds the `lib_*` tests (`-k lib_`), which drive the C API with `ctypes` and check the JSON contract at every poll; they are skipped without it.
- The GUI build needs Rust, `cargo install tauri-cli --version "^2" --locked` and the library already built: `gui/src-tauri/build.rs` looks for it in `build/` (or `RARFTP_LIB_DIR`) and fails with the CMake command otherwise. The bundle configs (`tauri.macos.conf.json`, `tauri.linux.conf.json`; Windows has none, it is built with `--no-bundle`) hard-code `../../build/...` (the macOS one is read even without bundling); with another `RARFTP_LIB_DIR` override them with `cargo tauri build --config '<json>'` (the CLI exports it to the build script as `TAURI_CONFIG`; setting `TAURI_CONFIG` by hand reaches only the build script, not the bundler).
- Formatting: `.clang-format` (Google style, 115 columns, left pointer alignment). Warnings are strict (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`; `/W4` on MSVC).
- Dependencies (fmt, CLI11, FTXUI, doctest, optionally curl) are fetched by `FetchContent` with pinned URL + SHA-256 in `cmake/Dependencies.cmake`; bump the hash together with the version. UnRAR is built from `unrarsrc/` by `cmake/UnRAR.cmake` as a static library using the DLL API (`RAR_TEST` mode). Rust crates are pinned by `gui/src-tauri/Cargo.lock`; new ones must be added to `THIRD_PARTY_NOTICES.md`.

## Architecture

`rarftp_core` (static library) is the engine only: archive listing, plan, FTP, transfer pipeline, progress/log, JSON and text helpers. It has no CLI11/FTXUI. It is linked into the `rarftp` executable (`src/main.cpp`, `options.cpp`, `ui_plain.cpp`, `ui_tui.cpp`: the only code using CLI11 and FTXUI), into `tests/`, and into the `rarftpcore` shared library (only with `RARFTP_BUILD_LIBRARY`).

Pipeline (`src/transfer.cpp`), two threads joined by a bounded `Pipe` (`src/pipe.hpp`):

1. **Extractor thread**: UnRAR in `RAR_TEST` mode (decompresses and verifies CRC/BLAKE2 without creating files) delivers data through a callback, which pushes `PipeMessage`s (`EnsureDir`, `FileBegin`, `Data`, `FileEnd`, `End`).
2. **Uploader thread**: pops messages and streams `Data` to a single libcurl FTP `STOR` on one control connection (`src/ftp_client.cpp`).

Key invariants:
- The `Pipe` capacity counts only `Data` bytes; that is what couples decompression speed to upload speed (`--buffer`, default 64 MiB). Oversized messages are accepted when the queue is empty.
- A file counts as uploaded only after the extractor reports `FileEnd` with `ok` (checksum verified). On any error the pipeline is **fail-fast**: both threads stop and the partial remote file is deleted.
- Cancellation (`Transfer::cancel`) is thread-safe and callable from the UI thread or the signal watcher.

Before transfer (`main.cpp` orchestration, repeated by `Job` in `src/capi/job.cpp`): `rar_archive` lists all headers of all volumes → `plan.cpp` sanitizes names (UnRAR-style: no `..`, absolute paths or control chars), detects duplicates/case collisions, ignores links, and builds a `TransferPlan` → `probe_remote` marks files already on the server with the same size as `Skip` (this is what makes re-runs resumable at file granularity).

CLI UI: `ui.hpp` declares two front-ends, `run_tui` (FTXUI full-screen, `ui_tui.cpp`) and `run_plain` (`ui_plain.cpp`, for `--no-tui` or non-terminals). Both observe `Progress` (thread-safe counters/speeds/ETAs) and `Logger` (log lines plus a bounded list of warnings/errors reprinted in the final summary). `summary.cpp` builds the final summary lines shared with the GUI. Platform/text helpers live in `src/util/`.

Exit codes: `0` ok, `1` error, `2` usage, `130` cancelled.

### Shared library and GUI

- `rarftpcore` (`src/capi/`): C API in `rarftp.h` (`rarftp_job_start/poll/answer_password/cancel/free`, `rarftp_version`, `rarftp_free`). `Job` (`job.cpp`) runs what `main.cpp` does (list archive → FTP login and destination check → plan and probe → `Transfer`) on its own controller thread, with GUI wording in its messages (no `--mkdir` etc.), and `poll()` returns the whole state as JSON (`src/util/json.hpp`): every key always present, `null` when not applicable, log lines numbered with a cursor (last 5000 kept). Only `rarftp_*` is exported (`-exported_symbol`, ELF version script, `dllexport`); UnRAR, libcurl and fmt are static inside it.
- GUI backend (`gui/src-tauri`, Rust): `ffi.rs` (bindings + safe `Job` wrapper), `memory.rs` (Memory Save/Recall/Clear in `~/.config/rarftp-gui/memory.ini`, plain text, the archive password is never stored), `lib.rs` (Tauri commands: `start_transfer`, `poll_transfer`, `answer_password`, `cancel_transfer`, `close_transfer`, `memory_*`, `app_info`; one job at a time, dropped on exit). `build.rs` links the library and sets rpaths.
- GUI frontend (`gui/ui`, vanilla JS, no npm, `withGlobalTauri`): polls `poll_transfer` every ~200 ms and renders the JSON. `tests/test_capi.cpp` and the `lib_*` integration tests check the same JSON, so change it in all three places.

More invariants:
- `PasswordSource` takes a prompt function (terminal in the CLI, GUI dialog in `Job`) and asks at most once per instance. `Job::read_archive` builds a new one and re-asks on `ArchivePasswordError` (prompt error `Wrong password`), and calls `disable_prompt()` before the transfer threads start.
- `FtpClient::set_cancel_check` lets `Job::cancel` interrupt a server that does not answer; `FtpConfig::mention_flags = false` keeps CLI options out of FTP error hints.
- `poll()` copies the log while holding the state lock, so a snapshot saying `finished` already has every log line.
- The CLI must never link the shared library (UnRAR stays static in `rarftp`). `src/version.hpp` must not exist: it would shadow UnRAR's `version.hpp`, included as `<version.hpp>` by `rar_archive.cpp`; hence `app_version.hpp`.

## CI

`.github/workflows/ci.yml` builds Linux/macOS/Windows with bundled static libcurl, downloads UnRAR sources (pinned SHA-256), runs the unit tests only (the Docker integration tests are local-only), and publishes a `.rar` per platform as an artifact (RARLAB's `rar` is used only to create those archives). A `gui` job builds `rarftpcore` (bundled curl, its `capi` test), runs `cargo test` and builds the GUI: AppImage (Linux x64/arm64, checks the library resolves inside it), universal `.app` (macOS, ad-hoc signed), `rarftp-gui.exe` + `rarftpcore.dll` with static CRTs (Windows, `--no-bundle`); the `package` job makes `rarftp-gui-<platform>.rar` too.

## Releases

Use the CI-generated artifacts to get the RAR files for binary distribution.

The project uses semantic versioning, tag the repository (`vX.X.X`), the GitHub version name follows the format `Version X.X.X`

Release notes format is a list of what's new for the user (do not include changes not relevant to the end user such as documentation, CI, etc.), ordered by significance.
