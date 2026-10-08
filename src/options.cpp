#include "options.hpp"

#include <cstdio>

#include <CLI/CLI.hpp>

namespace streamextract {

ParsedOptions parse_options(int argc, char** argv) {
  Options o;
  std::string mode = "passive";
  std::string protocol = "ftp";
  std::string ftps_mode = "explicit";
  std::string ca_certificate;
  std::string user;
  std::string password;
  std::string private_key;
  std::string known_hosts;
  std::string directory;
  std::string staging;
  std::string archive_password;

  CLI::App app{
      "Uploads the contents of a RAR, ZIP, 7z or tar archive, or an exFAT, PFS or UFS volume image, straight to "
      "an FTP, "
      "FTPS or SFTP server, without extracting it "
      "to disk.",
      "sext"};
  app.set_version_flag("--version", version_string());
  app.get_formatter()->column_width(28);

  app.add_option("--file", o.file,
                 "RAR, ZIP, 7z or tar archive (first volume of a set), or a single exFAT, PFS or UFS volume image")
      ->required()
      ->check(CLI::ExistingFile.description(""))
      ->type_name("PATH");
  app.add_option("--host", o.host, "Server name or IP address")->required()->type_name("HOST");
  app.add_option("--protocol", protocol, "Transfer protocol")
      ->check(CLI::IsMember({"ftp", "ftps", "sftp"}, CLI::ignore_case).description(""))
      ->capture_default_str()
      ->type_name("ftp|ftps|sftp");
  CLI::Option* ftps_mode_option =
      app.add_option("--ftps-mode", ftps_mode, "FTPS connection mode")
          ->check(CLI::IsMember({"explicit", "implicit"}, CLI::ignore_case).description(""))
          ->capture_default_str()
          ->type_name("explicit|implicit");
  CLI::Option* ca_option = app.add_option("--cacert", ca_certificate, "CA certificate file for FTPS (PEM)")
                               ->check(CLI::ExistingFile.description(""))
                               ->type_name("PATH");
  CLI::Option* port_option =
      app.add_option("--port", o.port, "Server port (FTP/FTPS: 21; implicit FTPS: 990; SSH: 22)")
          ->check(CLI::Range(1, 65535).description(""))
          ->type_name("PORT");
  app.add_option("--mode", mode, "Data connection mode")
      ->check(CLI::IsMember({"passive", "active"}, CLI::ignore_case).description(""))
      ->capture_default_str()
      ->type_name("passive|active");
  CLI::Option* user_option =
      app.add_option("--user", user, "User name (FTP: anonymous; SSH: current local user)")->type_name("NAME");
  CLI::Option* password_option =
      app.add_option("--password", password, "Login password (SSH never prompts)")->type_name("PASSWORD");
  CLI::Option* key_option =
      app.add_option("--private-key", private_key, "SSH private key (default: keys in ~/.ssh)")
          ->check(CLI::ExistingFile.description(""))
          ->type_name("PATH");
  CLI::Option* passphrase_option =
      app.add_option("--private-key-passphrase,--private-key-passphare", o.private_key_passphrase,
                     "Passphrase for encrypted SSH private keys; never prompted")
          ->type_name("PASSPHRASE");
  CLI::Option* known_hosts_option =
      app.add_option("--known-hosts", known_hosts,
                     "Verify SSH host keys against PATH; empty: ~/.ssh/known_hosts; omitted: accept any")
          ->type_name("PATH");
  CLI::Option* directory_option =
      app.add_option("--directory", directory, "Remote destination directory (default: the login directory)")
          ->type_name("DIR");
  app.add_flag("--mkdir", o.mkdir,
               "Create destination and staging directories if missing (one MKD each, not recursive)");
  CLI::Option* staging_option =
      app.add_option("--staging", staging,
                     "Upload and resume in this remote directory, then move files to the destination on success")
          ->type_name("DIR");
  app.add_option("--extraction-root", o.extraction_root,
                 "Upload only this archive directory's contents, without its path prefix. Relative to the archive "
                 "root, never starting with '/' (e.g. b/c); empty or omitted: the archive root")
      ->type_name("DIR");
  CLI::Option* archive_password_option = app.add_option("--archive-password", archive_password,
                                                        "Password of an encrypted archive; asked for when needed")
                                             ->type_name("PASSWORD");
  // Former name, kept as an alias (hidden from the help).
  CLI::Option* rar_password_option = app.add_option("--rar-password", archive_password)->group("");
  app.add_flag("--no-tui", o.no_tui, "Plain log output instead of the full-screen interface");
  app.add_flag("--verbose", o.verbose, "Log connection and protocol details");
  app.add_option(
         "--retries", o.retries,
         "Total attempts for connection/login and each file upload, including the first; 1 disables retries")
      ->check(CLI::PositiveNumber.description(""))
      ->capture_default_str()
      ->type_name("N");
  app.add_option("--buffer", o.buffer_mib, "Memory buffer between decompression and upload, in MiB")
      ->check(CLI::Range(1, 4096).description(""))
      ->capture_default_str()
      ->type_name("MIB");
  app.footer(
      "Without --user, FTP/FTPS uses anonymous login and SFTP uses the current local username. "
      "Equal-size remote files are skipped; smaller files "
      "are resumed; larger files are deleted before uploading from the beginning.");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    const int code = app.exit(error);
    return {std::nullopt, code == 0 ? 0 : 2};
  }

