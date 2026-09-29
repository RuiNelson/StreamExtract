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

```
rarftp --file archive.rar --host ftp.example.com --port 21 --mode passive \
       --user username --password password --directory "/destination/dir"
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

Developed and tested on macOS. Linux and Windows builds will come from CI in a
later version.

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

## Libraries

| Library | Use | License |
|---|---|---|
| [UnRAR](https://www.rarlab.com/rar_add.htm) | RAR decompression | UnRAR license (freeware) |
| [libcurl](https://curl.se/libcurl/) | FTP client | curl (MIT/X derivative) |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Terminal interface | MIT |
| [CLI11](https://github.com/CLIUtils/CLI11) | Command line parsing | BSD-3-Clause |
| [{fmt}](https://github.com/fmtlib/fmt) | Formatting | MIT |
| [doctest](https://github.com/doctest/doctest) | Unit tests | MIT |

## License

This project is licensed under the MIT license (see `LICENSE`). Binaries also
contain UnRAR, whose license allows free use in any software handling RAR
archives but forbids using its code to re-create the RAR compression
algorithm; see `THIRD_PARTY_NOTICES.md`.
