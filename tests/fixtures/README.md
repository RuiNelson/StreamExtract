# exFAT fixtures

`exfat.zip` contains real disk images created by macOS `hdiutil`, compressed
only to keep the repository small. Tests extract them with CMake or Python's
`zipfile`. They do not construct exFAT structures, and reading them requires
no mount or formatter on the test machine.

The commands used to create the images were:

```bash
hdiutil create -size 32m -srcfolder /tmp/rarftp-exfat-fixture-tree \
  -fs ExFAT -fsargs '-b 4096' -layout NONE -format UDRW \
  -volname RARFTP_TEST /tmp/rarftp-fixture-512.dmg

hdiutil create -size 32m -srcfolder /tmp/rarftp-exfat-fixture-tree \
  -fs ExFAT -layout MBRSPUD -format UDRW \
  -volname RARFTP_TEST /tmp/rarftp-fixture-disk.dmg
```

The images are stored as `volume.exfat` and `partitioned.exfat`. The first
has no partition table; the second is a disk image with an MBR and must be
rejected, even though it contains only one exFAT partition.

The source directory contained `hello.txt` (`hello\n`), `empty.txt`,
`empty-dir/`, `large.bin` (bytes 0..255 repeated 4608 times, then `end`), and
`sub/` with a Unicode file (`ação 日本 😀.txt`, `unicode\n`), a combining-name
file (`e` + U+0301 + `.txt`, `combining\n`), and an 80-character Japanese file
name (`日` repeated 80 times + `.txt`, `long name\n`). hdiutil stores these
names in NFC. User file mtimes were set to Unix second 1700000000.

The first image was then mounted normally to add `fragmented.bin` (bytes
0..255 repeated 20480 times, then `fragmented end`). To force fragmentation,
ten 2 MiB blocker files and a filler occupied the free space; alternating
blockers were deleted before writing it, and all remaining blockers/filler
were deleted afterwards. `uninitialized.bin` was created by writing `written`
and extending it to 17000 bytes using the filesystem's truncate operation.
Both files also have mtime 1700000000. All writes used normal filesystem
operations on the mounted image.

The macOS formatter/copier also created AppleDouble metadata files, which
remain in the fixture and are uploaded like ordinary files. `exfat.json`
contains their expected sizes and SHA-256 hashes, read from the mounted
image. macOS presents the root metadata name as `._.`, while its stored name
is `._` + U+F029; the manifest uses the stored name that FatFs returns.
