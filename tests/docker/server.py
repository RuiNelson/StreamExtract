"""Supervise real vsftpd listeners and remotely controlled FTP fault scenarios."""

import base64
import json
import os
import signal
import socket
import ssl
import subprocess
import threading
import uuid
import unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from fault_server import FtpServer


SESSIONS = {}
LOCK = threading.Lock()
PASSIVE = range(int(os.environ["PASSIVE_START"]), int(os.environ["PASSIVE_START"]) + 10)
CONTROLS = range(int(os.environ["CONTROL_START"]), int(os.environ["CONTROL_START"]) + 4)
SSH_PROCESS = None


def tls_context():
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain("/tmp/server-cert.pem", "/tmp/server-key.pem")
    return context


def generate_certificate():
    # Keep the fixture CA stable; the leaf also covers Docker's routable address.
    address = socket.gethostbyname(socket.gethostname())
    Path("/tmp/server.ext").write_text(
        f"subjectAltName=IP:127.0.0.1,IP:::1,IP:{address}\n"
        "basicConstraints=critical,CA:FALSE\nextendedKeyUsage=serverAuth\n")
    subprocess.run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", "/tmp/server-key.pem", "-out", "/tmp/server.csr",
                    "-subj", "/CN=StreamExtract Docker FTPS server"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["openssl", "x509", "-req", "-in", "/tmp/server.csr", "-CA", "ca/server-cert.pem",
                    "-CAkey", "ca/server-key.pem", "-CAcreateserial", "-days", "30", "-sha256",
                    "-extfile", "/tmp/server.ext", "-out", "/tmp/server-cert.pem"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def start_vsftpd():
    uid = os.environ.get("FTP_UID", "1000")
    subprocess.run(["adduser", "-D", "-H", "-u", uid, "-h", "/ftp/tester", "tester"], check=True)
    subprocess.run(["chpasswd"], input="tester:secret\n", text=True, check=True)
    Path("/ftp/tester").mkdir(parents=True, exist_ok=True)
    os.chown("/ftp/tester", int(uid), int(uid))
    Path("/var/empty").mkdir(exist_ok=True)
    common = (
        # Linux tests address the container by its IPv4 Docker-network address.
        # An IPv6-only listener is not reachable there on all Docker hosts.
        "listen=YES\nlisten_ipv6=NO\nbackground=NO\nanonymous_enable=NO\n"
        "local_enable=YES\nwrite_enable=YES\nlocal_umask=022\ncheck_shell=NO\n"
        "chroot_local_user=NO\nsecure_chroot_dir=/var/empty\nmdtm_write=YES\n"
        "ssl_enable=YES\nforce_local_logins_ssl=NO\nforce_local_data_ssl=NO\n"
        "require_ssl_reuse=NO\nssl_tlsv1=YES\nssl_sslv2=NO\nssl_sslv3=NO\n"
        "rsa_cert_file=/tmp/server-cert.pem\nrsa_private_key_file=/tmp/server-key.pem\n"
        f"pasv_min_port={PASSIVE.start}\npasv_max_port={PASSIVE.stop - 1}\n"
    )
    processes = []
    for port, implicit in ((21, False), (990, True)):
        config = Path(f"/tmp/vsftpd-{port}.conf")
        config.write_text(common + f"listen_port={port}\nimplicit_ssl={'YES' if implicit else 'NO'}\n")
        processes.append(subprocess.Popen(["vsftpd", str(config)]))
    return processes


def start_sshd(settings=None):
    global SSH_PROCESS
    settings = settings or {}
    if SSH_PROCESS is not None:
        SSH_PROCESS.terminate()
        SSH_PROCESS.wait(timeout=5)
    subprocess.run(["ssh-keygen", "-A"], check=True, stdout=subprocess.DEVNULL)
    local_user = os.environ.get("LOCAL_USER", "tester")
    if local_user != "tester" and local_user != "root":
        uid = os.environ.get("FTP_UID", "1000")
        passwd = Path("/etc/passwd")
        if not any(line.startswith(local_user + ":") for line in passwd.read_text().splitlines()):
            with passwd.open("a") as file:
                file.write(f"{local_user}:x:{uid}:{uid}::/ftp/tester:/bin/sh\n")
            subprocess.run(["chpasswd"], input=f"{local_user}:secret\n", text=True, check=True)
    Path("/tmp/ssh-authorized-keys").write_text(settings.get("authorized_keys", ""))
    Path("/tmp/ssh-authorized-keys").chmod(0o644)
    config = Path("/tmp/sshd-config")
    config.write_text(
        "Port 22\nListenAddress 0.0.0.0\nStrictModes no\nPerSourcePenalties no\n"
        "AuthorizedKeysFile /tmp/ssh-authorized-keys\nPermitRootLogin no\n"
        "Subsystem sftp internal-sftp\nLogLevel VERBOSE\n"
        f"PasswordAuthentication {'yes' if settings.get('password', True) else 'no'}\n"
        f"PubkeyAuthentication {'yes' if settings.get('public_key', True) else 'no'}\n"
        "KbdInteractiveAuthentication no\n"
    )
    SSH_PROCESS = subprocess.Popen(["/usr/sbin/sshd", "-D", "-e", "-f", str(config)])
    return SSH_PROCESS


class Api(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def respond(self, value, code=200):
        encoded = json.dumps(value).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def body(self):
        return json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")

    def do_POST(self):
        parts = self.path.strip("/").split("/")
        body = self.body()
        if parts == ["ssh"]:
            start_sshd(body)
            self.respond({})
            return
        if parts == ["sessions"]:
            options = body
            options["files"] = {key: base64.b64decode(value) for key, value in options.get("files", {}).items()}
            if options.pop("tls", False):
                options["tls_context"] = tls_context()
            callback = options.pop("after_failure", False)
            with LOCK:
                for port in CONTROLS:
                    try:
                        server = FtpServer(control_port=port, passive_ports=PASSIVE, **options)
                        break
                    except OSError:
                        continue
                else:
                    self.respond({"error": "no free control port"}, 503)
                    return
                server.failure_event = None
                server.failure_ack = threading.Event()
                if callback:
                    def after_failure(current, target):
                        current.failure_ack.clear()
                        current.failure_event = target
                        if not current.failure_ack.wait(15):
                            raise RuntimeError("host failure callback was not acknowledged")
                        current.failure_event = None
                    server.after_failure = after_failure
                session = uuid.uuid4().hex
                SESSIONS[session] = server
            self.respond({"id": session, "port": port})
            return
        server = SESSIONS[parts[1]]
        if parts[2] == "release":
            server.release.set()
        elif parts[2] == "ack":
            server.failure_ack.set()
        elif parts[2] == "file":
            with server.files_lock:
                if "data" in body:
                    server.files[body["path"]] = base64.b64decode(body["data"])
                else:
                    server.files.pop(body["path"], None)
        self.respond({})

    def do_GET(self):
        parts = self.path.strip("/").split("/")
        if parts == ["health"]:
            self.respond({"ready": True})
            return
        if parts == ["ssh-host-keys"]:
            self.respond([path.read_text().strip() for path in sorted(Path("/etc/ssh").glob("ssh_host_*_key.pub"))])
            return
        if parts == ["filesystem"]:
            root = Path("/ftp/tester")
            self.respond({unicodedata.normalize("NFC", item.relative_to(root).as_posix()):
                          "dir" if item.is_dir() else item.stat().st_size
                          for item in root.rglob("*")})
            return
        server = SESSIONS[parts[1]]
        name = parts[2]
        if name == "files":
            with server.files_lock:
                value = {key: base64.b64encode(data).decode() for key, data in server.files.items()}
        elif name == "uploads":
            value = [(cmd, path, base64.b64encode(data).decode()) for cmd, path, data in server.uploads]
        elif name == "stalled":
            value = server.stalled.is_set()
        elif name == "failure":
            value = server.failure_event
        else:
            value = getattr(server, name)
        self.respond(value)

    def do_DELETE(self):
        session = self.path.strip("/").split("/")[1]
        with LOCK:
            server = SESSIONS.pop(session)
        server.failure_ack.set()
        server.__exit__(None, None, None)
        self.respond({})


if __name__ == "__main__":
    generate_certificate()
    processes = start_vsftpd() if os.environ.get("MODE") == "filesystem" else []
    if processes:
        processes.append(start_sshd())

    def stop(_signum, _frame):
        for process in processes:
            process.terminate()
        if SSH_PROCESS is not None:
            SSH_PROCESS.terminate()
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    ThreadingHTTPServer(("0.0.0.0", 8100), Api).serve_forever()
