/* C API of librarftpcore, the shared library used by rarftp-gui.
 *
 * A job runs a whole upload (read the archive, log in, check what is already
 * on the server, transfer) on its own threads. The caller polls its state as
 * JSON and answers the archive password prompt when one is pending. Every
 * string is UTF-8. Every function is thread-safe for a given job.
 */

#ifndef RARFTP_H
#define RARFTP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#ifdef RARFTP_BUILDING_LIBRARY
#define RARFTP_API __declspec(dllexport)
#else
#define RARFTP_API __declspec(dllimport)
#endif
#else
#define RARFTP_API __attribute__((visibility("default")))
#endif

typedef struct rarftp_job rarftp_job;

typedef struct rarftp_job_config {
  const char* archive;          /* RAR, ZIP, 7z or tar archive (first volume), or a single exFAT volume image. */
  const char* archive_password; /* NULL: asked through the job when needed. */
  const char* host;             /* Host name or address, without ftp:// or a path. */
  int port;                     /* 1-65535. */
  int active_mode;              /* 0: passive, 1: active. */
  const char* user;             /* NULL or "": anonymous login. */
  const char* password;         /* NULL: empty. */
  const char* directory;        /* NULL or "": the login directory. */
  int mkdir;                    /* Create the destination if missing (one MKD). */
  int verbose;                  /* Log every FTP command and reply. */
  unsigned buffer_mib;          /* Buffer between decompression and upload; 0: 64. */
} rarftp_job_config;

/* "rarftp 2.3.1 (UnRAR 7.31, LZMA SDK 26.03, libarchive 3.8.9, libcurl 8.22.0)". Static
 * storage, never freed. */
RARFTP_API const char* rarftp_version(void);

/* Copies `config` and starts the job right away. Returns NULL only if
 * `config` is NULL; any other problem ends the job as failed. The first call
 * also sets LC_CTYPE from the environment, as the rarftp CLI does. */
RARFTP_API rarftp_job* rarftp_job_start(const rarftp_job_config* config);

/* Like rarftp_job_start, with the display units for sizes and speeds in logs,
 * summaries and JSON text fields. `si_units`: 0 = binary (1024, KiB/MiB/...),
 * nonzero = SI (1000, kB/MB/...). Numeric byte counts are always unchanged.
 * The original start function uses binary units; its config layout is unchanged. */
RARFTP_API rarftp_job* rarftp_job_start_with_units(const rarftp_job_config* config, int si_units);

/* Like rarftp_job_start_with_units, with a configurable total attempt count
 * for connection/login and each file upload (including the first).
 * `retries`: 0 = default (3), 1 = no
 * retries. The older start functions keep the default and the config layout
 * is unchanged. */
RARFTP_API rarftp_job* rarftp_job_start_with_options(const rarftp_job_config* config, int si_units,
                                                  unsigned retries);

/* Current state as a JSON object, including the log lines numbered
 * `log_cursor` and later. Free the result with rarftp_free(). Returns NULL
 * only if `job` is NULL or memory ran out. */
RARFTP_API char* rarftp_job_poll(rarftp_job* job, uint64_t log_cursor);

/* Answers a pending archive password prompt; NULL declines it. Ignored when
 * no prompt is pending. */
RARFTP_API void rarftp_job_answer_password(rarftp_job* job, const char* password);

/* Asks the job to stop; returns immediately. The incomplete remote file is
 * deleted before the job finishes. */
RARFTP_API void rarftp_job_cancel(rarftp_job* job);

/* Cancels the job if it is still running, waits for it and frees it. */
RARFTP_API void rarftp_job_free(rarftp_job* job);

/* Frees a string returned by this library. NULL is ignored. */
RARFTP_API void rarftp_free(char* string);

#ifdef __cplusplus
}
#endif

#endif /* RARFTP_H */
