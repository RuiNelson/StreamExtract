# Third-party notices

`streamextract` itself is MIT licensed. Its binaries include or link the components
below, under their own licenses. There are two programs: the command-line
`streamextract` (UnRAR, the LZMA SDK, FatFs, libarchive and its compression libraries,
libcurl, FTXUI, CLI11 and {fmt}) and the desktop app `streamextract-gui` (UnRAR, the
LZMA SDK, FatFs, libarchive and its compression libraries, libcurl and {fmt}, inside
its `libstreamextractcore` shared library, plus Tauri and the Rust crates below). FTXUI
and CLI11 are not in the GUI, and Tauri and the Rust crates are not in the
`streamextract` binary. The LZMA SDK (7-Zip's code) reads the 7z archives, with
zlib, bzip2 and Zstandard for the methods it lacks. libarchive reads the ZIP and
tar archives, with zlib, bzip2, liblzma (XZ Utils), Zstandard and LZ4 for
decompression. OpenSSL provides FTPS and SSH cryptography on every platform and
encrypted ZIP support on Linux. libssh2 provides libcurl's SFTP backend. These
libraries are linked statically into both the CLI and the GUI core.

## Completion sound (GUI only)

Thanks to **SoundShelfStudio** for the free SFX used when an upload completes
successfully (`gui/ui/assets/audio/completed.mp3`).

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

The bundled 8.22.0 source has SSH passphrase and zero-byte SFTP stat fixes in
`cmake/CurlPatches.cmake`; all transfer protocols use this same build.

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

## OpenSSL 3.5.9

Copyright (c) 1998-2026 The OpenSSL Project Authors. All rights reserved.
Statically linked for FTPS, SSH cryptography and encrypted ZIP files on Linux.
Licensed under Apache-2.0: <https://www.apache.org/licenses/LICENSE-2.0>.
Source and notices: <https://github.com/openssl/openssl/tree/openssl-3.5.9>.

## libssh2 1.11.1

Statically linked as libcurl's SFTP backend. Source: <https://libssh2.org/>.
Licensed under BSD-3-Clause:

```
/* Copyright (C) 2004-2007 Sara Golemon <sarag@libssh2.org>
 * Copyright (C) 2005,2006 Mikhail Gusarov <dottedmag@dottedmag.net>
 * Copyright (C) 2006-2007 The Written Word, Inc.
 * Copyright (C) 2007 Eli Fant <elifantu@mail.ru>
 * Copyright (C) 2009-2023 Daniel Stenberg
 * Copyright (C) 2008, 2009 Simon Josefsson
 * Copyright (C) 2000 Markus Friedl
 * Copyright (C) 2015 Microsoft Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 *   Redistributions of source code must retain the above
 *   copyright notice, this list of conditions and the
 *   following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials
 *   provided with the distribution.
 *
 *   Neither the name of the copyright holder nor the names
 *   of any other contributors may be used to endorse or
 *   promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */
```

libssh2 also includes the following notices for encrypted OpenSSH key support:

```
/* $OpenBSD: bcrypt_pbkdf.c,v 1.4 2013/07/29 00:55:53 tedu Exp $ */
```

```
/* $OpenBSD: blowfish.c,v 1.18 2004/11/02 17:23:26 hshoexer Exp $ */
```

## Tauri and Rust crates (`streamextract-gui` only)

The app is built with [Tauri](https://tauri.app) 2 (Copyright (c) 2017 - present
Tauri Apps Contributors: <https://github.com/tauri-apps/tauri>) and its
`tauri-plugin-dialog` and `tauri-plugin-opener` plugins, serde and serde_json (by Erick Tryzelaar, David
Tolnay and contributors: <https://github.com/serde-rs/serde>,
<https://github.com/serde-rs/json>), and with the Rust crates that these
depend on. Tauri, its plugins, serde and serde_json are licensed under MIT or
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


### Release checks and browser opening (GUI only)

Release checks use `github_release_check` 0.2.1, Copyright (c) 2022 Matt Boulanger
(<https://github.com/Celeo/github_release_check>), under MIT or Apache-2.0.
`semver`, by David Tolnay and contributors (<https://github.com/dtolnay/semver>),
compares versions, and `tokio`, by the Tokio contributors (<https://tokio.rs>),
passes the separate window's consent answer asynchronously; both are MIT or
Apache-2.0. `tauri-plugin-opener`, by the Tauri contributors
(<https://github.com/tauri-apps/plugins-workspace/tree/v2/plugins/opener>), is
MIT or Apache-2.0 and opens the fixed GitHub releases URL in the default browser.

The added transitive crates are listed below (including platform-specific
ones); their exact versions and sources are in `gui/src-tauri/Cargo.lock`, and
their license texts and copyright notices are in the crate distributions on
<https://crates.io>. OpenSSL bindings use the system OpenSSL libraries on Linux;
TLS uses the platform libraries on macOS and Windows.

| License choices | Added crates |
| --- | --- |
| (Apache-2.0 OR MIT) AND BSD-3-Clause | `encoding_rs` |
| Apache-2.0 | `openssl`, `sync_wrapper` |
| Apache-2.0 OR BSL-1.0 | `ryu` |
| Apache-2.0 OR ISC OR MIT | `rustls-pemfile` |
| Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT | `linux-raw-sys`, `rustix` |
| MIT | `endi`, `h2`, `http-body`, `hyper`, `is-docker`, `is-wsl`, `open`, `openssl-sys`, `schannel`, `tokio-native-tls`, `tracing-attributes`, `uds_windows`, `winreg`, `zbus`, `zbus_macros`, `zbus_names`, `zcheapstr`, `zvariant`, `zvariant_derive`, `zvariant_utils` |
| MIT or Apache-2.0 | `async-broadcast`, `async-channel`, `async-executor`, `async-io`, `async-lock`, `async-process`, `async-recursion`, `async-signal`, `async-task`, `async-trait`, `blocking`, `concurrent-queue`, `core-foundation`, `core_detect`, `enumflags2`, `enumflags2_derive`, `errno`, `event-listener`, `event-listener-strategy`, `foreign-types`, `foreign-types-shared`, `futures-lite`, `hermit-abi`, `http`, `httpdate`, `hyper-tls`, `multiversion_no_op`, `native-tls`, `openssl-macros`, `openssl-probe`, `ordered-stream`, `parking`, `piper`, `polling`, `reqwest`, `security-framework`, `security-framework-sys`, `serde_urlencoded`, `signal-hook-registry`, `simdutf8`, `socket2`, `system-configuration`, `system-configuration-sys`, `tempfile`, `vcpkg`, `windows-sys`, `windows-targets`, `windows_aarch64_gnullvm`, `windows_aarch64_msvc`, `windows_i686_gnu`, `windows_i686_msvc`, `windows_x86_64_gnu`, `windows_x86_64_gnullvm`, `windows_x86_64_msvc` |

MIT license for `github_release_check`:

```
The MIT License (MIT)
Copyright (c) 2022 Matt Boulanger

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## doctest (tests only)

Copyright (c) 2016-2023 Viktor Kirilov. MIT license:
<https://github.com/doctest/doctest/blob/master/LICENSE.txt>.

## FreeBSD UFS on-disk layouts

The UFS reader adapts on-disk layouts and cylinder-group addressing from
FreeBSD `sys/ufs/ffs/fs.h`, `sys/ufs/ufs/dinode.h` and `sys/ufs/ufs/dir.h`.
Source: <https://cgit.freebsd.org/src/tree/sys/ufs>. BSD-2-Clause and
BSD-3-Clause. The upstream copyright notices and licenses follow:

```text
/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*-
 * SPDX-License-Identifier: (BSD-2-Clause AND BSD-3-Clause)
 *
 * Copyright (c) 2002 Networks Associates Technology, Inc.
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Marshall
 * Kirk McKusick and Network Associates Laboratories, the Security
 * Research Division of Network Associates, Inc. under DARPA/SPAWAR
 * contract N66001-01-C-8035 ("CBOSS"), as part of the DARPA CHATS
 * research program
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (c) 1982, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the authors may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
```
