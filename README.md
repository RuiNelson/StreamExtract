# rarftp

Uploads the contents of a RAR archive straight to an FTP server, **without
extracting it to disk first**.

The usual way to publish a huge archive is to extract it (needing as much free
space as the unpacked data, with the CPU busy and the network idle) and then
upload it (network busy, CPU idle). `rarftp` streams each file from the
decompressor to the FTP data connection instead:

- **No temporary files**: nothing is written to the local disk.
- **Decompression and upload overlap**: a bounded memory buffer (64 MiB by
  default) sits between the two, so the CPU and the network work at the same
  time.
- **No local name collisions**: since nothing is extracted locally, archives
  with names that differ only in letter case (`README.txt` / `Readme.txt`)
  upload correctly from case-insensitive systems (macOS, Windows) to
  case-sensitive servers.

```bash
rarftp --file archive.rar --host ftp.example.com --port 21 --mode passive \
       --user username --password "password" --directory "/destination/dir"
```

`rarftp` supports advanced RAR functionalities like multi-part archives and
password protection:

```bash
--rar-password "*[open sesame]*"
```

## How it works

```
 extractor thread                      uploader thread
 UnRAR RAR_TEST ──(1 MiB blocks)──▶ [ bounded buffer ] ──▶ libcurl STOR
 (checks CRC/BLAKE2)                                         (one control connection)
```

UnRAR's test mode decompresses and verifies every file without creating it;
the decompressed data arrives in a callback and goes into the buffer, which the
FTP upload drains. A file only counts as uploaded after UnRAR confirmed its
checksum; on any error the incomplete remote file is deleted.

