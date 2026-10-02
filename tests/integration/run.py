#!/usr/bin/env python3
"""End-to-end tests for rarftp.

Creates archives with RARLAB's `rar`, Python's zipfile and tarfile, 7-Zip
(`7zz`) and Info-ZIP's `zip`, uploads them with rarftp to vsftpd running in Docker (image
delfer/alpine-ftp-server) and checks what arrives.

    python3 tests/integration/run.py --rarftp build/rarftp --rar /path/to/rar [--7z PATH] [--big] [--lib PATH]

With --lib (librarftpcore.dylib / .so / rarftpcore.dll, built with
-DRARFTP_BUILD_LIBRARY=ON) the `lib_*` tests also drive the shared library used
by rarftp-gui through its C API, with ctypes. Select them with `-k lib_`.

Needs Python 3.9+ (standard library only), Docker and `rar`. The tests that
need 7-Zip or Info-ZIP's `zip` are skipped without them; the Zstandard ZIP test
needs Python 3.14+.
"""

import argparse
import ctypes
import hashlib
import json
import lzma
import os
import random
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback
import unicodedata
import tarfile
import uuid
import zipfile
from pathlib import Path

IMAGE = "delfer/alpine-ftp-server"
USER = "tester"
PASSWORD = "secret"
HOME = f"/ftp/{USER}"  # Login directory on the server (not chrooted).
BASE_TIME = 1_700_000_000
ARCHIVE_PASSWORD = "s3cr3t pässwörd"
ZIP_AES_PASSWORD = "s3cr3t p4ssw0rd"  # 7-Zip refuses non-ASCII passwords for ZIP.


# --------------------------------------------------------------------------
# Helpers


class TestFailure(Exception):
    pass


class Skipped(Exception):
    pass


def check(condition, message, output=None):
    if not condition:
        if output:
            message += "\n----- rarftp output -----\n" + output.rstrip()[-4000:] + "\n-------------------------"
        raise TestFailure(message)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_snapshot(root, with_hashes=True):
    """{relative path: "dir" | (size, sha256)} for everything under root."""
    result = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames:
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            result[unicodedata.normalize("NFC", rel)] = "dir"
        for name in filenames:
            path = os.path.join(dirpath, name)
            if os.path.islink(path):
                continue
            rel = unicodedata.normalize("NFC", os.path.relpath(path, root))
            result[rel] = (os.path.getsize(path), sha256(path) if with_hashes else None)
    return result


def compare_trees(expected_root, actual_root, output, skip=()):
    expected = {k: v for k, v in tree_snapshot(expected_root).items() if k not in skip}
    check(Path(actual_root).is_dir(), f"{actual_root} was not created", output)
    actual = tree_snapshot(actual_root)
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    different = sorted(k for k in set(expected) & set(actual) if expected[k] != actual[k])
    check(not missing and not extra and not different,
          f"trees differ: missing={missing} extra={extra} different={different}", output)


def compare_mtimes(expected_root, actual_root, output):
    for dirpath, _, filenames in os.walk(expected_root):
        for name in filenames:
            src = Path(dirpath) / name
            dst = Path(actual_root) / src.relative_to(expected_root)
            delta = abs(src.stat().st_mtime - dst.stat().st_mtime)
            check(delta < 2, f"modification time of {dst} differs by {delta:.0f} s", output)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def free_port_range(count):
    for _ in range(200):
        base = random.randint(30000, 60000 - count)
        sockets = []
        try:
            for port in range(base, base + count):
                s = socket.socket()
                sockets.append(s)
                s.bind(("0.0.0.0", port))
            return base
        except OSError:
            continue
        finally:
            for s in sockets:
                s.close()
    raise RuntimeError("no free port range")


# --------------------------------------------------------------------------
# FTP server


