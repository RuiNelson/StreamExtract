# Building and testing

Run the commands below from the repository root unless stated otherwise.
For downloads and usage, see the [README](../README.md).

## Requirements

- A C++17 compiler, CMake 3.21+ and Ninja.
- Perl and Make for OpenSSL (NMake in a Visual Studio developer environment on Windows).
- Rust and the Tauri CLI for the desktop app.

UnRAR, the LZMA SDK and FatFs must be downloaded manually. CMake fetches the
remaining dependencies from pinned sources and builds them statically,
including the project's libcurl fork for FTPMC and block mode support.
The first build takes longer because it also builds these dependencies.

## Download dependencies

The UnRAR sources are not part of this repository (they have their own
license) and must be extracted into `unrarsrc/`:

```bash
curl -LO https://www.rarlab.com/rar/unrarsrc-7.3.1.tar.gz
mkdir unrarsrc && tar -xzf unrarsrc-7.3.1.tar.gz -C unrarsrc --strip-components=1
```

CMake builds a private copy with a small patch (`cmake/UnRARPatches.cmake`) so
that an archive cut short inside a header is reported as truncated instead of
ending early. The downloaded sources are unchanged.

7-Zip's [LZMA SDK](https://www.7-zip.org/sdk.html), which reads the 7z
archives, is not part of the repository either; extract it into `lzmasdk/`, with
bsdtar (the `tar` of macOS) or 7-Zip:

```bash
curl -LO https://github.com/ip7z/7zip/releases/download/26.03/lzma2603.7z
mkdir lzmasdk && tar -xf lzma2603.7z -C lzmasdk    # or: 7z x -olzmasdk lzma2603.7z
```

