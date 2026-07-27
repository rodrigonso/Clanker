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

static void TestRegex(Arena* a) {
  StrBuf err = StrBuf_Make(a, 64);

  Regex* re = Regex_Compile(a, SVL("a.c"), false, &err);
  CHECK(re != nullptr);
  CHECK_ALL(Regex_Test(re, SVL("xabcx")), !Regex_Test(re, SVL("axxc")));

  re = Regex_Compile(a, SVL("a+"), false, &err);
  CHECK(Regex_Test(re, SVL("baaa")));

  re = Regex_Compile(a, SVL("\\d+"), false, &err);
  u32 s = 0, l = 0;
  CHECK(Regex_Search(re, SVL("ab123cd"), &s, &l));
  CHECK(s == 2 && l == 3);

  re = Regex_Compile(a, SVL("foo|bar"), false, &err);
  CHECK(Regex_Test(re, SVL("xxbaryy")));

  re = Regex_Compile(a, SVL("^hello$"), false, &err);
  CHECK_ALL(Regex_Test(re, SVL("hello")), !Regex_Test(re, SVL("hello world")));

  re = Regex_Compile(a, SVL("[a-z]+"), false, &err);
  CHECK(Regex_Test(re, SVL("ABCdef")));

  re = Regex_Compile(a, SVL("Foo"), true, &err);
  CHECK(Regex_Test(re, SVL("xxFOOyy")));

  re = Regex_Compile(a, SVL("(ab)+c"), false, &err);
  CHECK(Regex_Test(re, SVL("zababcz")));
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
