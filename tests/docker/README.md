# Shared integration test server

Every runner in `tests/integration` uses this Docker image. The Python helpers
build it automatically, allocate ports, wait for readiness and remove their
containers when the runner exits. Docker must be running. The base image is
pinned by digest, and vsftpd/OpenSSL by Alpine package version.

vsftpd listens on port 21 for FTP or explicit FTPS and on 990 for implicit FTPS.
It uses the test-only `tester` / `secret` login and `/ftp/tester` home directory.
The fault suites use remotely configured FTP/FTPS sessions in the same image to
exercise interrupted uploads, refusals, retries, corruption and cancellation.
They inspect server state through an HTTP control API, rather than running a
second server on the host.

Build manually from the repository root:

```bash
docker build -t streamextract-integration -f tests/docker/Dockerfile tests
```

Run a standalone server (ports and passive range must match on both sides):

```bash
docker run --rm --name streamextract-test-server \
  -e MODE=filesystem -e FTP_UID=1000 -e CONTROL_START=21000 -e PASSIVE_START=30000 \
  -p 127.0.0.1:2121:21 -p 127.0.0.1:2990:990 \
  -p 127.0.0.1:30000-30009:30000-30009 \
  streamextract-integration
```

Trust `tests/fixtures/ftps/server-cert.pem` with `--cacert`. The image creates a
short-lived server certificate signed by that fixture CA, covering loopback and
its container IP. The CA's private key is public and strictly for tests.

Run the suites without RAR or Docker configuration arguments:

```bash
python3 tests/integration/ftps.py --sext build/sext --lib build/libstreamextractcore.dylib
python3 tests/integration/ftp_faults.py --sext build/sext --lib build/libstreamextractcore.dylib
```

The archive runner additionally needs RARLAB's `rar`; 7-Zip and Info-ZIP are
optional. Select the protocol for its CLI and library archive tests (`--lib`):

```bash
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar --protocol ftps
python3 tests/integration/run.py --sext build/sext --rar /path/to/rar --protocol ftps --ftps-mode implicit
```

`--lib` exercises the GUI C API through FTP. Active data connections are tested
on Linux, where the container address is routable; macOS/Windows skip them.
All passive-mode FTP/FTPS cases run on every host platform.
