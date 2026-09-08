#include <stdio.h>

#include "base/arena.h"
#include "base/process_file_lock.h"
#include "base/str.h"
#include "check.h"
#include "tools/diff.h"
#include "tools/filelock.h"
#include "tools/fs.h"
#include "tools/glob.h"
#include "tools/truncate.h"

static void TestGlob() {
  struct GlobCase {
    StrView Pattern;
    StrView Path;
    bool CaseInsensitive;
    bool Expected;
  };
  GlobCase cases[] = {
      {SVL("*.ts"), SVL("a.ts"), false, true},
      {SVL("*.ts"), SVL("src/a.ts"), false, false},
      {SVL("**/*.ts"), SVL("src/a.ts"), false, true},
      {SVL("src/**"), SVL("src/a/b.ts"), false, true},
      {SVL("a?c"), SVL("abc"), false, true},
      {SVL("a?c"), SVL("a/c"), false, false},
      {SVL("[abc]x"), SVL("bx"), false, true},
      {SVL("[abc]x"), SVL("zx"), false, false},
      {SVL("{a,b}.ts"), SVL("b.ts"), false, true},
      {SVL("**"), SVL("anything/here"), false, true},
      {SVL("*.TS"), SVL("a.ts"), true, true},
  };
  bool matched = true;
  for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    bool result =
        Glob_Match(cases[i].Pattern, cases[i].Path, cases[i].CaseInsensitive);
    matched = (result == cases[i].Expected) && matched;
  }
  CHECK(matched);
}

static void TestDiff(Arena* a) {
  DiffResult d = Diff_Make(a, SVL("f.txt"), SVL("a\nb\nc\n"), SVL("a\nB\nc\n"), 4);
  CHECK_ALL(d.FirstChangedLine == 2, StrView_Contains(d.Patch, "-b"),
            StrView_Contains(d.Patch, "+B"));

  DiffResult same = Diff_Make(a, SVL("f.txt"), SVL("x\n"), SVL("x\n"), 4);
  CHECK(same.FirstChangedLine == -1);
}

static void TestTruncate(Arena* a) {
  TruncResult h = Trunc_Head(a, SVL("a\nb\nc\nd\ne"), 3, 1000);
  CHECK_ALL(h.Truncated, !h.ByBytes, h.TotalLines == 5, h.OutputLines == 3);

  TruncResult none = Trunc_Head(a, SVL("a\nb"), 100, 1000);
  CHECK(!none.Truncated);

  TruncResult t = Trunc_Tail(a, SVL("a\nb\nc\nd\ne"), 2, 1000);
  CHECK_ALL(t.Truncated, t.OutputLines == 2, StrView_Contains(t.Content, "e"),
            !StrView_Contains(t.Content, "a"));

  // A byte cut must land on a character boundary. Splitting a multi-byte
  // character produces invalid UTF-8 that the provider rejects for the whole
  // request, so the cut moves off the character instead.
  StrView wide = SVL("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD");  // 3 chars, 9 bytes
  TruncResult head = Trunc_Head(a, wide, 100, 4);
  CHECK_ALL(head.Truncated, head.ByBytes, head.Content.Length == 3,
            (u8)head.Content.Data[0] == 0xE4);

  TruncResult tail = Trunc_Tail(a, wide, 100, 4);
  CHECK_ALL(tail.Truncated, tail.ByBytes, tail.Content.Length == 3,
            (u8)tail.Content.Data[0] == 0xE4);
}

static void TestFsPaths(Arena* a) {
  CHECK_ALL(StrView_EqCStr(Fs_Parent(SVL("a/b/c.txt")), "a/b"),
            StrView_EqCStr(Fs_Base(SVL("a/b/c.txt")), "c.txt"),
            StrView_EqCStr(Fs_Ext(SVL("a/b/c.txt")), ".txt"),
            StrView_EqCStr(Fs_Ext(SVL("noext")), ""),
            StrView_EqCStr(Fs_ToPosix(a, SVL("a\\b\\c")), "a/b/c"),
            StrView_EqCStr(Fs_ToPosix(a, Fs_Join(a, SVL("a/b"), SVL("c.txt"))),
                           "a/b/c.txt"),
            StrView_EqCStr(Fs_Relativize(a, SVL("src"), SVL("src/x/y.ts")),
                           "x/y.ts"));
  CHECK_ALL(StrView_EqCStr(Fs_Join(a, SVL("src"), SVL("/absolute")), "/absolute"),
            StrView_EqCStr(Fs_Join(a, SVL("src/"), SVL("/absolute")), "/absolute"),
            StrView_EqCStr(Fs_Join(a, SVL("src/"), SVL("child")), "src/child"),
            StrView_EqCStr(Fs_Join(a, SVL("src"), StrView{}), "src"),
            StrView_EqCStr(Fs_Join(a, StrView{}, SVL("child")), "child"),
            StrView_EqCStr(Fs_Relativize(a, SVL("src/"), SVL("src")), "."),
            StrView_EqCStr(Fs_Relativize(a, SVL("src"), SVL("src-other/x")),
                           "src-other/x"));
#if defined(_WIN32)
  CHECK(StrView_EqCStr(Fs_Relativize(a, SVL("SRC\\"), SVL("src\\x")), "x"));
#else
  CHECK(StrView_EqCStr(Fs_Relativize(a, SVL("SRC/"), SVL("src/x")), "src/x"));
#endif
}

static void TestFileLock() {
  FileLock_Init();
  FileLockToken t1 = FileLock_Acquire(SVL("lockpath/one"));
  FileLock_Release(t1);
  FileLockToken t2 = FileLock_Acquire(SVL("lockpath/one"));
  FileLock_Release(t2);
  FileLockToken t3 = FileLock_Acquire(SVL("lockpath/two"));
  FileLock_Release(t3);
  CHECK(true);  // reached here without deadlock
}

static void TestProcessFileLockProbe(Arena* a) {
  StrView path = SVL("clanker_lock_probe.tmp");
  remove("clanker_lock_probe.tmp");
  // Asking whether a lock is held must never author the sentinel. Session
  // listings ask this once per session, so a probe that created the file would
  // write one lock beside every log it merely displayed.
  CHECK_ALL(!ProcessFileLock_IsHeld(a, path), !Fs_Exists(path));

  ProcessFileLock lock = {};
  CHECK(ProcessFileLock_TryAcquire(a, path, &lock) ==
        ProcessFileLockStatus::Acquired);
  CHECK(Fs_Exists(path));
  ProcessFileLock_Release(&lock, path);
  // Release removes the sentinel, and probing the now-free lock still leaves
  // the directory exactly as it found it.
  CHECK_ALL(!Fs_Exists(path), !ProcessFileLock_IsHeld(a, path),
            !Fs_Exists(path));
}

void TestInfra() {
  Arena a = ArenaCreate();
  TestGlob();
  TestDiff(&a);
  TestTruncate(&a);
  TestFsPaths(&a);
  TestFileLock();
  TestProcessFileLockProbe(&a);
  ArenaRelease(&a);
}
