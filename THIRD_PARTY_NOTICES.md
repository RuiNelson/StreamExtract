# Third-party notices

`rarftp` itself is MIT licensed. Its binaries include or link the components
below, under their own licenses. There are two programs: the command-line
`rarftp` (UnRAR, the LZMA SDK, FatFs, libarchive and its compression libraries,
libcurl, FTXUI, CLI11 and {fmt}) and the desktop app `rarftp-gui` (UnRAR, the
LZMA SDK, FatFs, libarchive and its compression libraries, libcurl and {fmt}, inside
its `librarftpcore` shared library, plus Tauri and the Rust crates below). FTXUI
and CLI11 are not in the GUI, and Tauri and the Rust crates are not in the
`rarftp` binary. The LZMA SDK (7-Zip's code) reads the 7z archives, with
zlib, bzip2 and Zstandard for the methods it lacks. libarchive reads the ZIP and
tar archives, with zlib, bzip2, liblzma (XZ Utils), Zstandard and LZ4 for
decompression, and with mbed TLS for encrypted ZIP files in the Linux builds
only.

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

## LZMA SDK

By Igor Pavlov. Source: <https://www.7-zip.org/sdk.html>. The sources are not
distributed with this repository; they are built from the `lzmasdk/` directory
supplied by the user. From its `DOC/lzma-sdk.txt`:

```
LZMA SDK is written and placed in the public domain by Igor Pavlov.

Some code in LZMA SDK is based on public domain code from another developers:
  1) PPMd var.H (2001): Dmitry Shkarin
  2) SHA-256: Wei Dai (Crypto++ library)

Anyone is free to copy, modify, publish, use, compile, sell, or distribute the
original LZMA SDK code, either in source code form or as a compiled binary, for
any purpose, commercial or non-commercial, and by any means.
```

## FatFs

By ChaN. Source: <https://elm-chan.org/fsw/ff/>. FatFs R0.16 reads exFAT
volume images in both the CLI and shared library, configured read-only. The
sources are supplied manually in `fatfs/` and are not distributed in this
repository. The official patches 1 (2025-09-13) and 2 (2026-07-10) are applied
in the build directory. License from `fatfs/source/ff.c`:

```
Copyright (C) 2025, ChaN, all right reserved.

FatFs module is an open source software. Redistribution and use of FatFs in
source and binary forms, with or without modification, are permitted provided
that the following condition is met:

1. Redistributions of source code must retain the above copyright notice,
   this condition and the following disclaimer.

This software is provided by the copyright holder and contributors "AS IS"
and any warranties related to this software are DISCLAIMED.
The copyright owner or contributors be NOT LIABLE for any damages caused
by use of this software.
```

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

## libarchive

Copyright (c) 2003-2018 Tim Kientzle and the libarchive contributors. Source:
<https://www.libarchive.org>. BSD 2-clause license (a few files of the
distribution have other terms, listed in its `COPYING` file: the BLAKE2 code,
used here, is under CC0 1.0, OpenSSL or Apache 2.0 at your option):

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer
   in this position and unchanged.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## zlib

Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler. zlib license:
<https://zlib.net/zlib_license.html>.

## bzip2

Copyright (C) 1996-2019 Julian R Seward. bzip2 license:

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. The origin of this software must not be misrepresented; you must
   not claim that you wrote the original software.  If you use this
   software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.

3. Altered source versions must be plainly marked as such, and must
   not be misrepresented as being the original software.

4. The name of the author may not be used to endorse or promote
   products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## liblzma (XZ Utils)

The XZ Utils authors and contributors. liblzma is under the BSD Zero Clause
License (0BSD): <https://tukaani.org/xz/>.

## Zstandard

Copyright (c) Meta Platforms, Inc. and affiliates. BSD license (Zstandard is
dual-licensed; it is used here under the BSD license):

```
Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## LZ4

Copyright (c) 2011-2020, Yann Collet. The LZ4 library (`lib/`) is under the BSD
2-clause license:

```
Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## mbed TLS (Linux builds only)

Copyright The Mbed TLS Contributors. Only its crypto library is linked, for
AES-encrypted ZIP files. Dual-licensed Apache-2.0 or GPL-2.0-or-later; used here
under the Apache License 2.0: <https://www.apache.org/licenses/LICENSE-2.0>.
Source: <https://github.com/Mbed-TLS/mbedtls>.

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
