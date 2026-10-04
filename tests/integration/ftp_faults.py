#!/usr/bin/env python3
"""Local FTP resume and fault tests; no Docker or external archivers required.

python3 tests/integration/ftp_faults.py --rarftp build/rarftp --lib build/librarftpcore.dylib
"""

import argparse
import io
import posixpath
import socket
import socketserver
import subprocess
import tarfile
import tempfile
import threading
import unittest
import warnings
import zipfile
from pathlib import Path

from run import Library, LibJob, lib_log_text


class FtpHandler(socketserver.StreamRequestHandler):
    def handle(self):
        cwd = "/"
        passive = None

        def reply(text):
            self.wfile.write((text + "\r\n").encode("utf-8"))
            self.wfile.flush()

        def path(arg):
            return posixpath.normpath(arg if arg.startswith("/") else cwd + "/" + arg)

        reply("220 Local test server")
        try:
            for line in self.rfile:
                command, _, arg = line.decode("utf-8").rstrip("\r\n").partition(" ")
                self.server.commands.append((command, arg))
                if command == "USER":
                    reply("331 Password required")
                elif command == "PASS":
                    reply("230 Logged in")
                elif command == "PWD":
                    reply('257 "/"')
                elif command == "SYST":
                    reply("215 UNIX Type: L8")
                elif command in ("OPTS", "TYPE", "REST", "MFMT"):
                    reply("200 OK")
                elif command == "CWD":
                    if path(arg) == self.server.stall_directory:
                        self.server.pause()
                    cwd = path(arg)
                    reply("250 Directory changed")
                elif command == "SIZE":
                    contents = self.server.files.get(path(arg))
                    reply(f"213 {len(contents)}" if contents is not None else "550 File not found")
                elif command == "MDTM":
                    reply("213 20260101000000")
                elif command == "EPSV":
                    passive = socket.socket()
                    passive.settimeout(10)
                    passive.bind(("127.0.0.1", 0))
                    passive.listen()
                    reply(f"229 Extended Passive Mode (|||{passive.getsockname()[1]}|)")
                elif command == "NLST":
                    if self.server.stall_listing:
                        self.server.pause()
                    reply("150 Listing")
                    with passive.accept()[0] as data:
                        names = [posixpath.basename(p) for p in self.server.files if posixpath.dirname(p) == cwd]
                        data.sendall("".join(name + "\r\n" for name in names).encode("utf-8"))
                    passive.close()
                    passive = None
                    reply("226 Listing complete")
                elif command in ("STOR", "APPE"):
                    with passive.accept()[0] as data:
                        data.settimeout(10)
                        if self.server.reject_upload:
                            reply("550 Overwrite refused")
                        else:
                            target = path(arg)
                            if command == "STOR":
                                self.server.files[target] = b""
                            else:
                                self.server.files.setdefault(target, b"")
                            payload = bytearray()
                            reply("150 Upload accepted")
                            while True:
                                chunk = data.recv(65536)
                                if not chunk:
                                    break
                                payload.extend(chunk)
                                with self.server.files_lock:
                                    # Cleanup can unlink the upload while queued data is still arriving.
                                    if target in self.server.files:
                                        self.server.files[target] += chunk
                            self.server.uploads.append((command, target, bytes(payload)))
                    passive.close()
                    passive = None
                    if not self.server.reject_upload:
                        if self.server.stall_confirmation:
                            self.server.pause()
                        reply("226 Upload complete")
                elif command == "DELE":
                    target = path(arg)
                    self.server.deleted.append(target)
                    if self.server.reject_delete:
                        reply("550 Delete refused")
                    else:
                        with self.server.files_lock:
                            self.server.files.pop(target, None)
                        reply("250 File deleted")
                elif command == "QUIT":
                    reply("221 Goodbye")
                    break
                else:
                    reply("500 Unsupported command")
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, socket.timeout):
            pass  # Expected when the client cancels a stalled command.
        finally:
            if passive is not None:
                passive.close()


class FtpServer(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, *, files=None, reject_upload=False, reject_delete=False, stall_directory=None,
                 stall_listing=False, stall_confirmation=False):
        super().__init__(("127.0.0.1", 0), FtpHandler)
        self.files = dict(files or {})
        self.files_lock = threading.Lock()
        self.commands = []
        self.uploads = []
        self.deleted = []
        self.reject_upload = reject_upload
        self.reject_delete = reject_delete
        self.stall_directory = stall_directory
        self.stall_listing = stall_listing
        self.stall_confirmation = stall_confirmation
        self.stalled = threading.Event()
        self.release = threading.Event()
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()

    def pause(self):
        self.stalled.set()
        self.release.wait(10)

    def __exit__(self, *args):
        self.release.set()
        self.shutdown()
        self.thread.join()
        super().__exit__(*args)


class FtpFaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = Library(ARGS.lib)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rarftp-ftp-faults-")
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

    def job(self, archive, server):
        return LibJob(self.lib, {"archive": archive, "host": "127.0.0.1", "port": server.server_address[1],
                                 "directory": "/upload", "buffer_mib": 1})

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
                    result = subprocess.run([ARGS.rarftp, "--file", archive, "--host", "127.0.0.1",
                                             "--port", str(server.server_address[1]), "--directory", "/upload",
                                             "--no-tui"], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(server.files.get("/upload/hello.txt"), original)
                self.assertEqual(server.deleted, [])

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
    parser.add_argument("--rarftp", required=True)
    parser.add_argument("--lib", required=True)
    ARGS, remaining = parser.parse_known_args()
    ARGS.rarftp = str(Path(ARGS.rarftp).resolve())
    ARGS.lib = str(Path(ARGS.lib).resolve())
    unittest.main(argv=[__file__, *remaining])
