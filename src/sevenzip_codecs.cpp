// Part of the lzmasdk library (cmake/LzmaSdk.cmake), built with the SDK's
// settings.
//
// 7-Zip's codecs and tables are set up by static constructors in files that
// nothing else refers to, so a static library would leave them out of the
// program. They are all included here instead, and the 7z reader calls
// sevenzip_codecs_linked(), which brings this file in. The interface GUIDs are
// defined here too (once per program, see MyInitGuid.h).

#include "Common/Common.h"

#include "Common/MyInitGuid.h"

#include "7zip/ICoder.h"
#include "7zip/IPassword.h"
#include "7zip/IProgress.h"
#include "7zip/IStream.h"

#include "Common/CRC.cpp"
#include "Common/Sha256Prepare.cpp"

#include "7zip/Compress/Bcj2Register.cpp"
#include "7zip/Compress/BcjRegister.cpp"
#include "7zip/Compress/BranchRegister.cpp"
#include "7zip/Compress/ByteSwap.cpp"
#include "7zip/Compress/CopyRegister.cpp"
#include "7zip/Compress/DeltaFilter.cpp"
#include "7zip/Compress/Lzma2Register.cpp"
#include "7zip/Compress/LzmaRegister.cpp"
#include "7zip/Compress/PpmdRegister.cpp"
#include "7zip/Crypto/7zAesRegister.cpp"

namespace rarftp {

void sevenzip_codecs_linked() {}

}  // namespace rarftp
