#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "tools/diff.h"
#include "tools/filelock.h"
#include "tools/fs.h"
#include "tools/glob.h"
#include "tools/regex.h"
#include "tools/truncate.h"

static void TestGlob() {
  CHECK(Glob_Match(SVL("*.ts"), SVL("a.ts"), false));
  CHECK(!Glob_Match(SVL("*.ts"), SVL("src/a.ts"), false));
  CHECK(Glob_Match(SVL("**/*.ts"), SVL("src/a.ts"), false));
  CHECK(Glob_Match(SVL("src/**"), SVL("src/a/b.ts"), false));
  CHECK(Glob_Match(SVL("a?c"), SVL("abc"), false));
  CHECK(!Glob_Match(SVL("a?c"), SVL("a/c"), false));
  CHECK(Glob_Match(SVL("[abc]x"), SVL("bx"), false));
  CHECK(!Glob_Match(SVL("[abc]x"), SVL("zx"), false));
  CHECK(Glob_Match(SVL("{a,b}.ts"), SVL("b.ts"), false));
  CHECK(Glob_Match(SVL("**"), SVL("anything/here"), false));
  CHECK(Glob_Match(SVL("*.TS"), SVL("a.ts"), true));
}

static void TestRegex(Arena* a) {
  StrBuf err = StrBuf_Make(a, 64);

  Regex* re = Regex_Compile(a, SVL("a.c"), false, &err);
  CHECK(re != nullptr);
  CHECK(Regex_Test(re, SVL("xabcx")));
  CHECK(!Regex_Test(re, SVL("axxc")));

  re = Regex_Compile(a, SVL("a+"), false, &err);
  CHECK(Regex_Test(re, SVL("baaa")));

  re = Regex_Compile(a, SVL("\\d+"), false, &err);
  u32 s = 0, l = 0;
  CHECK(Regex_Search(re, SVL("ab123cd"), &s, &l));
  CHECK(s == 2 && l == 3);

  re = Regex_Compile(a, SVL("foo|bar"), false, &err);
  CHECK(Regex_Test(re, SVL("xxbaryy")));

  re = Regex_Compile(a, SVL("^hello$"), false, &err);
  CHECK(Regex_Test(re, SVL("hello")));
  CHECK(!Regex_Test(re, SVL("hello world")));

  re = Regex_Compile(a, SVL("[a-z]+"), false, &err);
  CHECK(Regex_Test(re, SVL("ABCdef")));

  re = Regex_Compile(a, SVL("Foo"), true, &err);
  CHECK(Regex_Test(re, SVL("xxFOOyy")));

  re = Regex_Compile(a, SVL("(ab)+c"), false, &err);
  CHECK(Regex_Test(re, SVL("zababcz")));
}

static void TestDiff(Arena* a) {
  DiffResult d = Diff_Make(a, SVL("f.txt"), SVL("a\nb\nc\n"), SVL("a\nB\nc\n"), 4);
  CHECK(d.FirstChangedLine == 2);
  CHECK(StrView_Contains(d.Patch, "-b"));
  CHECK(StrView_Contains(d.Patch, "+B"));

  DiffResult same = Diff_Make(a, SVL("f.txt"), SVL("x\n"), SVL("x\n"), 4);
  CHECK(same.FirstChangedLine == -1);
}

static void TestTruncate(Arena* a) {
  TruncResult h = Trunc_Head(a, SVL("a\nb\nc\nd\ne"), 3, 1000);
  CHECK(h.Truncated);
  CHECK(!h.ByBytes);
  CHECK(h.TotalLines == 5);
  CHECK(h.OutputLines == 3);

  TruncResult none = Trunc_Head(a, SVL("a\nb"), 100, 1000);
  CHECK(!none.Truncated);

  TruncResult t = Trunc_Tail(a, SVL("a\nb\nc\nd\ne"), 2, 1000);
  CHECK(t.Truncated);
  CHECK(t.OutputLines == 2);
  CHECK(StrView_Contains(t.Content, "e"));
  CHECK(!StrView_Contains(t.Content, "a"));
}

static void TestFsPaths(Arena* a) {
  CHECK(StrView_EqCStr(Fs_Parent(SVL("a/b/c.txt")), "a/b"));
  CHECK(StrView_EqCStr(Fs_Base(SVL("a/b/c.txt")), "c.txt"));
  CHECK(StrView_EqCStr(Fs_Ext(SVL("a/b/c.txt")), ".txt"));
  CHECK(StrView_EqCStr(Fs_Ext(SVL("noext")), ""));
  CHECK(StrView_EqCStr(Fs_ToPosix(a, SVL("a\\b\\c")), "a/b/c"));
  CHECK(StrView_EqCStr(Fs_ToPosix(a, Fs_Join(a, SVL("a/b"), SVL("c.txt"))), "a/b/c.txt"));
  CHECK(StrView_EqCStr(Fs_Relativize(a, SVL("src"), SVL("src/x/y.ts")), "x/y.ts"));
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

void TestInfra() {
  Arena a = ArenaCreate();
  TestGlob();
  TestRegex(&a);
  TestDiff(&a);
  TestTruncate(&a);
  TestFsPaths(&a);
  TestFileLock();
  ArenaRelease(&a);
}
