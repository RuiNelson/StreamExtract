#!/usr/bin/env python3
"""SFTP authentication, host verification and transfers through the shared Docker server."""
import argparse
import contextlib
import getpass
import io
import os
from pathlib import Path
import socket
import subprocess
import tarfile
import tempfile
import time
import unittest
import zipfile

from docker_server import Server, USER, PASSWORD, HOME
from run import Library, LibJob

ARGS = None

@contextlib.contextmanager
def home_directory(home):
    name = "USERPROFILE" if os.name == "nt" else "HOME"
    previous = os.environ.get(name)
    os.environ[name] = str(home)
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = previous

class SftpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="streamextract-sftp-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.remote = self.root / "remote"
        self.server = Server(self.remote)
        self.addCleanup(self.server.close)
        self.configure()

    def configure(self, **settings):
        self.server.request("POST", "/ssh", settings)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                with socket.create_connection((self.server.host, self.server.ssh_port), timeout=1) as conn:
                    if conn.recv(64).startswith(b"SSH-"):
                        return
            except OSError:
                pass
            time.sleep(0.05)
        self.fail("sshd did not become ready")

    def key(self, name, algorithm="ecdsa", passphrase="", pem=False):
        path = self.root / name
        command = ["ssh-keygen", "-q", "-t", algorithm, "-N", passphrase, "-f", str(path)]
        if algorithm == "rsa":
            command += ["-b", "2048"]
        if pem:
            command += ["-m", "PEM"]
        subprocess.run(command, check=True, capture_output=True)
        return path

    def archive(self, entries, streamed=False):
        path = self.root / ("test.tar.gz" if streamed else "test.zip")
        if streamed:
            with tarfile.open(path, "w:gz") as archive:
                for name, data in entries:
                    info = tarfile.TarInfo(name)
                    info.size, info.mtime = len(data), 1700000000
                    archive.addfile(info, io.BytesIO(data))
        else:
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for name, data in entries:
                    info = zipfile.ZipInfo(name, (2023, 11, 14, 22, 13, 20))
                    archive.writestr(info, data)
        return path

    def transfer(self, archive, **options):
        command = [ARGS.sext, "--file", str(archive), "--host", self.server.host,
                   "--port", str(self.server.ssh_port), "--protocol", "sftp", "--no-tui", "--retries", "1"]
        for name, value in options.items():
            if value is not None:
                command += ["--" + name.replace("_", "-"), str(value)]
        return subprocess.run(command, capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL)

    def success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_password_upload_resume_skip_replace_and_streaming(self):
        payload = bytes(range(251)) * 1000
        for streamed in (False, True):
            with self.subTest(streamed=streamed):
                directory = f"transfer-{streamed}"
                destination = self.remote / directory
                destination.mkdir()
                (destination / "same.txt").write_bytes(b"same")
                (destination / "larger.txt").write_bytes(b"old data longer than new")
                (destination / "resume.bin").write_bytes(b"trusted prefix")
                entries = [("same.txt", b"same"), ("larger.txt", b"new"), ("resume.bin", payload),
                           ('spaces 日本語/a "quote" #%.txt', b"escaped"), ("empty.txt", b"")]
                self.server.sync()
                result = self.transfer(self.archive(entries, streamed), user=USER, password=PASSWORD,
                                       directory=HOME + "/" + directory)
                self.success(result)
                self.assertIn("Resuming resume.bin at byte 14", result.stdout)
                for name, expected in entries:
                    if name == "resume.bin":
                        expected = b"trusted prefix" + payload[14:]
                    self.assertEqual((destination / name).read_bytes(), expected)
                self.assertAlmostEqual((destination / "larger.txt").stat().st_mtime, 1700000000, delta=2)
                repeated = self.transfer(self.archive(entries, streamed), user=USER, password=PASSWORD,
                                         directory=HOME + "/" + directory)
                self.success(repeated)
                self.assertIn("0 file(s)", repeated.stdout)
                self.assertIn("5 file(s)", repeated.stdout)

    def test_password_without_user_defaults_to_current_username(self):
        archive = self.archive([("default-user.txt", b"local username")])
        result = self.transfer(archive, password=PASSWORD)
        self.success(result)
        self.assertIn("Logged in as " + getpass.getuser(), result.stdout)
        self.assertEqual((self.remote / "default-user.txt").read_bytes(), b"local username")

    def test_explicit_rsa_private_key_without_public_key_file(self):
        key = self.key("rsa", "rsa", pem=True)
        self.configure(password=False, authorized_keys=key.with_suffix(".pub").read_text())
        key.with_suffix(".pub").unlink()
        result = self.transfer(self.archive([("key.txt", b"key")]), user=USER, private_key=key)
        self.success(result)

    def test_encrypted_openssh_ecdsa_key_and_separate_passphrase(self):
        key = self.key("encrypted", passphrase="key secret")
        self.configure(password=False, authorized_keys=key.with_suffix(".pub").read_text())
        archive = self.archive([("encrypted.txt", b"encrypted key")])
        result = self.transfer(archive, user=USER, private_key=key, private_key_passphrase="key secret")
        self.success(result)
        self.assertNotIn("key secret", result.stdout + result.stderr)
        for passphrase in (None, "wrong"):
            failed = self.transfer(archive, user=USER, private_key=key, private_key_passphrase=passphrase)
            self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)

    def test_openssh_rsa_ecdsa_ed25519_keys_without_public_files(self):
        archive = self.archive([("algorithms.txt", b"private keys")])
        for algorithm in ("rsa", "ecdsa", "ed25519"):
            for secret in ("", "encrypted secret"):
                with self.subTest(algorithm=algorithm, encrypted=bool(secret)):
                    key = self.key("key-" + algorithm + str(bool(secret)), algorithm, secret)
                    self.configure(password=False, authorized_keys=key.with_suffix(".pub").read_text())
                    key.with_suffix(".pub").unlink()
                    self.success(self.transfer(archive, user=USER, private_key=key,
                                               private_key_passphrase=secret))

    def test_automatic_private_keys_try_custom_names_and_skip_nonkeys(self):
        rejected = self.key("rejected", "rsa")
        accepted = self.key("accepted", "rsa")
        self.configure(password=False, authorized_keys=accepted.with_suffix(".pub").read_text())
        home = self.root / "home"
        ssh = home / ".ssh"
        ssh.mkdir(parents=True)
        (ssh / "a-custom-key").write_bytes(rejected.read_bytes())
        (ssh / "z-custom-key").write_bytes(accepted.read_bytes())
        (ssh / "config").write_text("Host *\n")
        (ssh / "authorized.pub").write_text(accepted.with_suffix(".pub").read_text())
        with home_directory(home):
            result = self.transfer(self.archive([("auto.txt", b"automatic")]), user=USER)
        self.success(result)

    def known_hosts(self, path, host=None):
        host = host or self.server.host
        keys = self.server.request("GET", "/ssh-host-keys")
        address = host if self.server.ssh_port == 22 else f"[{host}]:{self.server.ssh_port}"
        path.write_text("".join(f"{address} {' '.join(key.split()[:2])}\n" for key in keys))

    def test_known_hosts_explicit_hashed_default_and_omitted(self):
        archive = self.archive([("verified.txt", b"verified")])
        known = self.root / "known_hosts"
        self.known_hosts(known)
        self.success(self.transfer(archive, user=USER, password=PASSWORD, known_hosts=known))
        subprocess.run(["ssh-keygen", "-H", "-f", str(known)], check=True, capture_output=True)
        self.success(self.transfer(archive, user=USER, password=PASSWORD, known_hosts=known))
        home = self.root / "home"
        (home / ".ssh").mkdir(parents=True)
        (home / ".ssh" / "known_hosts").write_bytes(known.read_bytes())
        with home_directory(home):
            self.success(self.transfer(archive, user=USER, password=PASSWORD, known_hosts=""))
        self.known_hosts(known, "unknown.example")
        failed = self.transfer(archive, user=USER, password=PASSWORD, known_hosts=known)
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        key = self.key("wrong-host", "rsa")
        address = self.server.host if self.server.ssh_port == 22 else f"[{self.server.host}]:{self.server.ssh_port}"
        known.write_text(f"{address} {key.with_suffix('.pub').read_text()}")
        failed = self.transfer(archive, user=USER, password=PASSWORD, known_hosts=known)
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        self.success(self.transfer(archive, user=USER, password=PASSWORD))
        failed = self.transfer(archive, user=USER, password=PASSWORD, known_hosts=self.root / "missing")
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)

    def test_wrong_password_and_missing_keys_fail_without_prompting(self):
        archive = self.archive([("no.txt", b"no")])
        failed = self.transfer(archive, user=USER, password="wrong")
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        with home_directory(self.root / "no-keys"):
            failed = self.transfer(archive, user=USER)
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        self.assertFalse((self.remote / "no.txt").exists())

class LibrarySftpTests(SftpTests):
    def setUp(self):
        if not ARGS.lib:
            self.skipTest("--lib was not supplied")
        super().setUp()
        self.lib = Library(ARGS.lib)

    def transfer(self, archive, **options):
        config = {"archive": str(archive), "host": self.server.host, "port": self.server.ssh_port,
                  "protocol": "sftp", "buffer_mib": 1,
                  **{name: str(value) if isinstance(value, Path) else value for name, value in options.items()}}
        with LibJob(self.lib, config, retries=1) as job:
            job.wait(timeout=30)
        output = "\n".join(f"{line['level'].upper()} {line['text']}" for line in job.log)
        output += "\n" + "\n".join(job.result["summary"])
        return subprocess.CompletedProcess([], 0 if job.result["status"] == "success" else 1,
                                           stdout=output, stderr=job.result["error"] or "")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sext", required=True)
    parser.add_argument("--lib")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
