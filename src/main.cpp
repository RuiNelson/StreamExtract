// streamextract: uploads the contents of a RAR, ZIP, 7z or tar archive to an FTP server
// without extracting it to disk.

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include <fmt/format.h>
#ifdef _WIN32
#include <CLI/CLI.hpp>
#endif

#include "ftp_client.hpp"
#include "logger.hpp"
#include "options.hpp"
#include "plan.hpp"
#include "progress.hpp"
#include "summary.hpp"
#include "transfer.hpp"
#include "ui.hpp"
#include "util/remote_path.hpp"
#include "util/ssh_settings.hpp"
#include "util/terminal.hpp"
#include "util/text.hpp"

namespace streamextract {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitCancelled = 130;

std::string file_name_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void print_line(const LogLine& line) {
  std::fprintf(stdout, "%s\n", format_log_line(line).c_str());
  std::fflush(stdout);
}

void print_summary(const TransferResult& result, const Logger& log, bool show_problems) {
  std::string text;
  for (const std::string& line : summary_lines(result)) {
    text += line + "\n";
  }

  const auto problems = log.problems();
  if (show_problems && !problems.empty()) {
    text += "\nWarnings and errors:\n";
    for (const LogLine& line : problems) {
      text += "  " + format_log_line(line) + "\n";
    }
    if (log.problems_dropped() > 0) {
      text += fmt::format("  ... and {} more\n", log.problems_dropped());
    }
  }
  std::fputs(text.c_str(), stdout);
  std::fflush(stdout);
}

int run(const Options& options) {
  Logger log;
  log.set_verbose(options.verbose);
  log.set_sink(print_line);

  const bool interactive = stdin_is_terminal();
  const bool use_tui = !options.no_tui && interactive && stdout_is_terminal();

  FtpConfig ftp_config;
  ftp_config.host = options.host;
  ftp_config.port = options.port;
  ftp_config.mode = options.mode;
  ftp_config.protocol = options.protocol;
  ftp_config.ftps_mode = options.ftps_mode;
  ftp_config.ca_certificate = options.ca_certificate;
  ftp_config.user = options.user.value_or("");
  ftp_config.password = options.password.value_or("");
  ftp_config.password_supplied = options.password.has_value();
  ftp_config.private_key = options.private_key;
  ftp_config.private_key_passphrase = options.private_key_passphrase;
  ftp_config.known_hosts = options.known_hosts;
  if (!is_ssh(options.protocol) && options.user && !options.password) {
    if (!interactive) {
      log.error("--password is required when standard input is not a terminal");
      return kExitUsage;
    }
    const auto password = prompt_hidden(fmt::format("FTP password for {}@{}: ", *options.user, options.host));
    if (!password) {
      log.error("no FTP password entered");
      return kExitUsage;
    }
    ftp_config.password = *password;
  }

  // 1. Archive: list every volume first, so a missing volume or a wrong
  //    password fails before anything is sent.
  PasswordSource::Prompt prompt;
  if (interactive) {
    prompt = [&] { return prompt_hidden(fmt::format("Password for {}: ", file_name_of(options.file))); };
  }
  PasswordSource passwords(options.archive_password, std::move(prompt));
  ArchiveListing listing;
  try {
    log.info("Reading {}", options.file);
    listing = list_archive(options.file, passwords, log);
    validate_extraction_root(listing, options.extraction_root);
  } catch (const std::exception& error) {
    log.error(error.what());
    return kExitError;
  }
  const bool encrypted =
      listing.flags.encrypted_headers || std::any_of(listing.entries.begin(), listing.entries.end(),
                                                     [](const ArchiveEntry& e) { return e.encrypted; });
  if (encrypted && !passwords.has_password() && !passwords.get()) {
    log.error("the archive is encrypted: pass --archive-password");
    return kExitError;
  }
  passwords.disable_prompt();

  log.info("Archive: {}", describe_archive(listing, encrypted));
  if (!options.extraction_root.empty()) log.info("Extraction root: {}", options.extraction_root);

  // 2. FTP: log in and check the destination directory.
  FtpClient ftp(ftp_config, log, options.verbose);
  std::string target;
  try {
    if (is_ssh(options.protocol))
      log.info("Connecting to {}:{} (SFTP)", options.host, options.port);
    else
      log.info("Connecting to {}:{} ({} mode)", options.host, options.port,
               options.mode == FtpMode::Active ? "active" : "passive");
    const std::string home = normalize_remote_path(ftp.connect(options.retries));
    log.info("Logged in as {}",
             options.user.value_or(is_ssh(options.protocol) ? current_username() : "anonymous"));
    if (!options.directory) {
      target = home;
      log.warn("no --directory given: uploading to the server's default directory {}", target);
    } else {
      target = resolve_remote_path(home, *options.directory);
    }
    if (!ftp.directory_exists(target)) {
      if (!options.mkdir) {
        log.error("the remote directory {} does not exist (use --mkdir to create it)", target);
        return kExitError;
      }
      log.info("Creating the remote directory {}", target);
      ftp.make_directory(target);
      if (!ftp.directory_exists(target)) {
        log.error("the remote directory {} is not accessible after creating it", target);
        return kExitError;
      }
    }
  } catch (const FtpError& error) {
    log.error(error.what());
    return kExitError;
  }
  log.info("Destination: {}", ftp.url_for(target));

  // 3. Plan: map entries to remote paths, skip what is already there (for a
  //    streamed archive, the transfer does it file by file).
  TransferPlan plan = build_plan(options.file, listing, target, log, options.extraction_root);
  if (!plan.streamed && plan.upload_files > 0) {
    const bool live = stdout_is_terminal();
    size_t last_shown = 0;
    try {
      probe_remote(plan, ftp, log, [&](size_t done, size_t total) {
        if (live && (done == total || done - last_shown >= std::max<size_t>(1, total / 200))) {
          last_shown = done;
          std::fprintf(stdout, "\rChecking the files already on the server: %zu/%zu", done, total);
          if (done == total) {
            std::fputs("\n", stdout);
          }
          std::fflush(stdout);
        }
      });
    } catch (const FtpError& error) {
      if (live) {
        std::fputs("\n", stdout);
      }
      log.error(error.what());
      return kExitError;
    }
  }
  if (plan.skip_files > 0) {
    log.info("{} file(s), {} already on the server with the same size: skipping them", plan.skip_files,
             format_bytes(plan.skip_bytes));
  }
  if (!plan.streamed) {
    log.info("To upload: {} file(s), {}", plan.upload_files, format_bytes(plan.upload_bytes));
  }

  // 4. Transfer.
  Progress progress;
  Transfer transfer(plan, ftp, passwords, log, progress, options.buffer_mib << 20, options.retries);
  install_interrupt_handler();
  if (use_tui) {
    log.set_sink(nullptr);  // The dashboard shows the log.
  }
  transfer.start();
  if (use_tui) {
    UiHeader header;
    header.archive = file_name_of(options.file);
    header.target = ftp.url_for(target);
    header.mode = options.mode == FtpMode::Active ? "active" : "passive";
    header.user = options.user ? *options.user : "anonymous";
    run_tui(transfer, progress, log, header);
  } else {
    run_plain(transfer, progress);
  }
  const TransferResult result = transfer.wait();
  log.set_sink(print_line);

  print_summary(result, log, use_tui);
  switch (result.status) {
    case TransferResult::Status::Success:
      return kExitOk;
    case TransferResult::Status::Cancelled:
      return kExitCancelled;
    case TransferResult::Status::Failed:
      break;
  }
  return kExitError;
}

}  // namespace

}  // namespace streamextract

int main(int argc, char** argv) {
  std::setlocale(LC_CTYPE, "");  // UnRAR converts some names with the locale.
  streamextract::setup_console();
#ifdef _WIN32
  // Read the native command line here; parse_options also accepts synthetic UTF-8 argv.
  CLI::App utf8_arguments;
  argv = utf8_arguments.ensure_utf8(argv);
#endif
  const streamextract::ParsedOptions parsed = streamextract::parse_options(argc, argv);
  if (!parsed.options) {
    return parsed.exit_code;
  }
  try {
    return streamextract::run(*parsed.options);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "StreamExtract: %s\n", error.what());
    return 1;
  }
}
