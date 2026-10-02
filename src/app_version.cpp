#include "app_version.hpp"

#include <fmt/format.h>

#include "ftp_client.hpp"
#include "libarchive_reader.hpp"
#include "rar_archive.hpp"
#include "sevenzip_archive.hpp"

#ifndef RARFTP_VERSION
#define RARFTP_VERSION "0.0.0"
#endif

namespace rarftp {

std::string version_string() {
  return fmt::format("rarftp {} (UnRAR {}, LZMA SDK {}, FatFs R0.16, libarchive {}, libcurl {})", RARFTP_VERSION,
                     unrar_version(), lzma_sdk_version(), libarchive_version(), curl_version_string());
}

}  // namespace rarftp
