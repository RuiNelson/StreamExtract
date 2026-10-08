# Patches applied to the private copy of the UnRAR sources (see UnRAR.cmake).
#
# A RAR archive that ends inside a header (an incomplete download, a truncated
# copy) is reported by UnRAR's own tools as "Unexpected end of archive"
# (Archive::UnexpEndArcMsg), but the DLL API only raises a global warning:
# RARReadHeaderEx returns ERAR_END_ARCHIVE, as for a complete archive, and
# RARProcessFile returns ERAR_UNKNOWN. The listing would end early and the
# transfer would report success without the remaining files. Record the
# condition in the Archive object and return ERAR_TRUNCATED from both calls.
# An archive that ends exactly at a block boundary without an end-of-archive
# block (RAR 1.5, or rar -en) is still complete, as UnRAR itself decides.
#
# Each change must match exactly once, or the configuration fails: review the
# patch when updating UnRAR.

set(_unrar_patched_files dll.hpp archive.hpp archive.cpp arcread.cpp dll.cpp)

# Writes `name` from the downloaded sources into `output_dir`, patched; only
# rewrites it when the result changes, so a reconfiguration does not rebuild UnRAR.
function(_unrar_patch source_dir output_dir name)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_dir}/${name}")
  file(READ "${source_dir}/${name}" _text)
  macro(_change before after)
    string(FIND "${_text}" "${before}" _first)
    string(FIND "${_text}" "${before}" _last REVERSE)
    if(_first EQUAL -1 OR NOT _first EQUAL _last)
      message(FATAL_ERROR "UnRAR patch context not found exactly once in ${name}:\n${before}")
    endif()
    string(REPLACE "${before}" "${after}" _text "${_text}")
  endmacro()

  if(name STREQUAL "dll.hpp")
    _change("#define ERAR_LARGE_DICT         25\n"
      "#define ERAR_LARGE_DICT         25\n#define ERAR_TRUNCATED         100 // streamextract patch: the archive ends inside a header.\n")
  elseif(name STREQUAL "archive.hpp")
    _change("    bool BrokenHeader;\n"
      "    bool BrokenHeader;\n    bool UnexpectedEnd; // streamextract patch: a header was cut short by the end of the file.\n")
  elseif(name STREQUAL "archive.cpp")
    _change("  BrokenHeader=false;\n  LastReadBlock=0;\n"
      "  BrokenHeader=false;\n  UnexpectedEnd=false;\n  LastReadBlock=0;\n")
    _change("  BrokenHeader=false; // Might be left from previous volume.\n"
      "  BrokenHeader=false; // Might be left from previous volume.\n  UnexpectedEnd=false;\n")
  elseif(name STREQUAL "arcread.cpp")
    _change("    uiMsg(UIERROR_UNEXPEOF,FileName);\n"
      "    uiMsg(UIERROR_UNEXPEOF,FileName);\n    UnexpectedEnd=true;\n")
  elseif(name STREQUAL "dll.cpp")
    # RARReadHeaderEx: after the broken header and wrong password checks.
    _change("        return ERAR_BAD_PASSWORD;\n      \n      return ERAR_END_ARCHIVE;\n"
      "        return ERAR_BAD_PASSWORD;\n      \n      if (Data->Arc.UnexpectedEnd)\n        return ERAR_TRUNCATED;\n\n      return ERAR_END_ARCHIVE;\n")
    # ProcessFile reads the next header after the file: a cut there is not an unknown error.
    _change("  return Data->Cmd.DllError!=0 ? Data->Cmd.DllError : RarErrorToDll(ErrHandler.GetErrorCode());\n}\n\n\nint PASCAL RARProcessFile("
      "  if (Data->Cmd.DllError==0 && Data->Arc.UnexpectedEnd &&\n      (ErrHandler.GetErrorCode()==RARX_SUCCESS || ErrHandler.GetErrorCode()==RARX_WARNING))\n    return ERAR_TRUNCATED;\n  return Data->Cmd.DllError!=0 ? Data->Cmd.DllError : RarErrorToDll(ErrHandler.GetErrorCode());\n}\n\n\nint PASCAL RARProcessFile(")
  else()
    message(FATAL_ERROR "No UnRAR patch for ${name}")
  endif()

  set(_output "${output_dir}/${name}")
  set(_current "")
  if(EXISTS "${_output}")
    file(READ "${_output}" _current)
  endif()
  if(NOT _current STREQUAL _text)
    file(WRITE "${_output}" "${_text}")
  endif()
endfunction()
