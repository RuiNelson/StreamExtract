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

# SFTP v3's ordinary rename cannot overwrite an existing file, even when curl
# requests OVERWRITE. Prefer OpenSSH's atomic POSIX rename extension; keep the
# ordinary command as a fallback when the server does not support the extension.
# Retain the fallback choice across libssh2's nonblocking EAGAIN calls.
set(_curl_rename_file "${curl_SOURCE_DIR}/lib/vssh/libssh2.c")
file(READ "${_curl_rename_file}" _curl_rename_source)
set(_curl_old [=[  int rc =
    libssh2_sftp_rename_ex(sshc->sftp_session, sshc->quote_path1,
                           curlx_uztoui(strlen(sshc->quote_path1)),
                           sshc->quote_path2,
                           curlx_uztoui(strlen(sshc->quote_path2)),
                           LIBSSH2_SFTP_RENAME_OVERWRITE |
                           LIBSSH2_SFTP_RENAME_ATOMIC |
                           LIBSSH2_SFTP_RENAME_NATIVE);

  if(rc == LIBSSH2_ERROR_EAGAIN)
    return CURLE_AGAIN;]=])
set(_curl_new [=[  int rc;
  if(!sshc->sftp_rename_fallback) {
    rc = libssh2_sftp_posix_rename_ex(sshc->sftp_session, sshc->quote_path1,
                                     strlen(sshc->quote_path1),
                                     sshc->quote_path2,
                                     strlen(sshc->quote_path2));
    if(rc == LIBSSH2_ERROR_EAGAIN)
      return CURLE_AGAIN;
    if(rc == (int)LIBSSH2_FX_OP_UNSUPPORTED ||
       (rc == LIBSSH2_ERROR_SFTP_PROTOCOL &&
        libssh2_sftp_last_error(sshc->sftp_session) == LIBSSH2_FX_OP_UNSUPPORTED))
      sshc->sftp_rename_fallback = TRUE;
  }
  if(sshc->sftp_rename_fallback) {
    rc = libssh2_sftp_rename_ex(sshc->sftp_session, sshc->quote_path1,
                               curlx_uztoui(strlen(sshc->quote_path1)),
                               sshc->quote_path2,
                               curlx_uztoui(strlen(sshc->quote_path2)),
                               LIBSSH2_SFTP_RENAME_OVERWRITE |
                               LIBSSH2_SFTP_RENAME_ATOMIC |
                               LIBSSH2_SFTP_RENAME_NATIVE);
  }
  if(rc == LIBSSH2_ERROR_EAGAIN)
    return CURLE_AGAIN;
  sshc->sftp_rename_fallback = FALSE;]=])
string(FIND "${_curl_rename_source}" "${_curl_old}" _curl_old_at)
string(FIND "${_curl_rename_source}" "${_curl_new}" _curl_new_at)
if(NOT _curl_old_at EQUAL -1)
  string(REPLACE "${_curl_old}" "${_curl_new}" _curl_rename_source "${_curl_rename_source}")
  file(WRITE "${_curl_rename_file}" "${_curl_rename_source}")
elseif(_curl_new_at EQUAL -1)
  message(FATAL_ERROR "curl SFTP rename source changed; review CurlPatches.cmake")
endif()
set(_curl_ssh_header "${curl_SOURCE_DIR}/lib/vssh/ssh.h")
file(READ "${_curl_ssh_header}" _curl_ssh_header_source)
if(NOT _curl_ssh_header_source MATCHES "BIT\\(sftp_rename_fallback\\)")
  string(FIND "${_curl_ssh_header_source}" "  BIT(acceptfail);" _curl_old_at)
  if(_curl_old_at EQUAL -1)
    message(FATAL_ERROR "curl SSH connection source changed; review CurlPatches.cmake")
  endif()
  string(REPLACE "  BIT(acceptfail);" "  BIT(sftp_rename_fallback);\n  BIT(acceptfail);"
    _curl_ssh_header_source "${_curl_ssh_header_source}")
  file(WRITE "${_curl_ssh_header}" "${_curl_ssh_header_source}")
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
