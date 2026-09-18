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
  FileReader reader = {};
  mark = arena.Used;
  CHECK(File_OpenRead(&arena, boundedPath, &reader) == FileIoResult::Success);
  CHECK_ALL(reader.Handle != nullptr, arena.Used == mark);
  char chunk[4096];
  u32 total = 0;
  const u32 capacities[] = {1u, 16u, 4096u, 4096u, 4096u, 4096u};
  for (u32 capacity : capacities) {
    u32 count = 123;
    CHECK(File_ReadChunk(&reader, chunk, capacity, &count) == FileIoResult::Success);
    CHECK(count == ClankerMin(capacity, content.Length - total));
    CHECK(StrView_Eq(StrView{chunk, count}, StrView{content.Data + total, count}));
    total += count;
  }
  CHECK_ALL(total == content.Length, arena.Used == mark);
  CHECK(File_Close(&reader) == FileIoResult::Success);
  CHECK(reader.Handle == nullptr);
  CHECK(File_Close(&reader) == FileIoResult::Success);
  u32 count = 123;
  CHECK(File_ReadChunk(&reader, chunk, sizeof(chunk), &count) == FileIoResult::IoError);
  CHECK(count == 0);
  CHECK(File_OpenRead(&arena, SVL("out/clanker-file-io-missing/not-found"), &reader) ==
        FileIoResult::Missing);
  CHECK_ALL(reader.Handle == nullptr, arena.Used == mark);
  CHECK(ConfigFile_ReadAll(&arena, path, &bytes) == ConfigFileReadResult::Success);
  CHECK(StrView_Eq(bytes, content));

  // A regular file is read into exactly one block of its size plus the NUL.
  mark = arena.Used;
  CHECK(File_ReadAll(&arena, path, &bytes) == FileIoResult::Success);
  CHECK_ALL(StrView_Eq(bytes, content), arena.Used - mark == content.Length + 1u);
  CHECK(File_ReadAll(&arena, path, &bytes, content.Length) == FileIoResult::Success);
  CHECK(StrView_Eq(bytes, content));
  mark = arena.Used;
  bytes = SVL("stale");
  CHECK(File_ReadAll(&arena, path, &bytes, content.Length - 1u) == FileIoResult::TooLarge);
  CHECK_ALL(bytes.Data == nullptr, bytes.Length == 0, arena.Used == mark);

  CHECK(File_OpenRead(&arena, path, &reader) == FileIoResult::Success);
  CHECK(File_ReadChunk(&reader, chunk, 16, &count) == FileIoResult::Success);
  CHECK(count == 16);
  FILE* append = fopen(StrView_DupCStr(&arena, path), "ab");
  CHECK(append != nullptr);
  if (append) {
    CHECK(fputc('x', append) == 'x');
    CHECK(fclose(append) == 0);
  }
  total = count;
  for (;;) {
    CHECK(File_ReadChunk(&reader, chunk, sizeof(chunk), &count) == FileIoResult::Success);
    if (!count) break;
    CHECK(total + count <= 10001);
    if (total + count <= 10001)
      CHECK(StrView_Eq(StrView{chunk, count}, StrView{data + total, count}));
    total += count;
  }
  CHECK(total == 10001);
  CHECK(File_Close(&reader) == FileIoResult::Success);

  CHECK(File_WriteAll(&arena, path, SVL("short")) == FileIoResult::Success);
  CHECK(File_ReadAll(&arena, path, &bytes) == FileIoResult::Success);
  CHECK(StrView_Eq(bytes, SVL("short")));
  CHECK(File_WriteAll(&arena, path, StrView{}) == FileIoResult::Success);
  CHECK(File_ReadAll(&arena, path, &bytes) == FileIoResult::Success);
  CHECK_ALL(bytes.Length == 0, bytes.Data && bytes.Data[0] == '\0');
  CHECK(File_OpenRead(&arena, path, &reader) == FileIoResult::Success);
  count = 123;
  CHECK(File_ReadChunk(&reader, chunk, sizeof(chunk), &count) == FileIoResult::Success);
  CHECK(count == 0);
  CHECK(File_Close(&reader) == FileIoResult::Success);

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

  // Directory, replace, remove, and atomic config writes take UTF-8 paths too.
  StrView unicodeDir = Fs_Join(&arena, StrBuf_View(&root), SVL("d\xc3\xa9j\xc3\xa0"));
  CHECK(File_MakeDir(&arena, unicodeDir));
  CHECK(File_MakeDir(&arena, unicodeDir));
  StrView unicodeConfig = Fs_Join(&arena, unicodeDir, SVL("r\xc3\xa9glages.json"));
  CHECK(ConfigFile_WriteAtomic(&arena, unicodeConfig, SVL("{\"a\":1}")));
  CHECK(ConfigFile_WriteAtomic(&arena, unicodeConfig, SVL("{\"a\":2}")));
  CHECK(ConfigFile_ReadAll(&arena, unicodeConfig, &bytes) == ConfigFileReadResult::Success);
  CHECK(StrView_EqCStr(bytes, "{\"a\":2}"));
  CHECK(File_Replace(&arena, unicodePath, unicodeConfig));
  CHECK(File_ReadAll(&arena, unicodeConfig, &bytes) == FileIoResult::Success);
  CHECK(StrView_EqCStr(bytes, "unicode path"));
  CHECK(File_ReadAll(&arena, unicodePath, &bytes) == FileIoResult::Missing);
  CHECK(File_Remove(&arena, unicodeConfig));
  CHECK(!File_Remove(&arena, unicodeConfig));
#if defined(_WIN32)
  wchar_t native[4096];
  CHECK(Win32_Path(unicodeDir, native, 4096));
  CHECK(_wrmdir(native) == 0);
#else
  CHECK(rmdir(StrView_DupCStr(&arena, unicodeDir)) == 0);
#endif
#if !defined(_WIN32)
  CHECK(File_OpenRead(&arena, StrBuf_View(&root), &reader) == FileIoResult::Success);
  count = 123;
  CHECK(File_ReadChunk(&reader, chunk, sizeof(chunk), &count) == FileIoResult::IoError);
  CHECK(count == 0);
  CHECK(File_Close(&reader) == FileIoResult::Success);
  CHECK(File_OpenRead(&arena, path, &reader) == FileIoResult::Success);
  CHECK(close(fileno((FILE*)reader.Handle)) == 0);
  CHECK(File_Close(&reader) == FileIoResult::IoError);
  CHECK(reader.Handle == nullptr);
#endif

  CHECK(remove(StrView_DupCStr(&arena, path)) == 0);
#if defined(_WIN32)
  CHECK(_rmdir(StrBuf_CStr(&root)) == 0);
#else
  CHECK(rmdir(StrBuf_CStr(&root)) == 0);
#endif
  ArenaRelease(&arena);
}
