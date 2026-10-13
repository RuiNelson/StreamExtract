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
import time
import unittest
import warnings
import zipfile
from pathlib import Path
from types import SimpleNamespace

from run import Library, LibJob, lib_log_text
from docker_server import CA_CERTIFICATE, FtpServer

SELF_MAGIC = (0x1D3D154F).to_bytes(4, "little")  # PS4 SELF header, as in tests/docker/fault_server.py.


class FtpTestCase(unittest.TestCase):
    """Archives and both front-ends (C API job, CLI) against fault sessions."""

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

    def wait_for_progress(self, job, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = job.poll()
            progress = state["progress"]
            if progress is not None and predicate(progress):
                return progress
            if state["phase"] == "finished":
                break
            time.sleep(0.02)
        self.fail("expected transfer progress was not reached: " + job.describe())

    def cli(self, archive, server, *options):
        return subprocess.run([ARGS.sext, "--file", archive, "--host", getattr(server, "host", "127.0.0.1"),
                               "--port", str(server.server_address[1]), "--directory", "/upload",
                               "--no-tui", "--buffer", "1", *options],
                              capture_output=True, text=True, timeout=15)


class FtpFaultTests(FtpTestCase):
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

    def test_staging_resumes_and_skips_only_in_staging_and_moves_after_all_uploads(self):
        entries = [("same.txt", b"same"), ("resume.txt", b"abcdef"), ("large.txt", b"new"),
                   ("nested/a 日本語.txt", b"nested"), ("empty.txt", b""), ("empty-dir/", None),
                   ("duplicate.txt", b"first"), ("duplicate.txt", b"last")]
        initial = {"/stage/same.txt": b"same", "/stage/resume.txt": b"XY",
                   "/stage/large.txt": b"much larger old data", "/stage/unrelated.txt": b"keep",
                   "/upload/resume.txt": b"final!", "/upload/large.txt": b"old",
                   "/upload/unrelated.txt": b"preserve"}
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend), FtpServer(files=initial) as server:
                    archive = self.archive(entries, streamed=streamed)
                    if frontend == "library":
                        with self.job(archive, server, staging="/stage") as job:
                            job.wait(timeout=15)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            self.assertEqual(job.result["skipped_files"], 1)
                    else:
                        result = self.cli(archive, server, "--staging", "/stage")
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    expected = {"same.txt": b"same", "resume.txt": b"XYcdef", "large.txt": b"new",
                                "nested/a 日本語.txt": b"nested", "empty.txt": b"", "duplicate.txt": b"last"}
                    for name, contents in expected.items():
                        self.assertEqual(server.files["/upload/" + name], contents)
                        self.assertNotIn("/stage/" + name, server.files)
                    self.assertEqual(server.files["/stage/unrelated.txt"], b"keep")
                    self.assertEqual(server.files["/upload/unrelated.txt"], b"preserve")
                    uploads = [(index, command, arg) for index, (command, arg) in enumerate(server.commands)
                               if command in ("STOR", "APPE")]
                    first_move = next(i for i, (command, _) in enumerate(server.commands) if command == "RNFR")
                    self.assertTrue(all(i < first_move for i, _, _ in uploads))
                    self.assertIn(("APPE", "resume.txt"), [(command, arg) for _, command, arg in uploads])
                    self.assertFalse(any(arg == "same.txt" for _, _, arg in uploads))
                    self.assertEqual(sum(command == "RNTO" for command, _ in server.commands), len(expected))

    def test_staging_all_complete_still_moves_files(self):
        archive = self.archive([("done.txt", b"done")])
        with FtpServer(files={"/stage/done.txt": b"done"}) as server, self.job(archive, server, staging="/stage") as job:
            job.wait(timeout=5)
            self.assertEqual(job.result["status"], "success", job.describe())
            self.assertEqual(job.result["files_uploaded"], 0)
            self.assertEqual(server.files, {"/upload/done.txt": b"done"})

    def test_staging_new_destination_parent_skips_checks_for_its_absent_children(self):
        entries = [(f"tree/dir{index:02}/file{file}.txt", f"{index}:{file}\n".encode())
                   for index in range(12) for file in range(3)]
        initial = {"/stage/" + name: data for name, data in entries}
        initial["/stage/tree/unrelated.txt"] = b"keep in staging"
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend), FtpServer(
                        files=initial, directories=["/stage", "/upload"]) as server:
                    archive = self.archive(entries, streamed=streamed)
                    if frontend == "library":
                        with self.job(archive, server, staging="/stage") as job:
                            job.wait(timeout=15)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            self.assertEqual(job.result["files_uploaded"], 0)
                            self.assertEqual(job.result["skipped_files"], len(entries))
                    else:
                        result = self.cli(archive, server, "--staging", "/stage")
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    for name, data in entries:
                        self.assertEqual(server.files["/upload/" + name], data)
                        self.assertNotIn("/stage/" + name, server.files)
                    self.assertEqual(server.files["/stage/tree/unrelated.txt"], b"keep in staging")
                    self.assertEqual(server.uploads, [])
                    moves = [arg for command, arg in server.commands if command == "RNFR"]
                    self.assertEqual(moves, [f"/stage/tree/dir{index:02}" for index in range(12)])
                    redundant = [arg for command, arg in server.commands
                                 if command == "CWD" and arg.startswith("/upload/tree/dir")]
                    self.assertEqual(redundant, [])

    def test_staging_unowned_child_under_a_new_parent_uses_direct_mkd(self):
        entries = [(f"tree/dir{index}/file.txt", f"file {index}\n".encode()) for index in range(8)]
        initial = {"/stage/" + name: data for name, data in entries}
        initial["/stage/tree/unrelated.txt"] = b"keep parent"
        initial.update({f"/stage/tree/dir{index}/unrelated.txt": b"keep child" for index in range(8)})
        with FtpServer(files=initial, directories=["/stage", "/upload"]) as server:
            with self.job(self.archive(entries), server, staging="/stage") as job:
                job.wait(timeout=15)
                self.assertEqual(job.result["status"], "success", job.describe())
            for name, data in entries:
                self.assertEqual(server.files["/upload/" + name], data)
                self.assertNotIn("/stage/" + name, server.files)
            for path, data in initial.items():
                if path.endswith("unrelated.txt"):
                    self.assertEqual(server.files[path], data)
            children = {f"/upload/tree/dir{index}" for index in range(8)}
            direct_mkd = [arg for command, arg in server.commands if command == "MKD" and arg in children]
            self.assertEqual(set(direct_mkd), children)
            self.assertEqual(len(direct_mkd), len(children))
            self.assertFalse(any(command == "CWD" and arg in children for command, arg in server.commands))

    def test_staging_case_collisions_under_new_parents_keep_destination_checks(self):
        entries = [("tree/A/one.txt", b"one"), ("tree/a/two.txt", b"two")]
        initial = {"/stage/" + name: data for name, data in entries}
        initial["/stage/tree/unrelated.txt"] = b"keep"
        with FtpServer(files=initial, directories=["/stage", "/upload"]) as server:
            with self.job(self.archive(entries), server, staging="/stage") as job:
                job.wait(timeout=10)
                self.assertEqual(job.result["status"], "success", job.describe())
            self.assertTrue(any(command == "CWD" and arg == "/upload/tree/a"
                                for command, arg in server.commands))
            for name, data in entries:
                self.assertEqual(server.files["/upload/" + name], data)
            self.assertEqual(server.files["/stage/tree/unrelated.txt"], b"keep")

    def test_staging_normalized_names_under_new_parents_keep_destination_checks(self):
        for names in (("K", "\u00c4", "\u00e4"), ("dir", "dir."), ("dir", "dir "),
                      ("LongDirectory", "LONGDI~1")):
            with self.subTest(names=names):
                entries = [(f"tree/{name}/file{index}.txt", f"{index}\n".encode())
                           for index, name in enumerate(names)]
                initial = {"/stage/" + name: data for name, data in entries}
                initial["/stage/tree/unrelated.txt"] = b"keep"
                with FtpServer(files=initial, directories=["/stage", "/upload"]) as server:
                    with self.job(self.archive(entries), server, staging="/stage") as job:
                        job.wait(timeout=10)
                        self.assertEqual(job.result["status"], "success", job.describe())
                    created_parent = server.commands.index(("MKD", "/upload/tree"))
                    for name in names:
                        self.assertIn(("CWD", "/upload/tree/" + name), server.commands[created_parent + 1:])
                    for name, data in entries:
                        self.assertEqual(server.files["/upload/" + name], data)
                    self.assertEqual(server.files["/stage/tree/unrelated.txt"], b"keep")

    def test_staging_upload_failure_does_not_move_completed_files(self):
        archive = self.archive([("first.txt", b"first"), ("broken.txt", b"broken")])
        with FtpServer(files={"/upload/first.txt": b"original"}, fail_uploads=3,
                       fail_target="/stage/broken.txt", drop_after=1) as server:
            with self.job(archive, server, staging="/stage") as job:
                job.wait(timeout=15)
                self.assertEqual(job.result["status"], "failed", job.describe())
            self.assertEqual(server.files["/stage/first.txt"], b"first")
            self.assertEqual(server.files["/upload/first.txt"], b"original")
            self.assertFalse(any(command == "RNFR" for command, _ in server.commands))

    def test_staging_move_failure_and_cancellation_leave_staged_files(self):
        archive = self.archive([("done.txt", b"done")])
        for cancel in (False, True):
            with self.subTest(cancel=cancel), FtpServer(reject_rename=not cancel, stall_rename=cancel) as server:
                with self.job(archive, server, staging="/stage") as job:
                    if cancel:
                        self.assert_cancelled(job, server)
                    else:
                        job.wait(timeout=5)
                        self.assertEqual(job.result["status"], "failed", job.describe())
                        self.assertIn("cannot move staged path", job.result["error"])
                    if not cancel:
                        self.assertEqual(server.files, {"/stage/done.txt": b"done"})

    def test_staging_path_validation_prevents_unsafe_moves(self):
        for stage, entries in (("/upload", [("file.txt", b"file")]),
                               ("/", [("file.txt", b"file")]),
                               ("/upload/stage", [("stage/file.txt", b"file")])):
            with self.subTest(stage=stage), FtpServer() as server:
                with self.job(self.archive(entries), server, staging=stage) as job:
                    job.wait(timeout=5)
                    self.assertEqual(job.result["status"], "failed", job.describe())
                self.assertFalse(any(command == "RNFR" for command, _ in server.commands))

    def test_staging_recovery_after_a_directory_move_and_a_later_failure(self):
        entries = [("a-folder/one.txt", b"one"), ("a-folder/two.txt", b"two"),
                   ("a-folder/empty/", None), ("b-folder/three.txt", b"three"), ("tail.txt", b"tail")]
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                for lost_reply in (False, True):
                    with self.subTest(streamed=streamed, frontend=frontend, lost_reply=lost_reply):
                        archive = self.archive(entries, streamed=streamed)
                        fault = {"lose_rename_reply_after": 1} if lost_reply else {"fail_rename_after": 1}
                        with FtpServer(files={"/stage/unrelated.txt": b"keep", "/upload/unrelated.txt": b"preserve"},
                                       directories=["/stage", "/upload"], **fault) as server:
                            if frontend == "library":
                                with self.job(archive, server, staging="/stage") as job:
                                    job.wait(timeout=15)
                                    self.assertEqual(job.result["status"], "failed", job.describe())
                            else:
                                result = self.cli(archive, server, "--staging", "/stage")
                                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                            self.assertEqual(server.files["/upload/a-folder/one.txt"], b"one")
                            self.assertNotIn("/stage/a-folder/one.txt", server.files)
                            self.assertEqual(server.files["/stage/b-folder/three.txt"], b"three")
                            state = {"files": dict(server.files), "directories": server.get("directories")}
                        # A fresh process/client resumes from the remote state, including a lost RNTO reply.
                        for rerun in (0, 1):
                            with FtpServer(**state) as server:
                                if frontend == "library":
                                    with self.job(archive, server, staging="/stage") as job:
                                        job.wait(timeout=15)
                                        self.assertEqual(job.result["status"], "success", job.describe())
                                        self.assertEqual(job.result["files_uploaded"], 0)
                                        self.assertEqual(job.result["bytes_uploaded"], 0)
                                        self.assertEqual(job.result["skipped_files"], 4)
                                else:
                                    result = self.cli(archive, server, "--staging", "/stage")
                                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                                    if not streamed:
                                        self.assertIn("To upload: 0 file(s)", result.stdout)
                                self.assertEqual(server.uploads, [])
                                self.assertFalse(any(command == "RNFR" and arg.startswith("/stage/a-folder")
                                                     for command, arg in server.commands))
                                if rerun == 1:
                                    self.assertFalse(any(command == "RNFR" for command, _ in server.commands))
                                for name, contents in entries:
                                    if contents is not None:
                                        self.assertEqual(server.files["/upload/" + name], contents)
                                        self.assertNotIn("/stage/" + name, server.files)
                                self.assertEqual(server.files["/stage/unrelated.txt"], b"keep")
                                self.assertEqual(server.files["/upload/unrelated.txt"], b"preserve")
                                state = {"files": dict(server.files), "directories": server.get("directories")}

    def test_staging_recovery_prioritizes_staged_copies_and_only_skips_complete_destination_files(self):
        entries = [("moved/file.txt", b"moved"), ("same.txt", b"same"), ("resume.txt", b"abcdef"),
                   ("replace.txt", b"new"), ("empty.txt", b""), ("missing.txt", b"missing")]
        initial = {"/upload/moved/file.txt": b"moved", "/upload/empty.txt": b"", "/upload/missing.txt": b"x",
                   "/stage/same.txt": b"same", "/upload/same.txt": b"old!",
                   "/stage/resume.txt": b"XY", "/upload/resume.txt": b"final!",
                   "/stage/replace.txt": b"much larger", "/upload/replace.txt": b"old"}
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend), FtpServer(files=initial) as server:
                    archive = self.archive(entries, streamed=streamed)
                    if frontend == "library":
                        with self.job(archive, server, staging="/stage") as job:
                            job.wait(timeout=15)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            self.assertEqual(job.result["files_uploaded"], 3)
                            self.assertEqual(job.result["skipped_files"], 3)
                    else:
                        result = self.cli(archive, server, "--staging", "/stage")
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    for name, contents in entries:
                        self.assertEqual(server.files["/upload/" + name], b"XYcdef" if name == "resume.txt" else contents)
                        self.assertNotIn("/stage/" + name, server.files)
                    self.assertEqual({target for _, target, _ in server.uploads},
                                     {"/stage/resume.txt", "/stage/replace.txt", "/stage/missing.txt"})
                    self.assertIn(("APPE", "/stage/resume.txt", b"cdef"), server.uploads)
                    self.assertIn(("STOR", "/stage/missing.txt", b"missing"), server.uploads)
                    self.assertFalse(any(command == "RNFR" and arg in ("/stage/moved/file.txt", "/stage/empty.txt")
                                         for command, arg in server.commands))

    def test_staging_recovery_keeps_duplicate_entries_in_archive_order(self):
        entries = [("duplicate.txt", b"first"), ("duplicate.txt", b"first"),
                   ("duplicate.txt", b"first-more"), ("duplicate.txt", b"last")]
        for streamed in (False, True):
            with self.subTest(streamed=streamed), FtpServer(files={"/upload/duplicate.txt": b"first"}) as server:
                with self.job(self.archive(entries, streamed=streamed), server, staging="/stage") as job:
                    job.wait(timeout=15)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["files_uploaded"], 2)
                    self.assertEqual(job.result["skipped_files"], 2)
                self.assertEqual(server.files, {"/upload/duplicate.txt": b"last"})
                self.assertEqual(server.uploads, [("STOR", "/stage/duplicate.txt", b"first-more"),
                                                 ("STOR", "/stage/duplicate.txt", b"last")])

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

    def test_small_files_are_extracted_ahead_of_a_pending_upload_confirmation(self):
        entries = [("first.txt", b"first"), ("empty.txt", b""), ("nested/", None)]
        entries += [(f"nested/{index:03}.bin", bytes([index]) * 4096) for index in range(48)]
        files = [(name, data) for name, data in entries if data is not None]
        total_bytes = sum(len(data) for _, data in files)
        for streamed in (False, True):
            archive = self.archive(entries, streamed=streamed)
            with self.subTest(streamed=streamed), FtpServer(stall_confirmation=True) as server:
                with self.job(archive, server) as job:
                    try:
                        self.assertTrue(server.stalled.wait(5), job.describe())
                        progress = self.wait_for_progress(job, lambda p: p["unpacked_bytes"] == total_bytes
                                                          and p["buffer_used"] > 0)
                        self.assertEqual(progress["files_done"], 0)
                        self.assertEqual(progress["current_file"], "first.txt")
                        self.assertLessEqual(progress["buffer_used"], progress["buffer_capacity"])
                        self.assertEqual(server.uploads, [("STOR", "/upload/first.txt", b"first")])
                    finally:
                        server.release.set()
                    job.wait(timeout=15)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["files_uploaded"], len(files))
                    self.assertEqual(job.result["bytes_uploaded"], total_bytes)
                    self.assertEqual(job.state["progress"]["buffer_used"], 0)
                self.assertEqual(server.uploads, [("STOR", "/upload/" + name, data) for name, data in files])
                self.assertEqual(dict(server.files), {"/upload/" + name: data for name, data in files})

    def test_retry_can_finish_while_the_queue_of_following_small_files_is_full(self):
        first = b"first\n"
        entries = [("first.txt", first)]
        entries += [(f"{index:03}.bin", bytes([index]) * 32768) for index in range(64)]
        total_bytes = sum(len(data) for _, data in entries)
        for streamed in (False, True):
            archive = self.archive(entries, streamed=streamed)
            with self.subTest(streamed=streamed), FtpServer(fail_uploads=1, drop_after=2,
                                                         stall_retry_size=True) as server:
                with self.job(archive, server) as job:
                    try:
                        self.assertTrue(server.stalled.wait(5), job.describe())
                        progress = self.wait_for_progress(job, lambda p: p["buffer_used"] == p["buffer_capacity"])
                        self.assertGreater(progress["unpacked_bytes"], progress["buffer_capacity"])
                        self.assertEqual(progress["files_done"], 0)
                        self.assertEqual(server.uploads, [("STOR", "/upload/first.txt", first[:2])])
                    finally:
                        server.release.set()
                    job.wait(timeout=20)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["files_uploaded"], len(entries))
                    self.assertEqual(job.result["bytes_uploaded"], total_bytes)
                    self.assertEqual(job.state["progress"]["sent_bytes"], total_bytes)
                    self.assertEqual(job.state["progress"]["total_bytes"], total_bytes)
                    self.assertIn("Retrying first.txt at byte 2 (attempt 2/3)", lib_log_text(job))
                expected_uploads = [("STOR", "/upload/first.txt", first[:2]),
                                    ("APPE", "/upload/first.txt", first[2:])]
                expected_uploads += [("STOR", "/upload/" + name, data) for name, data in entries[1:]]
                self.assertEqual(server.uploads, expected_uploads)
                self.assertEqual(dict(server.files), {"/upload/" + name: data for name, data in entries})
                self.assertEqual(server.deleted, [])

    def test_cancellation_with_a_full_small_file_queue_stops_before_the_next_upload(self):
        entries = [("first.txt", b"first")]
        entries += [(f"{index:03}.bin", bytes([index]) * 32768) for index in range(64)]
        for streamed in (False, True):
            archive = self.archive(entries, streamed=streamed)
            with self.subTest(streamed=streamed), FtpServer(stall_confirmation=True) as server:
                with self.job(archive, server) as job:
                    try:
                        self.assertTrue(server.stalled.wait(5), job.describe())
                        self.wait_for_progress(job, lambda p: p["buffer_used"] == p["buffer_capacity"])
                        job.cancel()
                        job.wait(timeout=5)
                        self.assertEqual(job.result["status"], "cancelled", job.describe())
                        self.assertEqual(job.result["files_uploaded"], 0)
                        self.assertEqual(job.state["progress"]["buffer_used"], 0)
                        self.assertEqual(server.deleted, ["/upload/first.txt"])
                        self.assertEqual(server.uploads, [("STOR", "/upload/first.txt", b"first")])
                        self.assertEqual(dict(server.files), {})
                    finally:
                        server.release.set()

    def test_cancellation_interrupts_an_active_retry_with_following_files_queued(self):
        # Exceed TCP/curl buffering so the retry reader is still producing data when cancelled.
        source = bytes(range(251)) * 48000
        entries = [("first.bin", source)]
        entries += [(f"{index:03}.bin", bytes([index]) * 32768) for index in range(64)]
        for streamed in (False, True):
            archive = self.archive(entries, streamed=streamed)
            with self.subTest(streamed=streamed), FtpServer(fail_uploads=1, drop_after=65539,
                                                         stall_upload_after=200000) as server:
                with self.job(archive, server) as job:
                    try:
                        self.assertTrue(server.stalled.wait(5), job.describe())
                        progress = self.wait_for_progress(job, lambda p: p["buffer_used"] == p["buffer_capacity"])
                        self.assertEqual(progress["files_done"], 0)
                        self.assertIn("Retrying first.bin at byte 65539 (attempt 2/3)", lib_log_text(job))
                        job.cancel()
                        job.wait(timeout=5)
                        self.assertEqual(job.result["status"], "cancelled", job.describe())
                        self.assertEqual(job.result["files_uploaded"], 0)
                        self.assertEqual(job.state["progress"]["buffer_used"], 0)
                        self.assertEqual(server.deleted, ["/upload/first.bin"])
                        self.assertEqual(dict(server.files), {})
                        commands = [command for command, _ in server.commands if command in ("STOR", "APPE")]
                        self.assertEqual(commands, ["STOR", "APPE"])
                    finally:
                        server.release.set()

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


    def test_unknown_remote_sizes_are_overwritten_from_the_beginning(self):
        source = b"hello\n"
        for streamed in (False, True):
            for remote in (source, source[:2], b"larger remote contents"):
                with self.subTest(streamed=streamed, remote=remote):
                    archive = self.archive([("hello.txt", source)], streamed=streamed)
                    with FtpServer(files={"/upload/hello.txt": remote}, size_unsupported=True) as server:
                        with self.job(archive, server) as job:
                            job.wait(timeout=5)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            self.assertEqual(job.result["files_uploaded"], 1)
                            self.assertEqual(job.result["skipped_files"], 0)
                            self.assertEqual(job.result["bytes_uploaded"], len(source))
                        self.assertEqual(server.files["/upload/hello.txt"], source)
                        self.assertEqual(server.uploads, [("STOR", "/upload/hello.txt", source)])
                        self.assertEqual(server.deleted, [])

    def test_ftpsrv_self_conversion_is_turned_off_before_sizes_on_every_connection(self):
        # A SELF whose headers declare a smaller ELF: ftpsrv's SIZE would report it for any stored length.
        payload = bytes(range(251)) * 16
        source = SELF_MAGIC + (len(payload) + 12 - 64).to_bytes(8, "little") + payload
        cases = {"complete": ({}, source), "partial": ({}, source[:1000]),
                 "reconnect": ({"fail_uploads": 1, "drop_after": 500}, source[:1000]),
                 "without SELF": ({"ftpsrv_self": False}, source[:1000])}
        for streamed in (False, True):
            for case, (options, remote) in cases.items():
                with self.subTest(streamed=streamed, case=case):
                    archive = self.archive([("eboot.bin", source)], streamed=streamed)
                    with FtpServer(files={"/upload/eboot.bin": remote}, ftpsrv=True, timestamps="none",
                                   **options) as server, self.job(archive, server) as job:
                        job.wait(timeout=10)
                        self.assertEqual(job.result["status"], "success", job.describe())
                        self.assertEqual(job.result["skipped_files"], 1 if case == "complete" else 0)
                        self.assertEqual(job.result["bytes_uploaded"], len(source) - len(remote))
                        self.assertEqual(server.files["/upload/eboot.bin"], source)
                        commands = [command for command, _ in server.commands]
                        self.assertLess(commands.index("SELF"), commands.index("SIZE"))
                        # One per connection; a server without the command is not asked again.
                        self.assertEqual(commands.count("SELF"), 2 if case == "reconnect" else 1)
                        expected = [] if case == "complete" else [("APPE", "/upload/eboot.bin", source[1000:])]
                        if case == "reconnect":
                            expected = [("APPE", "/upload/eboot.bin", source[1000:1500]),
                                        ("APPE", "/upload/eboot.bin", source[1500:])]
                        self.assertEqual(server.uploads, expected)
        with FtpServer(files={"/upload/eboot.bin": source[:1000]}) as server:
            with self.job(self.archive([("eboot.bin", source)]), server) as job:
                job.wait(timeout=5)
                self.assertEqual(job.result["status"], "success", job.describe())
            self.assertEqual(server.files["/upload/eboot.bin"], source)
            self.assertNotIn("SELF", [command for command, _ in server.commands])

    def test_refused_listings_fall_back_to_asking_for_each_file(self):
        source = b"hello\n"
        for streamed in (False, True):
            with self.subTest(streamed=streamed):
                archive = self.archive([("same.txt", source), ("new.txt", source), ("short.txt", source)],
                                       streamed=streamed)
                files = {"/upload/same.txt": source, "/upload/short.txt": source[:2]}
                with FtpServer(files=files, refuse_listing=True) as server, self.job(archive, server) as job:
                    job.wait(timeout=5)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["skipped_files"], 1)
                    self.assertEqual(job.result["files_uploaded"], 2)
                    self.assertEqual(job.result["bytes_uploaded"], len(source) * 2 - 2)
                    commands = server.commands
                    self.assertIn("NLST", [command for command, _ in commands])
                    sized = [arg for command, arg in commands if command == "SIZE"]
                    self.assertTrue(any(arg.endswith("new.txt") for arg in sized), sized)  # Not trusted absent.
                    self.assertEqual(server.uploads, [("STOR", "/upload/new.txt", source),
                                                      ("APPE", "/upload/short.txt", source[2:])])

    def test_existing_files_avoid_repeated_rest_queries_after_binary_size_is_known(self):
        entries = [(f"{index:03}.txt", b"hello\n" * index) for index in range(80)]
        files = {"/upload/" + name: data for name, data in entries}
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend), FtpServer(
                        files=files, ascii_size_bias=1000) as server:
                    archive = self.archive(entries, streamed=streamed)
                    if frontend == "library":
                        with self.job(archive, server) as job:
                            job.wait(timeout=10)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            self.assertEqual(job.result["skipped_files"], len(entries))
                            self.assertEqual(job.result["files_uploaded"], 0)
                    else:
                        result = self.cli(archive, server)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    commands = [command for command, _ in server.commands]
                    self.assertEqual(commands.count("SIZE"), len(entries))
                    self.assertEqual(commands.count("REST"), 1)
                    self.assertEqual(server.uploads, [])
                    self.assertEqual(server.deleted, [])
                    self.assertEqual(dict(server.files), files)

    def test_size_queries_reestablish_binary_mode_after_reconnection(self):
        entries = [("first.txt", b"first\n"), ("next.txt", b"next\n"), ("empty.txt", b"")]
        files = {"/upload/" + name: data for name, data in entries}
        for streamed in (False, True):
            with self.subTest(streamed=streamed), FtpServer(
                    files=files, ascii_size_bias=1000, close_after_rest=True) as server:
                with self.job(self.archive(entries, streamed=streamed), server) as job:
                    job.wait(timeout=10)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["skipped_files"], len(entries))
                    self.assertEqual(job.result["files_uploaded"], 0)
                self.assertEqual(server.login_attempts, 2)
                self.assertEqual(server.uploads, [])
                self.assertEqual(server.deleted, [])
                self.assertEqual(dict(server.files), files)
                self.assertEqual(sum(command == "TYPE" and arg == "I" for command, arg in server.commands), 2)

    def test_size_reply_with_legacy_prefix_keeps_native_query_compatibility(self):
        entries = [("first.txt", b"first\n"), ("next.txt", b"next\n"), ("empty.txt", b"")]
        files = {"/upload/" + name: data for name, data in entries}
        with FtpServer(files=files, size_reply_prefix="size: ") as server:
            with self.job(self.archive(entries), server) as job:
                job.wait(timeout=10)
                self.assertEqual(job.result["status"], "success", job.describe())
                self.assertEqual(job.result["skipped_files"], len(entries))
                self.assertEqual(job.result["files_uploaded"], 0)
            self.assertEqual(server.uploads, [])
            self.assertEqual(server.deleted, [])
            self.assertEqual(dict(server.files), files)

    def test_cancellation_interrupts_a_size_only_query_during_retry(self):
        source = bytes(range(251)) * 12000
        entries = [("same.txt", b"same"), ("blocks.bin", source), ("after.txt", b"after")]
        for streamed in (False, True):
            with self.subTest(streamed=streamed), FtpServer(files={"/upload/same.txt": b"same",
                    "/upload/blocks.bin": source[:19]}, fail_uploads=1, stall_retry_size=True) as server:
                with self.job(self.archive(entries, streamed=streamed), server) as job:
                    self.assert_cancelled(job, server)
                    self.assertEqual(server.files["/upload/same.txt"], b"same")
                    self.assertNotIn("/upload/blocks.bin", server.files)
                    self.assertNotIn("/upload/after.txt", server.files)
                    self.assertEqual(server.deleted, ["/upload/blocks.bin"])
                    self.assertEqual(sum(command == "REST" for command, _ in server.commands), 1)

    def test_dotfiles_left_out_of_listings_are_still_checked(self):
        source = b"hello\n"
        for streamed in (False, True):
            with self.subTest(streamed=streamed):
                archive = self.archive([(".hidden", source), ("visible.txt", source)], streamed=streamed)
                files = {"/upload/.hidden": source, "/upload/visible.txt": source}
                with FtpServer(files=files, nlst_hides_dotfiles=True) as server, self.job(archive, server) as job:
                    job.wait(timeout=5)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["skipped_files"], 2)
                    self.assertEqual(job.result["files_uploaded"], 0)
                    self.assertEqual(server.uploads, [])

    def test_file_times_use_mfmt_then_vsftpd_mdtm_or_warn_once(self):
        archive = self.archive([("a.txt", b"a\n"), ("b.txt", b"b\n"), ("c.txt", b"c\n")])  # ZIP times.
        expected = {"mfmt": (3, 0), "mdtm": (1, 3), "none": (1, 1)}  # MFMT and "MDTM <time> <path>" sent.
        for timestamps, (mfmt, mdtm) in expected.items():
            with self.subTest(timestamps=timestamps):
                with FtpServer(timestamps=timestamps) as server, self.job(archive, server) as job:
                    job.wait(timeout=5)
                    self.assertEqual(job.result["status"], "success", job.describe())
                    self.assertEqual(job.result["files_uploaded"], 3)
                    self.assertEqual(len(server.files), 3)
                    commands = server.commands
                    self.assertEqual(sum(command == "MFMT" for command, _ in commands), mfmt)
                    self.assertEqual(sum(command == "MDTM" and re.match(r"\d{14} ", arg) is not None
                                         for command, arg in commands), mdtm)
                    log = lib_log_text(job)
                    warnings = log.count("the server cannot set modification times")
                    self.assertEqual(warnings, 1 if timestamps == "none" else 0, log)
                    self.assertNotIn("could not set the modification time", log)

    def test_block_mode_is_never_requested_by_default(self):
        # Without --mode-b / mode_b, MODE is never sent, even to a server that supports block mode.
        entries = [("a.txt", b"a"), ("nested/b.txt", b"b" * 70000), ("empty.txt", b"")]
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend), FtpServer(mode_b=True) as server:
                    archive = self.archive(entries, streamed=streamed)
                    if frontend == "library":
                        with self.job(archive, server) as job:
                            job.wait(timeout=15)
                            self.assertEqual(job.result["status"], "success", job.describe())
                            output = lib_log_text(job)
                    else:
                        result = self.cli(archive, server)
                        output = result.stdout + result.stderr
                        self.assertEqual(result.returncode, 0, output)
                    self.assertNotIn("MODE", [command for command, _ in server.commands])
                    self.assertNotIn("MODE B", output)
                    self.assertNotIn("Transfer mode", output)
                    starts = server.transfer_starts
                    self.assertTrue(starts and all(code == 150 for _, code in starts), starts)
                    self.assertEqual(server.data_connections, len(starts))
                    for name, contents in entries:
                        self.assertEqual(server.files["/upload/" + name], contents)