class Server:
    """vsftpd (delfer/alpine-ftp-server) with user tester/secret, whose home
    directory is `data_dir` on this machine."""

    def __init__(self, data_dir):
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        user = f"{USER}|{PASSWORD}|{HOME}"
        if os.getuid() != 0:
            user += f"|{os.getuid()}"  # Uploaded files belong to us: readable and deletable here.
        for attempt in range(3):  # Retry if Docker finds one of the ports taken.
            pasv = free_port_range(10)
            control = free_port()
            self.name = f"rarftp-it-{uuid.uuid4().hex[:8]}"
            proc = subprocess.run(["docker", "run", "-d", "--rm", "--name", self.name,
                                   "-e", f"USERS={user}", "-e", f"MIN_PORT={pasv}", "-e", f"MAX_PORT={pasv + 9}",
                                   "-v", f"{self.data_dir}:{HOME}",
                                   "-p", f"{control}:21", "-p", f"{pasv}-{pasv + 9}:{pasv}-{pasv + 9}", IMAGE],
                                  capture_output=True, text=True)
            if proc.returncode == 0:
                break
            subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)
            if "port" not in proc.stderr or attempt == 2:
                raise RuntimeError(f"cannot start the FTP server container:\n{proc.stderr.strip()}")
        # On Linux the container address is routable, so active mode works
        # too; Docker Desktop (macOS, Windows) only offers published ports.
        self.routable = sys.platform.startswith("linux")
        if self.routable:
            self.host = subprocess.run(
                ["docker", "inspect", "-f", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", self.name],
                check=True, capture_output=True, text=True).stdout.strip()
            self.port = 21
        else:
            self.host = "127.0.0.1"
            self.port = control
        self.wait_ready()

    def wait_ready(self, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                with socket.create_connection((self.host, self.port), timeout=2) as s:
                    if s.recv(64).startswith(b"220"):
                        return
            except OSError:
                pass
            time.sleep(0.3)
        logs = subprocess.run(["docker", "logs", self.name], capture_output=True, text=True)
        raise RuntimeError(f"FTP server not ready:\n{logs.stdout}{logs.stderr}")

    def close(self):
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)


# --------------------------------------------------------------------------
# Fixtures


def write_tree(root, files, dirs=(), links=None):
    for rel, data in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for rel in dirs:
        (root / rel).mkdir(parents=True, exist_ok=True)
    for i, rel in enumerate(sorted(files)):
        stamp = BASE_TIME + i * 3601
        os.utime(root / rel, (stamp, stamp))
    for rel, target in (links or {}).items():
        os.symlink(target, root / rel)


def zip_tree(path, parent, what="tree", compression=zipfile.ZIP_DEFLATED):
    """Like `zip -r`: every file, directory (empty ones too) and symbolic link under parent/what ("*": all of
    parent), named relative to parent."""
    parent = Path(parent)
    roots = sorted(parent.iterdir()) if what == "*" else [parent / what]
    with zipfile.ZipFile(path, "w", compression) as z:
        for root in roots:
            for p in ([root, *sorted(root.rglob("*"))] if root.is_dir() and not root.is_symlink() else [root]):
                name = p.relative_to(parent).as_posix()
                if p.is_symlink():
                    info = zipfile.ZipInfo(name, time.localtime(p.lstat().st_mtime)[:6])
                    info.create_system = 3  # Unix: the mode below is meaningful.
                    info.external_attr = 0o120777 << 16
                    z.writestr(info, os.readlink(p))
                else:
                    z.write(p, name)


def split_file(path, parts):
    """Cuts `path` into `parts` numbered files (path.001, ...), as 7-Zip's -v switch does."""
    data = Path(path).read_bytes()
    size = -(-len(data) // parts)
    for i in range(parts):
        Path(f"{path}.{i + 1:03}").write_bytes(data[i * size:(i + 1) * size])


def exfat_fixtures(archives):
    """Use volumes produced by hdiutil; never construct filesystem structures in tests."""
    with zipfile.ZipFile(Path(__file__).resolve().parents[1] / "fixtures" / "exfat.zip") as fixture:
        for source, destination in (("volume.exfat", "basic.exfat"), ("partitioned.exfat", "disk.exfat")):
            with fixture.open(source) as src, (archives / destination).open("wb") as dst:
                shutil.copyfileobj(src, dst)


def check_exfat_tree(root, output):
    manifest = json.loads((Path(__file__).resolve().parents[1] / "fixtures" / "exfat.json").read_text())
    expected = {name: (info["size"], info["sha256"]) for name, info in manifest["files"].items()}
    expected.update({name: "dir" for name in manifest["directories"]})
    actual = tree_snapshot(root)
    check(actual == expected, "exFAT directory structure, file sizes or hashes differ", output)
    for name in ("hello.txt", "large.bin", "fragmented.bin", "uninitialized.bin"):
        check(abs((Path(root) / name).stat().st_mtime - BASE_TIME) < 2, f"wrong mtime for {name}", output)


def build_fixtures(work, rar, big, sevenzip, infozip, lz4):
    rnd = random.Random(20260929)
    trees = work / "trees"
    archives = work / "archives"
    archives.mkdir(parents=True)
    exfat_fixtures(archives)

    main_tree = trees / "main" / "tree"
    write_tree(main_tree, {
        "empty.txt": b"",
        "one.bin": b"\x00",
        "small.txt": b"hello, world\n",
        "random-5M.bin": rnd.randbytes(5 << 20),
        "text-8M.txt": (b"The quick brown fox jumps over the lazy dog. 0123456789\n" * 160000)[: 8 << 20],
        "sub/dir/deep.bin": rnd.randbytes(300_000),
        "ação ç/ñandú 日本語.txt": "unicode ✓\n".encode(),
        "spaces and #hash; 50% ?.txt": b"special characters\n",
    }, dirs=["empty-dir", "sub/empty-nested"])

    # Several top-level entries, deep nesting, many siblings, empty directories
    # at every depth and names that need escaping.
    dirs_tree = trees / "dirs"
    dirs_files = {
        "root-file.txt": b"at the archive root\n",
        "a/a1.txt": b"a1\n",
        "a/b/c/sibling.bin": rnd.randbytes(70_000),
        "a/b/c/d/e/f/g/h/deep.txt": b"eight levels down\n",
        "Z/z.txt": b"upper-case directory\n",
        "dir with spaces/sub dir #1/file 1.txt": b"one\n",
        "dir with spaces/sub dir #2/file 2.txt": b"two\n",
        "ção/ñ/日本/unicode.txt": "unicode directories ✓\n".encode(),
        "100%/[brackets]/{braces}/a;b=c.txt": b"reserved characters\n",
    }
    for i in range(60):
        dirs_files[f"many/{i:03}/f.txt"] = f"file {i}\n".encode()
    write_tree(dirs_tree, dirs_files, dirs=["empty-top", "a/b/empty-nested", "a/b/c/d/e/empty-deep",
                                            "many/030/empty-leaf"])

    links_tree = trees / "links" / "tree"
    shared = rnd.randbytes(200_000)
    write_tree(links_tree, {"original.bin": shared, "zz-identical-copy.bin": shared, "plain.txt": b"plain\n"},
               links={"link-to-plain": "plain.txt"})

    tiny_tree = trees / "tiny" / "tree"
    write_tree(tiny_tree, {"tiny.txt": b"tiny\n"})

    # Long enough to interrupt: 4 GiB of zeros (sparse, compresses to little).
    slow_tree = trees / "slow" / "tree"
    slow_tree.mkdir(parents=True)
    with open(slow_tree / "zeros.bin", "wb") as f:
        f.truncate(4 << 30)

    def rar_a(name, cwd, *switches, what="tree"):
        subprocess.run([rar, "a", "-r", "-idq", "-y", *switches, str(archives / name), what], cwd=cwd, check=True)

    main_parent = trees / "main"
    rar_a("basic.rar", main_parent, "-m3")
    rar_a("solid.rar", main_parent, "-s", "-m5")
    rar_a("multivolume.rar", main_parent, "-m1", "-v1m")
    rar_a("solid-multivolume.rar", main_parent, "-s", "-m5", "-v2m")
    try:  # RAR 7 can no longer create RAR 4.x archives.
        rar_a("rar4.rar", main_parent, "-ma4", "-m3")
    except subprocess.CalledProcessError:
        pass
    rar_a("store.rar", main_parent, "-m0")
    rar_a("encrypted.rar", main_parent, f"-p{ARCHIVE_PASSWORD}")
    rar_a("encrypted-headers.rar", main_parent, f"-hp{ARCHIVE_PASSWORD}")
    rar_a("dirs.rar", dirs_tree, "-m3", what="*")
    rar_a("dirs-solid-multivolume.rar", dirs_tree, "-s", "-m5", "-v20k", what="*")
    rar_a("links.rar", trees / "links", "-ol", "-oi")
    rar_a("tiny.rar", trees / "tiny")
    rar_a("slow.rar", trees / "slow", "-m1")

    # ZIP, with Python's zipfile: every compression method it writes.
    zip_tree(archives / "basic.zip", main_parent)
    zip_tree(archives / "store.zip", main_parent, compression=zipfile.ZIP_STORED)
    zip_tree(archives / "bzip2.zip", main_parent, compression=zipfile.ZIP_BZIP2)
    zip_tree(archives / "lzma.zip", main_parent, compression=zipfile.ZIP_LZMA)
    if hasattr(zipfile, "ZIP_ZSTANDARD"):  # Python 3.14+.
        zip_tree(archives / "zstd.zip", main_parent, compression=zipfile.ZIP_ZSTANDARD)
    zip_tree(archives / "dirs.zip", dirs_tree, what="*")
    zip_tree(archives / "links.zip", trees / "links")
    zip_tree(archives / "tiny.zip", trees / "tiny")

    # tar, with Python's tarfile: plain, compressed in every way it can (the old
    # .lzma format and lz4 by hand), and split into parts.
    with tarfile.open(archives / "basic.tar", "w") as tar:
        tar.add(main_tree, arcname="tree")
    for suffix, mode in (("gz", "w:gz"), ("bz2", "w:bz2"), ("xz", "w:xz"), ("zst", "w:zst")):
        if mode == "w:zst" and sys.version_info < (3, 14):
            continue
        with tarfile.open(archives / f"basic.tar.{suffix}", mode) as tar:
            tar.add(main_tree, arcname="tree")
    (archives / "basic.tar.lzma").write_bytes(lzma.compress((archives / "basic.tar").read_bytes(),
                                                            format=lzma.FORMAT_ALONE))
    if lz4:
        subprocess.run([lz4, "-q", "-f", str(archives / "basic.tar"), str(archives / "basic.tar.lz4")], check=True)
    for name in ("split.tar", "split.tar.gz"):
        shutil.copy(archives / name.replace("split", "basic"), archives / name)
        split_file(archives / name, 3)
        (archives / name).unlink()

    # Info-ZIP's zip: Unicode names in its own extra field, traditional (ZipCrypto) encryption.
    if infozip:
        def zip_r(name, cwd, *switches):
            subprocess.run([infozip, "-r", "-q", "-y", *switches, str(archives / name), "tree"], cwd=cwd, check=True)

        zip_r("infozip.zip", main_parent)
        zip_r("zipcrypto.zip", main_parent, "-P", ARCHIVE_PASSWORD)

    # 7-Zip: 7z (solid by default) and the ZIP variants Python cannot write.
    if sevenzip:
        def sz_a(name, cwd, *switches, what="tree"):
            subprocess.run([sevenzip, "a", "-bd", "-bso0", "-bsp0", "-y", *switches, str(archives / name), what],
                           cwd=cwd, check=True)

        sz_a("basic.7z", main_parent)
        sz_a("multivolume.7z", main_parent, "-v2m")
        sz_a("bzip2.7z", main_parent, "-m0=BZip2")
        sz_a("deflate.7z", main_parent, "-m0=Deflate")
        sz_a("deflate64.7z", main_parent, "-m0=Deflate64")
        sz_a("ppmd.7z", main_parent, "-m0=PPMd")
        sz_a("dirs.7z", dirs_tree, what="*")
        sz_a("bcj2.7z", main_parent, "-mf=BCJ2")
        sz_a("encrypted.7z", main_parent, f"-p{ARCHIVE_PASSWORD}")
        sz_a("encrypted-headers.7z", main_parent, f"-p{ARCHIVE_PASSWORD}", "-mhe=on")
        sz_a("encrypted-nonsolid.7z", main_parent, f"-p{ARCHIVE_PASSWORD}", "-ms=off")
        sz_a("encrypted-multivolume.7z", main_parent, f"-p{ARCHIVE_PASSWORD}", "-mhe=on", "-v2m")
        sz_a("encrypted-bzip2.7z", main_parent, f"-p{ARCHIVE_PASSWORD}", "-m0=BZip2")
        sz_a("encrypted-deflate.7z", main_parent, f"-p{ARCHIVE_PASSWORD}", "-m0=Deflate", "-mhe=on")
        sz_a("aes.zip", main_parent, "-tzip", "-mem=AES256", f"-p{ZIP_AES_PASSWORD}")
        sz_a("split.zip", main_parent, "-tzip", "-v2m")
        sz_a("deflate64.zip", main_parent, "-tzip", "-mm=Deflate64")

    big_tree = None
    if big:
        big_tree = trees / "big" / "tree"
        big_tree.mkdir(parents=True)
        # 4.5 GiB: above the 32-bit limit. Mostly compressible so the archive
        # is quick to build, with random blocks so it is not trivial.
        block = rnd.randbytes(1 << 20)
        with open(big_tree / "huge.bin", "wb") as f:
            for i in range(4608):
                f.write(block if i % 16 == 0 else bytes(1 << 20))
        rar_a("big.rar", trees / "big", "-m1")
        zip_tree(archives / "big.zip", trees / "big")  # Zip64: a file over 4 GiB.

    return {"main": main_tree, "dirs": dirs_tree, "links": links_tree, "tiny": tiny_tree,
            "slow": slow_tree, "big": big_tree, "archives": archives}


# --------------------------------------------------------------------------
# C API of librarftpcore (src/capi/rarftp.h), through ctypes


class RarftpJobConfig(ctypes.Structure):
    """rarftp_job_config: same fields, same order as in rarftp.h."""

    _fields_ = [
        ("archive", ctypes.c_char_p),
        ("archive_password", ctypes.c_char_p),
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_int),
        ("active_mode", ctypes.c_int),
        ("user", ctypes.c_char_p),
        ("password", ctypes.c_char_p),
        ("directory", ctypes.c_char_p),
        ("mkdir", ctypes.c_int),
        ("verbose", ctypes.c_int),
        ("buffer_mib", ctypes.c_uint),
    ]


class Library:
    """librarftpcore loaded with the prototypes of rarftp.h."""

    def __init__(self, path):
        self.path = str(path)
        dll = ctypes.CDLL(self.path)
        # The job is an opaque pointer; strings returned by the library are
        # read through c_void_p so that they can be handed back to rarftp_free.
        dll.rarftp_version.argtypes = []
        dll.rarftp_version.restype = ctypes.c_char_p  # Static storage: a copy is fine.
        dll.rarftp_job_start.argtypes = [ctypes.POINTER(RarftpJobConfig)]
        dll.rarftp_job_start.restype = ctypes.c_void_p
        dll.rarftp_job_start_with_units.argtypes = [ctypes.POINTER(RarftpJobConfig), ctypes.c_int]
        dll.rarftp_job_start_with_units.restype = ctypes.c_void_p
        dll.rarftp_job_poll.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        dll.rarftp_job_poll.restype = ctypes.c_void_p
        dll.rarftp_job_answer_password.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        dll.rarftp_job_answer_password.restype = None
        dll.rarftp_job_cancel.argtypes = [ctypes.c_void_p]
        dll.rarftp_job_cancel.restype = None
        dll.rarftp_job_free.argtypes = [ctypes.c_void_p]
        dll.rarftp_job_free.restype = None
        dll.rarftp_free.argtypes = [ctypes.c_void_p]
        dll.rarftp_free.restype = None
        self.dll = dll

    def version(self):
        return self.dll.rarftp_version().decode("utf-8")


LIB_STATE_KEYS = {"phase", "cancelling", "prompt", "archive", "target", "mode", "user", "probe", "progress",
                  "log", "result"}
LIB_PHASES = ["reading", "connecting", "checking", "transferring", "finished"]
LIB_LEVELS = ["debug", "info", "warn", "error"]
LIB_ARCHIVE_KEYS = {"name", "format", "compression", "files", "bytes", "bytes_text", "volumes", "solid",
                    "encrypted"}
LIB_PROGRESS_INTS = ["total_files", "total_bytes", "archive_read", "archive_size", "sent_bytes", "unpacked_bytes",
                     "files_done", "files_skipped",
                     "skipped_bytes", "buffer_used", "buffer_capacity", "current_number", "current_size",
                     "current_sent"]
LIB_PROGRESS_NUMBERS = ["elapsed", "upload_rate", "average_rate", "unpack_rate", "eta_file", "eta_total"]
LIB_PROGRESS_TEXTS = ["total_bytes", "archive_read", "archive_size", "sent_bytes", "skipped_bytes", "current_size",
                      "current_sent", "buffer_capacity", "upload_rate", "average_rate", "unpack_rate", "eta_file",
                      "eta_total", "elapsed"]
LIB_RESULT_INTS = ["files_uploaded", "bytes_uploaded", "skipped_files", "skipped_bytes", "ignored",
                   "problems_dropped"]
LIB_CONFIG_DEFAULTS = {"archive": None, "archive_password": None, "host": None, "port": 21, "active_mode": 0,
                       "user": None, "password": None, "directory": None, "mkdir": 0, "verbose": 0,
                       "buffer_mib": 0}


def is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


class LibJob:
    """A rarftp_job: polls it (checking the JSON contract at every poll), collects its log through the
    cursor and answers the archive password prompt. Use it as a context manager: leaving it frees the
    job, which cancels and waits for it if it is still running."""

    def __init__(self, lib, config, si_units=False):
        unknown = set(config) - set(LIB_CONFIG_DEFAULTS)
        check(not unknown, f"unknown job config keys: {sorted(unknown)}")
        values = dict(LIB_CONFIG_DEFAULTS, **config)
        # The library copies the strings when the job starts; every string is UTF-8.
        self._config = RarftpJobConfig(**{key: value.encode("utf-8") if isinstance(value, str) else value
                                          for key, value in values.items()})
        self.lib = lib
        self.cursor = 0  # Sequence number of the next log line to fetch.
        self.log = []  # Every log line seen, oldest first.
        self.prompts = []  # Every password prompt seen, oldest first.
        self.state = None  # The last state polled.
        self._phase = 0
        self._had_progress = False
        self.handle = (lib.dll.rarftp_job_start_with_units(ctypes.byref(self._config), 1) if si_units
                       else lib.dll.rarftp_job_start(ctypes.byref(self._config)))
        check(self.handle, "rarftp_job_start returned NULL")

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()

    def close(self):
        if self.handle:
            self.lib.dll.rarftp_job_free(self.handle)
            self.handle = None

    def cancel(self):
        self.lib.dll.rarftp_job_cancel(self.handle)

    def answer_password(self, password):
        """None declines the prompt."""
        data = None if password is None else password.encode("utf-8")
        self.lib.dll.rarftp_job_answer_password(self.handle, data)

    @property
    def result(self):
        return self.state["result"] if self.state else None

    def describe(self):
        """The last state and log lines, as context for a failed check."""
        state = {key: value for key, value in (self.state or {}).items() if key != "log"}
        lines = [f"{line['time']} {line['level'].upper():5} {line['text']}" for line in self.log[-40:]]
        return "last state: " + json.dumps(state, indent=1, ensure_ascii=False) + "\nlog:\n" + "\n".join(lines)

    def _fetch(self, cursor):
        raw = self.lib.dll.rarftp_job_poll(self.handle, cursor)
        check(raw, "rarftp_job_poll returned NULL")
        try:
            text = ctypes.string_at(raw).decode("utf-8")  # Read, then hand the memory back to the library.
        finally:
            self.lib.dll.rarftp_free(raw)
        try:
            state = json.loads(text)
        except ValueError as error:
            raise TestFailure(f"rarftp_job_poll returned invalid JSON ({error}): {text[:500]!r}") from error
        check(isinstance(state, dict), f"the state is not a JSON object: {text[:200]!r}")
        return state

    def poll(self):
        """One poll from the current log cursor; returns the state."""
        state = self._fetch(self.cursor)
        self._check_state(state)
        log = state["log"]
        self.log.extend(log["lines"])
        self.cursor = log["next"]
        self.state = state
        return state

    def _check_state(self, state):
        def ok(condition, message):
            if not condition:
                check(False, f"{message} (polling from cursor {self.cursor})",
                      json.dumps(state, indent=1, ensure_ascii=False))

        ok(set(state) == LIB_STATE_KEYS, f"top-level keys differ: {sorted(set(state) ^ LIB_STATE_KEYS)}")
        ok(state["phase"] in LIB_PHASES, f"unknown phase {state['phase']!r}")
        phase = LIB_PHASES.index(state["phase"])
        ok(phase >= self._phase, f"phase went back to {state['phase']!r}")
        self._phase = phase
        finished = state["phase"] == "finished"
        ok(isinstance(state["cancelling"], bool), "cancelling is not a boolean")
        ok(state["mode"] in ("passive", "active"), f"unknown mode {state['mode']!r}")
        ok(isinstance(state["user"], str) and state["user"], "user is not a non-empty string")

        prompt = state["prompt"]
        if prompt is not None:
            ok(not finished, "a prompt is pending in the finished state")
            ok(set(prompt) == {"kind", "archive", "error"}, f"prompt keys differ: {sorted(prompt)}")
            ok(prompt["kind"] == "archive_password", f"unknown prompt kind {prompt['kind']!r}")
            ok(isinstance(prompt["archive"], str), "prompt.archive is not a string")
            ok(prompt["error"] is None or isinstance(prompt["error"], str), "prompt.error is not null or a string")

        archive = state["archive"]
        if archive is not None:
            ok(set(archive) == LIB_ARCHIVE_KEYS, f"archive keys differ: {sorted(set(archive) ^ LIB_ARCHIVE_KEYS)}")
            ok(isinstance(archive["name"], str), "archive.name is not a string")
            ok(archive["format"] in ("RAR", "ZIP", "7z", "tar", "exFAT"), f"unknown archive format {archive['format']!r}")
            ok(archive["compression"] in (None, "gzip", "bzip2", "xz", "lzma", "zstd", "lz4"),
               f"unknown compression {archive['compression']!r}")
            # A compressed tar is not listed first: its contents are unknown.
            unknown = archive["compression"] is not None
            ok(all((archive[key] is None) if unknown else (is_int(archive[key]) and archive[key] >= 0)
                   for key in ("files", "bytes")), "archive.files/bytes must be integers, or null exactly when "
                                                   "the archive is compressed")
            ok((archive["bytes_text"] is None) if unknown else isinstance(archive["bytes_text"], str),
               "archive.bytes_text")
            ok(is_int(archive["volumes"]) and archive["volumes"] >= 1, "archive.volumes is not a positive integer")
            ok(isinstance(archive["solid"], bool) and isinstance(archive["encrypted"], bool),
               "archive flags are not booleans")
        ok(state["target"] is None or isinstance(state["target"], str), "target is not null or a string")

        probe = state["probe"]
        if probe is not None:
            ok(set(probe) == {"done", "total"} and is_int(probe["done"]) and is_int(probe["total"]),
               f"bad probe {probe!r}")
            ok(0 <= probe["done"] <= probe["total"], f"probe out of range {probe!r}")

        progress = state["progress"]
        if progress is None:
            ok(not self._had_progress, "progress went back to null")
            ok(state["phase"] != "transferring", "progress is null while transferring")
        else:
            self._had_progress = True
            ok(phase >= LIB_PHASES.index("transferring"), f"progress while {state['phase']}")
            expected = set(LIB_PROGRESS_INTS + LIB_PROGRESS_NUMBERS + ["current_file", "activity", "text",
                                                                        "totals_known"])
            ok(set(progress) == expected, f"progress keys differ: {sorted(set(progress) ^ expected)}")
            ok(all(is_int(progress[key]) and progress[key] >= 0 for key in LIB_PROGRESS_INTS),
               "progress counters are not non-negative integers")
            ok(all(is_number(progress[key]) for key in LIB_PROGRESS_NUMBERS),
               "progress rates/ETAs are not numbers")
            ok(isinstance(progress["current_file"], str) and isinstance(progress["activity"], str),
               "current_file/activity are not strings")
            ok(isinstance(progress["totals_known"], bool), "totals_known is not a boolean")
            ok(progress["totals_known"] or (archive is not None and archive["compression"] is not None),
               "totals unknown for an archive that was listed")
            ok(progress["archive_read"] <= progress["archive_size"] or progress["archive_size"] == 0,
               "archive_read exceeds archive_size")
            ok(progress["sent_bytes"] <= progress["total_bytes"], "sent_bytes exceeds total_bytes")
            ok(progress["files_done"] <= progress["total_files"], "files_done exceeds total_files")
            text = progress["text"]
            ok(set(text) == set(LIB_PROGRESS_TEXTS),
               f"progress.text keys differ: {sorted(set(text) ^ set(LIB_PROGRESS_TEXTS))}")
            ok(all(isinstance(text[key], str) and text[key] for key in LIB_PROGRESS_TEXTS),
               "progress.text values are not non-empty strings")
            for key in ("eta_file", "eta_total", "elapsed"):
                ok(re.fullmatch(r"\d\d+:\d\d:\d\d|--:--:--", text[key]), f"progress.text.{key} = {text[key]!r}")

        log = state["log"]
        ok(set(log) == {"next", "lines"}, f"log keys differ: {sorted(log)}")
        ok(is_int(log["next"]) and log["next"] >= self.cursor, f"log.next went backwards ({log['next']})")
        ok(isinstance(log["lines"], list), "log.lines is not an array")
        last = self.cursor - 1
        for line in log["lines"]:
            ok(set(line) == {"seq", "time", "level", "text"}, f"log line keys differ: {sorted(line)}")
            ok(is_int(line["seq"]) and last < line["seq"] < log["next"] and line["seq"] >= self.cursor,
               f"log line seq {line['seq']} out of order")
            ok(re.fullmatch(r"\d\d:\d\d:\d\d", line["time"]) is not None, f"log time {line['time']!r}")
            ok(line["level"] in LIB_LEVELS, f"log level {line['level']!r}")
            ok(isinstance(line["text"], str), "log text is not a string")
            last = line["seq"]
        if log["lines"]:
            ok(last == log["next"] - 1, "the last log line is not the one before log.next")

        result = state["result"]
        ok((result is not None) == finished, "result must be null exactly until the job has finished")
        if result is not None:
            expected = set(LIB_RESULT_INTS + ["status", "error", "seconds", "summary", "problems"])
            ok(set(result) == expected, f"result keys differ: {sorted(set(result) ^ expected)}")
            ok(result["status"] in ("success", "failed", "cancelled"), f"unknown status {result['status']!r}")
            ok(isinstance(result["error"], str), "result.error is not a string")
            ok((result["status"] == "failed") == bool(result["error"]),
               "result.error must be set exactly on failure")
            ok(all(is_int(result[key]) and result[key] >= 0 for key in LIB_RESULT_INTS),
               "result counters are not non-negative integers")
            ok(is_number(result["seconds"]), "result.seconds is not a number")
            summary = result["summary"]
            ok(isinstance(summary, list) and summary and all(isinstance(line, str) and line for line in summary),
               "result.summary must be a non-empty array of strings")
            if result["status"] == "failed":
                ok(summary[0] == f"FAILED: {result['error']}", "summary does not start with the failure")
            ok(isinstance(result["problems"], list), "result.problems is not an array")
            for line in result["problems"]:
                ok(set(line) == {"seq", "time", "level", "text"}, f"problem keys differ: {sorted(line)}")
                ok(line["level"] in ("warn", "error"), f"problem level {line['level']!r}")

    def wait(self, on_prompt=None, on_state=None, timeout=900):
        """Polls every 50 ms until the job has finished. `on_prompt(prompt)` returns the archive password to
        answer a prompt with (None declines it); without it a prompt fails the test. `on_state(job, state)`
        sees every state. Returns the final state, with every log line fetched."""
        deadline = time.monotonic() + timeout
        while True:
            state = self.poll()
            prompt = state["prompt"]
            if prompt is not None:
                self.prompts.append(prompt)
                check(on_prompt is not None, f"unexpected password prompt {prompt}", self.describe())
                self.answer_password(on_prompt(prompt))
            if on_state is not None:
                on_state(self, state)
            if state["phase"] == "finished":
                break
            check(time.monotonic() < deadline, f"the job did not finish within {timeout} s", self.describe())
            time.sleep(0.05)

        # A poll reads the log just before the state, so the state that says "finished" can lack the last
        # lines; one more poll has them all. Nothing is logged after that.
        state = self.poll()
        check(state["phase"] == "finished" and state["prompt"] is None and state["cancelling"] is False,
              "the finished state changed", self.describe())
        again = self.poll()
        check(again["log"]["lines"] == [] and again["log"]["next"] == self.cursor
              and again["result"] == state["result"], "the finished state is not stable", self.describe())
        # Fetching everything from the start gives exactly the lines collected through the cursor.
        everything = self._fetch(0)["log"]
        check(everything["next"] == self.cursor and everything["lines"] == self.log,
              "the log collected through the cursor differs from the whole log", self.describe())
        return state


def lib_log_text(job):
    return "\n".join(line["text"] for line in job.log)


# --------------------------------------------------------------------------
# Runner


class Env:
    def __init__(self, args, work):
        self.rarftp = str(Path(args.rarftp).resolve())
        self.sevenzip = shutil.which(args.sevenzip) if args.sevenzip else (shutil.which("7zz") or shutil.which("7z"))
        self.infozip = shutil.which("zip")
        self.lz4 = shutil.which("lz4")
        self.lib = Library(Path(args.lib).resolve()) if args.lib else None  # Fails fast on a bad library.
        self.work = work
        self.server = Server(work / "ftp")  # First: fails fast without Docker.
        try:
            self.fixtures = build_fixtures(work, args.rar, args.big, self.sevenzip, self.infozip, self.lz4)
        except BaseException:
            self.server.close()
            raise

    def archive(self, name):
        return str(self.fixtures["archives"] / name)

    def volumes(self, stem):
        # rar numbers volumes part1.. or part01.. depending on their count.
        return sorted(p.name for p in self.fixtures["archives"].glob(f"{stem}.part*.rar"))

    def require(self, archive):
        """Skips the test when the archive could not be made (no 7-Zip, no zip, an old Python)."""
        if not Path(self.archive(archive)).exists():
            raise Skipped(f"{archive} was not created (needs 7-Zip, Info-ZIP's zip or a newer Python)")

    def remote(self, *parts):
        """Local view of a path in the FTP user's home directory."""
        return self.server.data_dir.joinpath(*parts)

    def command(self, archive, *args, login=True, host=None, no_tui=True):
        cmd = [self.rarftp, "--file", self.archive(archive), "--host", host or self.server.host,
               "--port", str(self.server.port)]
        if login:
            cmd += ["--user", USER, "--password", PASSWORD]
        if no_tui:
            cmd.append("--no-tui")
        return cmd + list(args)

    def run(self, archive, *args, expect=0, login=True, host=None, no_tui=True, timeout=900):
        cmd = self.command(archive, *args, login=login, host=host, no_tui=no_tui)
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
        output = proc.stdout + proc.stderr
        check(proc.returncode == expect, f"exit code {proc.returncode}, expected {expect}", output)
        return output

    def lib_config(self, archive, **overrides):
        """Job config for the C API: this server, the tester login and `overrides`."""
        config = {"archive": self.archive(archive), "host": self.server.host, "port": self.server.port,
                  "user": USER, "password": PASSWORD}
        config.update(overrides)
        return config

    def lib_run(self, archive, expect="success", on_prompt=None, on_state=None, timeout=900, si_units=False, **overrides):
        """Runs a library job to its end and returns the LibJob (its state, log and prompts)."""
        with LibJob(self.lib, self.lib_config(archive, **overrides), si_units=si_units) as job:
            job.wait(on_prompt, on_state, timeout)
        result = job.result
        check(result["status"] == expect, f"status {result['status']!r}, expected {expect!r}: {result['error']}",
              job.describe())
        return job


def test_unrar_is_linked_statically(env):
    if sys.platform == "darwin":
        deps = subprocess.run(["otool", "-L", env.rarftp], capture_output=True, text=True).stdout
    elif sys.platform.startswith("linux"):
        deps = subprocess.run(["ldd", env.rarftp], capture_output=True, text=True).stdout
    else:
        raise Skipped("no otool/ldd")
    check("unrar" not in deps.lower(), f"rarftp loads UnRAR as a shared library:\n{deps}")
    symbols = subprocess.run(["nm", env.rarftp], capture_output=True, text=True).stdout
    for name in ("RAROpenArchiveEx", "RARReadHeaderEx", "RARProcessFileW"):
        check(re.search(rf"\s[Tt]\s_?{name}$", symbols, re.MULTILINE), f"{name} is not defined inside rarftp")


def test_basic_upload(env):
    out = env.run("basic.rar", "--directory", "basic", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("basic", "tree"), out)
    compare_mtimes(env.fixtures["main"], env.remote("basic", "tree"), out)
    check(f"Logged in as {USER}" in out, "no login message", out)
    check(f"Destination: ftp://{env.server.host}:{env.server.port}{HOME}/basic" in out,
          "relative --directory not resolved against the login directory", out)


def test_rerun_skips_identical_files(env):
    out = env.run("basic.rar", "--directory", "basic")
    check("To upload: 0 file(s)" in out, "files were uploaded again", out)
    check("Skipped 8 file(s)" in out, "skipped count missing", out)


def test_changed_size_is_uploaded_again(env):
    with open(env.remote("basic", "tree", "random-5M.bin"), "r+b") as f:
        f.truncate(1234)
    time.sleep(1)  # Docker Desktop and OrbStack bind mounts can show the container a stale view for a moment.
    out = env.run("basic.rar", "--directory", "basic")
    check("To upload: 1 file(s)" in out, "expected exactly one upload", out)
    compare_trees(env.fixtures["main"], env.remote("basic", "tree"), out)


def test_solid(env):
    out = env.run("solid.rar", "--directory", f"{HOME}/solid", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("solid", "tree"), out)


def test_active_mode(env):
    if not env.server.routable:
        raise Skipped("active mode needs a routable server address (Linux); Docker Desktop only publishes ports")
    out = env.run("basic.rar", "--mode", "active", "--directory", "active", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("active", "tree"), out)
    check("active mode" in out, "active mode not reported", out)


def test_multivolume(env):
    volumes = env.volumes("multivolume")
    check(len(volumes) >= 3, f"expected several volumes, got {volumes}")
    out = env.run(volumes[0], "--directory", "multi", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("multi", "tree"), out)
    check(f"Reading volume {volumes[1]}" in out, "volume change not logged", out)


def test_solid_multivolume(env):
    out = env.run(env.volumes("solid-multivolume")[0], "--directory", "solid-multi", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("solid-multi", "tree"), out)


def test_many_directories(env):
    out = env.run("dirs.rar", "--directory", "dirs", "--mkdir")
    compare_trees(env.fixtures["dirs"], env.remote("dirs"), out)
    compare_mtimes(env.fixtures["dirs"], env.remote("dirs"), out)


def test_many_directories_solid_multivolume(env):
    volumes = env.volumes("dirs-solid-multivolume")
    check(len(volumes) >= 2, f"expected several volumes, got {volumes}")
    out = env.run(volumes[0], "--directory", "dirs-solid", "--mkdir")
    compare_trees(env.fixtures["dirs"], env.remote("dirs-solid"), out)


def test_many_directories_rerun_and_repair(env):
    out = env.run("dirs.rar", "--directory", "dirs")
    check("To upload: 0 file(s)" in out, "files were uploaded again", out)
    check("Skipped 69 file(s)" in out, "skipped count missing", out)
    # Damage the remote copy: drop a whole subtree (with a file several levels
    # below it), an empty directory and a leaf file; shorten another file.
    shutil.rmtree(env.remote("dirs", "a", "b", "c"))
    shutil.rmtree(env.remote("dirs", "empty-top"))
    env.remote("dirs", "many", "059", "f.txt").unlink()
    with open(env.remote("dirs", "dir with spaces", "sub dir #2", "file 2.txt"), "r+b") as f:
        f.truncate(1)
    out = env.run("dirs.rar", "--directory", "dirs")
    check("To upload: 4 file(s)" in out, "expected exactly the four damaged files", out)
    compare_trees(env.fixtures["dirs"], env.remote("dirs"), out)


def test_rar4_format(env):
    if not Path(env.archive("rar4.rar")).exists():
        raise Skipped("this rar version cannot create RAR 4.x archives")
    out = env.run("rar4.rar", "--directory", "rar4", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("rar4", "tree"), out)


def test_encrypted_files(env):
    out = env.run("encrypted.rar", "--directory", "enc", "--mkdir", expect=1)
    check("--archive-password" in out, "no hint about --archive-password", out)
    out = env.run("encrypted.rar", "--directory", "enc", "--mkdir", "--archive-password", ARCHIVE_PASSWORD)
    compare_trees(env.fixtures["main"], env.remote("enc", "tree"), out)


def test_encrypted_headers(env):
    out = env.run("encrypted-headers.rar", expect=1)
    check("--archive-password" in out, "no hint about --archive-password", out)
    out = env.run("encrypted-headers.rar", "--archive-password", "wrong", expect=1)
    check("wrong" in out.lower(), "wrong password not reported", out)
    # --rar-password is still accepted, as another name for --archive-password.
    out = env.run("encrypted-headers.rar", "--directory", "enc-headers", "--mkdir", "--rar-password",
                  ARCHIVE_PASSWORD)
    compare_trees(env.fixtures["main"], env.remote("enc-headers", "tree"), out)
    out = env.run("tiny.rar", "--rar-password", "a", "--archive-password", "b", expect=2)
    check("pass only one" in out, "both password options accepted", out)


def test_anonymous_login_is_attempted(env):
    # Without --user rarftp logs in anonymously, which this server refuses.
    out = env.run("tiny.rar", "--directory", "anon", login=False, expect=1)
    check("cannot log in" in out and "530" in out, "anonymous login refusal not reported", out)


def test_wrong_ftp_password(env):
    cmd = [env.rarftp, "--file", env.archive("tiny.rar"), "--host", env.server.host, "--port",
           str(env.server.port), "--user", USER, "--password", "not-the-password", "--no-tui"]
    proc = subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)
    out = proc.stdout + proc.stderr
    check(proc.returncode == 1 and "cannot log in" in out, "wrong FTP password not reported", out)


