"""Shared Docker image and lifecycle for every FTP/FTPS integration suite."""

import atexit
import base64
import hashlib
import json
import random
import socket
import subprocess
import sys
import threading
import time
import uuid
import unicodedata
from collections.abc import MutableMapping
from pathlib import Path
from urllib.error import URLError
from urllib.request import Request, urlopen


TESTS = Path(__file__).resolve().parents[1]
CA_CERTIFICATE = TESTS / "fixtures" / "ftps" / "server-cert.pem"
USER = "tester"
PASSWORD = "secret"
HOME = "/ftp/tester"


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def free_port_range(count):
    for _ in range(200):
        base = random.randint(30000, 60000 - count)
        sockets = []
        try:
            for port in range(base, base + count):
                listener = socket.socket()
                sockets.append(listener)
                listener.bind(("127.0.0.1", port))
            return base
        except OSError:
            continue
        finally:
            for listener in sockets:
                listener.close()
    raise RuntimeError("no free test port range")


_IMAGE = None


def image():
    global _IMAGE
    if _IMAGE is None:
        sources = [TESTS / "docker" / name for name in ("Dockerfile", "server.py", "fault_server.py")]
        sources += sorted((TESTS / "fixtures" / "ftps").glob("*.pem"))
        digest = hashlib.sha256()
        for source in sources:
            if source.is_file():
                digest.update(source.name.encode())
                digest.update(source.read_bytes())
        _IMAGE = "streamextract-integration:" + digest.hexdigest()[:16]
        result = subprocess.run(["docker", "image", "inspect", _IMAGE], capture_output=True)
        if result.returncode:
            subprocess.run(["docker", "build", "-t", _IMAGE, "-f", str(TESTS / "docker" / "Dockerfile"),
                            str(TESTS)], check=True)
    return _IMAGE


class Container:
    def __init__(self, data_dir=None):
        self.name = "streamextract-it-" + uuid.uuid4().hex[:12]
        self.routable = sys.platform.startswith("linux")
        self.control_start = free_port_range(4)
        self.passive_start = free_port_range(10)
        self.api_port = free_port()
        mode = "filesystem" if data_dir is not None else "faults"
        command = ["docker", "run", "-d", "--rm", "--name", self.name,
                   "-e", f"MODE={mode}", "-e", f"CONTROL_START={self.control_start}",
                   "-e", f"PASSIVE_START={self.passive_start}", "-p", f"127.0.0.1:{self.api_port}:8100",
                   "-p", f"127.0.0.1:{self.passive_start}-{self.passive_start + 9}:"
                         f"{self.passive_start}-{self.passive_start + 9}"]
        if data_dir is not None:
            import os
            self.data_dir = Path(data_dir).resolve()
            self.data_dir.mkdir(parents=True, exist_ok=True)
            uid = (os.getuid() if hasattr(os, "getuid") else 1000) or 1000
            command += ["-v", f"{self.data_dir}:{HOME}", "-e", f"FTP_UID={uid}",
                        "-p", f"127.0.0.1:{self.control_start}:21",
                        "-p", f"127.0.0.1:{self.control_start + 1}:990"]
        else:
            command += ["-p", f"127.0.0.1:{self.control_start}-{self.control_start + 3}:"
                             f"{self.control_start}-{self.control_start + 3}"]
        result = subprocess.run(command + [image()], capture_output=True, text=True)
        if result.returncode:
            subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)
            raise RuntimeError("cannot start the shared Docker test server: " + result.stderr.strip())
        atexit.register(self.close)
        self.host = "127.0.0.1"
        if self.routable:
            self.host = subprocess.run(
                ["docker", "inspect", "-f", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", self.name],
                check=True, capture_output=True, text=True).stdout.strip()
        self.api_url = f"http://{self.host}:8100" if self.routable else f"http://127.0.0.1:{self.api_port}"
        self.port = 21 if self.routable else self.control_start
        self.implicit_port = 990 if self.routable else self.control_start + 1
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                try:
                    if self.request("GET", "/health")["ready"]:
                        break
                except (URLError, OSError):
                    time.sleep(0.1)
            else:
                raise RuntimeError("Docker test server did not become ready")
            if data_dir is not None:
                while time.monotonic() < deadline:
                    try:
                        with socket.create_connection((self.host, self.port), timeout=2) as connection:
                            if connection.recv(64).startswith(b"220"):
                                break
                    except OSError:
                        pass
                    time.sleep(0.1)
                else:
                    raise RuntimeError("vsftpd did not become ready")
        except BaseException:
            logs = subprocess.run(["docker", "logs", self.name], capture_output=True, text=True)
            self.close()
            raise RuntimeError("Docker test server startup failed:\n" + logs.stdout + logs.stderr)

    def request(self, method, path, body=None):
        request = Request(self.api_url + path, data=None if body is None else json.dumps(body).encode(),
                          headers={"Content-Type": "application/json"}, method=method)
        with urlopen(request, timeout=20) as response:
            return json.load(response)

    def sync(self):
        # Docker Desktop can briefly cache bind-mount metadata after the host
        # changes a remote fixture. Check its visible sizes before probing FTP.
        expected = {unicodedata.normalize("NFC", item.relative_to(self.data_dir).as_posix()):
                    "dir" if item.is_dir() else item.stat().st_size
                    for item in self.data_dir.rglob("*")}
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.request("GET", "/filesystem") == expected:
                return
            time.sleep(0.1)
        raise RuntimeError("the Docker bind mount did not reflect the remote fixture changes")

    def close(self):
        if self.name is not None:
            subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)
            self.name = None
            atexit.unregister(self.close)


