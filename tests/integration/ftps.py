#!/usr/bin/env python3
"""CLI FTPS tests using a local TLS server and Python's standard library."""

import argparse
import io
import ssl
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from ftp_faults import FtpServer


FIXTURES = Path(__file__).parents[1] / "fixtures" / "ftps"


class FtpsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(FIXTURES / "server-cert.pem", FIXTURES / "server-key.pem")

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

    def cli(self, archive, server, *options, trusted=True, host="127.0.0.1"):
        command = [ARGS.sext, "--file", str(archive), "--host", host,
                   "--port", str(server.server_address[1]), "--directory", "/upload",
                   "--protocol", "ftps", "--no-tui", "--buffer", "1", "--verbose"]
        if trusted:
            command += ["--cacert", str(FIXTURES / "server-cert.pem")]
        return subprocess.run(command + list(options), capture_output=True, text=True, timeout=15)

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
                    with FtpServer(tls_context=self.context, implicit_tls=implicit, files=files,
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
                        tls_context=self.context, implicit_tls=implicit) as server:
                    options = ["--ftps-mode", "implicit"] if implicit else []
                    # The fixture certificate only identifies 127.0.0.1.
                    host = "localhost" if trusted else "127.0.0.1"
                    result = self.cli(archive, server, "--retries", "1", *options, trusted=trusted, host=host)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn("cert", (result.stdout + result.stderr).lower())
                    self.assertFalse(any(command in ("USER", "PASS", "STOR") for command, _ in server.commands))

    def test_active_mode_encrypts_data_in_both_ftps_modes(self):
        archive = self.archive([("hello.txt", b"hello")])
        for implicit in (False, True):
            with self.subTest(implicit=implicit), FtpServer(
                    tls_context=self.context, implicit_tls=implicit) as server:
                options = ["--ftps-mode", "implicit"] if implicit else []
                result = self.cli(archive, server, "--mode", "active", *options)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(server.files["/upload/hello.txt"], b"hello")
                self.assertGreaterEqual(server.tls_data_connections, 2)

    def test_exhausted_ftps_retries_delete_partial_upload(self):
        archive = self.archive([("hello.txt", b"hello"), ("after.txt", b"after")])
        with FtpServer(tls_context=self.context, fail_uploads=3, drop_after=1) as server:
            result = self.cli(archive, server)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertNotIn("/upload/hello.txt", server.files)
            self.assertNotIn("/upload/after.txt", server.files)
            self.assertEqual(server.deleted, ["/upload/hello.txt"])

    def test_server_must_accept_encrypted_data(self):
        archive = self.archive([("hello.txt", b"hello")])
        with FtpServer(tls_context=self.context, reject_private_data=True) as server:
            result = self.cli(archive, server, "--retries", "1")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(("PROT", "P"), server.commands)
            self.assertEqual(server.uploads, [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sext", required=True)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