def test_missing_directory_is_an_error(env):
    out = env.run("tiny.rar", "--directory", "does-not-exist", expect=1)
    check("does not exist (use --mkdir" in out, "missing directory not reported", out)
    check(not env.remote("does-not-exist").exists(), "directory created without --mkdir", out)


def test_mkdir_is_not_recursive(env):
    out = env.run("tiny.rar", "--directory", "a1/b/c", "--mkdir", expect=1)
    check(f"cannot create remote directory {HOME}/a1/b/c" in out, "MKD failure not reported", out)


def test_default_directory_warning(env):
    # Also without --no-tui: stdout is a pipe, so plain output is used anyway.
    out = env.run("tiny.rar", no_tui=False)
    check(f"no --directory given: uploading to the server's default directory {HOME}" in out,
          "default directory warning missing", out)
    check(env.remote("tree", "tiny.txt").read_bytes() == b"tiny\n", "file not in the login directory", out)


def test_relative_directory(env):
    out = env.run("tiny.rar", "--directory", "x/../rel", "--mkdir")
    check(env.remote("rel", "tree", "tiny.txt").is_file(), "relative directory not resolved", out)


def test_corrupted_archive(env):
    data = bytearray(Path(env.archive("store.rar")).read_bytes())
    source = (env.fixtures["main"] / "random-5M.bin").read_bytes()
    offset = data.find(source[3 << 20:(3 << 20) + 64])
    check(offset > 0, "could not locate file data in store.rar")
    data[offset] ^= 0xFF
    (env.fixtures["archives"] / "corrupted.rar").write_bytes(bytes(data))
    out = env.run("corrupted.rar", "--directory", "corrupted", "--mkdir", expect=1)
    check("checksum" in out, "checksum error not reported", out)
    check(not env.remote("corrupted", "tree", "random-5M.bin").exists(), "corrupt file left on the server", out)


