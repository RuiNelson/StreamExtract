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
                    if name.endswith("/"):
                        info.type = tarfile.DIRTYPE
                        archive.addfile(info)
                    else:
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
            if isinstance(value, bool):
                if value:
                    command += ["--" + name.replace("_", "-")]
            elif value is not None:
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

    def test_staging_resumes_then_renames_with_quoted_paths(self):
        entries = [("same.txt", b"same"), ("resume.txt", b"abcdef"),
                   ('spaces 日本語/a "quote" #%.txt', b"escaped"), ("empty.txt", b"")]
        for streamed in (False, True):
            with self.subTest(streamed=streamed):
                directory = f"staging-{streamed}"
                destination = self.remote / directory
                stage = destination / '.stage "quoted"'
                stage.mkdir(parents=True)
                (stage / "same.txt").write_bytes(b"same")
                (stage / "resume.txt").write_bytes(b"XY")
                (stage / "unrelated.txt").write_bytes(b"keep")
                (destination / "resume.txt").write_bytes(b"final!")
                self.server.sync()
                result = self.transfer(self.archive(entries, streamed), user=USER, password=PASSWORD,
                                       directory=directory, staging=directory + '/.stage "quoted"')
                self.success(result)
                self.assertIn("Resuming resume.txt at byte 2", result.stdout)
                for name, contents in entries:
                    self.assertEqual((destination / name).read_bytes(), b"XYcdef" if name == "resume.txt" else contents)
                    self.assertFalse((stage / name).exists())
                self.assertEqual((stage / "unrelated.txt").read_bytes(), b"keep")

    def test_mkdir_controls_both_staging_and_destination(self):
        archive = self.archive([("file.txt", b"file")])
        for destination_exists, staging_exists in ((True, False), (False, True), (False, False)):
            with self.subTest(destination_exists=destination_exists, staging_exists=staging_exists):
                name = f"mkdir-{destination_exists}-{staging_exists}"
                destination = self.remote / (name + "-destination")
                staging = self.remote / (name + "-staging")
                if destination_exists:
                    destination.mkdir()
                if staging_exists:
                    staging.mkdir()
                self.server.sync()
                options = dict(user=USER, password=PASSWORD, directory=destination.name, staging=staging.name)
                result = self.transfer(archive, **options)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("does not exist", result.stdout + result.stderr)
                self.assertEqual(destination.is_dir(), destination_exists)
                self.assertEqual(staging.is_dir(), staging_exists)
                self.success(self.transfer(archive, mkdir=True, **options))
                self.assertEqual((destination / "file.txt").read_bytes(), b"file")
                self.assertEqual(list(staging.iterdir()), [])

    def test_staging_moves_whole_folders_and_keeps_unrelated_hidden_files(self):
        entries = [('whole 日本語/a "quote".txt', b"a"), ("whole 日本語/sub/b.txt", b"b"),
                   ("whole 日本語/empty/", b""), ("existing/subtree/file.txt", b"new"),
                   ("mixed/file.txt", b"file")]
        for streamed in (False, True):
            with self.subTest(streamed=streamed):
                name = f"folder-moves-{streamed}"
                destination = self.remote / (name + "-destination")
                stage = self.remote / (name + "-stage")
                (destination / "existing").mkdir(parents=True)
                (destination / "existing/unrelated.txt").write_bytes(b"preserve")
                (stage / "mixed").mkdir(parents=True)
                (stage / "mixed/.unrelated.txt").write_bytes(b"keep")
                self.server.sync()
                result = self.transfer(self.archive(entries, streamed), user=USER, password=PASSWORD,
                                       directory=destination.name, staging=stage.name)
                self.success(result)
                for entry, contents in entries:
                    if entry.endswith("/"):
                        self.assertTrue((destination / entry).is_dir())
                    else:
                        self.assertEqual((destination / entry).read_bytes(), contents)
                    self.assertFalse((stage / entry).exists())
                self.assertEqual((destination / "existing/unrelated.txt").read_bytes(), b"preserve")
                self.assertEqual((stage / "mixed/.unrelated.txt").read_bytes(), b"keep")
                self.assertFalse((destination / "mixed/.unrelated.txt").exists())
                self.assertIn("Moved directory " + HOME + "/" + stage.name + "/whole 日本語 with its contents", result.stdout)
                self.assertIn("Moved directory " + HOME + "/" + stage.name + "/existing/subtree with its contents", result.stdout)
                self.assertNotIn("Moved directory " + HOME + "/" + stage.name + "/mixed with its contents", result.stdout)

    def test_staging_recovers_contents_split_between_stage_and_destination(self):
        entries = [("moved/file.txt", b"moved"), ("moved/empty/", b""),
                   ("mixed/already.txt", b"already"), ("mixed/pending.txt", b"pending"),
                   ("mixed/resume.txt", b"abcdef"), ("not-complete.txt", b"complete"), ("empty.txt", b"")]
        for streamed in (False, True):
            with self.subTest(streamed=streamed):
                name = f"recovery-{streamed}"
                destination = self.remote / (name + "-destination")
                stage = self.remote / (name + "-stage")
                (destination / "moved/empty").mkdir(parents=True)
                (destination / "moved/file.txt").write_bytes(b"moved")
                moved_time = (destination / "moved/file.txt").stat().st_mtime_ns
                (destination / "mixed").mkdir()
                (destination / "mixed/already.txt").write_bytes(b"already")
                (destination / "mixed/resume.txt").write_bytes(b"final!")
                (destination / "not-complete.txt").write_bytes(b"much larger old data")
                (destination / "empty.txt").write_bytes(b"")
                (stage / "mixed").mkdir(parents=True)
                (stage / "mixed/pending.txt").write_bytes(b"pending")
                (stage / "mixed/resume.txt").write_bytes(b"XY")
                self.server.sync()
                result = self.transfer(self.archive(entries, streamed), user=USER, password=PASSWORD,
                                       directory=destination.name, staging=stage.name)
                self.success(result)
                self.assertIn("Resuming mixed/resume.txt at byte 2", result.stdout)
                self.assertNotIn("Uploaded moved/file.txt", result.stdout)
                self.assertNotIn("Uploaded mixed/already.txt", result.stdout)
                self.assertNotIn("Uploaded empty.txt", result.stdout)
                for entry, contents in entries:
                    if entry.endswith("/"):
                        self.assertTrue((destination / entry).is_dir())
                    else:
                        self.assertEqual((destination / entry).read_bytes(),
                                         b"XYcdef" if entry == "mixed/resume.txt" else contents)
                    self.assertFalse((stage / entry).exists())
                self.assertEqual((destination / "moved/file.txt").stat().st_mtime_ns, moved_time)
                self.assertFalse((stage / "moved").exists())

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