  if (o.host.find_first_of("/ \t") != std::string::npos || o.host.empty()) {
    std::fprintf(stderr, "--host: expected a host name or address, without a URL scheme or a path\n");
    return {std::nullopt, 2};
  }
  o.mode = CLI::detail::to_lower(mode) == "active" ? FtpMode::Active : FtpMode::Passive;
  const auto selected_protocol = CLI::detail::to_lower(protocol);
  o.protocol = selected_protocol == "sftp"   ? FtpProtocol::Sftp
               : selected_protocol == "ftps" ? FtpProtocol::Ftps
                                             : FtpProtocol::Ftp;
  o.ftps_mode = CLI::detail::to_lower(ftps_mode) == "implicit" ? FtpsMode::Implicit : FtpsMode::Explicit;
  if (o.protocol != FtpProtocol::Ftps && (ftps_mode_option->count() > 0 || ca_option->count() > 0)) {
    std::fprintf(stderr, "--ftps-mode and --cacert require --protocol ftps\n");
    return {std::nullopt, 2};
  }
  if (!is_ssh(o.protocol) && (key_option->count() || passphrase_option->count() || known_hosts_option->count())) {
    std::fprintf(stderr, "SSH key and known-hosts options require --protocol sftp\n");
    return {std::nullopt, 2};
  }
  if (!is_ssh(o.protocol) && password_option->count() && !user_option->count()) {
    std::fprintf(stderr, "--password requires --user for FTP and FTPS\n");
    return {std::nullopt, 2};
  }
  if (is_ssh(o.protocol) && port_option->count() == 0) o.port = 22;
  if (key_option->count()) o.private_key = private_key;
  if (known_hosts_option->count()) o.known_hosts = known_hosts;
  if (o.protocol == FtpProtocol::Ftps && o.ftps_mode == FtpsMode::Implicit && port_option->count() == 0) {
    o.port = 990;
  }
  if (ca_option->count() > 0) {
    o.ca_certificate = ca_certificate;
  }
  if (user_option->count() > 0 && !user.empty()) {
    o.user = user;
  }
  if (password_option->count() > 0) {
    o.password = password;
  }
  if (directory_option->count() > 0) {
    if (directory.empty()) {
      std::fprintf(stderr, "--directory: empty path\n");
      return {std::nullopt, 2};
    }
    o.directory = directory;
  }
  if (staging_option->count() > 0) {
    if (staging.empty()) {
      std::fprintf(stderr, "--staging: empty path\n");
      return {std::nullopt, 2};
    }
    o.staging = staging;
  }
  if (archive_password_option->count() > 0 && rar_password_option->count() > 0) {
    std::fprintf(stderr, "--rar-password is another name for --archive-password: pass only one\n");
    return {std::nullopt, 2};
  }
  if (archive_password_option->count() > 0 || rar_password_option->count() > 0) {
    o.archive_password = archive_password;
  }
  return {o, 0};
}

}  // namespace streamextract