def test_missing_volume(env):
    volumes = env.volumes("multivolume")
    partial = env.fixtures["archives"] / "partial"
    partial.mkdir()
    for name in volumes:
        if name != volumes[2]:
            shutil.copy(env.archive(name), partial / name)
    out = env.run(f"partial/{volumes[0]}", expect=1)
    check("volume not found" in out, "missing volume not reported", out)


def test_not_first_volume(env):
    out = env.run(env.volumes("multivolume")[1], expect=1)
    check("not the first volume" in out, "not-first volume not reported", out)


def test_links_and_references(env):
    out = env.run("links.rar", "--directory", "links", "--mkdir")
    check("skipping symbolic link" in out, "symlink warning missing", out)
    check("reference to an identical file" in out, "file reference warning missing", out)
    # rar keeps the first of the identical files it meets and stores the other
    # one as a reference, which is not uploaded.
    copies = [n for n in ("original.bin", "zz-identical-copy.bin") if env.remote("links", "tree", n).exists()]
    check(len(copies) == 1, f"expected one of the identical files, found {copies}", out)
    skipped = {"original.bin", "zz-identical-copy.bin"} - set(copies)
    compare_trees(env.fixtures["links"], env.remote("links", "tree"), out, skip=skipped)


def test_cancel_removes_partial_file(env):
    cmd = env.command("slow.rar", "--directory", "cancel", "--mkdir")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            stdin=subprocess.DEVNULL)
    lines = []
    for line in proc.stdout:  # Wait until the upload starts.
        lines.append(line)
        if "To upload:" in line:
            break
    time.sleep(1.0)
    proc.send_signal(signal.SIGINT)
    rest, _ = proc.communicate(timeout=300)
    out = "".join(lines) + rest
    check(proc.returncode == 130, f"exit code {proc.returncode}, expected 130", out)
    check("Cancelled" in out, "no cancellation summary", out)
    check("Removed the incomplete remote file" in out, "partial file not removed", out)
    check(not env.remote("cancel", "tree", "zeros.bin").exists(), "partial file left on the server", out)


