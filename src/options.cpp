#include "options.hpp"

#include <cstdio>

#include <CLI/CLI.hpp>

namespace rarftp {

ParsedOptions parse_options(int argc, char** argv) {
  Options o;
  std::string mode = "passive";
  std::string user;
  std::string password;
  std::string directory;
  std::string rar_password;

  CLI::App app{"Uploads the contents of a RAR archive straight to an FTP server, without extracting it to disk.",
               "rarftp"};
  argv = app.ensure_utf8(argv);
  app.set_version_flag("--version", version_string());
  app.get_formatter()->column_width(28);

  app.add_option("--file", o.file, "RAR archive (the first volume of a multi-volume set)")
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
  CLI::Option* rar_password_option =
      app.add_option("--rar-password", rar_password, "Password of an encrypted archive; asked for when needed")
          ->type_name("PASSWORD");
  app.add_flag("--no-tui", o.no_tui, "Plain log output instead of the full-screen interface");
  app.add_flag("--verbose", o.verbose, "Log every FTP command and reply");
  app.add_option("--buffer", o.buffer_mib, "Memory buffer between decompression and upload, in MiB")
      ->check(CLI::Range(1, 4096).description(""))
      ->capture_default_str()
      ->type_name("MIB");
  app.footer(
      "Anonymous login is used when no --user is given. Files already on the server with the same size are "
      "skipped; files with a different size are overwritten.");

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
  if (rar_password_option->count() > 0) {
    o.rar_password = rar_password;
  }
  return {o, 0};
}

}  // namespace rarftp
