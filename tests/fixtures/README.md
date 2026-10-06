# Filesystem fixtures

## exFAT

`exfat.zip` contains real disk images created by macOS `hdiutil`, compressed
only to keep the repository small. Tests extract them with CMake or Python's
`zipfile`. They do not construct exFAT structures, and reading them requires
no mount or formatter on the test machine.

The commands used to create the images were:

```bash
hdiutil create -size 32m -srcfolder /tmp/streamextract-exfat-fixture-tree \
  -fs ExFAT -fsargs '-b 4096' -layout NONE -format UDRW \
  -volname STREAMEXTRACT_TEST /tmp/streamextract-fixture-512.dmg

hdiutil create -size 32m -srcfolder /tmp/streamextract-exfat-fixture-tree \
  -fs ExFAT -layout MBRSPUD -format UDRW \
  -volname STREAMEXTRACT_TEST /tmp/streamextract-fixture-disk.dmg
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

## PFS/PFSC and UFS

`pfs.zip` contains real images generated with MkPFS 1.1.0, commit
`1d5df56ae29390900e888fa12e5f9856aa797adc` from
<https://github.com/PSBrew/MkPFS>, and Debian bookworm's `makefs` package
`20190105-3`. No formatter or Python package is needed to read these fixtures.
The ZIP compresses padding and is unpacked by CMake or the integration runner.

- `raw.ffpfsc`: a PS5 PFS with a direct directory tree, default 64 KiB filesystem
  blocks, raw files and PFSC files. `mixed.bin` has both compressed and raw PFSC
  blocks and a partial final logical block.
- `raw-ps4.ffpfs`: the same tree in an uncompressed PS4 PFS with 4 KiB blocks.
- `single.ffpfs`: a single ordinary `hello.txt`, without an inner filesystem.
- `exfat.ffpfsc`: a PFSC-compressed wrapper around the existing `volume.exfat`
  produced by hdiutil. `nested.ffpfsc` wraps `exfat.ffpfsc` again.
- `volume.ufs`, `big-ufs1.ufs`, `little-ufs2.ufs`, `volume2.ufs`: UFS1 in little
  and big endian, and UFS2 in little and big endian. `makefs` chose 8 KiB blocks
  and 1 KiB fragments. The files exercise indirect addressing, including double
  indirection in UFS2. The filesystem also contains an empty directory, a
  symbolic link and a hard link. File mtimes are 1700000000.
- `ufs.ffpfsc`, `ufs2.ffpfsc`: MkPFS wrappers around `volume.ufs` and `volume2.ufs`.
- `checksums.ufs`: `little-ufs2.ufs` with FreeBSD's `FS_METACKHASH`,
  `CK_SUPERBLOCK` and `CK_INODE` fields enabled. CRC32C values were calculated
  independently with `google-crc32c` 1.9.0, XORing its result with `0xffffffff`
  to match FreeBSD's CRC convention. The unit tests also corrupt each checksum.

The direct PFS tree contained `hello.txt` (`hello\n`), `empty.txt`, `sub/raw.bin`
(bytes 0..255), and `large.bin` (bytes 0..255 repeated 4608 times, then `end`).
`mixed.bin` contains 65536 zero bytes, 65536 bytes from Python's
`random.Random(123).randbytes`, 65536 `a` bytes, another 65536 random bytes from
the same generator, then `tail`. MkPFS omits empty source directories; the
reader's empty-directory test removes a child entry from an existing PFS
directory. PFS inode timestamps are MkPFS's build timestamps.

The UFS tree omits `mixed.bin`, adds `double.bin` (10 MiB of zeroes, then
`last indirect block\n`), `link -> hello.txt`, `linked.txt` as a hard link to
`hello.txt`, and `empty-dir/`. `pfs.json` and `ufs.json` record the expected
file sizes and SHA-256 hashes; the symlink and second hard-link reference are
skipped by the normal transfer planner.

Representative generation commands (with the matching MkPFS checkout on
`PYTHONPATH`) were:

```bash
python3 -m mkpfs pack folder --raw --compression-backend zlib --cpu-count 1 \
  --no-adjust-output-file-extension tree raw.ffpfsc
python3 -m mkpfs pack folder --raw --version PS4 --block-size 4096 --no-compress \
  --compression-backend zlib --cpu-count 1 --no-adjust-output-file-extension \
  tree raw-ps4.ffpfs
python3 -m mkpfs pack file --compression-backend zlib --cpu-count 1 \
  --no-adjust-output-file-extension --no-rename-inner-image volume.exfat exfat.ffpfsc
python3 -m mkpfs pack file --compression-backend zlib --cpu-count 1 \
  --no-adjust-output-file-extension --no-rename-inner-image exfat.ffpfsc nested.ffpfsc
makefs -t ffs -B little -o version=1 volume.ufs tree-ufs
makefs -t ffs -B big -o version=1 big-ufs1.ufs tree-ufs
makefs -t ffs -B little -o version=2 little-ufs2.ufs tree-ufs
makefs -t ffs -B big -o version=2 volume2.ufs tree-ufs
python3 -m mkpfs pack file --compression-backend zlib --cpu-count 1 \
  --no-adjust-output-file-extension --no-rename-inner-image volume.ufs ufs.ffpfsc
```
