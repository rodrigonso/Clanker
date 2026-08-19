#include "app/config_paths.h"

#include <string.h>

#include "base/sysinfo.h"

StrView ClankerDataRoot(Arena* arena) { return ClankerDataPath(arena, nullptr); }

StrView ClankerDataPath(Arena* arena, const char* name) {
  StrView home = Sys_HomeDir(arena);
  u32 extra = name ? (u32)strlen(name) : 0;
  StrBuf path = StrBuf_Make(arena, home.Length + extra + 16);
  StrBuf_Append(&path, home.Length ? home : SVL("."));
  StrBuf_AppendCStr(&path, "/.clanker");
  if (name && *name) {
    StrBuf_AppendChar(&path, '/');
    StrBuf_AppendCStr(&path, name);
  }
  return StrBuf_View(&path);
}
