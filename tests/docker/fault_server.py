"""Deterministic FTP/FTPS failure scenarios, served only inside the test container."""

import contextlib
import posixpath
import socket
import socketserver
import ssl
import threading
from concurrent.futures import ThreadPoolExecutor

# SELF header magic (PS4, little-endian); see FtpServer's `ftpsrv` option.
SELF_MAGIC = (0x1D3D154F).to_bytes(4, "little")

# RFC 959 block mode (MODE B): 3-byte header (descriptor, big-endian payload count), then the payload.
BLOCK_EOR, BLOCK_EOF, BLOCK_ERRORS, BLOCK_RESTART = 0x80, 0x40, 0x20, 0x10
BLOCK_MAX = 65535


def recv_exact(data, count):
    """Reads `count` bytes, never more (the next block mode transfer may follow); short only at EOF."""
    received = bytearray()
    while len(received) < count:
        chunk = data.recv(count - len(received))
        if not chunk:
            break
        received.extend(chunk)
    return bytes(received)


def block_frames(payload, *, eof_on_data=False, extra_blocks=False, bad_descriptor=False):
    """`payload` as block mode frames. By default every frame has at most 65535 bytes and the end of file is
    a separate zero-length frame. The options exercise what receivers must also accept, or reject."""
    frames = []
    if extra_blocks:
        # A restart marker's payload is not file data, and an empty frame without EOF is legal.
        frames += [bytes([BLOCK_RESTART, 0, 6]) + b"marker", bytes([0, 0, 0])]
    chunks = [payload[i:i + BLOCK_MAX] for i in range(0, len(payload), BLOCK_MAX)]
    for index, chunk in enumerate(chunks):
        descriptor = BLOCK_EOF if eof_on_data and index == len(chunks) - 1 else 0
        if extra_blocks and index == 0:
            descriptor |= BLOCK_EOR | BLOCK_ERRORS  # Accepted and ignored: still file data.
        if bad_descriptor:
            descriptor |= 0x01  # Outside 0xF0: a protocol error.
        frames.append(bytes([descriptor]) + len(chunk).to_bytes(2, "big") + chunk)
    if not (eof_on_data and chunks):
        frames.append(bytes([BLOCK_EOF, 0, 0]))
    return b"".join(frames)


def parallel_data(function, items):
    with ThreadPoolExecutor(max_workers=len(items)) as pool:
        return list(pool.map(function, items))


def join_stripes(parts, stripe):
    """Reconstruct FTPMC's round-robin stream, rejecting bytes after the first short stripe."""
    result, positions, index = bytearray(), [0] * len(parts), 0
    while True:
        channel = index % len(parts)
        chunk = parts[channel][positions[channel]:positions[channel] + stripe]
        positions[channel] += len(chunk)
        result.extend(chunk)
        if len(chunk) < stripe:
            if any(position != len(part) for position, part in zip(positions, parts)):
                raise ValueError("inconsistent FTPMC stripes")
            return bytes(result)
        index += 1


