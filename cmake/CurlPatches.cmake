# curl 8.22.0 reads the SSH passphrase from the TLS configuration, which is
# empty for SFTP's origin. Read CURLOPT_KEYPASSWD directly instead. Upstream
# also reads STRING_KEY_PASSWD in lib/vssh/vssh.c after 8.22.0:
# https://github.com/curl/curl/blob/master/lib/vssh/vssh.c
# Remove this patch when updating to a release with that fix.
set(_curl_ssh_file "${curl_SOURCE_DIR}/lib/vssh/vssh.c")
file(READ "${_curl_ssh_file}" _curl_ssh_source)
set(_curl_old "sshc->passphrase = data->set.ssl.primary.key_passwd;")
set(_curl_new "sshc->passphrase = CURL_EASY_STR(data, STRING_KEY_PASSWD);")
string(FIND "${_curl_ssh_source}" "${_curl_old}" _curl_old_at)
string(FIND "${_curl_ssh_source}" "${_curl_new}" _curl_new_at)
if(NOT _curl_old_at EQUAL -1)
  string(REPLACE "${_curl_old}" "${_curl_new}" _curl_ssh_source "${_curl_ssh_source}")
  file(WRITE "${_curl_ssh_file}" "${_curl_ssh_source}")
elseif(_curl_new_at EQUAL -1)
  message(FATAL_ERROR "curl SSH passphrase source changed; review CurlPatches.cmake")
endif()

# SFTP stat explicitly reports zero-byte files, but curl 8.22.0 treats zero as
# an unknown size. Preserve the SIZE attribute so equal-size empty files skip.
set(_curl_stat_file "${curl_SOURCE_DIR}/lib/vssh/libssh2.c")
file(READ "${_curl_stat_file}" _curl_stat_source)
set(_curl_old "!(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ||\n     (attrs.filesize == 0)")
set(_curl_new "!(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)")
string(FIND "${_curl_stat_source}" "${_curl_old}" _curl_old_at)
if(NOT _curl_old_at EQUAL -1)
  string(REPLACE "${_curl_old}" "${_curl_new}" _curl_stat_source "${_curl_stat_source}")
  string(REPLACE "     * OR file size is 0\n" "" _curl_stat_source "${_curl_stat_source}")
  file(WRITE "${_curl_stat_file}" "${_curl_stat_source}")
else()
  string(FIND "${_curl_stat_source}" "if(rc ||\n     !(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))" _curl_new_at)
  if(_curl_new_at EQUAL -1)
    message(FATAL_ERROR "curl SFTP size source changed; review CurlPatches.cmake")
  endif()
endif()
