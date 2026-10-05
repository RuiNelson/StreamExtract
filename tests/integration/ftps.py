#!/usr/bin/env python3
"""CLI and GUI core FTP/FTPS tests against the shared Docker server."""

import argparse
import io
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from docker_server import FtpServer, HOME, PASSWORD, USER, Server
from run import Library, LibJob


FIXTURES = Path(__file__).parents[1] / "fixtures" / "ftps"


class FtpsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def archive(self, entries, streamed=False):
        target = Path(self.temp.name) / ("source.tar.gz" if streamed else "source.zip")
        if streamed:
            with tarfile.open(target, "w:gz") as archive:
                for name, payload in entries:
                    header = tarfile.TarInfo(name)
                    header.size = len(payload)
                    archive.addfile(header, io.BytesIO(payload))
        else:
            with zipfile.ZipFile(target, "w") as archive:
                for name, payload in entries:
                    archive.writestr(name, payload)
        return target

    def cli(self, archive, server, *options, trusted=True, host=None):
        command = [ARGS.sext, "--file", str(archive), "--host", host or server.host,
                   "--port", str(server.server_address[1]), "--directory", "/upload",
                   "--protocol", "ftps", "--no-tui", "--buffer", "1", "--verbose"]
        if trusted:
            command += ["--cacert", str(FIXTURES / "server-cert.pem")]
        return self.transfer(command + list(options))

    def transfer(self, command):
        return subprocess.run(command, capture_output=True, text=True, timeout=30)

    def test_real_server_ftp_explicit_and_implicit_ftps(self):
        root = Path(self.temp.name) / "remote"
        payload = bytes(range(251)) * 12000
        entries = [("same.txt", b"same"), ("larger.txt", b"new"), ("nested/data.bin", payload),
                   ("empty.txt", b"")]
        with_server = Server(root)
        self.addCleanup(with_server.close)
        for protocol, mode in (("ftp", "explicit"), ("ftps", "explicit"), ("ftps", "implicit")):
            for streamed in (False, True):
                with self.subTest(protocol=protocol, mode=mode, streamed=streamed):
                    directory = f"{protocol}-{mode}-{streamed}"
                    destination = root / directory
                    (destination / "nested").mkdir(parents=True)
                    (destination / "same.txt").write_bytes(b"same")
                    (destination / "larger.txt").write_bytes(b"larger old file")
                    (destination / "nested" / "data.bin").write_bytes(payload[:19])
                    with_server.sync()
                    archive = self.archive(entries, streamed)
                    port = with_server.implicit_port if mode == "implicit" else with_server.port
                    command = [ARGS.sext, "--file", str(archive), "--host", with_server.host,
                               "--port", str(port), "--user", USER, "--password", PASSWORD,
                               "--directory", HOME + "/" + directory, "--no-tui", "--verbose"]
                    if protocol == "ftps":
                        command += ["--protocol", "ftps", "--ftps-mode", mode,
                                    "--cacert", str(FIXTURES / "server-cert.pem")]
                    result = self.transfer(command)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("Resuming nested/data.bin at byte 19", result.stdout)
                    self.assertIn("DEBUG > APPE data.bin", result.stdout)
                    for name, expected in entries:
                        self.assertEqual((destination / name).read_bytes(), expected)

    def test_explicit_and_implicit_encrypt_uploads_listings_and_retries(self):
        payload = bytes(range(251)) * 12000
        entries = [("same.txt", b"same"), ("larger.txt", b"new"), ("nested/data.bin", payload),
                   ("after.txt", b"after"), ("empty.txt", b"")]
        for implicit in (False, True):
            for streamed in (False, True):
                with self.subTest(implicit=implicit, streamed=streamed):
                    archive = self.archive(entries, streamed)
                    files = {"/upload/same.txt": b"same", "/upload/larger.txt": b"old contents",
                             "/upload/nested/data.bin": payload[:19]}
                    with FtpServer(tls=True, implicit_tls=implicit, files=files,
                                   fail_uploads=1, fail_target="/upload/nested/data.bin") as server:
                        options = ["--ftps-mode", "implicit"] if implicit else []
                        result = self.cli(archive, server, *options)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        for name, expected in entries:
                            self.assertEqual(server.files["/upload/" + name], expected)
                        self.assertEqual(server.deleted, ["/upload/larger.txt"])
                        self.assertEqual([command for command, target, _ in server.uploads
                                          if target == "/upload/nested/data.bin"], ["APPE", "APPE"])
                        self.assertGreater(server.tls_data_connections, len(server.uploads))
                        self.assertIn(("PROT", "P"), server.commands)
                        self.assertEqual(any(command == "AUTH" for command, _ in server.commands), not implicit)
                        if not implicit:
                            self.assertEqual(server.commands[0], ("AUTH", "TLS"))

    def test_ftps_does_not_fall_back_to_plain_ftp(self):
        archive = self.archive([("hello.txt", b"hello")])
        with FtpServer() as server:
            result = self.cli(archive, server, "--retries", "1")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertFalse(any(command in ("USER", "PASS", "STOR") for command, _ in server.commands))

    def test_untrusted_and_wrong_host_certificates_fail_before_login(self):
        archive = self.archive([("hello.txt", b"hello")])
        for implicit in (False, True):
            for trusted in (False, True):
                with self.subTest(implicit=implicit, trusted=trusted), FtpServer(
                        tls=True, implicit_tls=implicit) as server:
                    options = ["--ftps-mode", "implicit"] if implicit else []
                    # The Docker leaf has IP SANs, but does not identify localhost by DNS name.
                    host = "localhost" if trusted else server.host
                    result = self.cli(archive, server, "--retries", "1", *options, trusted=trusted, host=host)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn("cert", (result.stdout + result.stderr).lower())
                    self.assertFalse(any(command in ("USER", "PASS", "STOR") for command, _ in server.commands))

    def test_active_mode_encrypts_data_in_both_ftps_modes(self):
        archive = self.archive([("hello.txt", b"hello")])
        for implicit in (False, True):
            with self.subTest(implicit=implicit), FtpServer(
                    tls=True, implicit_tls=implicit) as server:
                if not server.routable:
                    self.skipTest("Docker active data connections require a routable Linux container")
                options = ["--ftps-mode", "implicit"] if implicit else []
                result = self.cli(archive, server, "--mode", "active", *options)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(server.files["/upload/hello.txt"], b"hello")
                self.assertGreaterEqual(server.tls_data_connections, 2)

    def test_exhausted_ftps_retries_delete_partial_upload(self):
        archive = self.archive([("hello.txt", b"hello"), ("after.txt", b"after")])
        with FtpServer(tls=True, fail_uploads=3, drop_after=1) as server:
            result = self.cli(archive, server)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertNotIn("/upload/hello.txt", server.files)
            self.assertNotIn("/upload/after.txt", server.files)
            self.assertEqual(server.deleted, ["/upload/hello.txt"])

    def test_server_must_accept_encrypted_data(self):
        archive = self.archive([("hello.txt", b"hello")])
        with FtpServer(tls=True, reject_private_data=True) as server:
            result = self.cli(archive, server, "--retries", "1")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(("PROT", "P"), server.commands)
            self.assertEqual(server.uploads, [])


