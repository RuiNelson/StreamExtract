/* FatFs R0.16 configuration for reading exFAT volume images. */
#define FFCONF_DEF 80386

#define FF_FS_READONLY 1
#define FF_FS_MINIMIZE 0
#define FF_USE_FIND 0
#define FF_USE_MKFS 0
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND 0
#define FF_USE_CHMOD 0
#define FF_USE_LABEL 0
#define FF_USE_FORWARD 0
#define FF_USE_STRFUNC 0
#define FF_PRINT_LLI 0
#define FF_PRINT_FLOAT 0
#define FF_STRF_ENCODE 0

#define FF_CODE_PAGE 437
/* Static name/entry buffers: every FatFs call is serialized by our adapter,
 * including mount/unmount. The entry buffer also gives us exFAT's UTC offset
 * and ValidDataLength, which FILINFO does not expose. */
#define FF_USE_LFN 1
#define FF_MAX_LFN 255
#define FF_LFN_UNICODE 2
#define FF_LFN_BUF 765
#define FF_SFN_BUF 34
#define FF_FS_RPATH 0
#define FF_PATH_DEPTH 10

#define FF_VOLUMES 10
#define FF_STR_VOLUME_ID 0
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS 512
#define FF_MAX_SS 4096
#define FF_LBA64 1
#define FF_MIN_GPT 0x10000000
#define FF_USE_TRIM 0

#define FF_FS_TINY 0
#define FF_FS_EXFAT 1
#define FF_FS_NORTC 1
#define FF_NORTC_MON 1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2025
#define FF_FS_CRTIME 0
#define FF_FS_NOFSINFO 0
#define FF_FS_LOCK 0
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT 1000