def test_tui_renders_and_quits_with_q(env):
    if not hasattr(os, "fork"):
        raise Skipped("needs a POSIX pseudo-terminal")
    import fcntl
    import pty
    import select
    import struct
    import termios

    cmd = env.command("slow.rar", "--directory", "tui", "--mkdir", no_tui=False)
    pid, fd = pty.fork()
    if pid == 0:  # Child: a 100x30 terminal.
        fcntl.ioctl(sys.stdout.fileno(), termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
        os.execv(cmd[0], cmd)

    raw = b""
    sent_quit = False
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        if not sent_quit and b"Archive total" in raw:
            time.sleep(1.0)  # Let it draw a few frames mid-transfer.
            os.write(fd, b"q")
            sent_quit = True
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            if not data:
                break
            raw += data
    _, status = os.waitpid(pid, 0)
    os.close(fd)
    code = os.waitstatus_to_exitcode(status)
    screen = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw.decode("utf-8", "replace"))
    check(code == 130, f"exit code {code}, expected 130", screen)
    for text in ("Archive total", "q: cancel", "Upload", "Buffer", "ETA", "Log"):
        check(text in screen, f"{text!r} not drawn", screen)
    check("Cancelled" in screen, "no summary after leaving the dashboard", screen)


def test_ipv6(env):
    if env.server.routable:
        raise Skipped("the container has no IPv6 address")
    try:
        socket.create_connection(("::1", env.server.port), timeout=3).close()
    except OSError as error:
        raise Skipped(f"no IPv6 access to the published port: {error}") from error
    out = env.run("tiny.rar", "--directory", "ipv6", "--mkdir", host="::1")
    check(env.remote("ipv6", "tree", "tiny.txt").is_file(), "IPv6 upload failed", out)
    check("ftp://[::1]:" in out, "IPv6 URL not bracketed", out)


def test_zip_basic(env):
    out = env.run("basic.zip", "--directory", "zip", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("zip", "tree"), out)
    compare_mtimes(env.fixtures["main"], env.remote("zip", "tree"), out)
    check("Archive: ZIP, 8 file(s)" in out, "format not reported", out)
    out = env.run("basic.zip", "--directory", "zip")
    check("To upload: 0 file(s)" in out and "Skipped 8 file(s)" in out, "files were uploaded again", out)


def test_zip_compression_methods(env):
    for name in ("store.zip", "bzip2.zip", "lzma.zip", "zstd.zip", "infozip.zip"):
        if not Path(env.archive(name)).exists():
            print(f"    {name} skipped: not created")
            continue
        directory = name.replace(".", "-")
        out = env.run(name, "--directory", directory, "--mkdir")
        compare_trees(env.fixtures["main"], env.remote(directory, "tree"), out)


def test_zip_many_directories(env):
    out = env.run("dirs.zip", "--directory", "dirs-zip", "--mkdir")
    compare_trees(env.fixtures["dirs"], env.remote("dirs-zip"), out)
    compare_mtimes(env.fixtures["dirs"], env.remote("dirs-zip"), out)


def test_zip_links(env):
    out = env.run("links.zip", "--directory", "links-zip", "--mkdir")
    check("skipping symbolic link" in out, "symlink warning missing", out)
    compare_trees(env.fixtures["links"], env.remote("links-zip", "tree"), out)
    check(not env.remote("links-zip", "tree", "link-to-plain").exists(), "the link was uploaded", out)


def test_zip_corrupted(env):
    data = bytearray(Path(env.archive("store.zip")).read_bytes())
    source = (env.fixtures["main"] / "random-5M.bin").read_bytes()
    offset = data.find(source[3 << 20:(3 << 20) + 64])
    check(offset > 0, "could not locate file data in store.zip")
    data[offset] ^= 0xFF
    (env.fixtures["archives"] / "corrupted.zip").write_bytes(bytes(data))
    out = env.run("corrupted.zip", "--directory", "corrupted-zip", "--mkdir", expect=1)
    check("checksum" in out, "checksum error not reported", out)
    check(not env.remote("corrupted-zip", "tree", "random-5M.bin").exists(), "corrupt file left on the server", out)


def test_zip_traditional_encryption(env):
    env.require("zipcrypto.zip")
    out = env.run("zipcrypto.zip", "--directory", "zipcrypto", "--mkdir", expect=1)
    check("--archive-password" in out, "no hint about --archive-password", out)
    out = env.run("zipcrypto.zip", "--directory", "zipcrypto", "--mkdir", "--archive-password", ARCHIVE_PASSWORD)
    compare_trees(env.fixtures["main"], env.remote("zipcrypto", "tree"), out)


def test_zip_aes_encryption(env):
    env.require("aes.zip")
    # A wrong password is found while reading the archive, before anything is sent.
    out = env.run("aes.zip", "--directory", "aes-zip", "--mkdir", "--archive-password", "wrong", expect=1)
    check("wrong archive password" in out, "wrong password not reported", out)
    check("Connecting" not in out and not env.remote("aes-zip").exists(), "went on after a wrong password", out)
    out = env.run("aes.zip", "--directory", "aes-zip", "--mkdir", "--archive-password", ZIP_AES_PASSWORD)
    check(", encrypted" in out, "encryption not reported", out)
    compare_trees(env.fixtures["main"], env.remote("aes-zip", "tree"), out)


def test_zip_split(env):
    env.require("split.zip.001")
    parts = sorted(p.name for p in env.fixtures["archives"].glob("split.zip.0*"))
    check(len(parts) >= 3, f"expected several parts, got {parts}")
    out = env.run(parts[0], "--directory", "split-zip", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("split-zip", "tree"), out)
    check(f"{len(parts)} volumes" in out, "volume count not reported", out)
    out = env.run(parts[1], expect=1)
    check(f"is not the first part of a split archive; pass {parts[0]}" in out, "later part accepted", out)
    partial = env.fixtures["archives"] / "partial-zip"
    partial.mkdir()
    for name in parts:
        if name != parts[1]:
            shutil.copy(env.archive(name), partial / name)
    out = env.run(f"partial-zip/{parts[0]}", expect=1)
    check(f"volume not found: {partial / parts[1]}" in out, "missing part not reported", out)


