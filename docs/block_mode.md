# Block mode (MODE B)

Block mode is an optional setting for FTP and FTPS uploads that keeps the data
connection open between files. It can make uploading archives with many small
files faster.

## Why use it?

Normally, FTP opens a data connection for each file and closes it when that file
finishes. For an archive containing thousands of small files, repeatedly opening
connections can take a significant part of the total upload time.

With block mode, the same connection stays open for the next file, reducing the
wait between uploads. With FTPS, it also avoids setting up a new encrypted data
connection for every file.

The benefit is greatest with many small files or connections where the server
takes longer to respond. For a single large file, the difference may be small.
The files and their contents are uploaded in the same way; block mode changes
how the connection is reused.

## A compatible server is required

**The server must support FTP block mode (MODE B).** Supporting ordinary FTP or
FTPS alone is not enough. One compatible server is
[ftpsrv-streamextract](https://github.com/RuiNelson/ftpsrv-streamextract).

If the server refuses block mode, StreamExtract shows a warning in the log and
continues uploading normally. You do not need to restart the transfer.

## How to enable it

- **Desktop app**: in **Server**, select **Use block mode (MODE B)**.
  The setting is included when you save the server with **Memory Save**.
- **Command line**: add `--mode-b` to your usual `sext` command.

Block mode is off by default. It applies to FTP and FTPS; it is not used with SFTP.