Server = Container
_FAULT_CONTAINER = None


class RemoteFiles(MutableMapping):
    def __init__(self, server):
        self.server = server

    def snapshot(self):
        return {path: base64.b64decode(data) for path, data in self.server.get("files").items()}

    def __getitem__(self, key):
        return self.snapshot()[key]

    def __setitem__(self, key, value):
        self.server.post("file", {"path": key, "data": base64.b64encode(value).decode()})

    def __delitem__(self, key):
        if key not in self:
            raise KeyError(key)
        self.server.post("file", {"path": key})

    def __iter__(self):
        return iter(self.snapshot())

    def __len__(self):
        return len(self.snapshot())


class RemoteEvent:
    def __init__(self, server, name):
        self.server, self.name = server, name

    def set(self):
        self.server.post(self.name)

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.server.get(self.name):
                return True
            time.sleep(0.05)
        return False


class FtpServer:
    """A fault session in the shared container, with the original test-facing API."""
    def __init__(self, *, files=None, after_failure=None, tls=False, **options):
        global _FAULT_CONTAINER
        if _FAULT_CONTAINER is None:
            _FAULT_CONTAINER = Container()
        self.container = _FAULT_CONTAINER
        self.host = self.container.host
        self.routable = self.container.routable
        response = self.container.request("POST", "/sessions", {
            "files": {path: base64.b64encode(data).decode() for path, data in (files or {}).items()},
            "tls": tls, "after_failure": after_failure is not None, **options})
        self.prefix = "/sessions/" + response["id"] + "/"
        self.server_address = (self.host, response["port"])
        self.files = RemoteFiles(self)
        self.stalled = RemoteEvent(self, "stalled")
        self.release = RemoteEvent(self, "release")
        self.done = threading.Event()
        self.callback_error = None
        self.callback_thread = None
        if after_failure is not None:
            def callbacks():
                while not self.done.wait(0.02):
                    target = self.get("failure")
                    if target is not None:
                        try:
                            after_failure(self, target)
                        except BaseException as error:
                            self.callback_error = error
                        finally:
                            self.post("ack")
                        # Wait for the server to consume the acknowledgment.
                        while not self.done.wait(0.02) and self.get("failure") is not None:
                            pass
            self.callback_thread = threading.Thread(target=callbacks, daemon=True)
            self.callback_thread.start()

    def get(self, name):
        return self.container.request("GET", self.prefix + name)

    def post(self, name, body=None):
        return self.container.request("POST", self.prefix + name, body or {})

    @property
    def commands(self):
        return [tuple(value) for value in self.get("commands")]

    @property
    def uploads(self):
        return [(cmd, path, base64.b64decode(data)) for cmd, path, data in self.get("uploads")]

    @property
    def deleted(self):
        return self.get("deleted")

    @property
    def login_attempts(self):
        return self.get("login_attempts")

    @property
    def tls_data_connections(self):
        return self.get("tls_data_connections")

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.done.set()
        if self.callback_thread is not None:
            self.callback_thread.join(timeout=20)
        self.container.request("DELETE", self.prefix.rstrip("/"))
        if self.callback_error is not None:
            raise self.callback_error