class LibraryFtpsTests(FtpsTests):
    """Run the same TLS, resume and failure cases through the API used by the GUI."""

    def setUp(self):
        if not ARGS.lib:
            self.skipTest("--lib was not supplied")
        super().setUp()
        self.lib = Library(ARGS.lib)

    def transfer(self, command):
        parser = argparse.ArgumentParser()
        parser.add_argument("--file")
        parser.add_argument("--host")
        parser.add_argument("--port", type=int)
        parser.add_argument("--directory")
        parser.add_argument("--user")
        parser.add_argument("--password")
        parser.add_argument("--protocol", default="ftp")
        parser.add_argument("--ftps-mode", default="explicit")
        parser.add_argument("--mode", default="passive")
        parser.add_argument("--cacert")
        parser.add_argument("--buffer", type=int, default=1)
        parser.add_argument("--retries", type=int, default=3)
        parser.add_argument("--no-tui", action="store_true")
        parser.add_argument("--verbose", action="store_true")
        options = parser.parse_args(command[1:])
        protocol = "ftp" if options.protocol == "ftp" else "ftps_" + options.ftps_mode
        with LibJob(self.lib, {
                "archive": options.file, "host": options.host, "port": options.port,
                "directory": options.directory, "user": options.user, "password": options.password,
                "active_mode": options.mode == "active", "verbose": options.verbose,
                "buffer_mib": options.buffer, "protocol": protocol,
                "ca_certificate": options.cacert}, retries=options.retries) as job:
            job.wait(timeout=30)
        # Present library logs in the same format so all assertions cover both callers.
        output = "\n".join(f"{line['level'].upper()} {line['text']}" for line in job.log)
        return subprocess.CompletedProcess(command, 0 if job.result["status"] == "success" else 1,
                                           stdout=output, stderr=job.result["error"] or "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sext", required=True)
    parser.add_argument("--lib", help="also test the GUI core library")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
