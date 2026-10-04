#include "app_version.hpp"

#include <fmt/format.h>

#include "ftp_client.hpp"
#include "libarchive_reader.hpp"
#include "rar_archive.hpp"
#include "sevenzip_archive.hpp"

#ifndef STREAMEXTRACT_VERSION
#define STREAMEXTRACT_VERSION "0.0.0"
#endif

namespace streamextract {

std::string version_string() {
  return fmt::format("StreamExtract {} (UnRAR {}, LZMA SDK {}, FatFs R0.16, libarchive {}, libcurl {})", STREAMEXTRACT_VERSION,
                     unrar_version(), lzma_sdk_version(), libarchive_version(), curl_version_string());
}

}  // namespace streamextract
