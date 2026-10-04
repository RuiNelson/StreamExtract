#include "options.hpp"

#include <cstdio>

#include <CLI/CLI.hpp>

namespace streamextract {

ParsedOptions parse_options(int argc, char** argv) {
  Options o;
  std::string mode = "passive";
  std::string user;
  std::string password;
  std::string directory;
  std::string archive_password;

  CLI::App app{
      "Uploads the contents of a RAR, ZIP, 7z or tar archive, or an exFAT volume image, straight to an FTP "
      "server, without extracting it "
      "to disk.",
      "sext"};
  argv = app.ensure_utf8(argv);
  app.set_version_flag("--version", version_string());
  app.get_formatter()->column_width(28);

  app.add_option("--file", o.file,
                 "RAR, ZIP, 7z or tar archive (first volume of a set), or a single exFAT volume image")
      ->required()
      ->check(CLI::ExistingFile.description(""))
      ->type_name("PATH");
  app.add_option("--host", o.host, "FTP server name or IP address")->required()->type_name("HOST");
  app.add_option("--port", o.port, "FTP server port")
      ->check(CLI::Range(1, 65535).description(""))
      ->capture_default_str()
      ->type_name("PORT");
  app.add_option("--mode", mode, "Data connection mode")
      ->check(CLI::IsMember({"passive", "active"}, CLI::ignore_case).description(""))
      ->capture_default_str()
      ->type_name("passive|active");
  CLI::Option* user_option =
      app.add_option("--user", user, "FTP user name; anonymous login if omitted")->type_name("NAME");
  CLI::Option* password_option =
      app.add_option("--password", password, "FTP password; asked for when --user is given without it")
          ->needs(user_option)
          ->type_name("PASSWORD");
  CLI::Option* directory_option =
      app.add_option("--directory", directory, "Remote destination directory (default: the login directory)")
          ->type_name("DIR");
  app.add_flag("--mkdir", o.mkdir, "Create the destination directory if missing (one MKD, not recursive)");
  CLI::Option* archive_password_option =
      app.add_option("--archive-password", archive_password,
                     "Password of an encrypted archive; asked for when needed")
          ->type_name("PASSWORD");
  // Former name, kept as an alias (hidden from the help).
  CLI::Option* rar_password_option = app.add_option("--rar-password", archive_password)->group("");
  app.add_flag("--no-tui", o.no_tui, "Plain log output instead of the full-screen interface");
  app.add_flag("--verbose", o.verbose, "Log every FTP command and reply");
  app.add_option("--retries", o.retries,
                 "Total attempts for connection/login and each file upload, including the first; 1 disables retries")
      ->check(CLI::PositiveNumber.description(""))
      ->capture_default_str()
      ->type_name("N");
  app.add_option("--buffer", o.buffer_mib, "Memory buffer between decompression and upload, in MiB")
      ->check(CLI::Range(1, 4096).description(""))
      ->capture_default_str()
      ->type_name("MIB");
  app.footer(
      "Anonymous login is used when no --user is given. Equal-size remote files are skipped; smaller files "
      "are resumed; larger files are deleted before uploading from the beginning.");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    const int code = app.exit(error);
    return {std::nullopt, code == 0 ? 0 : 2};
  }

  if (o.host.find_first_of("/ \t") != std::string::npos || o.host.empty()) {
    std::fprintf(stderr, "--host: expected a host name or address, without ftp:// or a path\n");
    return {std::nullopt, 2};
  }
  o.mode = CLI::detail::to_lower(mode) == "active" ? FtpMode::Active : FtpMode::Passive;
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
