"""Deterministic FTP/FTPS failure scenarios, served only inside the test container."""

import contextlib
import posixpath
import socket
import socketserver
import ssl
import threading

class FtpHandler(socketserver.StreamRequestHandler):
    def handle(self):
        cwd = "/"
        passive = None
        active = None
        protected = False
        rename_source = None

        def start_tls():
            self.rfile.close()
            self.wfile.close()
            self.connection = self.server.tls_context.wrap_socket(self.connection, server_side=True)
            self.rfile = self.connection.makefile("rb")
            self.wfile = self.connection.makefile("wb")

        @contextlib.contextmanager
        def accept_data():
            data = passive.accept()[0] if passive is not None else socket.create_connection(active, timeout=10)
            try:
                if protected:
                    data = self.server.tls_context.wrap_socket(data, server_side=True)
                    self.server.tls_data_connections += 1
                yield data
                if protected:
                    # A clean TLS EOF, as real FTPS servers send after the data.
                    data = data.unwrap()
            finally:
                data.close()

        def reply(text):
            self.wfile.write((text + "\r\n").encode("utf-8"))
            self.wfile.flush()

        def path(arg):
            return posixpath.normpath(arg if arg.startswith("/") else posixpath.join(cwd, arg))

        try:
            if self.server.implicit_tls:
                start_tls()
            reply("220 Local test server")
            while True:
                line = self.rfile.readline()
                if not line:
                    break
                command, _, arg = line.decode("utf-8").rstrip("\r\n").partition(" ")
                self.server.commands.append((command, arg))
                if command == "AUTH" and self.server.tls_context:
                    reply("234 Start TLS")
                    start_tls()
                elif command == "PBSZ":
                    reply("200 OK")
                elif command == "PROT":
                    protected = arg == "P" and not self.server.reject_private_data
                    reply("200 OK" if protected else "534 Private data required")
                elif command == "USER":
                    if self.server.tls_context and not isinstance(self.connection, ssl.SSLSocket):
                        reply("530 TLS required")
                        return
                    reply("331 Password required")
                elif command == "PASS":
                    self.server.login_attempts += 1
                    if self.server.stall_login_retry and self.server.login_attempts > 1:
                        self.server.pause()
                    if self.server.fail_logins > 0:
                        self.server.fail_logins -= 1
                        reply("530 Login temporarily unavailable")
                        return
                    reply("230 Logged in")
                elif command == "PWD":
                    reply('257 "/"')
                elif command == "SYST":
                    reply("215 UNIX Type: L8")
                elif command in ("OPTS", "TYPE", "REST"):
                    reply("200 OK")
                elif command == "MFMT":
                    reply("200 OK" if self.server.timestamps == "mfmt" else "500 Unsupported command")
                elif command == "CWD":
                    if path(arg) == self.server.stall_directory:
                        self.server.pause()
                    directory = path(arg)
                    if self.server.directories is not None and directory not in self.server.directories:
                        reply("550 Directory not found")
                    else:
                        cwd = directory
                        reply("250 Directory changed")
                elif command == "SIZE":
                    if self.server.stall_retry_size and self.server.failed_uploads:
                        self.server.pause()
                    if self.server.size_unsupported:
                        reply("502 Command not implemented")
                        continue
                    contents = self.server.files.get(path(arg))
                    reply(f"213 {len(contents)}" if contents is not None else "550 File not found")
                elif command == "MDTM":
                    # "MDTM <time> <path>" sets the time (vsftpd); "MDTM <path>" asks for it.
                    setting = arg[:14].isdigit() and arg[14:15] == " "
                    reply("550 Cannot set the time" if setting and self.server.timestamps == "none"
                          else "213 20260101000000")
                elif command == "EPSV":
                    passive = self.server.passive_socket()
                    reply(f"229 Extended Passive Mode (|||{passive.getsockname()[1]}|)")
                elif command == "EPRT":
                    _, _, host, port, _ = arg.split(arg[0])
                    active = (host, int(port))
                    reply("200 Active endpoint accepted")
                elif command == "NLST":
                    if self.server.stall_listing:
                        self.server.pause()
                    if self.server.refuse_listing:
                        if passive is not None:
                            passive.close()
                        passive = None
                        reply("550 Listing refused")
                        continue
                    reply("150 Listing")
                    with accept_data() as data:
                        names = [posixpath.basename(p) for p in self.server.files if posixpath.dirname(p) == cwd]
                        if self.server.directories is not None:
                            names += [posixpath.basename(p) for p in self.server.directories
                                      if p != cwd and posixpath.dirname(p) == cwd]
                        if self.server.nlst_hides_dotfiles and "-a" not in arg.split():
                            names = [name for name in names if not name.startswith(".")]
                        data.sendall("".join(name + "\r\n" for name in names).encode("utf-8"))
                    if passive is not None:
                        passive.close()
                    passive = None
                    reply("226 Listing complete")
                elif command in ("STOR", "APPE"):
                    target = path(arg)
                    fail_upload = (self.server.fail_uploads > 0 and
                                   (self.server.fail_target is None or target == self.server.fail_target))
                    if fail_upload:
                        self.server.fail_uploads -= 1
                    reply("550 Overwrite refused" if self.server.reject_upload else "150 Upload accepted")
                    with accept_data() as data:
                        data.settimeout(10)
                        if self.server.reject_upload:
                            pass
                        else:
                            if command == "STOR":
                                self.server.files[target] = b""
                            else:
                                self.server.files.setdefault(target, b"")
                            payload = bytearray()
                            while True:
                                limit = min(65536, self.server.drop_after - len(payload)) if fail_upload else 65536
                                chunk = data.recv(limit)
                                if not chunk:
                                    break
                                payload.extend(chunk)
                                with self.server.files_lock:
                                    # Cleanup can unlink the upload while queued data is still arriving.
                                    if target in self.server.files:
                                        self.server.files[target] += chunk
                                if fail_upload and len(payload) >= self.server.drop_after:
                                    break
                            self.server.uploads.append((command, target, bytes(payload)))
                            # Complete host-side mutations before closing the data socket.
                            # Otherwise curl can reconnect and SIZE the old prefix while
                            # the Docker control API is still delivering the callback.
                            if fail_upload:
                                self.server.failed_uploads += 1
                                if self.server.after_failure:
                                    self.server.after_failure(self.server, target)
                    if passive is not None:
                        passive.close()
                    passive = None
                    if fail_upload and not self.server.reject_upload:
                        reply("426 Data connection interrupted")
                        return  # Drop the control connection too; retry must reconnect.
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
                elif command == "RNFR":
                    rename_source = path(arg)
                    exists = rename_source in self.server.files or (self.server.directories is not None
                                                                   and rename_source in self.server.directories)
                    reply("350 Ready to rename" if exists else "550 File not found")
                elif command == "RNTO":
                    if self.server.stall_rename:
                        self.server.pause()
                    target = path(arg)
                    directory = self.server.directories is not None and rename_source in self.server.directories
                    fail = (self.server.fail_rename_after is not None
                            and self.server.successful_renames >= self.server.fail_rename_after)
                    if self.server.reject_rename or fail or (not directory and rename_source not in self.server.files):
                        reply("550 Rename refused")
                    else:
                        with self.server.files_lock:
                            if directory:
                                if target in self.server.directories or target in self.server.files:
                                    reply("550 Directory already exists")
                                    continue
                                prefix = rename_source + "/"
                                files = {name: data for name, data in self.server.files.items() if name.startswith(prefix)}
                                for name, data in files.items():
                                    self.server.files[target + name[len(rename_source):]] = data
                                    del self.server.files[name]
                                directories = {name for name in self.server.directories
                                               if name == rename_source or name.startswith(prefix)}
                                self.server.directories.difference_update(directories)
                                self.server.directories.update(target + name[len(rename_source):] for name in directories)
                            else:
                                self.server.files[target] = self.server.files.pop(rename_source)
                            self.server.successful_renames += 1
                        if self.server.successful_renames == self.server.lose_rename_reply_after:
                            return  # The rename succeeded, but the client never receives its acknowledgement.
                        reply("250 File renamed")
                    rename_source = None
                elif command == "MKD":
                    directory = path(arg)
                    if self.server.directories is not None:
                        if posixpath.dirname(directory) not in self.server.directories:
                            reply("550 Parent not found")
                            continue
                        self.server.directories.add(directory)
                    reply("257 Directory created")
                elif command == "RMD":
                    directory = path(arg)
                    if self.server.directories is not None:
                        if (directory not in self.server.directories
                                or any(name.startswith(directory + "/") for name in self.server.files)
                                or any(name.startswith(directory + "/") for name in self.server.directories)):
                            reply("550 Directory missing or not empty")
                            continue
                        self.server.directories.remove(directory)
                    reply("250 Directory removed")
                elif command == "QUIT":
                    reply("221 Goodbye")
                    break
                else:
                    reply("500 Unsupported command")
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, socket.timeout, ssl.SSLError):
            pass  # Expected when the client cancels a stalled command.
        finally:
            if passive is not None:
                passive.close()

    def finish(self):
        try:
            super().finish()
        finally:
            self.connection.close()


class FtpServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, *, files=None, reject_upload=False, reject_delete=False, stall_directory=None,
                 stall_listing=False, stall_confirmation=False, fail_uploads=0, drop_after=65539,
                 fail_target=None, stall_retry_size=False, after_failure=None, fail_logins=0,
                 stall_login_retry=False, tls_context=None, implicit_tls=False, reject_private_data=False,
                 reject_rename=False, stall_rename=False,
                 fail_rename_after=None, lose_rename_reply_after=None, directories=None,
                 size_unsupported=False, refuse_listing=False, nlst_hides_dotfiles=False, timestamps="mfmt",
                 control_port=0, passive_ports=range(30000, 30010)):
        self.passive_ports = passive_ports
        super().__init__(("0.0.0.0", control_port), FtpHandler)
        self.files = dict(files or {})
        self.directories = None if directories is None else set(directories) | {"/"}
        if self.directories is not None:
            for name in list(self.directories) + list(self.files):
                directory = posixpath.dirname(name)
                while directory not in ("", "/"):
                    self.directories.add(directory)
                    directory = posixpath.dirname(directory)
        self.files_lock = threading.Lock()
        self.commands = []
        self.uploads = []
        self.deleted = []
        self.reject_upload = reject_upload
        self.reject_delete = reject_delete
        self.reject_rename = reject_rename
        self.stall_rename = stall_rename
        self.fail_rename_after = fail_rename_after
        self.lose_rename_reply_after = lose_rename_reply_after
        self.successful_renames = 0
        self.stall_directory = stall_directory
        self.stall_listing = stall_listing
        self.size_unsupported = size_unsupported
        self.refuse_listing = refuse_listing
        self.nlst_hides_dotfiles = nlst_hides_dotfiles
        self.timestamps = timestamps  # "mfmt", "mdtm" (vsftpd's "MDTM <time> <path>" only) or "none".
        self.stall_confirmation = stall_confirmation
        self.fail_uploads = fail_uploads
        self.failed_uploads = 0
        self.drop_after = drop_after
        self.fail_target = fail_target
        self.stall_retry_size = stall_retry_size
        self.after_failure = after_failure
        self.fail_logins = fail_logins
        self.login_attempts = 0
        self.stall_login_retry = stall_login_retry
        self.tls_context = tls_context
        self.implicit_tls = implicit_tls
        self.reject_private_data = reject_private_data
        self.tls_data_connections = 0
        self.stalled = threading.Event()
        self.release = threading.Event()
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()

    def passive_socket(self):
        for port in self.passive_ports:
            listener = socket.socket()
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.settimeout(10)
            try:
                listener.bind(("0.0.0.0", port))
                listener.listen()
                return listener
            except OSError:
                listener.close()
        raise RuntimeError("no passive test port available")

    def pause(self):
        self.stalled.set()
        self.release.wait(10)

    def __exit__(self, *args):
        self.release.set()
        self.shutdown()
        self.thread.join()
        super().__exit__(*args)