def test_zip_deflate64_is_reported(env):
    env.require("deflate64.zip")
    out = env.run("deflate64.zip", "--directory", "deflate64", "--mkdir", expect=1)
    check("Unsupported ZIP compression method" in out, "Deflate64 not reported", out)
    # 7-Zip stores the other files, so only text-8M.txt is Deflate64 and the files before it are uploaded.
    check(not env.remote("deflate64", "tree", "text-8M.txt").exists(), "unsupported file left on the server", out)
    expected = tree_snapshot(env.fixtures["main"])
    damaged = sorted(k for k, v in tree_snapshot(env.remote("deflate64", "tree")).items() if expected.get(k) != v)
    check(not damaged, f"files on the server differ from the source: {damaged}", out)


def test_7z_basic(env):
    env.require("basic.7z")
    out = env.run("basic.7z", "--directory", "sevenzip", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("sevenzip", "tree"), out)
    compare_mtimes(env.fixtures["main"], env.remote("sevenzip", "tree"), out)
    check("Archive: 7z, 8 file(s)" in out, "format not reported", out)


def test_7z_rerun_and_repair(env):
    env.require("basic.7z")
    env.run("basic.7z", "--directory", "sevenzip-repair", "--mkdir")
    # Files before and after the damaged ones are skipped, decompressing what the solid block needs.
    env.remote("sevenzip-repair", "tree", "sub", "dir", "deep.bin").unlink()
    with open(env.remote("sevenzip-repair", "tree", "small.txt"), "r+b") as f:
        f.truncate(1)
    time.sleep(1)  # Docker Desktop and OrbStack bind mounts can show the container a stale view for a moment.
    out = env.run("basic.7z", "--directory", "sevenzip-repair")
    check("To upload: 2 file(s)" in out, "expected exactly the two damaged files", out)
    compare_trees(env.fixtures["main"], env.remote("sevenzip-repair", "tree"), out)


def test_7z_methods_and_directories(env):
    # BZip2 and Deflate: rarftp's own decoders (7-Zip's SDK lacks them); BCJ2 has four streams per block.
    for name, tree, directory in (("bzip2.7z", "main", "bzip2-7z"), ("deflate.7z", "main", "deflate-7z"),
                                  ("ppmd.7z", "main", "ppmd-7z"), ("bcj2.7z", "main", "bcj2-7z"),
                                  ("dirs.7z", "dirs", "dirs-7z")):
        env.require(name)
        out = env.run(name, "--directory", directory, "--mkdir")
        actual = env.remote(directory, "tree") if tree == "main" else env.remote(directory)
        compare_trees(env.fixtures[tree], actual, out)


def test_7z_multivolume(env):
    env.require("multivolume.7z.001")
    parts = sorted(p.name for p in env.fixtures["archives"].glob("multivolume.7z.0*"))
    check(len(parts) >= 3, f"expected several parts, got {parts}")
    out = env.run(parts[0], "--directory", "multi-7z", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("multi-7z", "tree"), out)
    check(f"{len(parts)} volumes" in out, "volume count not reported", out)


def test_7z_encrypted(env):
    for name in ("encrypted.7z", "encrypted-headers.7z"):
        env.require(name)
        directory = "enc-" + name.replace(".", "-")
        out = env.run(name, "--directory", directory, "--mkdir", expect=1)
        check("--archive-password" in out, f"{name}: no hint about --archive-password", out)
        # Wrong: found while reading the archive (its names, or the start of the first file).
        out = env.run(name, "--directory", directory, "--mkdir", "--archive-password", "wrong", expect=1)
        check("wrong archive password" in out, f"{name}: wrong password not reported", out)
        check(not env.remote(directory).exists(), f"{name}: went on after reading the archive", out)
        out = env.run(name, "--directory", directory, "--mkdir", "--archive-password", ARCHIVE_PASSWORD)
        check("encrypted" in out, f"{name}: not reported as encrypted", out)
        compare_trees(env.fixtures["main"], env.remote(directory, "tree"), out)
        compare_mtimes(env.fixtures["main"], env.remote(directory, "tree"), out)


def test_7z_encrypted_rerun(env):
    env.require("encrypted-nonsolid.7z")
    env.run("encrypted-nonsolid.7z", "--directory", "enc-7z-rerun", "--mkdir", "--archive-password",
            ARCHIVE_PASSWORD)
    env.remote("enc-7z-rerun", "tree", "sub", "dir", "deep.bin").unlink()
    time.sleep(1)  # Docker Desktop and OrbStack bind mounts can show the container a stale view for a moment.
    out = env.run("encrypted-nonsolid.7z", "--directory", "enc-7z-rerun", "--archive-password", ARCHIVE_PASSWORD)
    check("To upload: 1 file(s)" in out, "expected exactly the deleted file", out)
    compare_trees(env.fixtures["main"], env.remote("enc-7z-rerun", "tree"), out)


def test_7z_encrypted_multivolume(env):
    env.require("encrypted-multivolume.7z.001")
    parts = sorted(p.name for p in env.fixtures["archives"].glob("encrypted-multivolume.7z.0*"))
    check(len(parts) >= 3, f"expected several parts, got {parts}")
    out = env.run(parts[0], "--directory", "enc-multi-7z", "--mkdir", "--archive-password", ARCHIVE_PASSWORD)
    compare_trees(env.fixtures["main"], env.remote("enc-multi-7z", "tree"), out)
    check(f"{len(parts)} volumes" in out, "volume count not reported", out)


def test_7z_encrypted_methods(env):
    # Methods 7-Zip's SDK lacks, decrypted by it and decoded by rarftp's own decoders.
    for name in ("encrypted-bzip2.7z", "encrypted-deflate.7z"):
        env.require(name)
        directory = "enc-" + name.replace(".", "-")
        out = env.run(name, "--directory", directory, "--mkdir", "--archive-password", ARCHIVE_PASSWORD)
        compare_trees(env.fixtures["main"], env.remote(directory, "tree"), out)


def test_7z_deflate64_is_reported(env):
    env.require("deflate64.7z")
    out = env.run("deflate64.7z", "--directory", "deflate64-7z", "--mkdir", expect=1)
    check("unsupported 7z compression method: Deflate64" in out, "not reported", out)
    check(not env.remote("deflate64-7z").exists(), "went on after reading the archive", out)


def test_exfat_basic(env):
    out = env.run("basic.exfat", "--directory", "exfat", "--mkdir")
    check_exfat_tree(env.remote("exfat"), out)
    check("Archive: exFAT, 19 file(s)" in out, "format or file count not reported", out)
    check("exFAT images have no checksum of the file contents" in out, "no verification warning", out)
    out = env.run("basic.exfat", "--directory", "exfat")
    check("To upload: 0 file(s)" in out and "Skipped 19 file(s)" in out, "files were uploaded again", out)
    (env.remote("exfat") / "fragmented.bin").write_bytes(b"incomplete")
    out = env.run("basic.exfat", "--directory", "exfat")
    check("To upload: 1 file(s)" in out, "changed-size file was not repaired", out)
    check_exfat_tree(env.remote("exfat"), out)


def test_exfat_partitioned_is_rejected(env):
    out = env.run("disk.exfat", "--directory", "exfat-rejected", "--mkdir", expect=1)
    check("expected a single raw exFAT volume" in out, "partitioned disk was not rejected", out)
    check(not env.remote("exfat-rejected").exists(), "connected to FTP after rejecting the image", out)


def test_tar_basic(env):
    out = env.run("basic.tar", "--directory", "tar", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("tar", "tree"), out)
    compare_mtimes(env.fixtures["main"], env.remote("tar", "tree"), out)
    check("Archive: tar, 8 file(s)" in out, "format not reported", out)
    check("tar archives have no checksum of the file contents" in out, "no warning about verification", out)
    out = env.run("basic.tar", "--directory", "tar")
    check("To upload: 0 file(s)" in out and "Skipped 8 file(s)" in out, "files were uploaded again", out)


def test_tar_split(env):
    out = env.run("split.tar.001", "--directory", "split-tar", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("split-tar", "tree"), out)
    check("3 volumes" in out, "volume count not reported", out)


def test_tar_gz_is_read_as_it_is_uploaded(env):
    out = env.run("basic.tar.gz", "--directory", "tgz", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("tgz", "tree"), out)
    compare_mtimes(env.fixtures["main"], env.remote("tgz", "tree"), out)
    check("Archive: tar (gzip), read as it is uploaded" in out, "streamed reading not reported", out)
    check("Checking the files already on the server" not in out and "To upload:" not in out,
          "the archive was listed first", out)
    check("Done: 8 file(s)" in out, "wrong count", out)
    # Again: everything is on the server, checked file by file.
    out = env.run("basic.tar.gz", "--directory", "tgz")
    check("Done: 0 file(s)" in out and "Skipped 8 file(s)" in out, "files were uploaded again", out)
    # Damaged: only those two are sent.
    env.remote("tgz", "tree", "sub", "dir", "deep.bin").unlink()
    with open(env.remote("tgz", "tree", "small.txt"), "r+b") as f:
        f.truncate(1)
    time.sleep(1)  # Docker Desktop and OrbStack bind mounts can show the container a stale view for a moment.
    out = env.run("basic.tar.gz", "--directory", "tgz")
    check("Done: 2 file(s)" in out and "Skipped 6 file(s)" in out, "expected exactly the two damaged files", out)
    compare_trees(env.fixtures["main"], env.remote("tgz", "tree"), out)


def test_tar_compressions(env):
    for suffix in ("bz2", "xz", "lzma", "zst", "lz4"):
        name = f"basic.tar.{suffix}"
        if not Path(env.archive(name)).exists():
            print(f"    {name} skipped: not created (Python 3.14+ for zst, the lz4 command for lz4)")
            continue
        out = env.run(name, "--directory", f"tar-{suffix}", "--mkdir")
        compare_trees(env.fixtures["main"], env.remote(f"tar-{suffix}", "tree"), out)
    out = env.run("split.tar.gz.001", "--directory", "split-tgz", "--mkdir")
    compare_trees(env.fixtures["main"], env.remote("split-tgz", "tree"), out)

def require_lib(env):
    if env.lib is None:
        raise Skipped("pass --lib")


