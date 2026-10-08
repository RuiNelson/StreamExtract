// C API of libstreamextractcore (see streamextract.h). Every function catches everything:
// no exception ever crosses the C boundary.

#include "capi/streamextract.h"

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include "app_version.hpp"
#include "capi/job.hpp"
#include "logger.hpp"
#include "plan.hpp"
#include "util/json.hpp"

struct streamextract_job {
  explicit streamextract_job(streamextract::JobConfig config) : job(std::move(config)) {}
  streamextract::Job job;
};

namespace {

std::string copy_string(const char* text) { return text != nullptr ? std::string(text) : std::string(); }

void initialize_locale() {
  static std::once_flag locale_once;
  std::call_once(locale_once, [] { std::setlocale(LC_CTYPE, ""); });
}

// malloc(), so that streamextract_free() is a plain free() whatever the allocator of
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

const char* streamextract_version(void) {
  try {
    static const std::string version = streamextract::version_string();
    return version.c_str();
  } catch (...) {
    return "StreamExtract";
  }
}

streamextract_job* streamextract_job_start(const streamextract_job_config* config) {
  return streamextract_job_start_with_units(config, 0);
}

streamextract_job* streamextract_job_start_with_units(const streamextract_job_config* config, int si_units) {
  return streamextract_job_start_with_options(config, si_units, 0);
}

streamextract_job* streamextract_job_start_with_options(const streamextract_job_config* config, int si_units,
                                                        unsigned retries) {
  return streamextract_job_start_with_protocol(config, si_units, retries, STREAMEXTRACT_PROTOCOL_FTP, nullptr);
}

streamextract_job* streamextract_job_start_with_protocol(const streamextract_job_config* config, int si_units,
                                                         unsigned retries, int protocol,
                                                         const char* ca_certificate) {
  return streamextract_job_start_with_connection(config, si_units, retries, protocol, ca_certificate, nullptr);
}

streamextract_job* streamextract_job_start_with_connection(const streamextract_job_config* config, int si_units,
                                                           unsigned retries, int protocol,
                                                           const char* ca_certificate,
                                                           const streamextract_ssh_options* ssh) {
  return streamextract_job_start_with_extraction_root(config, si_units, retries, protocol, ca_certificate, ssh,
                                                      nullptr);
}

streamextract_job* streamextract_job_start_with_extraction_root(const streamextract_job_config* config,
                                                                int si_units, unsigned retries, int protocol,
                                                                const char* ca_certificate,
                                                                const streamextract_ssh_options* ssh,
                                                                const char* extraction_root) {
  return streamextract_job_start_with_staging(config, si_units, retries, protocol, ca_certificate, ssh,
                                            extraction_root, nullptr);
}

streamextract_job* streamextract_job_start_with_staging(
    const streamextract_job_config* config, int si_units, unsigned retries, int protocol,
    const char* ca_certificate, const streamextract_ssh_options* ssh, const char* extraction_root,
    const char* staging) {
  if (config == nullptr) {
    return nullptr;
  }
  try {
    initialize_locale();  // UnRAR converts some names with it.

    streamextract::JobConfig copy;
    copy.archive = copy_string(config->archive);
    copy.extraction_root = copy_string(extraction_root);
    copy.staging = copy_string(staging);
    if (config->archive_password != nullptr) {
      copy.archive_password = std::string(config->archive_password);
    }
    copy.host = copy_string(config->host);
    copy.port = config->port;
    copy.protocol = protocol;
    if (ca_certificate != nullptr) {
      copy.ca_certificate = std::string(ca_certificate);
    }
    copy.active_mode = config->active_mode != 0;
    copy.user = copy_string(config->user);
    copy.password = copy_string(config->password);
    copy.password_supplied = config->password != nullptr;
    if (ssh) {
      if (ssh->private_key) copy.private_key = std::string(ssh->private_key);
      copy.private_key_passphrase = copy_string(ssh->private_key_passphrase);
      if (ssh->known_hosts) copy.known_hosts = std::string(ssh->known_hosts);
    }
    copy.directory = copy_string(config->directory);
    copy.mkdir = config->mkdir != 0;
    copy.verbose = config->verbose != 0;
    copy.buffer_mib = config->buffer_mib;
    copy.retries = retries == 0 ? 3 : retries;
    copy.units = si_units != 0 ? streamextract::ByteUnits::Si : streamextract::ByteUnits::Binary;
    return new streamextract_job(std::move(copy));
  } catch (...) {
    return nullptr;
  }
}

char* streamextract_archive_directories(const char* archive, const char* password) {
  try {
    initialize_locale();
    std::vector<std::string> directories;
    bool password_required = false;
    std::optional<std::string> error;
    try {
      streamextract::Logger log;
      streamextract::PasswordSource passwords(password ? std::optional<std::string>(password) : std::nullopt, {});
      const auto listing = streamextract::list_archive(copy_string(archive), passwords, log, true);
      directories = streamextract::archive_directories(listing);
    } catch (const streamextract::ArchivePasswordRequired&) {
      password_required = true;
    } catch (const streamextract::ArchivePasswordError&) {
      password_required = true;
      error = "Wrong password";
    } catch (const std::exception& exception) {
      error = exception.what();
    }
    streamextract::JsonWriter json;
    json.begin_object();
    json.key("directories");
    json.begin_array();
    for (const auto& directory : directories) json.value(directory);
    json.end_array();
    json.member("password_required", password_required);
    json.key("error");
    if (error)
      json.value(*error);
    else
      json.null();
    json.end_object();
    return duplicate(json.str());
  } catch (...) {
    return nullptr;
  }
}

char* streamextract_job_poll(streamextract_job* job, uint64_t log_cursor) {
  if (job == nullptr) {
    return nullptr;
  }
  try {
    return duplicate(job->job.poll(log_cursor));
  } catch (...) {
    return nullptr;
  }
}

void streamextract_job_answer_password(streamextract_job* job, const char* password) {
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

void streamextract_job_cancel(streamextract_job* job) {
  if (job == nullptr) {
    return;
  }
  try {
    job->job.cancel();
  } catch (...) {
  }
}

void streamextract_job_free(streamextract_job* job) {
  try {
    delete job;
  } catch (...) {
  }
}

void streamextract_free(char* string) { std::free(string); }

}  // extern "C"
