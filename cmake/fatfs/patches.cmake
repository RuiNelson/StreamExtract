# Official R0.16 patches from https://elm-chan.org/fsw/ff/patches.html:
# ff16p1.diff (2025-09-13) and ff16p2.diff (2026-07-10).
# Text replacements work on Windows too, without requiring a patch executable.
function(_fatfs_patch file)
  file(READ "${file}" _text)
  string(REPLACE "\r\n" "\n" _text "${_text}")
  if(NOT _text MATCHES "FatFs - Generic FAT Filesystem Module  R0.16 ")
    message(FATAL_ERROR "Expected unpatched FatFs R0.16 in fatfs/source/ff.c")
  endif()
  macro(_change before after)
    string(FIND "${_text}" "${before}" _first)
    string(FIND "${_text}" "${before}" _last REVERSE)
    if(_first EQUAL -1 OR NOT _first EQUAL _last)
      message(FATAL_ERROR "FatFs patch context not found exactly once:\n${before}")
    endif()
    string(REPLACE "${before}" "${after}" _text "${_text}")
  endmacro()
  _change("R0.16                               /" "R0.16 w/patch 2                     /")
  _change("#define MAX_EXFAT\t0x7FFFFFFD\t\t/* Max exFAT clusters (differs from specs, implementation limit) */"
    "#define MAX_EXFAT\t0x7FFFFFFD\t\t/* Max exFAT clusters (differs from specs, implementation limit) */\n#define MIN_EXFAT\t0x00000100\t\t/* Min exFAT clusters (Not defined in specs, implementation limit) */\n#define MIN_FAT12\t32\t\t\t\t/* Min FAT12 clusters (Not defined in specs, implementation limit) */\n#define MIN_VOLUME\t64\t\t\t\t/* Min volume sectors (Not defined in specs, implementation limit) */")
  _change("ld_16(fs->win + BPB_TotSec16) >= 128" "ld_16(fs->win + BPB_TotSec16) >= MIN_VOLUME")
  _change("Properness of volume size (>=128)" "Properness of volume size")
  _change("if (nclst <= MAX_FAT12) fmt = FS_FAT12;"
    "if (nclst <= MAX_FAT12) fmt = FS_FAT12;\n\t\tif (nclst <= MIN_FAT12) fmt = 0;")
  _change("if (sz_vol < 128) LEAVE_MKFS(FR_MKFS_ABORTED);\t/* Check if volume size is >=128 sectors */"
    "if (sz_vol < MIN_VOLUME) LEAVE_MKFS(FR_MKFS_ABORTED);\t/* Check if volume size is not too small */")
  _change("if (sz_vol < b_data + pau * 16 - b_vol) LEAVE_MKFS(FR_MKFS_ABORTED);\t/* Too small volume? */"
    "if (sz_vol < b_data + pau * MIN_FAT12 - b_vol) LEAVE_MKFS(FR_MKFS_ABORTED);\t/* Too small volume for this configuration? */")
  _change("if (ncl > MAX_EXFAT) return FR_NO_FILESYSTEM;\t/* (Too many clusters) */"
    "if (ncl < MIN_EXFAT || ncl > MAX_EXFAT) return FR_NO_FILESYSTEM;\t/* (Wrong cluster count) */")
  _change("if (fasize == 0) fasize = ld_32(fs->win + BPB_FATSz32);"
    "if (fasize == 0) fasize = ld_32(fs->win + BPB_FATSz32);\n\t\tif (fasize >= 0x200000) return FR_NO_FILESYSTEM;\t/* (Must be smaller than max FAT size) */")
  _change("si < dj.dir[XDIR_NumLabel]; si++" "si < dj.dir[XDIR_NumLabel] && si < 11; si++")
  file(WRITE "${file}" "${_text}")
endfunction()
