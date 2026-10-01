# Third-party notices

`rarftp` itself is MIT licensed. Its binaries include or link the components
below, under their own licenses. There are two programs: the command-line
`rarftp` (UnRAR, libcurl, FTXUI, CLI11 and {fmt}) and the desktop app
`rarftp-gui` (UnRAR, libcurl and {fmt}, inside its `librarftpcore` shared
library, plus Tauri and the Rust crates below). FTXUI and CLI11 are not in the
GUI, and Tauri and the Rust crates are not in the `rarftp` binary.

## UnRAR

Copyright (c) Alexander L. Roshal. Source: <https://www.rarlab.com/rar_add.htm>.
The sources are not distributed with this repository; they are built from the
`unrarsrc/` directory supplied by the user.

UnRAR source code may be used in any software to handle RAR archives without
limitations free of charge, but cannot be used to develop RAR (WinRAR)
compatible archiver and to re-create RAR compression algorithm, which is
proprietary. Distribution of modified UnRAR source code in separate form or as
a part of other software is permitted, provided that full text of this
paragraph, starting from "UnRAR source code" words, is included in license, or
in documentation if license is not available, and in source code comments of
resulting package.

The full license is in `unrarsrc/license.txt`.

## libcurl

Copyright (c) 1996 - 2026, Daniel Stenberg and many contributors. curl license
(an MIT/X derivative): <https://curl.se/docs/copyright.html>.

## FTXUI

Copyright (c) 2019 Arthur Sonzogni. MIT license:
<https://github.com/ArthurSonzogni/FTXUI/blob/main/LICENSE>.

## CLI11

Copyright (c) 2017-2026 University of Cincinnati, developed by Henry Schreiner
under NSF AWARD 1414736. BSD 3-Clause license:
<https://github.com/CLIUtils/CLI11/blob/main/LICENSE>.

## {fmt}

Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors. MIT license:
<https://github.com/fmtlib/fmt/blob/master/LICENSE>.

## Tauri and Rust crates (`rarftp-gui` only)

The app is built with [Tauri](https://tauri.app) 2 (Copyright (c) 2017 - present
Tauri Apps Contributors: <https://github.com/tauri-apps/tauri>) and its
`tauri-plugin-dialog` plugin, serde and serde_json (by Erick Tryzelaar, David
Tolnay and contributors: <https://github.com/serde-rs/serde>,
<https://github.com/serde-rs/json>), and with some 240 Rust crates that these
depend on. Tauri, its plugin, serde and serde_json are licensed under MIT or
Apache-2.0, at your option. The crates and versions are pinned in `gui/src-tauri/Cargo.lock`; each
crate's license is stated in its `Cargo.toml` and on <https://crates.io>.

Checked for the macOS build: nearly all the other crates are MIT and/or
Apache-2.0, some alternatively Zlib, Unlicense, 0BSD, CC0-1.0 or MIT-0. A few
have other permissive terms: Unicode-3.0 (the ICU crates and related ones),
BSD-3-Clause with MIT (the brotli and alloc-stdlib crates) and Zlib (`foldhash`,
`zlib-rs`); `tao` is Apache-2.0. Five crates (`cssparser`, `cssparser-macros`,
`selectors`, `dtoa-short`, `option-ext`) are under the Mozilla Public License
2.0 (<https://www.mozilla.org/MPL/2.0/>), which applies to their own files; they
are used unmodified and their source is on <https://crates.io>. The crates that
only the Windows and Linux builds pull in have not been reviewed yet.

## doctest (tests only)

Copyright (c) 2016-2023 Viktor Kirilov. MIT license:
<https://github.com/doctest/doctest/blob/master/LICENSE.txt>.
