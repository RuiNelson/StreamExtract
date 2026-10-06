#!/usr/bin/env python3
"""Docker FTP resume and fault tests; no external archivers required.

python3 tests/integration/ftp_faults.py --sext build/sext --lib build/libstreamextractcore.dylib
"""

import argparse
import io
import re
import socket
import subprocess
import tarfile
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path
from types import SimpleNamespace

from run import Library, LibJob, lib_log_text
from docker_server import FtpServer


class FtpFaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = Library(ARGS.lib)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="streamextract-ftp-faults-")
        self.addCleanup(self.temp.cleanup)

    def archive(self, entries, *, streamed=False):
        """None denotes a directory; repeated names are kept in archive order."""
        archive = Path(self.temp.name) / ("test.tar.gz" if streamed else "test.zip")
        if streamed:
            with tarfile.open(archive, "w:gz") as tar:
                for name, data in entries:
                    header = tarfile.TarInfo(name)
                    if data is None:
                        header.type = tarfile.DIRTYPE
                        tar.addfile(header)
                    else:
                        header.size = len(data)
                        tar.addfile(header, io.BytesIO(data))
        else:
            with zipfile.ZipFile(archive, "w") as zip_archive, warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)  # Deliberate duplicate ZIP members.
                for name, data in entries:
                    zip_archive.writestr(name.rstrip("/") + "/" if data is None else name,
                                         b"" if data is None else data)
        return str(archive)

    def job(self, archive, server, retries=None, **options):
        return LibJob(self.lib, {"archive": archive, "host": getattr(server, "host", "127.0.0.1"), "port": server.server_address[1],
                                 "directory": "/upload", "buffer_mib": 1, **options}, retries=retries)

    def assert_cancelled(self, job, server):
        try:
            self.assertTrue(server.stalled.wait(5), job.describe())
            self.assertEqual(job.poll()["phase"], "transferring")
            job.cancel()
            job.wait(timeout=5)
            self.assertEqual(job.result["status"], "cancelled", job.describe())
        finally:
            # Release before LibJob.__exit__ waits for the controller, even if the test fails.
            server.release.set()

    def test_rejected_resume_preserves_existing_prefix(self):
        original = b"he"
        archive = self.archive([("hello.txt", b"hello\n")])
        for frontend in ("library", "cli"):
            with self.subTest(frontend=frontend), FtpServer(files={"/upload/hello.txt": original},
                                                         reject_upload=True) as server:
                if frontend == "library":
                    with self.job(archive, server) as job:
                        job.wait(timeout=5)
                        self.assertEqual(job.result["status"], "failed", job.describe())
                        self.assertEqual(job.result["files_uploaded"], 0)
                else:
                    result = subprocess.run([ARGS.sext, "--file", archive, "--host", server.host,
                                             "--port", str(server.server_address[1]), "--directory", "/upload",
                                             "--no-tui"], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(server.files.get("/upload/hello.txt"), original)
                self.assertEqual(server.deleted, [])
                attempts = [command for command, _ in server.commands if command in ("STOR", "APPE")]
                self.assertEqual(attempts, ["APPE"] * 3)

    def cli(self, archive, server, *options):
        return subprocess.run([ARGS.sext, "--file", archive, "--host", getattr(server, "host", "127.0.0.1"),
                               "--port", str(server.server_address[1]), "--directory", "/upload",
                               "--no-tui", "--buffer", "1", *options],
                              capture_output=True, text=True, timeout=15)

    def test_upload_failures_resume_immediately_and_continue_in_archive_order(self):
        source = bytes(range(251)) * 12000  # Larger than both the pipe and curl buffers.
        entries = [("before.txt", b"before"), ("nested/", None), ("nested/blocks.bin", source),
                   ("nested/blocks.bin", source + b"tail"), ("after.txt", b"after")]
        for streamed in (False, True):
            archive = self.archive(entries, streamed=streamed)
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend):
                    prefix = source[:19]
                    with FtpServer(files={"/upload/nested/blocks.bin": prefix}, fail_uploads=2,
                                   fail_target="/upload/nested/blocks.bin") as server:
                        if frontend == "library":
                            with self.job(archive, server) as job:
                                job.wait(timeout=15)
                                self.assertEqual(job.result["status"], "success", job.describe())
                                self.assertEqual(job.result["files_uploaded"], 4)
                                expected_bytes = len(source) - len(prefix) + 4 + 6 + 5
                                self.assertEqual(job.result["bytes_uploaded"], expected_bytes)
                                self.assertEqual(job.state["progress"]["total_bytes"], expected_bytes)
                                self.assertEqual(job.state["progress"]["sent_bytes"], expected_bytes)
                                output = lib_log_text(job)
                        else:
                            result = self.cli(archive, server)
                            output = result.stdout + result.stderr
                            self.assertEqual(result.returncode, 0, output)
                        self.assertIn("attempt 2/3", output)
                        self.assertIn("attempt 3/3", output)
                        self.assertEqual(server.files["/upload/nested/blocks.bin"], source + b"tail")
                        self.assertEqual(server.files["/upload/after.txt"], b"after")
                        self.assertEqual(server.deleted, [])
                        uploads = [(command, data) for command, name, data in server.uploads
                                   if name == "/upload/nested/blocks.bin"]
                        self.assertEqual(uploads, [("APPE", source[19:19 + 65539]),
                                                   ("APPE", source[19 + 65539:19 + 2 * 65539]),
                                                   ("APPE", source[19 + 2 * 65539:]), ("APPE", b"tail")])

    def test_exhausted_attempts_remove_partial_and_stop_before_the_next_file(self):
        source = bytes(range(251)) * 12000
        for streamed in (False, True):
            archive = self.archive([("blocks.bin", source), ("after.txt", b"after")], streamed=streamed)
            with self.subTest(streamed=streamed), FtpServer(fail_uploads=3) as server:
                with self.job(archive, server) as job:
                    job.wait(timeout=15)
                    self.assertEqual(job.result["status"], "failed", job.describe())
                    self.assertIn("after 3 attempt(s)", job.result["error"])
                    self.assertEqual(job.result["files_uploaded"], 0)
                self.assertNotIn("/upload/blocks.bin", server.files)
                self.assertNotIn("/upload/after.txt", server.files)
                self.assertEqual(server.deleted, ["/upload/blocks.bin"])
                self.assertEqual([command for command, _, _ in server.uploads], ["STOR", "APPE", "APPE"])

    def test_cli_retries_sets_total_attempts_and_one_disables_retries(self):
        archive = self.archive([("hello.txt", b"hello")])
        for attempts in (1, 2, 4):
            with self.subTest(attempts=attempts), FtpServer(reject_upload=True) as server:
                result = self.cli(archive, server, "--retries", str(attempts))
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"after {attempts} attempt(s)", result.stdout + result.stderr)
                self.assertEqual(sum(command == "STOR" for command, _ in server.commands), attempts)
        for value in ("0", "-1", "1.5", "oops", "4294967296"):
            with self.subTest(value=value), FtpServer() as server:
                result = self.cli(archive, server, "--retries", value)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(server.commands)

    def test_gui_attempt_setting_reaches_the_engine(self):
        archive = self.archive([("hello.txt", b"hello\n")])
        for attempts, expected in ((0, 3), (1, 1), (2, 2), (4, 4)):
            with self.subTest(attempts=attempts), FtpServer(reject_upload=True) as server:
                with self.job(archive, server, retries=attempts) as job:
                    job.wait(timeout=15)
                    self.assertEqual(job.result["status"], "failed", job.describe())
                    self.assertIn(f"after {expected} attempt(s)", job.result["error"])
                self.assertEqual(sum(command == "STOR" for command, _ in server.commands), expected)

        with FtpServer(fail_uploads=3, drop_after=1) as server:
            with self.job(archive, server, retries=4) as job:
                job.wait(timeout=15)
                self.assertEqual(job.result["status"], "success", job.describe())
                self.assertIn("attempt 4/4", lib_log_text(job))
            self.assertEqual(server.files["/upload/hello.txt"], b"hello\n")

    def test_refused_initial_connection_uses_the_configured_attempt_count(self):
        archive = self.archive([("hello.txt", b"hello\n")])
        for frontend in ("library", "cli"):
            for attempts in (None, 1, 4):
                with self.subTest(frontend=frontend, attempts=attempts), socket.socket() as refused:
                    # Close an unused local port so every connect is refused.
                    # A bound but non-listening socket can drop SYNs on macOS.
                    refused.bind(("127.0.0.1", 0))
                    server = SimpleNamespace(server_address=refused.getsockname())
                    refused.close()
                    expected = 3 if attempts is None else attempts
                    if frontend == "library":
                        with self.job(archive, server, retries=attempts, verbose=1) as job:
                            job.wait(timeout=15)
                            self.assertEqual(job.result["status"], "failed", job.describe())
                            self.assertIn(f"after {expected} attempt(s)", job.result["error"])
                            self.assertIsNone(job.state["progress"])
                            output = lib_log_text(job)
                    else:
                        options = [] if attempts is None else ["--retries", str(attempts)]
                        result = self.cli(archive, server, "--verbose", *options)
                        output = result.stdout + result.stderr
                        self.assertEqual(result.returncode, 1, output)
                        self.assertIn(f"after {expected} attempt(s)", output)
                    # curl reports each real TCP connection attempt, including the first.
                    self.assertEqual(output.count("Trying 127.0.0.1:"), expected, output)

    def test_initial_login_retries_can_recover_or_exhaust_the_limit(self):
        archive = self.archive([("hello.txt", b"hello\n")])
        for frontend in ("library", "cli"):
            for failures in (2, 3):
                with self.subTest(frontend=frontend, failures=failures), FtpServer(fail_logins=failures) as server:
                    if frontend == "library":
                        with self.job(archive, server) as job:
                            job.wait(timeout=15)
                            self.assertEqual(job.result["status"], "success" if failures == 2 else "failed",
                                             job.describe())
                            output = lib_log_text(job)
                    else:
                        result = self.cli(archive, server)
                        output = result.stdout + result.stderr
                        self.assertEqual(result.returncode, 0 if failures == 2 else 1, output)
                    self.assertEqual(server.login_attempts, 3)
                    self.assertIn("Retrying connection to", output)
                    self.assertIn("attempt 3/3", output)
                    self.assertEqual(server.files.get("/upload/hello.txt"), b"hello\n" if failures == 2 else None)

    def test_connection_retries_do_not_consume_the_file_upload_attempts(self):
        archive = self.archive([("hello.txt", b"hello\n")])
        with FtpServer(fail_logins=2, fail_uploads=2, drop_after=1) as server, self.job(archive, server) as job:
            job.wait(timeout=15)
            self.assertEqual(job.result["status"], "success", job.describe())
            self.assertIn("Retrying connection to", lib_log_text(job))
            self.assertIn("Retrying hello.txt at byte 2 (attempt 3/3)", lib_log_text(job))
            self.assertEqual(server.files["/upload/hello.txt"], b"hello\n")

    def test_cancellation_during_login_retry_prevents_another_attempt(self):
        archive = self.archive([("hello.txt", b"hello\n")])
        with FtpServer(fail_logins=1, stall_login_retry=True) as server, self.job(archive, server) as job:
            try:
                self.assertTrue(server.stalled.wait(5), job.describe())
                self.assertEqual(job.poll()["phase"], "connecting")
                job.cancel()
                job.wait(timeout=5)
                self.assertEqual(job.result["status"], "cancelled", job.describe())
                self.assertEqual(server.login_attempts, 2)
                self.assertNotIn("attempt 3/3", lib_log_text(job))
                self.assertEqual(server.uploads, [])
            finally:
                server.release.set()

    def test_cancellation_interrupts_size_check_before_retry_and_removes_partial(self):
        archive = self.archive([("blocks.bin", bytes(range(251)) * 12000)])
        with FtpServer(fail_uploads=1, stall_retry_size=True) as server, self.job(archive, server) as job:
            self.assert_cancelled(job, server)
            self.assertEqual(server.deleted, ["/upload/blocks.bin"])
            self.assertEqual(sum(command in ("STOR", "APPE") for command, _ in server.commands), 1)

    def test_rereading_rar_solid_7z_and_encrypted_archives(self):
        fixtures = (Path(__file__).parents[1] / "archive_fixtures.hpp").read_text()
        for fixture, target, expected, password in (
                ("kTinyRar", "hello.txt", b"hello\n", None),
                ("kSolid7z", "b.txt", b"second\n", None),
                ("kAesNonSolid7z", "b.txt", b"second\n", "secret"),
                ("kAesHeaders7z", "hello.txt", b"hello\n", "secret"),
                ("kAesZip", "hello.txt", b"hello\n", "secret")):
            with self.subTest(fixture=fixture):
                # Reuse real archives already checked by the C++ tests.
                match = re.search(rf"{fixture}\[\] = \{{(.*?)\}};", fixtures, re.DOTALL)
                self.assertIsNotNone(match)
                archive = Path(self.temp.name) / fixture
                archive.write_bytes(bytes(int(value, 16) for value in re.findall(r"0x[0-9a-f]+", match[1])))
                with FtpServer(fail_uploads=2, drop_after=2, fail_target="/upload/" + target) as server:
                    with self.job(str(archive), server, archive_password=password) as job:
                        job.wait(timeout=15)
                        self.assertEqual(job.result["status"], "success", job.describe())
                        self.assertIn("attempt 3/3", lib_log_text(job))
                    self.assertEqual(server.files["/upload/" + target], expected)
                    self.assertEqual(server.deleted, [])
                    if target == "b.txt":
                        self.assertEqual(server.files["/upload/a.txt"], b"first\n")

    def test_rereading_a_real_exfat_volume(self):
        with zipfile.ZipFile(Path(__file__).parents[1] / "fixtures" / "exfat.zip") as fixtures:
            archive = Path(self.temp.name) / "volume.exfat"
            archive.write_bytes(fixtures.read("volume.exfat"))
        with FtpServer(fail_uploads=2, drop_after=2, fail_target="/upload/hello.txt") as server:
            with self.job(str(archive), server) as job:
                job.wait(timeout=15)
                self.assertEqual(job.result["status"], "success", job.describe())
                self.assertIn("attempt 3/3", lib_log_text(job))
            self.assertEqual(server.files["/upload/hello.txt"], b"hello\n")
            self.assertEqual(server.deleted, [])

    def test_rereading_pfs_pfsc_and_wrapped_volumes(self):
        source = bytes(range(256)) * 4608 + b"end"
        with zipfile.ZipFile(Path(__file__).parents[1] / "fixtures" / "pfs.zip") as fixtures:
            for name in ("raw.ffpfsc", "nested.ffpfsc", "ufs.ffpfsc", "ufs2.ffpfsc"):
                archive = Path(self.temp.name) / name
                archive.write_bytes(fixtures.read(name))
                for frontend in ("library", "cli"):
                    with self.subTest(name=name, frontend=frontend), FtpServer(
                            files={"/upload/large.bin": source[:19]}, fail_uploads=2, drop_after=65539,
                            fail_target="/upload/large.bin") as server:
                        if frontend == "library":
                            with self.job(str(archive), server) as job:
                                job.wait(timeout=30)
                                self.assertEqual(job.result["status"], "success", job.describe())
                                output = lib_log_text(job)
                        else:
                            result = self.cli(str(archive), server)
                            output = result.stdout + result.stderr
                            self.assertEqual(result.returncode, 0, output)
                        self.assertIn("attempt 3/3", output)
                        self.assertEqual(server.files["/upload/large.bin"], source)
                        self.assertEqual(server.deleted, [])

    def test_pfs_cancellation_removes_partial_file(self):
        with zipfile.ZipFile(Path(__file__).parents[1] / "fixtures" / "pfs.zip") as fixtures:
            for name in ("raw.ffpfsc", "nested.ffpfsc", "ufs.ffpfsc"):
                archive = Path(self.temp.name) / name
                archive.write_bytes(fixtures.read(name))
                with self.subTest(name=name), FtpServer(fail_uploads=1, stall_retry_size=True,
                                                       fail_target="/upload/large.bin") as server:
                    with self.job(str(archive), server) as job:
                        self.assert_cancelled(job, server)
                        self.assertNotIn("/upload/large.bin", server.files)

    def test_retry_handles_a_lost_original_prefix_without_invalid_progress(self):
        source = bytes(range(251)) * 12000
        for streamed in (False, True):
            archive = self.archive([("blocks.bin", source)], streamed=streamed)
            with self.subTest(streamed=streamed), FtpServer(
                    files={"/upload/blocks.bin": source[:19]}, fail_uploads=1,
                    after_failure=lambda server, target: server.files.pop(target)) as server:
                with self.job(archive, server) as job:
                    def check_progress(current_job, state):
                        progress = state["progress"]
                        if progress is not None:
                            self.assertLessEqual(progress["sent_bytes"], progress["total_bytes"],
                                                 current_job.describe())
                    job.wait(timeout=15, on_state=check_progress)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["bytes_uploaded"], len(source))
                    self.assertEqual(job.state["progress"]["sent_bytes"], len(source))
                    self.assertEqual(job.state["progress"]["total_bytes"], len(source))
                self.assertEqual(server.files["/upload/blocks.bin"], source)
                self.assertEqual([command for command, _, _ in server.uploads], ["APPE", "STOR"])

    def test_checksum_failure_during_rereading_stops_without_another_retry(self):
        archive = Path(self.archive([("hello.txt", b"hello\n")]))

        def corrupt_archive(server, target):
            damaged = bytearray(archive.read_bytes())
            offset = 30 + int.from_bytes(damaged[26:28], "little") + int.from_bytes(damaged[28:30], "little")
            damaged[offset] ^= 1
            archive.write_bytes(damaged)

        with FtpServer(fail_uploads=1, drop_after=2, after_failure=corrupt_archive) as server:
            with self.job(str(archive), server) as job:
                job.wait(timeout=15)
                self.assertEqual(job.result["status"], "failed", job.describe())
                self.assertIn("checksum", job.result["error"].lower())
                self.assertEqual(job.result["files_uploaded"], 0)
                self.assertNotIn("attempt 3/3", lib_log_text(job))
            self.assertNotIn("/upload/hello.txt", server.files)
            self.assertEqual(server.deleted, ["/upload/hello.txt"])

    def test_cancellation_interrupts_directory_creation_and_streamed_probe(self):
        for streamed, entries in ((False, [("stall", None)]), (True, [("stall", None)]),
                                  (True, [("stall/hello.txt", b"hello\n")])):
            with self.subTest(streamed=streamed, entries=entries):
                archive = self.archive(entries, streamed=streamed)
                with FtpServer(stall_directory="/upload/stall") as server, self.job(archive, server) as job:
                    self.assert_cancelled(job, server)
                    self.assertEqual(server.deleted, [])

    def test_cancellation_interrupts_streamed_listing(self):
        archive = self.archive([("hello.txt", b"hello\n")], streamed=True)
        with FtpServer(stall_listing=True) as server, self.job(archive, server) as job:
            self.assert_cancelled(job, server)

    def test_cancelled_empty_upload_is_removed(self):
        archive = self.archive([("empty.txt", b"")])
        with FtpServer(stall_confirmation=True) as server, self.job(archive, server) as job:
            self.assert_cancelled(job, server)
            self.assertNotIn("/upload/empty.txt", server.files)
            self.assertEqual(server.deleted, ["/upload/empty.txt"])

    def test_remote_sizes_select_skip_resume_or_delete_and_replace(self):
        source = b"hello\n"
        for streamed in (False, True):
            for name in ("hello.txt", ".hidden"):
                for remote in (None, b"", source[:2], source[:-1], b"other\n", b"XY", b"larger remote contents"):
                    with self.subTest(streamed=streamed, name=name, remote=remote):
                        archive = self.archive([(name, source)], streamed=streamed)
                        files = {"/upload/other.txt": b"hi"}
                        if remote is not None:
                            files["/upload/" + name] = remote
                        with FtpServer(files=files) as server, self.job(archive, server) as job:
                            job.wait(timeout=5)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            skipped = remote is not None and len(remote) == len(source)
                            resumed = remote is not None and 0 < len(remote) < len(source)
                            replaced = remote is not None and len(remote) > len(source)
                            sent = 0 if skipped else len(source) - (len(remote) if resumed else 0)
                            self.assertEqual(job.result["files_uploaded"], 0 if skipped else 1)
                            self.assertEqual(job.result["skipped_files"], 1 if skipped else 0)
                            self.assertEqual(job.result["bytes_uploaded"], sent)
                            self.assertEqual(job.state["progress"]["total_bytes"], sent)
                            self.assertEqual(job.state["progress"]["sent_bytes"], sent)
                            expected = remote if skipped else remote + source[len(remote):] if resumed else source
                            self.assertEqual(server.files["/upload/" + name], expected)
                            self.assertEqual(server.deleted, ["/upload/" + name] if replaced else [])
                            uploads = [] if skipped else [("APPE" if resumed else "STOR", "/upload/" + name,
                                                          source[-sent:] if sent else b"")]
                            self.assertEqual(server.uploads, uploads)
                            if replaced:
                                commands = [command for command, _ in server.commands]
                                self.assertLess(commands.index("DELE"), commands.index("STOR"))

    def test_failed_delete_stops_before_uploading_a_larger_remote_file(self):
        original = b"larger remote contents"
        for streamed in (False, True):
            with self.subTest(streamed=streamed):
                archive = self.archive([("hello.txt", b"hello\n")], streamed=streamed)
                with FtpServer(files={"/upload/hello.txt": original}, reject_delete=True) as server:
                    with self.job(archive, server) as job:
                        job.wait(timeout=5)
                        self.assertEqual(job.result["status"], "failed", job.describe())
                        self.assertIn("cannot delete the larger remote file", job.result["error"])
                    self.assertEqual(server.files["/upload/hello.txt"], original)
                    self.assertFalse(any(command in ("STOR", "APPE") for command, _ in server.commands))

    def test_resume_offsets_across_pipe_blocks_send_only_the_remaining_bytes(self):
        source = bytes(range(251)) * 12000
        for streamed in (False, True):
            archive = self.archive([("blocks.bin", source)], streamed=streamed)
            for offset in ((1 << 20) - 3, 1 << 20, (1 << 20) + 19, len(source) - 1):
                with self.subTest(streamed=streamed, offset=offset):
                    with FtpServer(files={"/upload/blocks.bin": source[:offset]}) as server:
                        with self.job(archive, server) as job:
                            job.wait(timeout=5)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            self.assertEqual(job.result["bytes_uploaded"], len(source) - offset)
                            self.assertEqual(job.state["progress"]["sent_bytes"], len(source) - offset)
                        self.assertEqual(server.files["/upload/blocks.bin"], source)
                        self.assertEqual(server.uploads, [("APPE", "/upload/blocks.bin", source[offset:])])
                        self.assertEqual(server.deleted, [])

    def test_resume_still_checks_the_checksum_of_the_discarded_prefix(self):
        source = bytes(range(251)) * 12000
        archive = Path(self.archive([("blocks.bin", source)]))
        damaged = bytearray(archive.read_bytes())
        data_offset = 30 + int.from_bytes(damaged[26:28], "little") + int.from_bytes(damaged[28:30], "little")
        damaged[data_offset + 1] ^= 1  # Damage a byte inside the prefix which will be discarded.
        archive.write_bytes(damaged)
        prefix = source[:(1 << 20) + 19]
        with FtpServer(files={"/upload/blocks.bin": prefix}) as server, self.job(str(archive), server) as job:
            job.wait(timeout=5)
            self.assertEqual(job.result["status"], "failed", job.describe())
            self.assertIn("checksum", job.result["error"].lower())
            self.assertEqual(job.result["files_uploaded"], 0)

    def test_duplicate_names_use_the_size_left_by_the_previous_copy(self):
        name = "nested/hello.txt"
        entries = [(name, b"first\n"), (name, b"first\nsecond\n"), (name, b"other\nsecond\n"), (name, b"last\n")]
        for streamed in (False, True):
            for remote in (None, b"fi", b"other\n"):
                with self.subTest(streamed=streamed, remote=remote):
                    archive = self.archive(entries, streamed=streamed)
                    files = {} if remote is None else {"/upload/" + name: remote}
                    with FtpServer(files=files) as server, self.job(archive, server) as job:
                        job.wait(timeout=5)
                        self.assertEqual(job.result["status"], "success", job.describe())
                        self.assertEqual(job.result["files_uploaded"], 2 if remote == b"other\n" else 3)
                        self.assertEqual(job.result["skipped_files"], 2 if remote == b"other\n" else 1)
                        self.assertEqual(job.result["bytes_uploaded"], 18 - (len(remote) if remote else 0))
                        self.assertEqual(server.files["/upload/" + name], b"last\n")
                        self.assertEqual(server.deleted, ["/upload/" + name])
                        self.assertEqual(server.uploads[-2:], [("APPE", "/upload/" + name, b"second\n"),
                                                             ("STOR", "/upload/" + name, b"last\n")])
                        self.assertIn("copies are processed in archive order", lib_log_text(job))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sext", required=True)
    parser.add_argument("--lib", required=True)
    ARGS, remaining = parser.parse_known_args()
    ARGS.sext = str(Path(ARGS.sext).resolve())
    ARGS.lib = str(Path(ARGS.lib).resolve())
    unittest.main(argv=[__file__, *remaining])