class FtpMcTests(FtpTestCase):
    """The same bounded archive stream over FTPMC, through the CLI and the GUI's C API."""

    def transfer(self, frontend, archive, server, *, mode_b=False, stripe=4096, ftps=None):
        if frontend == "library":
            options = {"ftp_mc_connections": 4, "ftp_mc_stripe": stripe, "mode_b": mode_b}
            if ftps:
                options.update(protocol="ftps_" + ftps, ca_certificate=str(CA_CERTIFICATE))
            with self.job(archive, server, **options) as job:
                job.wait(timeout=30)
            self.assertEqual(job.result["status"], "success", job.describe())
            return lib_log_text(job)
        arguments = ["--ftp-mc", "4", "--ftp-mc-stripe", str(stripe)]
        if mode_b:
            arguments += ["--mode-b"]
        if ftps:
            arguments += ["--protocol", "ftps", "--ftps-mode", ftps, "--cacert", str(CA_CERTIFICATE)]
        result = self.cli(archive, server, *arguments)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_disabled_by_default(self):
        archive = self.archive([("hello.txt", b"hello")])
        for frontend in ("library", "cli"):
            with self.subTest(frontend=frontend), FtpServer(mc_max=3) as server:
                if frontend == "library":
                    with self.job(archive, server) as job:
                        job.wait(timeout=30)
                    self.assertEqual(job.result["status"], "success", job.describe())
                else:
                    result = self.cli(archive, server)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(any(cmd in ("FEAT", "MPSV") or arg.startswith("MC ")
                                     for cmd, arg in server.commands), server.commands)

    def test_stream_and_block_uploads_with_server_limits(self):
        entries = [(f"file-{size}.bin", (bytes(range(251)) * ((size + 250) // 251))[:size])
                   for size in (0, 1, 999, 1000, 1001, 3000, 3001, 65536, 301201)]
        for streamed in (False, True):
            archive = self.archive(entries, streamed=streamed)
            for mode_b in (False, True):
                for frontend in ("library", "cli"):
                    with self.subTest(streamed=streamed, mode_b=mode_b, frontend=frontend), FtpServer(
                            mc_max=3, mc_max_stripe=1000, mode_b=True) as server:
                        log = self.transfer(frontend, archive, server, mode_b=mode_b, stripe=65536)
                        self.assertEqual(server.files, {"/upload/" + name: data for name, data in entries})
                        self.assertIn("FTPMC: 3 parallel data connection(s) per file", log)
                        self.assertEqual([arg for cmd, arg in server.commands if cmd == "OPTS" and arg.startswith("MC")],
                                         ["MC 3 65536"])
                        commands = [cmd for cmd, _ in server.commands]
                        self.assertEqual(commands.count("FEAT"), 1)
                        self.assertEqual(server.data_connections, commands.count("MPSV") * 3)
                        if mode_b:
                            self.assertEqual(server.data_connections, 3)
                        else:
                            self.assertGreater(commands.count("MPSV"), 1)

    def test_remote_size_policy_and_rerun(self):
        source = bytes(range(251)) * 1201
        entries = [("resume.bin", source), ("larger.bin", b"new"), ("same.bin", b"same"), ("empty.bin", b"")]
        initial = {"/upload/resume.bin": source[:19], "/upload/larger.bin": b"oversized",
                   "/upload/same.bin": b"same", "/upload/empty.bin": b""}
        for streamed in (False, True):
            archive = self.archive(entries, streamed=streamed)
            for mode_b in (False, True):
                for frontend in ("library", "cli"):
                    with self.subTest(streamed=streamed, mode_b=mode_b, frontend=frontend), FtpServer(
                            mc_max=3, mode_b=True, files=initial) as server:
                        self.transfer(frontend, archive, server, mode_b=mode_b)
                        self.assertEqual(server.files, {"/upload/" + name: data for name, data in entries})
                        self.assertIn(("APPE", "/upload/resume.bin", source[19:]), server.uploads)
                        self.assertIn("/upload/larger.bin", server.deleted)
                        self.assertEqual(len(server.uploads), 2)
                        self.transfer(frontend, archive, server, mode_b=mode_b)
                        self.assertEqual(len(server.uploads), 2)

    def test_refusal_falls_back_to_one_connection(self):
        archive = self.archive([("a.txt", b"a"), ("b.bin", bytes(range(251)) * 800)])
        for options in ({"mc_max": 0}, {"mc_max": 3, "mc_opts_refusal": "502 No MC"},
                        {"mc_max": 3, "mc_mpsv_refusal": "502 No MPSV"}):
            for mode_b in (False, True):
                for frontend in ("library", "cli"):
                    with self.subTest(options=options, mode_b=mode_b, frontend=frontend), FtpServer(
                            mode_b=True, **options) as server:
                        log = self.transfer(frontend, archive, server, mode_b=mode_b)
                        self.assertEqual(server.files["/upload/b.bin"], bytes(range(251)) * 800)
                        self.assertEqual(log.count("the server did not accept FTPMC; using one data connection"), 1)
                        self.assertEqual([cmd for cmd, _ in server.commands].count("FEAT"), 1)
                        if mode_b and not options.get("mc_mpsv_refusal"):
                            self.assertEqual(server.data_connections, 1)

    def test_retry_reconnects_and_resumes(self):
        source = bytes(range(251)) * 1500
        archive = self.archive([("retry.bin", source)])
        for mode_b in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(mode_b=mode_b, frontend=frontend), FtpServer(
                        mc_max=3, mode_b=True, fail_uploads=1, drop_after=19) as server:
                    self.transfer(frontend, archive, server, mode_b=mode_b)
                    self.assertEqual(server.files["/upload/retry.bin"], source)
                    self.assertIn(("APPE", "/upload/retry.bin", source[19:]), server.uploads)
                    self.assertEqual([cmd for cmd, _ in server.commands].count("FEAT"), 2)

    def test_exhausted_attempts_delete_the_partial_file(self):
        archive = self.archive([("failed.bin", bytes(range(251)) * 1200)])
        for mode_b in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(mode_b=mode_b, frontend=frontend), FtpServer(
                        mc_max=3, mode_b=True, fail_uploads=1, drop_after=19) as server:
                    if frontend == "library":
                        with self.job(archive, server, retries=1, ftp_mc_connections=4, mode_b=mode_b) as job:
                            job.wait(timeout=30)
                        self.assertEqual(job.result["status"], "failed", job.describe())
                    else:
                        arguments = ["--ftp-mc", "4", "--retries", "1"] + (["--mode-b"] if mode_b else [])
                        result = self.cli(archive, server, *arguments)
                        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn("/upload/failed.bin", server.deleted)
                    self.assertNotIn("/upload/failed.bin", server.files)

    def test_cancellation_deletes_the_partial_file(self):
        archive = self.archive([("cancel.bin", bytes(range(251)) * 20000)])
        for mode_b in (False, True):
            with self.subTest(mode_b=mode_b), FtpServer(
                    mc_max=3, mode_b=True, stall_upload_after=200000) as server:
                with self.job(archive, server, ftp_mc_connections=4, mode_b=mode_b) as job:
                    self.assert_cancelled(job, server)
                self.assertIn("/upload/cancel.bin", server.deleted)
                self.assertNotIn("/upload/cancel.bin", server.files)

    def test_ftps_encrypts_every_data_connection(self):
        archive = self.archive([("tls.bin", bytes(range(251)) * 1000), ("small.bin", b"small")])
        for ftps in ("explicit", "implicit"):
            for mode_b in (False, True):
                for frontend in ("library", "cli"):
                    with self.subTest(ftps=ftps, mode_b=mode_b, frontend=frontend), FtpServer(
                            mc_max=3, mode_b=True, tls=True, implicit_tls=ftps == "implicit") as server:
                        self.transfer(frontend, archive, server, mode_b=mode_b, ftps=ftps)
                        self.assertEqual(server.files["/upload/tls.bin"], bytes(range(251)) * 1000)
                        self.assertGreaterEqual(server.data_connections, 3)
                        self.assertEqual(server.tls_data_connections, server.data_connections)


class FtpModeBTests(FtpTestCase):
    """Block mode (--mode-b, C API mode_b): MODE B once per control connection, then one data connection kept
    open for every transfer; stream mode with a warning if the server refuses."""

    REFUSED = "the server refused block mode (MODE B); using stream mode (MODE S)"
    IN_USE = "Transfer mode: block (MODE B)"

    def transfer(self, frontend, archive, server, *, mode_b=True, ftps=None, active=False, retries=None):
        """Runs one front-end; returns (succeeded, log text, job or None). `ftps`: "explicit" or "implicit"."""
        if frontend == "library":
            options = {"mode_b": True} if mode_b else {}
            if ftps:
                options.update(protocol="ftps_" + ftps, ca_certificate=str(CA_CERTIFICATE))
            if active:
                options["active_mode"] = 1
            with self.job(archive, server, retries=retries, **options) as job:
                job.wait(timeout=30)
            return job.result["status"] == "success", lib_log_text(job) + "\n" + job.result["error"], job
        arguments = ["--mode-b"] if mode_b else []
        if ftps:
            arguments += ["--protocol", "ftps", "--ftps-mode", ftps, "--cacert", str(CA_CERTIFICATE)]
        if active:
            arguments += ["--mode", "active"]
        if retries:
            arguments += ["--retries", str(retries)]
        result = self.cli(archive, server, *arguments)
        return result.returncode == 0, result.stdout + result.stderr, None

    def assert_one_data_connection(self, server, connections=1):
        commands = [command for command, _ in server.commands]
        starts = server.transfer_starts
        self.assertEqual(server.data_connections, connections, starts)
        self.assertEqual(commands.count("EPSV") + commands.count("PASV") + commands.count("EPRT"), connections,
                         commands)
        self.assertEqual([code for _, code in starts].count(150), connections, starts)
        self.assertTrue(all(code in (125, 150) for _, code in starts), starts)

    def test_one_data_connection_carries_every_file_and_listing(self):
        large = bytes(range(251)) * 1200  # Several 64 KiB frames.
        entries = [("same.txt", b"same"), ("a.txt", b"a"), ("nested/", None), ("nested/b.txt", b"b"),
                   ("nested/deeper/c.bin", large), ("frame.bin", b"f" * 65535), ("frame+1.bin", b"g" * 65536),
                   ("empty.txt", b""), ("日本語.txt", b"unicode")]
        entries += [(f"many/{i:02}.txt", f"file {i}\n".encode()) for i in range(20)]
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend), FtpServer(
                        mode_b=True, files={"/upload/same.txt": b"same"}) as server:
                    ok, output, job = self.transfer(frontend, self.archive(entries, streamed=streamed), server)
                    self.assertTrue(ok, output)
                    for name, contents in entries:
                        if contents is not None:
                            self.assertEqual(server.files["/upload/" + name], contents, name)
                    self.assertEqual(len(server.uploads), sum(data is not None for _, data in entries) - 1)
                    if job is not None:
                        self.assertEqual(job.result["skipped_files"], 1)
                    commands = [command for command, _ in server.commands]
                    # Once, after login and PWD, before any data connection.
                    self.assertEqual(server.commands.count(("MODE", "B")), 1)
                    self.assertLess(commands.index("PWD"), commands.index("MODE"))
                    self.assertLess(commands.index("MODE"), commands.index("EPSV"))
                    self.assert_one_data_connection(server)
                    self.assertIn("NLST", [command for command, _ in server.transfer_starts])
                    self.assertEqual(output.count(self.IN_USE), 1, output)
                    self.assertLess(output.index("Logged in as"), output.index(self.IN_USE))
                    self.assertNotIn(self.REFUSED, output)

    def test_refused_block_mode_warns_and_continues_in_stream_mode(self):
        entries = [("a.txt", b"a"), ("nested/b.bin", bytes(range(251)) * 400), ("empty.txt", b"")]
        refusals = (None, "500 Unknown command", "502 Command not implemented", "504 Unsupported transfer mode.",
                    "202 Command not implemented, superfluous at this site.", "450 Block mode unavailable")
        for refusal in refusals:
            for frontend in ("library", "cli"):
                with self.subTest(refusal=refusal, frontend=frontend), FtpServer(mode_b=refusal) as server:
                    ok, output, job = self.transfer(frontend, self.archive(entries), server)
                    self.assertTrue(ok, output)
                    for name, contents in entries:
                        self.assertEqual(server.files["/upload/" + name], contents)
                    # Asked once; the control connection stays in stream mode.
                    self.assertEqual(server.commands.count(("MODE", "B")), 1)
                    starts = server.transfer_starts
                    self.assertTrue(all(code == 150 for _, code in starts), starts)
                    self.assertEqual(server.data_connections, len(starts))
                    self.assertEqual(output.count(self.REFUSED), 1, output)
                    self.assertNotIn(self.IN_USE, output)
                    if job is not None:
                        self.assertIn(self.REFUSED, [line["text"] for line in job.result["problems"]])

    def test_resume_appends_only_the_remaining_bytes(self):
        source = bytes(range(251)) * 12000
        for streamed in (False, True):
            archive = self.archive([("hello.txt", b"hello\n"), ("blocks.bin", source)], streamed=streamed)
            for offset in ((1 << 20) - 3, (1 << 20) + 19, len(source) - 1):
                for frontend in ("library", "cli"):
                    with self.subTest(streamed=streamed, offset=offset, frontend=frontend), FtpServer(
                            mode_b=True, files={"/upload/hello.txt": b"he",
                                                "/upload/blocks.bin": source[:offset]}) as server:
                        ok, output, job = self.transfer(frontend, archive, server)
                        self.assertTrue(ok, output)
                        self.assertEqual(server.files["/upload/hello.txt"], b"hello\n")
                        self.assertEqual(server.files["/upload/blocks.bin"], source)
                        self.assertEqual(server.uploads, [("APPE", "/upload/hello.txt", b"llo\n"),
                                                          ("APPE", "/upload/blocks.bin", source[offset:])])
                        self.assertEqual(server.deleted, [])
                        if job is not None:
                            self.assertEqual(job.result["bytes_uploaded"], 4 + len(source) - offset)
                            self.assertEqual(job.state["progress"]["sent_bytes"], 4 + len(source) - offset)
                        self.assert_one_data_connection(server)

    def test_retry_after_an_interrupted_upload_negotiates_block_mode_again(self):
        source = bytes(range(251)) * 12000
        entries = [("before.txt", b"before"), ("nested/", None), ("nested/blocks.bin", source),
                   ("after.txt", b"after")]
        for streamed in (False, True):
            for frontend in ("library", "cli"):
                with self.subTest(streamed=streamed, frontend=frontend), FtpServer(
                        mode_b=True, files={"/upload/nested/blocks.bin": source[:19]}, fail_uploads=2,
                        fail_target="/upload/nested/blocks.bin") as server:
                    ok, output, job = self.transfer(frontend, self.archive(entries, streamed=streamed), server)
                    self.assertTrue(ok, output)
                    self.assertIn("attempt 2/3", output)
                    self.assertIn("attempt 3/3", output)
                    self.assertEqual(server.files["/upload/nested/blocks.bin"], source)
                    self.assertEqual(server.files["/upload/after.txt"], b"after")
                    self.assertEqual(server.deleted, [])
                    # The server counts payload bytes: block headers never reach the file.
                    self.assertEqual([(command, data) for command, name, data in server.uploads
                                      if name == "/upload/nested/blocks.bin"],
                                     [("APPE", source[19:19 + 65539]),
                                      ("APPE", source[19 + 65539:19 + 2 * 65539]),
                                      ("APPE", source[19 + 2 * 65539:])])
                    # Each failure drops the control connection: three connections, each negotiating again.
                    self.assertEqual(server.commands.count(("MODE", "B")), 3)
                    self.assertEqual(output.count(self.IN_USE), 3, output)
                    self.assert_one_data_connection(server, connections=3)

    def test_exhausted_attempts_and_cancellations_remove_partial_files(self):
        source = bytes(range(251)) * 12000
        archive = self.archive([("blocks.bin", source), ("after.txt", b"after")])
        for frontend in ("library", "cli"):
            with self.subTest(frontend=frontend), FtpServer(mode_b=True, fail_uploads=3) as server:
                ok, output, _ = self.transfer(frontend, archive, server)
                self.assertFalse(ok, output)
                self.assertIn("after 3 attempt(s)", output)
                self.assertNotIn("/upload/blocks.bin", server.files)
                self.assertNotIn("/upload/after.txt", server.files)
                self.assertEqual(server.deleted, ["/upload/blocks.bin"])
        # A cancelled upload is closed without an EOF frame: the server sees it truncated, not complete. The
        # file is larger than what socket and Docker port-forwarding buffers can hold while the server waits.
        archive = self.archive([("blocks.bin", bytes(range(251)) * 70000), ("after.txt", b"after")])
        with FtpServer(mode_b=True, stall_upload_after=200000) as server, self.job(archive, server,
                                                                                   mode_b=True) as job:
            self.assert_cancelled(job, server)
            self.assertEqual(server.deleted, ["/upload/blocks.bin"])
            deadline = time.monotonic() + 10
            while server.truncated_uploads == 0 and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertEqual(server.truncated_uploads, 1)
            self.assertNotIn("/upload/blocks.bin", server.files)
        archive = self.archive([("empty.txt", b"")])
        with FtpServer(mode_b=True, stall_confirmation=True) as server, self.job(archive, server,
                                                                                 mode_b=True) as job:
            self.assert_cancelled(job, server)
            self.assertNotIn("/upload/empty.txt", server.files)
            self.assertEqual(server.deleted, ["/upload/empty.txt"])

    def test_servers_that_close_or_forget_kept_connections(self):
        entries = [(f"{i}.txt", f"file {i}".encode()) for i in range(5)]
        archive = self.archive(entries)
        for options in ({"mode_b_keep": False}, {"mode_b_close_idle": True}, {"mode_b_refuse_reuse": 1}):
            for frontend in ("library", "cli"):
                with self.subTest(options=options, frontend=frontend), FtpServer(mode_b=True, **options) as server:
                    ok, output, _ = self.transfer(frontend, archive, server)
                    self.assertTrue(ok, output)
                    for name, contents in entries:
                        self.assertEqual(server.files["/upload/" + name], contents)
                    self.assertEqual(len(server.uploads), len(entries))  # A 425 opens no file.
                    starts = [code for _, code in server.transfer_starts]
                    if "mode_b_refuse_reuse" in options:
                        # Retried once with a new data connection; reused again afterwards.
                        self.assertEqual(starts.count(425), 1, starts)
                        self.assertEqual(starts[starts.index(425) + 1], 150, starts)
                        self.assertIn(125, starts[starts.index(425) + 1:], starts)
                    else:
                        # 226, or a kept connection that is gone: never reused.
                        self.assertNotIn(125, starts)
                        self.assertEqual(server.data_connections, starts.count(150))

    def test_listing_frames_receivers_must_accept_or_reject(self):
        source = b"hello\n"
        archive = self.archive([("same.txt", source), ("new.txt", source), ("short.txt", source)])
        files = {"/upload/same.txt": source, "/upload/short.txt": source[:2]}
        for options in ({"mode_b_eof_on_data": True}, {"mode_b_extra_blocks": True},
                        {"mode_b_bad_descriptor": True}):
            with self.subTest(options=options), FtpServer(mode_b=True, files=files, **options) as server:
                ok, output, job = self.transfer("library", archive, server)
                self.assertTrue(ok, output)
                self.assertEqual(job.result["skipped_files"], 1)
                self.assertEqual(server.uploads, [("STOR", "/upload/new.txt", source),
                                                  ("APPE", "/upload/short.txt", source[2:])])
                sized = [arg for command, arg in server.commands if command == "SIZE"]
                if "mode_b_bad_descriptor" in options:
                    # The listing failed: every file is checked with SIZE instead.
                    self.assertTrue(any(arg.endswith("new.txt") for arg in sized), sized)
                else:
                    self.assertFalse(any(arg.endswith("new.txt") for arg in sized), sized)

    def test_ftpsrv_emulation_with_block_mode(self):
        payload = bytes(range(251)) * 16
        source = SELF_MAGIC + (len(payload) + 12 - 64).to_bytes(8, "little") + payload
        others = [("param.sfo", b"sfo"), ("icon0.png", b"icon"), ("pic1.png", b"picture")]
        cases = {"complete": ({}, source), "partial": ({}, source[:1000]),
                 "reconnect": ({"fail_uploads": 1, "drop_after": 500, "fail_target": "/upload/eboot.bin"},
                               source[:1000]),
                 "without SELF": ({"ftpsrv_self": False}, source[:1000])}
        for streamed in (False, True):
            for case, (options, remote) in cases.items():
                with self.subTest(streamed=streamed, case=case):
                    archive = self.archive([("eboot.bin", source)] + others, streamed=streamed)
                    with FtpServer(files={"/upload/eboot.bin": remote}, ftpsrv=True, timestamps="none",
                                   mode_b=True, **options) as server:
                        ok, output, job = self.transfer("library", archive, server)
                        self.assertTrue(ok, output)
                        self.assertEqual(job.result["skipped_files"], 1 if case == "complete" else 0)
                        self.assertEqual(server.files["/upload/eboot.bin"], source)
                        for name, contents in others:
                            self.assertEqual(server.files["/upload/" + name], contents)
                        commands = [command for command, _ in server.commands]
                        self.assertLess(commands.index("SELF"), commands.index("SIZE"))
                        # curl closes the control connection after a failed quote command (SELF without
                        # support, the timestamp commands ftpsrv refuses): each new one negotiates again.
                        self.assertEqual(server.commands.count(("MODE", "B")), commands.count("USER"))
                        self.assertEqual(output.count(self.IN_USE), commands.count("USER"), output)
                        # The files on the last control connection share one data connection, set up with PASV
                        # (no EPSV); ftpsrv refuses NLST after its PASV.
                        last = commands[len(commands) - 1 - commands[::-1].index("USER"):]
                        self.assertEqual(last.count("PASV"), 1 + last.count("NLST"), commands)
                        self.assertEqual(server.transfer_starts[-1], ("STOR", 125))

    def test_ftps_keeps_one_tls_data_connection(self):
        source = bytes(range(251)) * 1200
        entries = [("a.txt", b"a"), ("nested/b.bin", source), ("empty.txt", b""), ("c.txt", b"c")]
        for ftps in ("explicit", "implicit"):
            for refusal in (None, "504 Unsupported transfer mode."):
                for frontend in ("library", "cli"):
                    with self.subTest(ftps=ftps, refusal=refusal, frontend=frontend), FtpServer(
                            tls=True, implicit_tls=ftps == "implicit", mode_b=refusal or True,
                            files={"/upload/nested/b.bin": source[:19]}) as server:
                        ok, output, _ = self.transfer(frontend, self.archive(entries), server, ftps=ftps)
                        self.assertTrue(ok, output)
                        for name, contents in entries:
                            self.assertEqual(server.files["/upload/" + name], contents)
                        self.assertIn(("PROT", "P"), server.commands)
                        self.assertIn(("APPE", "/upload/nested/b.bin", source[19:]), server.uploads)
                        if refusal is None:
                            self.assert_one_data_connection(server)
                            self.assertEqual(server.tls_data_connections, 1)
                        else:
                            self.assertEqual(server.tls_data_connections, len(server.transfer_starts))
                            self.assertIn(self.REFUSED, output)

    def test_active_mode_keeps_one_data_connection(self):
        entries = [(f"{i}.txt", f"file {i}".encode()) for i in range(5)]
        for frontend in ("library", "cli"):
            with self.subTest(frontend=frontend), FtpServer(mode_b=True) as server:
                if not server.routable:
                    self.skipTest("Docker active data connections require a routable Linux container")
                ok, output, _ = self.transfer(frontend, self.archive(entries), server, active=True)
                self.assertTrue(ok, output)
                for name, contents in entries:
                    self.assertEqual(server.files["/upload/" + name], contents)
                self.assert_one_data_connection(server)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sext", required=True)
    parser.add_argument("--lib", required=True)
    ARGS, remaining = parser.parse_known_args()
    ARGS.sext = str(Path(ARGS.sext).resolve())
    ARGS.lib = str(Path(ARGS.lib).resolve())
    unittest.main(argv=[__file__, *remaining])
