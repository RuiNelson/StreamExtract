/* C API of libstreamextractcore, the shared library used by StreamExtract.
 *
 * A job runs a whole upload (read the archive, log in, check what is already
 * on the server, transfer) on its own threads. The caller polls its state as
 * JSON and answers the archive password prompt when one is pending. Every
 * string is UTF-8. Every function is thread-safe for a given job.
 */

#ifndef STREAMEXTRACT_H
#define STREAMEXTRACT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#ifdef STREAMEXTRACT_BUILDING_LIBRARY
#define STREAMEXTRACT_API __declspec(dllexport)
#else
#define STREAMEXTRACT_API __declspec(dllimport)
#endif
#else
#define STREAMEXTRACT_API __attribute__((visibility("default")))
#endif

typedef struct streamextract_job streamextract_job;

typedef struct streamextract_job_config {
  const char* archive;          /* RAR, ZIP, 7z or tar archive (first volume), or a single exFAT volume image. */
  const char* archive_password; /* NULL: asked through the job when needed. */
  const char* host;             /* Host name or address, without a URL scheme or a path. */
  int port;                     /* 1-65535. */
  int active_mode;              /* 0: passive, 1: active. */
  const char* user;             /* NULL or "": anonymous for FTP/FTPS; local username for SFTP. */
  const char* password;         /* NULL: empty for FTP/FTPS; no supplied password for SFTP. */
  const char* directory;        /* NULL or "": the login directory. */
  int mkdir;                    /* Create the destination if missing (one MKD). */
  int verbose;                  /* Log every FTP command and reply. */
  unsigned buffer_mib;          /* Buffer between decompression and upload; 0: 64. */
} streamextract_job_config;

/* "StreamExtract 2.5.0 (UnRAR 7.31, LZMA SDK 26.03, libarchive 3.8.9, libcurl 8.22.0)". Static
 * storage, never freed. */
STREAMEXTRACT_API const char* streamextract_version(void);

/* Copies `config` and starts the job right away. Returns NULL only if
 * `config` is NULL; any other problem ends the job as failed. The first call
 * also sets LC_CTYPE from the environment, as the streamextract CLI does. */
STREAMEXTRACT_API streamextract_job* streamextract_job_start(const streamextract_job_config* config);

/* Like streamextract_job_start, with the display units for sizes and speeds in logs,
 * summaries and JSON text fields. `si_units`: 0 = binary (1024, KiB/MiB/...),
 * nonzero = SI (1000, kB/MB/...). Numeric byte counts are always unchanged.
 * The original start function uses binary units; its config layout is unchanged. */
STREAMEXTRACT_API streamextract_job* streamextract_job_start_with_units(const streamextract_job_config* config,
                                                                        int si_units);

/* Like streamextract_job_start_with_units, with a configurable total attempt count
 * for connection/login and each file upload (including the first).
 * `retries`: 0 = default (3), 1 = no
 * retries. The older start functions keep the default and the config layout
 * is unchanged. */
STREAMEXTRACT_API streamextract_job* streamextract_job_start_with_options(const streamextract_job_config* config,
                                                                          int si_units, unsigned retries);

/* Transfer protocol. The existing start functions continue to use plain FTP. */
typedef enum streamextract_protocol {
  STREAMEXTRACT_PROTOCOL_FTP = 0,
  STREAMEXTRACT_PROTOCOL_FTPS_EXPLICIT = 1,
  STREAMEXTRACT_PROTOCOL_FTPS_IMPLICIT = 2,
  STREAMEXTRACT_PROTOCOL_SFTP = 3
} streamextract_protocol;

/* Like streamextract_job_start_with_options, with a protocol and optional PEM CA
 * certificate path (NULL: system trust). FTPS requires TLS on control and data
 * connections and verifies the certificate and host. `config->port` is used as
 * supplied (usually 21 for FTP/explicit FTPS, 990 for implicit FTPS, 22 for SFTP).
 * Strings are copied; the config layout and older entry points are unchanged.
 * An invalid protocol ends the job as failed. */
STREAMEXTRACT_API streamextract_job* streamextract_job_start_with_protocol(const streamextract_job_config* config,
                                                                           int si_units, unsigned retries,
                                                                           int protocol,
                                                                           const char* ca_certificate);

/* SSH settings for SFTP. NULL private_key: use ~/.ssh keys when no password is
 * supplied. NULL known_hosts: accept any host key; "": ~/.ssh/known_hosts.
 * NULL config->password means no password was supplied (an empty string is an
 * explicitly supplied password). NULL/empty user: current local username.
 * Only password and private key authentication are allowed; no login prompts.
 * Strings are copied. The original config layout is unchanged. */
typedef struct streamextract_ssh_options {
  const char* private_key;
  const char* private_key_passphrase;
  const char* known_hosts;
} streamextract_ssh_options;

STREAMEXTRACT_API streamextract_job* streamextract_job_start_with_connection(
    const streamextract_job_config* config, int si_units, unsigned retries, int protocol,
    const char* ca_certificate, const streamextract_ssh_options* ssh);

/* Current state as a JSON object, including the log lines numbered
 * `log_cursor` and later. Free the result with streamextract_free(). Returns NULL
 * only if `job` is NULL or memory ran out. */
STREAMEXTRACT_API char* streamextract_job_poll(streamextract_job* job, uint64_t log_cursor);

/* Answers a pending archive password prompt; NULL declines it. Ignored when
 * no prompt is pending. */
STREAMEXTRACT_API void streamextract_job_answer_password(streamextract_job* job, const char* password);

/* Asks the job to stop; returns immediately. The incomplete remote file is
 * deleted before the job finishes. */
STREAMEXTRACT_API void streamextract_job_cancel(streamextract_job* job);

/* Cancels the job if it is still running, waits for it and frees it. */
STREAMEXTRACT_API void streamextract_job_free(streamextract_job* job);

/* Frees a string returned by this library. NULL is ignored. */
STREAMEXTRACT_API void streamextract_free(char* string);

#ifdef __cplusplus
}
#endif

#endif /* STREAMEXTRACT_H */