class FtpHandler(socketserver.StreamRequestHandler):
    def handle(self):
        cwd = "/"
        passive = None
        active = None
        protected = False
        rename_source = None
        self_conversion = True  # ftpsrv's per-connection SELF to ELF conversion.
        block_mode = False  # MODE B accepted on this control connection.
        kept = None  # Block mode: the data connection kept open after a successful transfer.
        active_pending = False  # EPRT/PORT since the last transfer (block mode bookkeeping).
        mc_count, mc_stripe = 0, 4096
        mc_pending, mc_kept = [], []

        def start_tls():
            self.rfile.close()
            self.wfile.close()
            self.connection = self.server.tls_context.wrap_socket(self.connection, server_side=True)
            self.rfile = self.connection.makefile("rb")
            self.wfile = self.connection.makefile("wb")

        @contextlib.contextmanager
        def accept_data():
            data = passive.accept()[0] if passive is not None else socket.create_connection(active, timeout=10)
            self.server.data_connections += 1
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

        def stall_upload(received):
            """Stops reading an upload once, after `stall_upload_after` bytes of it, until released."""
            limit = self.server.stall_upload_after
            if limit is not None and received >= limit and not self.server.stalled.is_set():
                self.server.pause()

        def close_kept():
            nonlocal kept, mc_kept
            if kept is not None:
                with contextlib.suppress(OSError):
                    kept.close()
                kept = None
            for data in mc_kept:
                with contextlib.suppress(OSError):
                    data.close()
            mc_kept = []

        def close_pending():
            """Drops a data connection set up for a transfer that will not happen."""
            nonlocal passive, active_pending, mc_pending
            if passive is not None:
                passive.close()
            passive = None
            active_pending = False
            for listener in mc_pending:
                listener.close()
            mc_pending = []

        def open_mc_data(command):
            nonlocal mc_pending, mc_kept
            if mc_pending:
                reply("150 Opening FTPMC data connections.")
                self.server.transfer_starts.append((command, 150))
                sockets = [listener.accept()[0] for listener in mc_pending]
                close_pending()
                self.server.data_connections += len(sockets)
                if protected:
                    sockets = parallel_data(lambda data: self.server.tls_context.wrap_socket(data, server_side=True),
                                            sockets)
                    self.server.tls_data_connections += len(sockets)
                for data in sockets:
                    data.settimeout(10)
                return sockets
            sockets, mc_kept = mc_kept, []
            reply("125 FTPMC data connections already open.")
            self.server.transfer_starts.append((command, 125))
            return sockets

        def finish_mc_data(sockets):
            nonlocal mc_kept
            if block_mode and self.server.mode_b_keep:
                mc_kept = sockets
                reply("250 Transfer complete.")
            else:
                def close(data):
                    try:
                        if protected:
                            data = data.unwrap()
                    finally:
                        data.close()
                parallel_data(close, sockets)
                reply("226 Transfer complete.")

        def receive_mc_data(data):
            payload = bytearray()
            while True:
                if block_mode:
                    header = recv_exact(data, 3)
                    if len(header) != 3 or header[0] & 0x0F:
                        raise ValueError("truncated FTPMC block")
                    length = int.from_bytes(header[1:], "big")
                    chunk = recv_exact(data, length)
                    if len(chunk) != length:
                        raise ValueError("truncated FTPMC payload")
                    if not header[0] & BLOCK_RESTART:
                        payload.extend(chunk)
                    if header[0] & BLOCK_EOF:
                        break
                else:
                    chunk = data.recv(65536)
                    if not chunk:
                        break
                    payload.extend(chunk)
                stall_upload(len(payload))
            return bytes(payload)

        def upload_mc(command, target):
            sockets = open_mc_data(command)
            with self.server.files_lock:
                if command == "STOR":
                    self.server.files[target] = b""
                else:
                    self.server.files.setdefault(target, b"")
            try:
                payload = join_stripes(parallel_data(receive_mc_data, sockets), mc_stripe)
            except (OSError, ValueError):
                for data in sockets:
                    data.close()
                reply("426 Incomplete FTPMC upload.")
                return False
            fail = self.server.fail_uploads > 0
            if fail:
                self.server.fail_uploads -= 1
                payload = payload[:self.server.drop_after]
            with self.server.files_lock:
                if target in self.server.files:
                    self.server.files[target] += payload
            self.server.uploads.append((command, target, payload))
            if fail:
                self.server.failed_uploads += 1
                for data in sockets:
                    data.close()
                reply("426 FTPMC upload interrupted.")
                return True  # Drop the control connection; retry must negotiate FTPMC again.
            finish_mc_data(sockets)
            return False

        def open_block_data(command):
            """Block mode: a new data connection after PASV/EPSV/EPRT/PORT (150), the kept one (125), or None
            after replying 425. A failed transfer must close what this returns; finish_block_data keeps it."""
            nonlocal passive, kept, active_pending
            if passive is None and not active_pending and kept is not None and self.server.mode_b_refuse_reuse > 0:
                self.server.mode_b_refuse_reuse -= 1  # Like a server that lost track of the connection.
                close_kept()
            if passive is not None or active_pending:
                close_kept()
                data = passive.accept()[0] if passive is not None else socket.create_connection(active, timeout=10)
                close_pending()
                self.server.data_connections += 1
                reply("150 Opening data connection.")
                if protected:
                    data = self.server.tls_context.wrap_socket(data, server_side=True)
                    self.server.tls_data_connections += 1
                data.settimeout(10)
                self.server.transfer_starts.append((command, 150))
                return data
            if kept is not None:
                data, kept = kept, None
                reply("125 Data connection already open; transfer starting.")
                self.server.transfer_starts.append((command, 125))
                return data
            self.server.transfer_starts.append((command, 425))
            reply("425 Use PORT or PASV first.")
            return None

        def finish_block_data(data):
            """Block mode, after a successful transfer: keep the connection (250), or close it (226)."""
            nonlocal kept
            if not self.server.mode_b_keep:
                data.close()
                reply("226 Transfer complete; closing data connection.")
                return
            kept = data
            reply("250 Transfer complete.")
            if self.server.mode_b_close_idle:
                close_kept()  # Like an idle timeout right after the reply: the client must notice.

        def send_data(command, payload, start, done):
            """Sends a listing or a file in the mode of the control connection."""
            nonlocal passive
            if mc_pending or mc_kept:
                sockets = open_mc_data(command)
                parts = [bytearray() for _ in sockets]
                for offset in range(0, len(payload), mc_stripe):
                    parts[(offset // mc_stripe) % len(sockets)].extend(payload[offset:offset + mc_stripe])
                parallel_data(lambda pair: pair[0].sendall(block_frames(pair[1]) if block_mode else pair[1]),
                              list(zip(sockets, parts)))
                finish_mc_data(sockets)
                return
            if block_mode:
                data = open_block_data(command)
                if data is None:
                    return
                try:
                    data.sendall(block_frames(payload, eof_on_data=self.server.mode_b_eof_on_data,
                                              extra_blocks=self.server.mode_b_extra_blocks,
                                              bad_descriptor=self.server.mode_b_bad_descriptor))
                except OSError:
                    data.close()
                    reply("426 Connection closed; transfer aborted.")
                    return
                finish_block_data(data)
                return
            reply(start)
            self.server.transfer_starts.append((command, 150))
            with accept_data() as data:
                data.sendall(payload)
            if passive is not None:
                passive.close()
            passive = None
            reply(done)

        def listing(long_format):
            names = [(posixpath.basename(p), len(data)) for p, data in self.server.files.items()
                     if posixpath.dirname(p) == cwd]
            if self.server.directories is not None:
                names += [(posixpath.basename(p), None) for p in self.server.directories
                          if p != cwd and posixpath.dirname(p) == cwd]
            if not long_format:
                return [name for name, _ in names]
            return [(f"drwxr-xr-x 1 ftp ftp 0 Jan 01 2026 {name}" if size is None
                     else f"-rw-r--r-- 1 ftp ftp {size} Jan 01 2026 {name}") for name, size in names]

        def path(arg):
            return posixpath.normpath(arg if arg.startswith("/") else posixpath.join(cwd, arg))

        try:
            if self.server.implicit_tls:
                start_tls()
            if self.server.ftpsrv:
                reply("220-Welcome to ftpsrv.elf running on pid 1\r\n220-Version: test\r\n220 Service is ready")
            else:
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
                elif self.server.ftpsrv and command in ("EPSV", "NLST"):
                    if command == "NLST" and passive is not None:
                        passive.close()  # curl connected before sending NLST.
                        passive = None
                    reply("502 Command not recognized")
                elif self.server.ftpsrv and command == "SELF" and self.server.ftpsrv_self:
                    self_conversion = not self_conversion
                    reply(f"226 SELF transfer mode {'enabled' if self_conversion else 'disabled'}")
                elif self.server.ftpsrv and command == "SELF":
                    reply("502 Command not recognized")
                elif self.server.ftpsrv and command == "USER":
                    reply("230 User logged in")
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
                elif command == "FEAT":
                    reply(f"211-Features\r\n MC {self.server.mc_max}\r\n211 End" if self.server.mc_max
                          else "211 No extensions")
                elif command == "OPTS" and arg.startswith("MC "):
                    if self.server.mc_opts_refusal:
                        reply(self.server.mc_opts_refusal)
                    else:
                        values = arg.split()
                        mc_count = min(int(values[1]), self.server.mc_max)
                        mc_stripe = min(int(values[2]) if len(values) > 2 else 4096, self.server.mc_max_stripe)
                        reply(f"200 MC {mc_count} {mc_stripe}")
                elif command == "MPSV":
                    if self.server.mc_mpsv_refusal:
                        reply(self.server.mc_mpsv_refusal)
                    else:
                        close_pending()
                        close_kept()
                        mc_pending = [self.server.passive_socket() for _ in range(mc_count)]
                        ports = ",".join(str(listener.getsockname()[1]) for listener in mc_pending)
                        reply(f"224 Entering Multi Passive Mode (|||{ports}|)")
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
                    if contents is None:
                        reply("550 File not found")
                    elif (self.server.ftpsrv and self.server.ftpsrv_self and self_conversion
                          and contents[:4] == SELF_MAGIC and len(contents) >= 12):
                        # Like ftpsrv, report the ELF size read from the headers, whatever the stored size.
                        reply(f"213 {int.from_bytes(contents[4:12], 'little')}")
                    else:
                        reply(f"213 {len(contents)}")
                elif command == "MDTM":
                    # "MDTM <time> <path>" sets the time (vsftpd); "MDTM <path>" asks for it.
                    setting = arg[:14].isdigit() and arg[14:15] == " "
                    reply("550 Cannot set the time" if setting and self.server.timestamps == "none"
                          else "213 20260101000000")
                elif command == "MODE" and self.server.mode_b not in (None, False):
                    # RFC 959 transfer mode. A string `mode_b` is the refusal of MODE B.
                    if not arg.strip():
                        reply("501 Usage: MODE <S|B>")
                    elif arg.strip().upper() == "S":
                        block_mode = False
                        close_kept()
                        reply("200 Mode set to S.")
                    elif arg.strip().upper() == "B" and self.server.mode_b is True:
                        block_mode = True
                        reply("200 Mode set to B.")
                    elif arg.strip().upper() == "B":
                        reply(self.server.mode_b)
                    else:
                        reply("504 Unsupported transfer mode.")
                elif command == "EPSV":
                    close_kept()  # A new data port closes a kept connection (RFC 959).
                    passive = self.server.passive_socket()
                    reply(f"229 Extended Passive Mode (|||{passive.getsockname()[1]}|)")
                elif command == "PASV":
                    close_kept()
                    if passive is not None:
                        passive.close()
                    passive = self.server.passive_socket()
                    port = passive.getsockname()[1]
                    reply(f"227 Entering Passive Mode (127,0,0,1,{port >> 8},{port & 0xFF}).")
                elif command == "EPRT":
                    close_kept()
                    _, _, host, port, _ = arg.split(arg[0])
                    active = (host, int(port))
                    active_pending = True
                    reply("200 Active endpoint accepted")
                elif command == "PORT":
                    close_kept()
                    numbers = arg.split(",")
                    active = (".".join(numbers[:4]), int(numbers[4]) << 8 | int(numbers[5]))
                    active_pending = True
                    reply("200 Active endpoint accepted")
                elif command == "NLST":
                    if self.server.stall_listing:
                        self.server.pause()
                    if self.server.refuse_listing:
                        close_pending()
                        close_kept()
                        reply("550 Listing refused")
                        continue
                    names = listing(False)
                    if self.server.nlst_hides_dotfiles and "-a" not in arg.split():
                        names = [name for name in names if not name.startswith(".")]
                    send_data(command, "".join(name + "\r\n" for name in names).encode("utf-8"),
                              "150 Listing", "226 Listing complete")
                elif command == "LIST":
                    send_data(command, "".join(line + "\r\n" for line in listing(True)).encode("utf-8"),
                              "150 Listing", "226 Listing complete")
                elif command == "RETR":
                    contents = self.server.files.get(path(arg))
                    if contents is None:
                        close_pending()
                        close_kept()
                        reply("550 File not found")
                        continue
                    send_data(command, contents, "150 Sending file", "226 Transfer complete")
                elif command in ("STOR", "APPE") and (mc_pending or mc_kept):
                    if upload_mc(command, path(arg)):
                        return
                elif command in ("STOR", "APPE") and block_mode:
                    target = path(arg)
                    fail_upload = (self.server.fail_uploads > 0 and
                                   (self.server.fail_target is None or target == self.server.fail_target))
                    if self.server.reject_upload:
                        close_pending()
                        close_kept()
                        reply("550 Overwrite refused")
                        continue
                    data = open_block_data(command)
                    if data is None:
                        continue  # 425: no file is opened or created.
                    if fail_upload:
                        self.server.fail_uploads -= 1
                    with self.server.files_lock:
                        if command == "STOR":
                            self.server.files[target] = b""
                        else:
                            self.server.files.setdefault(target, b"")
                    payload = bytearray()
                    complete = False
                    try:
                        while not complete:
                            header = recv_exact(data, 3)
                            if len(header) < 3:
                                break  # Closed before the EOF block: truncated.
                            descriptor, remaining = header[0], int.from_bytes(header[1:3], "big")
                            if descriptor & 0x0F:
                                break  # Not a block mode descriptor: protocol error.
                            while remaining:
                                limit = min(remaining, 65536)
                                if fail_upload:
                                    limit = min(limit, self.server.drop_after - len(payload))
                                stall_upload(len(payload))
                                chunk = data.recv(limit)
                                if not chunk:
                                    break
                                remaining -= len(chunk)
                                if descriptor & BLOCK_RESTART:
                                    continue  # A restart marker is not file data.
                                payload.extend(chunk)
                                with self.server.files_lock:
                                    if target in self.server.files:
                                        self.server.files[target] += chunk
                                if fail_upload and len(payload) >= self.server.drop_after:
                                    break
                            if remaining or (fail_upload and len(payload) >= self.server.drop_after):
                                break
                            complete = bool(descriptor & BLOCK_EOF)
                    except (OSError, ssl.SSLError):
                        complete = False
                    self.server.uploads.append((command, target, bytes(payload)))
                    if fail_upload:
                        # As in stream mode: host-side mutations before the data socket closes.
                        self.server.failed_uploads += 1
                        if self.server.after_failure:
                            self.server.after_failure(self.server, target)
                        data.close()
                        reply("426 Data connection interrupted")
                        return  # Drop the control connection too; retry must reconnect.
                    if not complete:
                        self.server.truncated_uploads += 1
                        data.close()  # The partial bytes stay, as after an interrupted stream upload.
                        reply("426 Connection closed; transfer aborted.")
                        continue
                    if self.server.stall_confirmation:
                        self.server.pause()
                    finish_block_data(data)
                elif command in ("STOR", "APPE"):
                    target = path(arg)
                    fail_upload = (self.server.fail_uploads > 0 and
                                   (self.server.fail_target is None or target == self.server.fail_target))
                    if fail_upload:
                        self.server.fail_uploads -= 1
                    reply("550 Overwrite refused" if self.server.reject_upload else "150 Upload accepted")
                    if not self.server.reject_upload:
                        self.server.transfer_starts.append((command, 150))
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
                                stall_upload(len(payload))
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
            close_kept()

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
                 ftpsrv=False, ftpsrv_self=True, mode_b=None, mode_b_keep=True, mode_b_close_idle=False,
                 mode_b_refuse_reuse=0, mode_b_eof_on_data=False, mode_b_extra_blocks=False,
                 mode_b_bad_descriptor=False, stall_upload_after=None, control_port=0,
                 passive_ports=range(30000, 30010), mc_max=0, mc_max_stripe=1048576,
                 mc_opts_refusal=None, mc_mpsv_refusal=None):
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
        # ps5-payload-dev/ftpsrv: its greeting, USER without PASS, no EPSV or NLST, and, unless
        # `ftpsrv_self` is false (older versions), SIZE of SELF files converted until SELF toggles it off.
        self.ftpsrv = ftpsrv
        self.ftpsrv_self = ftpsrv_self
        # RFC 959 block mode, as in the StreamExtract MODE B contract. None (default): MODE is an unknown
        # command (500), like a server without it. True: MODE S/B are accepted; transfers are framed and
        # the data connection stays open after each success (250, reused with 125). A string: MODE S is
        # accepted and MODE B gets that reply, e.g. "504 Unsupported transfer mode.".
        self.mode_b = mode_b
        self.mc_max = mc_max
        self.mc_max_stripe = mc_max_stripe
        self.mc_opts_refusal = mc_opts_refusal
        self.mc_mpsv_refusal = mc_mpsv_refusal
        self.mode_b_keep = mode_b_keep  # False: close the data connection after each transfer (226).
        self.mode_b_close_idle = mode_b_close_idle  # Close a kept connection right after its 250.
        self.mode_b_refuse_reuse = mode_b_refuse_reuse  # Answer 425 to this many reuses of a kept connection.
        # Listing/download framing: EOF on the last data frame, extra frames receivers must ignore, or an
        # invalid descriptor that they must reject.
        self.mode_b_eof_on_data = mode_b_eof_on_data
        self.mode_b_extra_blocks = mode_b_extra_blocks
        self.mode_b_bad_descriptor = mode_b_bad_descriptor
        self.data_connections = 0  # Data connections opened, in either mode.
        self.transfer_starts = []  # (command, 150 new connection | 125 kept connection | 425 none).
        self.truncated_uploads = 0  # Block mode uploads that ended without an EOF frame.
        self.stall_confirmation = stall_confirmation
        self.stall_upload_after = stall_upload_after  # Stop reading an upload after this many bytes (once).
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
