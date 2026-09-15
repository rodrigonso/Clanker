#include <stdio.h>

#include "base/config_file.h"
#include "base/file_io.h"
#include "base/thread.h"
#include "base/win32_utf8.h"
#include "check.h"
#include "tools/fs.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

void TestFileIo() {
  Arena arena = ArenaCreate();
  StrBuf root = StrBuf_Make(&arena, 64);
  StrBuf_AppendCStr(&root, "out/clanker-file-io-");
  StrBuf_AppendU64(&root, MonotonicMillis());
  CHECK(Fs_MkdirP(&arena, StrBuf_View(&root)));
  StrView path = Fs_Join(&arena, StrBuf_View(&root), SVL("bytes.bin"));

  StrView bytes = SVL("stale");
  usize mark = arena.Used;
  CHECK(File_ReadAll(&arena, path, &bytes) == FileIoResult::Missing);
  CHECK_ALL(bytes.Data == nullptr, bytes.Length == 0, arena.Used == mark);
  CHECK(ConfigFile_ReadAll(&arena, path, &bytes) == ConfigFileReadResult::Missing);

  char* data = ArenaPushArray(&arena, char, 10001);
  for (u32 i = 0; i < 10000; i++) data[i] = (char)(i % 256);
  data[10000] = 'x';
  StrView content{data, 10000};
  // Neither the content nor the pathname needs a terminator at its view boundary.
  StrBuf pathWithSuffix = StrBuf_Make(&arena, path.Length + 16);
  StrBuf_Append(&pathWithSuffix, path);
  StrBuf_AppendCStr(&pathWithSuffix, ".not-the-path");
  StrView boundedPath{pathWithSuffix.Data, path.Length};
  mark = arena.Used;
  CHECK(File_WriteAll(&arena, boundedPath, content) == FileIoResult::Success);
  CHECK(arena.Used == mark);
  CHECK(File_ReadAll(&arena, boundedPath, &bytes) == FileIoResult::Success);
  CHECK_ALL(StrView_Eq(bytes, content), bytes.Data && bytes.Data[bytes.Length] == '\0');
  CHECK(ConfigFile_ReadAll(&arena, path, &bytes) == ConfigFileReadResult::Success);
  CHECK(StrView_Eq(bytes, content));

  CHECK(File_WriteAll(&arena, path, SVL("short")) == FileIoResult::Success);
  CHECK(File_ReadAll(&arena, path, &bytes) == FileIoResult::Success);
  CHECK(StrView_Eq(bytes, SVL("short")));
  CHECK(File_WriteAll(&arena, path, StrView{}) == FileIoResult::Success);
  CHECK(File_ReadAll(&arena, path, &bytes) == FileIoResult::Success);
  CHECK_ALL(bytes.Length == 0, bytes.Data && bytes.Data[0] == '\0');

  mark = arena.Used;
  CHECK(File_ReadAll(&arena, StrBuf_View(&root), &bytes) != FileIoResult::Success);
  CHECK_ALL(bytes.Data == nullptr, bytes.Length == 0, arena.Used == mark);
  CHECK(File_WriteAll(&arena, StrBuf_View(&root), SVL("x")) != FileIoResult::Success);
  CHECK(ConfigFile_ReadAll(&arena, StrBuf_View(&root), &bytes) ==
        ConfigFileReadResult::Failure);
  StrView unicodePath = Fs_Join(&arena, StrBuf_View(&root),
                                SVL("\xe4\xb8\xad\xf0\x9f\x98\x80.txt"));
  CHECK(File_WriteAll(&arena, unicodePath, SVL("unicode path")) == FileIoResult::Success);
  CHECK(File_ReadAll(&arena, unicodePath, &bytes) == FileIoResult::Success);
  CHECK(StrView_EqCStr(bytes, "unicode path"));
#if defined(_WIN32)
  wchar_t native[4096];
  CHECK(Win32_Path(unicodePath, native, 4096));
  CHECK(_wremove(native) == 0);
#else
  CHECK(remove(StrView_DupCStr(&arena, unicodePath)) == 0);
#endif

  CHECK(remove(StrView_DupCStr(&arena, path)) == 0);
#if defined(_WIN32)
  CHECK(_rmdir(StrBuf_CStr(&root)) == 0);
#else
  CHECK(rmdir(StrBuf_CStr(&root)) == 0);
#endif
  ArenaRelease(&arena);
}
