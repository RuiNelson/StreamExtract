#pragma once

namespace streamextract {

// Adds to 7-Zip's code the 7z methods its SDK lacks, decoded with the libraries
// libarchive uses too: Deflate (zlib), BZip2 (bzip2) and Zstandard (zstd).
// Thread-safe; only the first call does something.
void register_sevenzip_methods();

}  // namespace streamextract
