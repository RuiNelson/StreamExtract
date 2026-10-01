// C API of librarftpcore (see rarftp.h). Every function catches everything:
// no exception ever crosses the C boundary.

#include "capi/rarftp.h"

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include "app_version.hpp"
#include "capi/job.hpp"

struct rarftp_job {
  explicit rarftp_job(rarftp::JobConfig config) : job(std::move(config)) {}
  rarftp::Job job;
};

namespace {

std::string copy_string(const char* text) { return text != nullptr ? std::string(text) : std::string(); }

// malloc(), so that rarftp_free() is a plain free() whatever the allocator of
// the caller's language.
char* duplicate(const std::string& text) {
  char* copy = static_cast<char*>(std::malloc(text.size() + 1));
  if (copy != nullptr) {
    std::memcpy(copy, text.c_str(), text.size() + 1);
  }
  return copy;
}

}  // namespace

extern "C" {

const char* rarftp_version(void) {
  try {
    static const std::string version = rarftp::version_string();
    return version.c_str();
  } catch (...) {
    return "rarftp";
  }
}

rarftp_job* rarftp_job_start(const rarftp_job_config* config) {
  if (config == nullptr) {
    return nullptr;
  }
  try {
    static std::once_flag locale_once;
    std::call_once(locale_once, [] { std::setlocale(LC_CTYPE, ""); });  // UnRAR converts some names with it.

    rarftp::JobConfig copy;
    copy.archive = copy_string(config->archive);
    if (config->rar_password != nullptr) {
      copy.rar_password = std::string(config->rar_password);
    }
    copy.host = copy_string(config->host);
    copy.port = config->port;
    copy.active_mode = config->active_mode != 0;
    copy.user = copy_string(config->user);
    copy.password = copy_string(config->password);
    copy.directory = copy_string(config->directory);
    copy.mkdir = config->mkdir != 0;
    copy.verbose = config->verbose != 0;
    copy.buffer_mib = config->buffer_mib;
    return new rarftp_job(std::move(copy));
  } catch (...) {
    return nullptr;
  }
}

char* rarftp_job_poll(rarftp_job* job, uint64_t log_cursor) {
  if (job == nullptr) {
    return nullptr;
  }
  try {
    return duplicate(job->job.poll(log_cursor));
  } catch (...) {
    return nullptr;
  }
}

void rarftp_job_answer_password(rarftp_job* job, const char* password) {
  if (job == nullptr) {
    return;
  }
  try {
    std::optional<std::string> answer;
    if (password != nullptr) {
      answer = std::string(password);
    }
    job->job.answer_password(std::move(answer));
  } catch (...) {
  }
}

void rarftp_job_cancel(rarftp_job* job) {
  if (job == nullptr) {
    return;
  }
  try {
    job->job.cancel();
  } catch (...) {
  }
}

void rarftp_job_free(rarftp_job* job) {
  try {
    delete job;
  } catch (...) {
  }
}

void rarftp_free(char* string) { std::free(string); }

}  // extern "C"
