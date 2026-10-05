These are public, test-only credentials for the localhost FTPS server. Never
use this private key outside tests. The self-signed certificate identifies only
127.0.0.1 and is valid from October 2026 through September 2126.

Generated with:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout server-key.pem \
  -out server-cert.pem -days 36500 -subj '/CN=StreamExtract FTPS test server' \
  -addext 'subjectAltName=IP:127.0.0.1' -addext 'basicConstraints=critical,CA:TRUE'
```