def tree_totals(root):
    """(number of files, their total size) under root."""
    sizes = [entry[0] for entry in tree_snapshot(root, with_hashes=False).values() if entry != "dir"]
    return len(sizes), sum(sizes)


def has_fields(actual, **expected):
    return actual is not None and all(actual[key] == value for key, value in expected.items())


def answer_with(*passwords):
    """on_prompt callback that answers the prompts in turn (None declines); one more fails the test."""
    queue = list(passwords)

    def on_prompt(prompt):
        check(queue, f"unexpected password prompt {prompt}")
        return queue.pop(0)

    return on_prompt


def test_lib_version(env):
    require_lib(env)
    version = env.lib.version()
    check(version.startswith("rarftp "), f"unexpected version string {version!r}")
    check(re.fullmatch(r"rarftp \d+\.\d+\.\d+\S* \(UnRAR .+, libarchive .+, libcurl .+\)", version),
          f"unexpected version string {version!r}")
    cli = subprocess.run([env.rarftp, "--version"], capture_output=True, text=True, timeout=60)
    check(cli.stdout.split()[:2] == version.split()[:2],
          f"library {version!r} and command line {cli.stdout.strip()!r} have different versions")


def test_lib_si_units(env):
    require_lib(env)
    # 10480 bytes rounds to 10.2 KiB in binary, which would convert to 10.4 kB:
    # SI must instead use the original bytes and show 10.5 kB. The file name stays literal.
    archive = Path(env.archive("si-units.zip"))
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as writer:
        writer.writestr("64 MiB.txt", b"x" * 10480)
    job = env.lib_run("si-units.zip", directory="lib-si", mkdir=1, si_units=True)
    out = job.describe()
    check(job.state["archive"]["bytes_text"] == "10.5 kB", "wrong SI archive size", out)
    check(job.state["progress"]["text"]["sent_bytes"] == "10.5 kB", "wrong SI progress size", out)
    check(job.state["progress"]["text"]["buffer_capacity"] == "67.1 MB", "wrong SI buffer capacity", out)
    check("10.5 kB uploaded" in job.result["summary"][0], "wrong SI summary size", out)
    log = lib_log_text(job)
    check("Archive: ZIP, 1 file(s), 10.5 kB" in log, "wrong SI archive log", out)
    check("To upload: 1 file(s), 10.5 kB" in log, "wrong SI plan log", out)
    check("Uploaded 64 MiB.txt (10.5 kB," in log, "wrong SI upload log or altered filename", out)
    check(env.remote("lib-si", "64 MiB.txt").read_bytes() == b"x" * 10480, "wrong uploaded data", out)
    skipped = env.lib_run("si-units.zip", directory="lib-si", si_units=True)
    check("10.5 kB already" in lib_log_text(skipped), "wrong SI skipped log", skipped.describe())
    check("10.5 kB already" in skipped.result["summary"][1], "wrong SI skipped summary", skipped.describe())


def test_lib_basic_upload(env):
    require_lib(env)
    files, size = tree_totals(env.fixtures["main"])
    job = env.lib_run("basic.rar", directory="lib-basic", mkdir=1, buffer_mib=8)
    state, result = job.state, job.result
    out = job.describe()
    compare_trees(env.fixtures["main"], env.remote("lib-basic", "tree"), out)
    compare_mtimes(env.fixtures["main"], env.remote("lib-basic", "tree"), out)
    check(has_fields(state["archive"], name="basic.rar", files=files, bytes=size, volumes=1, solid=False,
                     encrypted=False), f"wrong archive info {state['archive']}", out)
    check(state["target"] is not None and state["target"].startswith(
        f"ftp://{env.server.host}:{env.server.port}{HOME}/lib-basic"), f"wrong target {state['target']!r}", out)
    check((state["mode"], state["user"]) == ("passive", USER), "wrong mode or user", out)
    check(state["probe"] == {"done": files, "total": files}, f"wrong probe {state['probe']}", out)
    check(has_fields(state["progress"], total_files=files, files_done=files, total_bytes=size, sent_bytes=size,
                     buffer_used=0, buffer_capacity=8 << 20, current_file=""),
          f"wrong final progress {state['progress']}", out)
    check(has_fields(result, files_uploaded=files, bytes_uploaded=size, skipped_files=0, skipped_bytes=0,
                     ignored=0), f"wrong counters {result}", out)
    check(len(result["summary"]) == 1 and result["summary"][0].startswith(f"Done: {files} file(s), "),
          f"wrong summary {result['summary']}", out)
    log = lib_log_text(job)
    for text in (f"Reading {env.archive('basic.rar')}", f"Logged in as {USER}", "Destination: ftp://",
                 f"To upload: {files} file(s)"):
        check(text in log, f"{text!r} not logged", out)
    check(not job.prompts, "the job asked for a password", out)


def test_lib_rerun_skips_identical_files(env):
    require_lib(env)
    files, size = tree_totals(env.fixtures["main"])
    env.lib_run("basic.rar", directory="lib-rerun", mkdir=1)
    job = env.lib_run("basic.rar", directory="lib-rerun")
    result = job.result
    out = job.describe()
    check(has_fields(result, skipped_files=files, skipped_bytes=size, files_uploaded=0, bytes_uploaded=0),
          f"wrong counters {result}", out)
    check(any(line.startswith(f"Skipped {files} file(s), ") for line in result["summary"]),
          f"no skip line in the summary {result['summary']}", out)
    check(job.state["probe"] == {"done": files, "total": files}, f"wrong probe {job.state['probe']}", out)
    check(job.state["progress"]["total_files"] == 0, "files were going to be uploaded again", out)
    check("To upload: 0 file(s)" in lib_log_text(job), "files were uploaded again", out)
    compare_trees(env.fixtures["main"], env.remote("lib-rerun", "tree"), out)


def test_lib_multivolume(env):
    require_lib(env)
    volumes = env.volumes("multivolume")
    check(len(volumes) >= 3, f"expected several volumes, got {volumes}")
    job = env.lib_run(volumes[0], directory="lib-multi", mkdir=1)
    out = job.describe()
    compare_trees(env.fixtures["main"], env.remote("lib-multi", "tree"), out)
    check(job.state["archive"]["volumes"] == len(volumes), f"expected {len(volumes)} volumes", out)
    check(f"Reading volume {volumes[1]}" in lib_log_text(job), "volume change not logged", out)


def test_lib_encrypted_headers(env):
    require_lib(env)
    job = env.lib_run("encrypted-headers.rar", directory="lib-enc-headers", mkdir=1,
                      on_prompt=answer_with("wrong", ARCHIVE_PASSWORD))
    out = job.describe()
    check(job.prompts == [{"kind": "archive_password", "archive": "encrypted-headers.rar", "error": None},
                          {"kind": "archive_password", "archive": "encrypted-headers.rar", "error": "Wrong password"}],
          f"wrong prompts {job.prompts}", out)
    check(job.state["archive"]["encrypted"] is True, "archive not reported as encrypted", out)
    compare_trees(env.fixtures["main"], env.remote("lib-enc-headers", "tree"), out)


def test_lib_encrypted_files(env):
    require_lib(env)
    job = env.lib_run("encrypted.rar", directory="lib-enc", mkdir=1, archive_password=ARCHIVE_PASSWORD)
    out = job.describe()
    check(not job.prompts, "the job asked for a password that was in the config", out)
    check(job.state["archive"]["encrypted"] is True, "archive not reported as encrypted", out)
    compare_trees(env.fixtures["main"], env.remote("lib-enc", "tree"), out)


def test_lib_encrypted_files_asks_for_the_password(env):
    require_lib(env)
    job = env.lib_run("encrypted.rar", directory="lib-enc-prompt", mkdir=1,
                      on_prompt=answer_with(ARCHIVE_PASSWORD))
    out = job.describe()
    check(job.prompts == [{"kind": "archive_password", "archive": "encrypted.rar", "error": None}],
          f"wrong prompts {job.prompts}", out)
    compare_trees(env.fixtures["main"], env.remote("lib-enc-prompt", "tree"), out)


def test_lib_declined_password_prompt(env):
    require_lib(env)
    job = env.lib_run("encrypted-headers.rar", expect="failed", directory="lib-declined", mkdir=1,
                      on_prompt=answer_with(None))
    out = job.describe()
    check(len(job.prompts) == 1, f"expected one prompt, got {job.prompts}", out)
    check(job.result["error"] == "the archive is encrypted and no password was entered",
          f"wrong error {job.result['error']!r}", out)
    check(job.state["archive"] is None and job.state["target"] is None and job.state["progress"] is None,
          "the job went past reading the archive", out)
    check(not env.remote("lib-declined").exists(), "directory created before the archive was read", out)
    # Declining after a wrong answer reports the wrong password instead.
    job = env.lib_run("encrypted-headers.rar", expect="failed", directory="lib-declined", mkdir=1,
                      on_prompt=answer_with("wrong", None))
    check(job.result["error"] == "wrong archive password", f"wrong error {job.result['error']!r}", job.describe())


def test_lib_missing_directory_is_an_error(env):
    require_lib(env)
    job = env.lib_run("tiny.rar", expect="failed", directory="lib-does-not-exist")
    out = job.describe()
    error = job.result["error"]
    check(error == f'the remote directory {HOME}/lib-does-not-exist does not exist '
                   '(enable "Create directory if missing")', f"wrong error {error!r}", out)
    check("--mkdir" not in error, "the error names a command-line option", out)
    check(job.state["archive"] is not None and job.state["target"] is None and job.state["progress"] is None,
          "unexpected state for a failure before the transfer", out)
    check(any(line["level"] == "error" and line["text"] == error for line in job.result["problems"]),
          "the error is not among the problems", out)
    check(not env.remote("lib-does-not-exist").exists(), "directory created without mkdir", out)


def test_lib_cancel_removes_partial_file(env):
    require_lib(env)
    seen = {"upload_started": None, "cancelled": False}

    def on_state(job, state):
        progress = state["progress"]
        if seen["upload_started"] is None:  # Wait until the upload starts, then let it run for a second.
            if progress is not None and progress["current_file"]:
                seen["upload_started"] = time.monotonic()
        elif not seen["cancelled"] and time.monotonic() - seen["upload_started"] >= 1.0:
            job.cancel()
            seen["cancelled"] = True
            after = job.poll()
            check(after["cancelling"] or after["phase"] == "finished", "cancelling not reported", job.describe())

    job = env.lib_run("slow.rar", expect="cancelled", on_state=on_state, timeout=300, directory="lib-cancel",
                      mkdir=1)
    out = job.describe()
    check(seen["cancelled"], "the job finished before it could be cancelled", out)
    check(job.result["summary"][0].startswith("Cancelled: "), f"wrong summary {job.result['summary']}", out)
    check("Removed the incomplete remote file" in lib_log_text(job), "partial file not removed", out)
    check(not env.remote("lib-cancel", "tree", "zeros.bin").exists(), "partial file left on the server", out)