[FatFs R0.16](https://elm-chan.org/fsw/ff/), the exFAT reader, is also supplied
manually, from its author's website (there is no official GitHub repository):

```bash
curl -LO https://elm-chan.org/fsw/ff/arc/ff16.zip
mkdir fatfs && unzip ff16.zip -d fatfs    # or: 7z x -ofatfs ff16.zip
```

The zip's SHA-256 is
`99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`.
CMake builds a private copy with a read-only exFAT configuration, UTF-8 names
and the official R0.16 patches 1 and 2. The downloaded sources are unchanged,
and CMake does not download FatFs.

## Build the command-line tool

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is `build/sext`. UnRAR, the LZMA SDK, FatFs, libarchive and libcurl are always
linked statically into it.

## Build the desktop app

Besides the above, install Rust ([rustup](https://rustup.rs)) and the Tauri
CLI:

```bash
cargo install tauri-cli --version "^2" --locked
```

To build manually, run these commands from the repository root:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTREAMEXTRACT_BUILD_LIBRARY=ON
cmake --build build

cd gui
cargo tauri build     # macOS: src-tauri/target/release/bundle/macos/StreamExtract.app
cargo tauri dev       # or: run the app without bundling it
```

The app links `build/libstreamextractcore.dylib` (`libstreamextractcore.so` on Linux,
`streamextractcore.dll` on Windows). Build this library before running the Tauri commands.

On Linux `cargo tauri build` makes an AppImage; on Windows use
`cargo tauri build --no-bundle` and keep `streamextractcore.dll` next to
`src-tauri/target/release/StreamExtract.exe` (the build copies it there). Linux
needs the WebKitGTK development packages that Tauri documents in its
[prerequisites](https://v2.tauri.app/start/prerequisites/#linux); on
Debian/Ubuntu, for example:

```bash
sudo apt install libwebkit2gtk-4.1-dev build-essential curl wget file libxdo-dev libssl-dev \
                 libayatana-appindicator3-dev librsvg2-dev
```

The Rust build looks for the library in `build/` (and `build/Release`); set
`STREAMEXTRACT_LIB_DIR` to the directory that holds it to use another build directory.
The bundling settings (`gui/src-tauri/tauri.<os>.conf.json`) also name the
library under `../../build/`, so with another directory override that path too,
with the Tauri CLI's `--config` (a JSON merge into its configuration, handed to
the build script as well). On macOS:

```bash
STREAMEXTRACT_LIB_DIR=/path/to/dir cargo tauri build \
    --config '{"bundle":{"macOS":{"frameworks":["/path/to/dir/libstreamextractcore.dylib"]}}}'
```

A macOS app built from source is signed ad hoc, not notarized, so Gatekeeper
blocks it once it has been copied to another Mac. Remove the quarantine
attribute there to open it:

```bash
xattr -dr com.apple.quarantine StreamExtract.app
```

## Testing

Unit tests:

```bash
ctest --test-dir build --output-on-failure
```

End-to-end tests upload archives created with RARLAB's `rar`, Python's
`zipfile` and `tarfile`, 7-Zip and Info-ZIP's `zip` to the shared Docker server
in [`tests/docker`](../tests/docker/README.md)
and compare what arrives, byte by byte. They need Docker, `rar` and Python 3.9+
(standard library only). Archives that cannot be made are skipped: those that
need 7-Zip (`7zz`, found in `PATH` or given with `--7z`), Info-ZIP's `zip`, the
`lz4` command (for `.tar.lz4`) or Python 3.14+ (for Zstandard):

```bash
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar --7z /path/to/7zz
```

All integration runners automatically build the same pinned Docker image and
remove their containers on exit. `ftps.py` tests real FTP, explicit FTPS and
implicit FTPS uploads, plus certificate checks and TLS failures. Add `--lib`
to run the same cases through the core API used by the GUI. `sftp.py` covers
SFTP authentication, host verification and file transfer behavior. `ftp_faults.py`
tests resume, retry and cancellation using controlled scenarios in that image,
block mode (`--mode-b`), and FTPMC (`--ftp-mc`, including FTPS) against servers that support or refuse them:

```bash
python3 tests/integration/ftps.py --sext build/sext
python3 tests/integration/sftp.py --sext build/sext --lib build/libstreamextractcore.dylib
python3 tests/integration/ftp_faults.py --sext build/sext --lib build/libstreamextractcore.dylib
python3 tests/integration/run.py --sext build/sext --lib build/libstreamextractcore.dylib -k pfs
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar --protocol sftp
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar --protocol ftps
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar --protocol ftps --ftps-mode implicit
```

The PFS-only run (`-k pfs`) uses checked-in MkPFS and makefs fixtures and needs
no external archiver. It checks PFS, PFSC, wrapped exFAT/UFS images and nested
wrappers through both the CLI and C API, including re-runs, resume and replacement.

To register the Docker suites with CTest, configure with
`-DSTREAMEXTRACT_BUILD_INTEGRATION_TESTS=ON` (and
`-DSTREAMEXTRACT_BUILD_LIBRARY=ON` for the fault suite). The normal unit and
C API smoke tests do not require Docker; CI runs only those unit-level tests.

`--big` adds a 4.5 GiB file. Active mode is only tested on Linux, where the
container address is reachable directly; Docker Desktop only publishes ports.

With the library built (`-DSTREAMEXTRACT_BUILD_LIBRARY=ON`), `ctest` also runs a
smoke test of its C API, and `--lib` adds tests that drive the library the way
the app does (through its C API, with `ctypes`): uploads, re-runs, multi-volume
and encrypted archives, ZIP, 7z and compressed tar, the password prompt, errors
and cancelling, checking the JSON state at every poll. They are
named `lib_*`:

```bash
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar \
        --lib build/libstreamextractcore.dylib -k lib_
```

The Rust side has its own unit tests, which link the built library:
`cargo test --manifest-path gui/src-tauri/Cargo.toml`.

## Version updates

Run `scripts/bump_version X.Y.Z` with Python 3 to update the project version,
including the GUI and API examples. Dependency versions are unchanged.
