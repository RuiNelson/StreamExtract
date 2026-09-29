#!/usr/bin/env python3
"""End-to-end tests for rarftp.

Creates archives with RARLAB's `rar`, uploads them with rarftp to vsftpd
running in Docker (image delfer/alpine-ftp-server) and checks what arrives.

    python3 tests/integration/run.py --rarftp build/rarftp --rar /path/to/rar [--big]

Needs Python 3.9+ (standard library only), Docker and `rar`.
"""

import argparse
import hashlib
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
import uuid
from pathlib import Path

IMAGE = "delfer/alpine-ftp-server"
USER = "tester"
PASSWORD = "secret"
HOME = f"/ftp/{USER}"  # Login directory on the server (not chrooted).
BASE_TIME = 1_700_000_000
ARCHIVE_PASSWORD = "s3cr3t pässwörd"


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


def build_fixtures(work, rar, big):
    rnd = random.Random(20260929)
    trees = work / "trees"
    archives = work / "archives"
    archives.mkdir(parents=True)

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

    return {"main": main_tree, "dirs": dirs_tree, "links": links_tree, "tiny": tiny_tree,
            "slow": slow_tree, "big": big_tree, "archives": archives}


# --------------------------------------------------------------------------
# Runner


class Env:
    def __init__(self, args, work):
        self.rarftp = str(Path(args.rarftp).resolve())
        self.work = work
        self.server = Server(work / "ftp")  # First: fails fast without Docker.
        try:
            self.fixtures = build_fixtures(work, args.rar, args.big)
        except BaseException:
            self.server.close()
            raise

    def archive(self, name):
        return str(self.fixtures["archives"] / name)

    def volumes(self, stem):
        # rar numbers volumes part1.. or part01.. depending on their count.
        return sorted(p.name for p in self.fixtures["archives"].glob(f"{stem}.part*.rar"))

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
    check("--rar-password" in out, "no hint about --rar-password", out)
    out = env.run("encrypted.rar", "--directory", "enc", "--mkdir", "--rar-password", ARCHIVE_PASSWORD)
    compare_trees(env.fixtures["main"], env.remote("enc", "tree"), out)


def test_encrypted_headers(env):
    out = env.run("encrypted-headers.rar", expect=1)
    check("--rar-password" in out, "no hint about --rar-password", out)
    out = env.run("encrypted-headers.rar", "--rar-password", "wrong", expect=1)
    check("wrong" in out.lower(), "wrong password not reported", out)
    out = env.run("encrypted-headers.rar", "--directory", "enc-headers", "--mkdir", "--rar-password",
                  ARCHIVE_PASSWORD)
    compare_trees(env.fixtures["main"], env.remote("enc-headers", "tree"), out)


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


def test_big_file(env):
    if env.fixtures["big"] is None:
        raise Skipped("pass --big")
    time_cmd = ["/usr/bin/time", "-l"] if sys.platform == "darwin" else ["/usr/bin/time", "-v"]
    cmd = time_cmd + env.command("big.rar", "--directory", "big", "--mkdir")
    proc = subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=3600)
    out = proc.stdout + proc.stderr
    check(proc.returncode == 0, f"exit code {proc.returncode}", out)
    compare_trees(env.fixtures["big"], env.remote("big", "tree"), out)
    match = (re.search(r"(\d+)\s+maximum resident set size", out) or
             re.search(r"Maximum resident set size \(kbytes\): (\d+)", out))
    if match:
        rss = int(match.group(1)) * (1 if sys.platform == "darwin" else 1024)
        print(f"    max RSS {rss / (1 << 20):.0f} MiB for a 4.5 GiB file")
        check(rss < 512 << 20, f"memory use too high: {rss} bytes", out)


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
    test_big_file,
]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rarftp", required=True, help="path to the rarftp binary")
    parser.add_argument("--rar", default="rar", help="path to RARLAB's rar")
    parser.add_argument("--big", action="store_true", help="also test a 4.5 GiB file")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("-k", dest="filter", default="", help="only run tests whose name contains this")
    args = parser.parse_args()

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
