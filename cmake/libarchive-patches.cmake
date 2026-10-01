# Fixes applied to the libarchive 3.8.9 sources (run by cmake/LibArchive.cmake
# as the PATCH_COMMAND, in the source directory: cmake -P this file). Plain
# text replacements, so no patch tool is needed on any platform; each one fails
# loudly if its context is not found, e.g. after a version bump.

# Replaces `before`, which must occur exactly once in `file`, with `after`.
# Nothing to do when `after` is already there (the step ran before).
function(_replace file before after)
  file(READ "${file}" text)
  string(FIND "${text}" "${after}" done)
  if(NOT done EQUAL -1)
    return()
  endif()
  string(FIND "${text}" "${before}" first)
  string(FIND "${text}" "${before}" last REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR "libarchive patch: context not found exactly once in ${file}:\n${before}")
  endif()
  string(REPLACE "${before}" "${after}" text "${text}")
  file(WRITE "${file}" "${text}")
endfunction()

# 1. 7z, PPMd: track the input consumed across read blocks. Without it, PPMd
#    streams larger than a read block (1 MiB here) fail with "Failed to decode
#    PPMd". Backport of upstream commit a6306a2 (pull request #3340, issue
#    #3337), not in any release yet.
set(_7zip libarchive/archive_read_support_format_7zip.c)
_replace(${_7zip}
"		 * last resort to read using __archive_read_ahead.
		 */
		const uint8_t *data"
"		 * last resort to read using __archive_read_ahead.
		 */
		if (zip->pack_stream_inbytes_remaining <= 0 ||
		    zip->ppstream.stream_in >=
		    (uint64_t)zip->pack_stream_inbytes_remaining) {
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_FILE_FORMAT,
			    \"Truncated 7z file data\");
			zip->ppstream.overconsumed = 1;
			return (0);
		}
		const uint8_t *data")
_replace(${_7zip}
"	if (ret != ARCHIVE_OK && ret != ARCHIVE_EOF)
		return (ret);

	*used = o_avail_in - t_avail_in;"
"	if (ret != ARCHIVE_OK && ret != ARCHIVE_EOF)
		return (ret);

	if (zip->codec == _7Z_PPMD)
		*used = zip->ppstream.stream_in;
	else
		*used = o_avail_in - t_avail_in;")

# 2. ZIP, Zstandard: when the last call fills the output buffer exactly, the end
#    of the entry was not noticed and the next read failed with "Truncated zstd
#    file body" (e.g. a 5 MiB file). The frame is complete (zstd returned 0) and
#    the entry's data is all consumed: that is the end. Not fixed upstream yet.
set(_zip libarchive/archive_read_support_format_zip.c)
_replace(${_zip}
"	total_out = out.pos;

	zip->entry_bytes_remaining -= to_consume;
	zip->entry_compressed_bytes_read += to_consume;
	zip->entry_uncompressed_bytes_read += total_out;

	zip_read_decrypt_update(zip, to_consume, sp);

	if (zip->end_of_entry && zip->hctx_valid) {"
"	total_out = out.pos;

	zip->entry_bytes_remaining -= to_consume;
	zip->entry_compressed_bytes_read += to_consume;
	zip->entry_uncompressed_bytes_read += total_out;

	zip_read_decrypt_update(zip, to_consume, sp);

	if (ret == 0 && !zip->end_of_entry && zip->entry_bytes_remaining == 0 &&
	    0 == (zip->entry->zip_flags & ZIP_LENGTH_AT_END)) {
		zip->end_of_entry = 1;
		ZSTD_freeDStream(zip->zstdstream);
		zip->zstdstream_valid = 0;
	}

	if (zip->end_of_entry && zip->hctx_valid) {")
