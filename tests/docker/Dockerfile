FROM python:3.13-alpine@sha256:2d9aefe2fef018a7eb2c13064c89c71929800fd2e5dccdbf52ea5da5bb8d929a

RUN apk add --no-cache vsftpd=3.0.5-r3 openssl=3.5.9-r0 openssh-server=10.3_p1-r1 openssh-sftp-server=10.3_p1-r1
WORKDIR /test-server
COPY docker/fault_server.py docker/server.py ./
COPY fixtures/ftps/server-cert.pem fixtures/ftps/server-key.pem ./ca/
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 \
    MODE=filesystem FTP_UID=1000 CONTROL_START=21000 PASSIVE_START=30000
EXPOSE 21 22 990 8100
ENTRYPOINT ["python3", "server.py"]