The engine is also built as a shared library (`librarftpcore`, C API in
`src/capi/rarftp.h`), which the [GUI](#gui-rarftp-gui) uses.

## Usage

| Option | Description |
|---|---|
| `--file PATH` | RAR archive. For multi-volume sets, the first volume (`.part1.rar`, `.rar`). |
| `--host HOST` | FTP server name or address (IPv4 or IPv6). |
| `--port PORT` | Default `21`. |
| `--mode passive\|active` | Data connection mode. Default `passive`. |
| `--user NAME` | Without it, the login is anonymous. |
| `--password PASSWORD` | Requires `--user`. If omitted, it is asked for (hidden) on the terminal. |
| `--directory DIR` | Destination, absolute or relative to the login directory. Without it the login directory is used and a warning says which one. |
| `--mkdir` | Create the destination if it does not exist, with a single `MKD` (not recursive). Without it, a missing destination is an error. |
| `--rar-password PASSWORD` | For encrypted archives; asked for on the terminal when needed. |
| `--no-tui` | Plain log output instead of the full-screen interface (automatic when not on a terminal). |
| `--verbose` | Log every FTP command and reply (the password is masked). |
| `--buffer MIB` | Memory buffer between decompression and upload. Default `64`. |

Behaviour:

- **Files already on the server** with the same size are skipped; with a
  different size they are overwritten. Re-running after a failure only sends
  what is missing or incomplete.
- **Directories** of the archive, including empty ones, are created.
- **Modification times** are preserved with `MFMT` (or vsftpd's `MDTM` form)
  when the server allows it.
- **Multi-volume**, **solid** and **encrypted** (`-p`, `-hp`) archives are
  supported, in every format UnRAR reads (RAR 5.x and older).
- **Paths are sanitized** like UnRAR does: `..` components, absolute paths and
  control characters never escape the destination directory.
- **Fail-fast**: a checksum error, a missing volume or a network failure stops
  the transfer and removes the incomplete remote file.
- **Cancel** with `q`, `Esc` or `Ctrl-C` (or `SIGINT`/`SIGTERM` in plain mode).

The interface shows a fixed log panel, the progress of the current file and of
the whole archive with their ETAs, the upload and decompression speeds, and
how full the buffer is (full: the network is the bottleneck; empty: the CPU
is).

Exit codes: `0` success, `1` error, `2` invalid command line, `130` cancelled.

### Not supported yet

FTPS, resuming or retrying a file after a network failure, extracting only
some files, and several parallel connections. Symbolic links, hard links and
file references (`rar -oi`) are skipped with a warning, since FTP cannot
create links.

## Install

Prebuilt binaries are published as assets of each release on the repository's
[**Releases** page](https://github.com/RuiNelson/rarftp/releases). Download the file for your platform:

| Platform | Asset |
|---|---|
| Windows (x64) | `rarftp-windows-x64.rar` |
| macOS (Intel and Apple Silicon) | `rarftp-macos-universal.rar` |
| Linux (x64, glibc 2.35+) | `rarftp-linux-x64.rar` |
| Linux (arm64, glibc 2.35+) | `rarftp-linux-arm64.rar` |

Each archive holds the `rarftp` executable, `LICENSE`, `README.md` and
`THIRD_PARTY_NOTICES.md` . Extracting it needs a program that reads RAR5 (WinRAR,
7-Zip, Keka, `unrar`, ...). The executables are statically linked and need no other
installation.

### Windows

1. Extract `rarftp-windows-x64.rar` into `C:\rarftp`, so that the
   result is `C:\rarftp\rarftp.exe`.

2. Run it from a terminal:

   ```powershell
   C:\rarftp\rarftp.exe --version
   ```

The binary is not code-signed, so if it does not start at all, check whether
your antivirus quarantined it.

### macOS

The binary is not code-signed or notarized, so Gatekeeper blocks it after it
is downloaded (*"rarftp" cannot be opened because the developer cannot be
verified*). Extract the archive into a directory of its own, remove the
quarantine attribute, and install it:

```bash
xattr -d com.apple.quarantine rarftp                # in the extracted directory
sudo install -d /usr/local/bin
sudo install -m 755 rarftp /usr/local/bin/rarftp
rarftp --version
```

### Linux

```bash
unrar x rarftp-linux-x64.rar rarftp/        # or: 7z x -orarftp rarftp-linux-x64.rar
sudo install -m 755 rarftp/rarftp /usr/local/bin/rarftp
rarftp --version
```

Without root, install it into a directory in your `PATH` instead, for example
`install -D -m 755 rarftp/rarftp ~/.local/bin/rarftp`. The binary is built on
Ubuntu 22.04, so it runs on distributions with glibc 2.35 or newer; libstdc++
is linked statically.

## Building

Requirements: a C++17 compiler, CMake 3.21+, and libcurl (the system one is
used when present; otherwise, or with `-DRARFTP_BUNDLED_CURL=ON`, an FTP-only
libcurl is built from source). The other libraries are fetched by CMake.

The UnRAR sources are not part of this repository (they have their own
license) and must be extracted into `unrarsrc/`:

```bash
curl -LO https://www.rarlab.com/rar/unrarsrc-7.3.1.tar.gz
mkdir unrarsrc && tar -xzf unrarsrc-7.3.1.tar.gz -C unrarsrc --strip-components=1
```

Then:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is `build/rarftp`. UnRAR is always linked statically into it.

Developed and tested on macOS. The GitHub Actions workflow
(`.github/workflows/ci.yml`) builds and tests Linux (x64 and arm64), macOS (universal)
and Windows (x64), always with libcurl built from source and linked
statically (Windows does not ship it), and produces one `.rar` per platform,
for the command line and for the GUI.

## GUI (rarftp-gui)

`rarftp-gui` is a desktop app for Windows, macOS and Linux that does what
`rarftp` does, from a window instead of a command line. It runs the same engine
(through the `librarftpcore` shared library) and is built with
[Tauri](https://tauri.app); the interface is plain HTML, CSS and JavaScript, with
no JavaScript framework.

Releases include portable builds, next to the command-line ones:

| Platform | Asset | Contents |
|---|---|---|
| Windows 10/11 (x64) | `rarftp-gui-windows-x64.rar` | `rarftp-gui.exe` and `rarftpcore.dll` (keep them together; uses the WebView2 runtime that Windows ships) |
| macOS 12+ (Intel and Apple Silicon) | `rarftp-gui-macos-universal.rar` | `rarftp-gui.app` |
| Linux (x64, glibc 2.35+) | `rarftp-gui-linux-x64.rar` | `rarftp-gui.AppImage` (needs FUSE 2, e.g. `libfuse2`) |
| Linux (arm64, glibc 2.35+) | `rarftp-gui-linux-arm64.rar` | `rarftp-gui.AppImage` |

The window offers:

- **Archive**: drop a `.rar` on the window (the first volume, for multi-volume
  sets) or pick it with a file dialog. When the archive is encrypted and no
  password was typed in, the app asks for it, and asks again if it was wrong.
- **Server**: host, port, passive or active mode, user and password (anonymous
  without a user), destination directory and *Create directory if missing*.
  *Advanced* has the buffer size and the verbose log.
- **Memory Save**, **Memory Recall** and **Memory Clear**: keep the server
  settings between runs in `~/.config/rarftp-gui/memory.ini` (under the home
  directory on every OS), a **plain text** file. The first time a login with a
  user name is saved, the app asks whether the user name and password may be
  stored unencrypted; if not, they are left out (Memory Clear also forgets the
  answer). The archive password is never stored.
- **Progress**: the current step, the current file and the whole archive with
  their ETAs, the upload and decompression speeds, the buffer fill, a live log,
  and the same final summary as the command line. **Cancel** stops the transfer
  and removes the incomplete remote file; closing the window during a transfer
  asks for confirmation first.

### Building the GUI

Besides what the command-line build needs, install Rust
([rustup](https://rustup.rs)) and the Tauri CLI:

```bash
cargo install tauri-cli --version "^2" --locked
```

From the repository root, build the shared library, then the app:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRARFTP_BUILD_LIBRARY=ON
cmake --build build

cd gui
cargo tauri build     # macOS: src-tauri/target/release/bundle/macos/rarftp-gui.app
cargo tauri dev       # or: run the app without bundling it
```

The library is `build/librarftpcore.dylib` (`.so` on Linux, `rarftpcore.dll` on
Windows). UnRAR and {fmt} are linked statically into it, and it exports only the
`rarftp_*` functions of `src/capi/rarftp.h`; the `rarftp` executable does not use
it. Add `-DRARFTP_BUNDLED_CURL=ON` to link libcurl statically too, which makes
the library self-contained instead of relying on the system's libcurl.

The Rust build looks for the library in `build/` (and `build/Release`); set
`RARFTP_LIB_DIR` to the directory that holds it to use another build directory.
The bundling settings (`gui/src-tauri/tauri.<os>.conf.json`) also name the
library under `../../build/`, so with another directory override that path too,
with the Tauri CLI's `--config` (a JSON merge into its configuration, handed to
the build script as well). On macOS:

```bash
RARFTP_LIB_DIR=/path/to/dir cargo tauri build \
    --config '{"bundle":{"macOS":{"frameworks":["/path/to/dir/librarftpcore.dylib"]}}}'
```

Linux needs the WebKitGTK development packages that Tauri documents in its
[prerequisites](https://v2.tauri.app/start/prerequisites/#linux); on
Debian/Ubuntu, for example, `libwebkit2gtk-4.1-dev` and its companions:

```bash
sudo apt install libwebkit2gtk-4.1-dev build-essential curl wget file libxdo-dev libssl-dev \
                 libayatana-appindicator3-dev librsvg2-dev
```

On Linux `cargo tauri build` makes an AppImage, on Windows use
`cargo tauri build --no-bundle` and keep `rarftpcore.dll` next to
`src-tauri/target/release/rarftp-gui.exe` (the build copies it there). The CI
builds all of them (see below).

The macOS app is signed ad hoc, not notarized, so Gatekeeper blocks it once it
has been downloaded to another Mac. Remove the quarantine attribute and open it:

```bash
xattr -dr com.apple.quarantine rarftp-gui.app
```

## Testing

Unit tests:

```bash
ctest --test-dir build --output-on-failure
```

End-to-end tests upload archives created with RARLAB's `rar` to vsftpd running
in Docker ([delfer/alpine-ftp-server](https://hub.docker.com/r/delfer/alpine-ftp-server))
and compare what arrives, byte by byte. They need Docker, `rar` and Python 3.9+
(standard library only):

```bash
python3 tests/integration/run.py --rarftp build/rarftp --rar /path/to/rar
```

`--big` adds a 4.5 GiB file. Active mode is only tested on Linux, where the
container address is reachable directly; Docker Desktop only publishes ports.

With the library built (`-DRARFTP_BUILD_LIBRARY=ON`), `ctest` also runs a
smoke test of its C API, and `--lib` adds tests that drive the library the way
the GUI does (through its C API, with `ctypes`): uploads, re-runs, multi-volume
and encrypted archives, the password prompt, errors and cancelling. They are
named `lib_*`:

```bash
python3 tests/integration/run.py --rarftp build/rarftp --rar /path/to/rar \
        --lib build/librarftpcore.dylib -k lib_
```

The Rust side has its own unit tests, which link the built library:
`cargo test --manifest-path gui/src-tauri/Cargo.toml`.

## Libraries

| Library | Use | License |
|---|---|---|
| [UnRAR](https://www.rarlab.com/rar_add.htm) | RAR decompression | UnRAR license (freeware) |
| [libcurl](https://curl.se/libcurl/) | FTP client | curl (MIT/X derivative) |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Terminal interface | MIT |
| [CLI11](https://github.com/CLIUtils/CLI11) | Command line parsing | BSD-3-Clause |
| [{fmt}](https://github.com/fmtlib/fmt) | Formatting | MIT |
| [doctest](https://github.com/doctest/doctest) | Unit tests | MIT |
| [Tauri](https://tauri.app) | Desktop app (GUI only), with its Rust dependencies | MIT or Apache-2.0 |

## License

This project is licensed under the MIT license (see `LICENSE`). Binaries also
contain UnRAR, whose license allows free use in any software handling RAR
archives but forbids using its code to re-create the RAR compression
algorithm; see `THIRD_PARTY_NOTICES.md`, which also covers what only the GUI
contains (Tauri and its Rust dependencies).