def test_lib_zip_7z_and_tar(env):
    require_lib(env)
    env.require("basic.7z")
    files, size = tree_totals(env.fixtures["main"])
    for name, fmt, directory in (("basic.zip", "ZIP", "lib-zip"), ("basic.7z", "7z", "lib-7z"),
                                 ("basic.tar", "tar", "lib-tar")):
        job = env.lib_run(name, directory=directory, mkdir=1)
        out = job.describe()
        compare_trees(env.fixtures["main"], env.remote(directory, "tree"), out)
        check(has_fields(job.state["archive"], name=name, format=fmt, files=files, bytes=size, volumes=1,
                         encrypted=False), f"wrong archive info {job.state['archive']}", out)
        if fmt == "tar":
            check(any(line["level"] == "warn" and "no checksum" in line["text"] for line in job.result["problems"]),
                  "no warning about verification", out)


def test_lib_exfat(env):
    require_lib(env)
    job = env.lib_run("basic.exfat", directory="lib-exfat", mkdir=1)
    out = job.describe()
    check_exfat_tree(env.remote("lib-exfat"), out)
    check(has_fields(job.state["archive"], format="exFAT", compression=None, files=19, bytes=6484635,
                     volumes=1, solid=False, encrypted=False), f"wrong archive info {job.state['archive']}", out)
    check(has_fields(job.result, files_uploaded=19, skipped_files=0), "wrong transfer counts", out)
    check(not job.prompts, "exFAT asked for an archive password", out)
    job = env.lib_run("basic.exfat", directory="lib-exfat")
    check(has_fields(job.result, files_uploaded=0, skipped_files=19, skipped_bytes=6484635),
          "wrong counters on re-run", job.describe())
    job = env.lib_run("disk.exfat", expect="failed", directory="lib-exfat-rejected", mkdir=1)
    check(job.state["archive"] is None and job.state["progress"] is None, "invalid image was accepted", job.describe())
    check(not env.remote("lib-exfat-rejected").exists(), "created a destination for an invalid image", job.describe())


def test_lib_zip_password_prompt(env):
    require_lib(env)
    env.require("aes.zip")
    job = env.lib_run("aes.zip", directory="lib-aes-zip", mkdir=1, on_prompt=answer_with("wrong", ZIP_AES_PASSWORD))
    out = job.describe()
    check(job.prompts == [{"kind": "archive_password", "archive": "aes.zip", "error": None},
                          {"kind": "archive_password", "archive": "aes.zip", "error": "Wrong password"}],
          f"wrong prompts {job.prompts}", out)
    check(has_fields(job.state["archive"], format="ZIP", encrypted=True), "archive not reported as encrypted", out)
    compare_trees(env.fixtures["main"], env.remote("lib-aes-zip", "tree"), out)


def test_lib_7z_password_prompt(env):
    require_lib(env)
    for name in ("encrypted.7z", "encrypted-headers.7z"):
        env.require(name)
        directory = "lib-enc-" + name.replace(".", "-")
        job = env.lib_run(name, directory=directory, mkdir=1, on_prompt=answer_with("wrong", ARCHIVE_PASSWORD))
        out = job.describe()
        check(job.prompts == [{"kind": "archive_password", "archive": name, "error": None},
                              {"kind": "archive_password", "archive": name, "error": "Wrong password"}],
              f"{name}: wrong prompts {job.prompts}", out)
        check(has_fields(job.state["archive"], format="7z", encrypted=True), "archive not reported as encrypted", out)
        compare_trees(env.fixtures["main"], env.remote(directory, "tree"), out)


def test_lib_7z_multivolume(env):
    require_lib(env)
    env.require("multivolume.7z.001")
    parts = sorted(p.name for p in env.fixtures["archives"].glob("multivolume.7z.0*"))
    job = env.lib_run(parts[0], directory="lib-multi-7z", mkdir=1)
    out = job.describe()
    compare_trees(env.fixtures["main"], env.remote("lib-multi-7z", "tree"), out)
    check(job.state["archive"]["volumes"] == len(parts), f"expected {len(parts)} volumes", out)


def test_lib_streamed_tar(env):
    require_lib(env)
    files, size = tree_totals(env.fixtures["main"])
    job = env.lib_run("basic.tar.xz", directory="lib-txz", mkdir=1)
    out = job.describe()
    compare_trees(env.fixtures["main"], env.remote("lib-txz", "tree"), out)
    check(has_fields(job.state["archive"], format="tar", compression="xz", files=None, bytes=None),
          f"wrong archive info {job.state['archive']}", out)
    check(job.state["probe"] is None, "a separate check of the server ran", out)
    check(has_fields(job.state["progress"], totals_known=True, total_files=files, total_bytes=size,
                     files_done=files), f"wrong final progress {job.state['progress']}", out)
    check(job.state["progress"]["archive_read"] == job.state["progress"]["archive_size"] > 0,
          "the archive was not read to its end", out)
    check(has_fields(job.result, files_uploaded=files, skipped_files=0), f"wrong counters {job.result}", out)
    job = env.lib_run("basic.tar.xz", directory="lib-txz")
    check(has_fields(job.result, files_uploaded=0, skipped_files=files, skipped_bytes=size),
          f"wrong counters on the second run {job.result}", job.describe())


def test_big_file(env):
    if env.fixtures["big"] is None:
        raise Skipped("pass --big")
    time_cmd = ["/usr/bin/time", "-l"] if sys.platform == "darwin" else ["/usr/bin/time", "-v"]
    for archive, directory in (("big.rar", "big"), ("big.zip", "big-zip")):
        cmd = time_cmd + env.command(archive, "--directory", directory, "--mkdir")
        proc = subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=3600)
        out = proc.stdout + proc.stderr
        check(proc.returncode == 0, f"{archive}: exit code {proc.returncode}", out)
        compare_trees(env.fixtures["big"], env.remote(directory, "tree"), out)
        match = (re.search(r"(\d+)\s+maximum resident set size", out) or
                 re.search(r"Maximum resident set size \(kbytes\): (\d+)", out))
        if match:
            rss = int(match.group(1)) * (1 if sys.platform == "darwin" else 1024)
            print(f"    {archive}: max RSS {rss / (1 << 20):.0f} MiB for a 4.5 GiB file")
            check(rss < 512 << 20, f"{archive}: memory use too high: {rss} bytes", out)


TESTS = [
    test_unrar_is_linked_statically,
    test_basic_upload,
    test_rerun_skips_identical_files,
    test_changed_size_is_uploaded_again,
    test_solid,
    test_active_mode,
    test_multivolume,
    test_solid_multivolume,
    test_many_directories,
    test_many_directories_solid_multivolume,
    test_many_directories_rerun_and_repair,
    test_rar4_format,
    test_encrypted_files,
    test_encrypted_headers,
    test_anonymous_login_is_attempted,
    test_wrong_ftp_password,
    test_missing_directory_is_an_error,
    test_mkdir_is_not_recursive,
    test_default_directory_warning,
    test_relative_directory,
    test_corrupted_archive,
    test_missing_volume,
    test_not_first_volume,
    test_links_and_references,
    test_cancel_removes_partial_file,
    test_tui_renders_and_quits_with_q,
    test_ipv6,
    test_zip_basic,
    test_zip_compression_methods,
    test_zip_many_directories,
    test_zip_links,
    test_zip_corrupted,
    test_zip_traditional_encryption,
    test_zip_aes_encryption,
    test_zip_split,
    test_zip_deflate64_is_reported,
    test_7z_basic,
    test_7z_rerun_and_repair,
    test_7z_methods_and_directories,
    test_7z_multivolume,
    test_7z_encrypted,
    test_7z_encrypted_rerun,
    test_7z_encrypted_multivolume,
    test_7z_encrypted_methods,
    test_7z_deflate64_is_reported,
    test_exfat_basic,
    test_exfat_partitioned_is_rejected,
    test_tar_basic,
    test_tar_split,
    test_tar_gz_is_read_as_it_is_uploaded,
    test_tar_compressions,
    test_lib_version,
    test_lib_si_units,
    test_lib_basic_upload,
    test_lib_rerun_skips_identical_files,
    test_lib_multivolume,
    test_lib_encrypted_headers,
    test_lib_encrypted_files,
    test_lib_encrypted_files_asks_for_the_password,
    test_lib_declined_password_prompt,
    test_lib_missing_directory_is_an_error,
    test_lib_cancel_removes_partial_file,
    test_lib_zip_7z_and_tar,
    test_lib_exfat,
    test_lib_zip_password_prompt,
    test_lib_7z_password_prompt,
    test_lib_7z_multivolume,
    test_lib_streamed_tar,
    test_big_file,
]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rarftp", required=True, help="path to the rarftp binary")
    parser.add_argument("--rar", default="rar", help="path to RARLAB's rar")
    parser.add_argument("--7z", dest="sevenzip", help="path to 7-Zip's 7zz (default: 7zz or 7z from PATH)")
    parser.add_argument("--big", action="store_true", help="also test a 4.5 GiB file")
    parser.add_argument("--lib", help="path to librarftpcore (.dylib/.so/.dll): also test its C API (lib_* tests)")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("-k", dest="filter", default="", help="only run tests whose name contains this")
    args = parser.parse_args()
    if args.lib and not Path(args.lib).is_file():
        parser.error(f"--lib: {args.lib} is not a file")

    work = Path(tempfile.mkdtemp(prefix="rarftp-it-"))
    print(f"work directory: {work}")
    failures = 0
    env = None
    try:
        env = Env(args, work)
        for test in TESTS:
            if args.filter not in test.__name__:
                continue
            started = time.monotonic()
            try:
                test(env)
                print(f"PASS {test.__name__} ({time.monotonic() - started:.1f} s)")
            except Skipped as reason:
                print(f"SKIP {test.__name__}: {reason}")
            except TestFailure as failure:
                failures += 1
                print(f"FAIL {test.__name__}: {failure}")
            except Exception:  # noqa: BLE001
                failures += 1
                print(f"ERROR {test.__name__}:\n{traceback.format_exc()}")
    finally:
        if env is not None:
            env.server.close()
        if args.keep or failures:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("all tests passed" if failures == 0 else f"{failures} test(s) failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
