#include <stdio.h>

#include "base/arena.h"
#include "base/file_io.h"
#include "base/process_file_lock.h"
#include "base/str.h"
#include "base/thread.h"
#include "check.h"
#include "tools/diff.h"
#include "tools/filelock.h"
#include "tools/fs.h"
#include "tools/glob.h"
#include "tools/truncate.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
      {SVL("**/*.ts"), SVL("a.ts"), false, true},
      {SVL("a/**/b"), SVL("a/b"), false, true},
      {SVL("a/**/b"), SVL("a/x/y/b"), false, true},
      {SVL("a/**/b"), SVL("a/xb"), false, false},
      {SVL("src/**"), SVL("src/a/b.ts"), false, true},
      {SVL("a?c"), SVL("abc"), false, true},
      {SVL("a?c"), SVL("a/c"), false, false},
      {SVL("[abc]x"), SVL("bx"), false, true},
      {SVL("[abc]x"), SVL("zx"), false, false},
      {SVL("[!a-c]?.[ch]"), SVL("z1.c"), false, true},
      {SVL("[!a-c]?.[ch]"), SVL("b1.h"), false, false},
      {SVL("\\!name"), SVL("!name"), false, true},
      {SVL("\\#name"), SVL("#name"), false, true},
      {SVL("literal\\*"), SVL("literal*"), false, true},
      {SVL("[\\]]"), SVL("]"), false, true},
      {SVL("{a\\,b,c}"), SVL("a,b"), false, true},
      {SVL("{a,b}.ts"), SVL("b.ts"), false, true},
      {SVL("{**/*.h,*.cpp}"), SVL("search_regex.h"), false, true},
      {SVL("{**/*.h,*.cpp}"), SVL("src/search_regex.h"), false, true},
      {SVL("src/{**/*.h,*.cpp}"), SVL("src/search_regex.h"), false, true},
      {SVL("a{**/b,c}"), SVL("ab"), false, false},
      {SVL("{,a}**/b"), SVL("b"), false, true},
      {SVL("{[s,t]earch_regex.h,glob.h}"), SVL("search_regex.h"), false, true},
      {SVL("{[s,t]earch_regex.h,glob.h}"), SVL(",earch_regex.h"), false, true},
      {SVL("{[s,t]earch_regex.h,glob.h}"), SVL("glob.h"), false, true},
      {SVL("{[s}]earch_regex.h,glob.h}"), SVL("}earch_regex.h"), false, true},
      {SVL("{[s}]earch_regex.h,glob.h}"), SVL("glob.h"), false, true},
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
  char pattern[162] = {};
  char path[81] = {};
  for (u32 i = 0; i < 80; i++) {
    pattern[2 * i] = '*';
    pattern[2 * i + 1] = 'a';
    path[i] = 'a';
  }
  pattern[160] = 'b';
  CHECK(!Glob_MatchChecked(SV(pattern), SV(path), false, &matched));
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
  CHECK(StrView_EqCStr(Fs_Parent(SVL("C:\\child")), "C:\\"));
#else
  CHECK(StrView_EqCStr(Fs_Relativize(a, SVL("SRC/"), SVL("src/x")), "src/x"));
  CHECK(StrView_EqCStr(Fs_Parent(SVL("/child")), "/"));
#endif
}

static int Infra_RemoveDir(const char* path) {
#if defined(_WIN32)
  return _rmdir(path);
#else
  return rmdir(path);
#endif
}

static void Infra_Write(Arena* a, StrView root, StrView rel, StrView text = SVL("text\n")) {
  StrView path = Fs_Join(a, root, rel);
  CHECK(Fs_MkdirP(a, Fs_Parent(path)));
  CHECK(File_WriteAll(a, path, text) == FileIoResult::Success);
}

static void Infra_RemoveTree(Arena* a, StrView root) {
  bool ok = false;
  Array<FsEntry> entries = Fs_ListDir(a, root, &ok);
  CHECK(ok);
  for (u32 i = 0; i < entries.Count; i++) {
    const FsEntry& entry = entries.Data[i];
    StrView path = Fs_Join(a, root, entry.Name);
    if (entry.IsDir && !entry.IsSymlink) Infra_RemoveTree(a, path);
    else if (entry.IsDir) CHECK(Infra_RemoveDir(StrView_DupCStr(a, path)) == 0);
    else CHECK(remove(StrView_DupCStr(a, path)) == 0);
  }
  CHECK(Infra_RemoveDir(StrView_DupCStr(a, root)) == 0);
}

static void TestIgnoreRules(Arena* a, StrView root) {
  Infra_Write(a, root, SVL(".gitignore"), SVL(
      "# comment\n*.log\n!keep.log\nagain.log\n!again.log\nagain.log\n"
      "root/only.txt\n/anchored.txt\ncache/\n!cache/keep.txt\n"
      "file?.[ch]\n**/generated?.[ch]\nfoo/**/bar\n\\#hash\n\\!bang\n"
      " leading\ntrailing   \nescaped\\ \n"));
  Array<FsIgnorePattern> rules = Array_Make<FsIgnorePattern>(a);
  Fs_LoadIgnoreFile(a, &rules, root, StrView{}, SVL(".gitignore"));
  struct IgnoreCase {
    StrView Path;
    bool Directory;
    bool Ignored;
  };
  IgnoreCase cases[] = {
      {SVL("error.log"), false, true},
      {SVL("a/error.log"), false, true},
      {SVL("a/keep.log"), false, false},
      {SVL("again.log"), false, true},
      {SVL("root/only.txt"), false, true},
      {SVL("other/root/only.txt"), false, false},
      {SVL("anchored.txt"), false, true},
      {SVL("other/anchored.txt"), false, false},
      {SVL("cache"), true, true},
      {SVL("cache"), false, false},
      {SVL("cache/keep.txt"), false, true},
      {SVL("other/cache/keep.txt"), false, true},
      {SVL("other/file1.c"), false, true},
      {SVL("other/file12.c"), false, false},
      {SVL("generated1.h"), false, true},
      {SVL("a/b/generated2.c"), false, true},
      {SVL("foo/bar"), false, true},
      {SVL("foo/a/b/bar"), false, true},
      {SVL("other/foo/bar"), false, false},
      {SVL("#hash"), false, true},
      {SVL("!bang"), false, true},
      {SVL(" leading"), false, true},
      {SVL("leading"), false, false},
      {SVL("trailing"), false, true},
      {SVL("escaped "), false, true},
  };
  for (const IgnoreCase& test : cases)
    CHECK(Fs_IsIgnored(&rules, test.Path, test.Directory) == test.Ignored);

  Infra_Write(a, root, SVL("sub/.gitignore"),
              SVL("/local.txt\npath/item?.[ch]\n*.tmp\n!keep.tmp\n"));
  Fs_LoadIgnoreFile(a, &rules, Fs_Join(a, root, SVL("sub")), SVL("sub"), SVL(".gitignore"));
  CHECK_ALL(Fs_IsIgnored(&rules, SVL("sub/local.txt"), false),
            !Fs_IsIgnored(&rules, SVL("sub/deep/local.txt"), false),
            !Fs_IsIgnored(&rules, SVL("sibling/local.txt"), false),
            Fs_IsIgnored(&rules, SVL("sub/path/item1.c"), false),
            !Fs_IsIgnored(&rules, SVL("sub/deep/path/item1.c"), false),
            Fs_IsIgnored(&rules, SVL("sub/deep/a.tmp"), false),
            !Fs_IsIgnored(&rules, SVL("sub/deep/keep.tmp"), false));
}

struct InfraWalk {
  Arena* Mem;
  Array<StrView> Paths;
  volatile i32* Cancel;
  bool Stop;
  bool RemoveDirectory;
};

static bool Infra_Visit(void* user, StrView abs, StrView rel, const FsEntry& entry) {
  InfraWalk* state = (InfraWalk*)user;
  Array_Push(&state->Paths, StrView_Dup(state->Mem, rel));
  if (state->Cancel) *state->Cancel = 1;
  if (state->RemoveDirectory && entry.IsDir)
    CHECK(Infra_RemoveDir(StrView_DupCStr(state->Mem, abs)) == 0);
  return !state->Stop;
}

static bool Infra_Has(const InfraWalk& state, StrView path) {
  for (u32 i = 0; i < state.Paths.Count; i++)
    if (StrView_Eq(state.Paths.Data[i], path)) return true;
  return false;
}

static void TestCheckedWalk(Arena* a, StrView root) {
  Infra_Write(a, root, SVL(".git"), SVL("gitdir: elsewhere\n"));
  Infra_Write(a, root, SVL(".gitignore"), SVL(
      "*.log\n!keep.log\nblocked/\n!blocked/keep.txt\nanchor/file.txt\n"
      "parent-only.txt\n!priority.txt\n!nested/priority.txt\n"));
  Infra_Write(a, root, SVL(".ignore"), SVL("priority.txt\n!rescue.log\n!cross.log\n"));
  Infra_Write(a, root, SVL("nested/.gitignore"),
              SVL("/local.txt\ninner/item?.[ch]\ncross.log\n"));
  Infra_Write(a, root, SVL("nested/.ignore"), SVL("!priority.txt\n"));
  struct WalkCase {
    StrView Path;
    bool Included;
  };
  WalkCase files[] = {
      {SVL("keep.log"), true}, {SVL("hidden.log"), false},
      {SVL("rescue.log"), true}, {SVL("priority.txt"), false},
      {SVL("blocked/keep.txt"), false}, {SVL("anchor/file.txt"), false},
      {SVL("other/anchor/file.txt"), true}, {SVL("nested/cross.log"), true},
      {SVL("nested/priority.txt"), true}, {SVL("nested/local.txt"), false},
      {SVL("nested/deep/local.txt"), true}, {SVL("nested/inner/item1.c"), false},
      {SVL("nested/deep/inner/item1.c"), true}, {SVL("nested/parent-only.txt"), false},
      {SVL("nested/anchor/file.txt"), true}, {SVL("sibling/local.txt"), true},
  };
  for (const WalkCase& file : files) Infra_Write(a, root, file.Path);
  CHECK_ALL(Fs_Stat(Fs_Join(a, root, SVL("keep.log"))).IsRegular,
            !Fs_Stat(root).IsRegular);
  FsWalkOptions opts = {};
  opts.RespectGitignore = true;
  opts.RespectIgnoreFiles = true;
  opts.SkipLinks = true;
  opts.LoadParentIgnores = true;
  InfraWalk state = {};
  state.Mem = a;
  state.Paths = Array_Make<StrView>(a);
  StrBuf err = StrBuf_Make(a);
  CHECK(Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK(err.Length == 0);
  for (const WalkCase& file : files) CHECK(Infra_Has(state, file.Path) == file.Included);
  CHECK(!Infra_Has(state, SVL(".git")));

  Array_Clear(&state.Paths);
  StrView nested = Fs_Join(a, root, SVL("nested"));
  CHECK(Fs_WalkChecked(a, nested, opts, Infra_Visit, &state, &err));
  CHECK_ALL(!Infra_Has(state, SVL("parent-only.txt")), Infra_Has(state, SVL("cross.log")),
            Infra_Has(state, SVL("priority.txt")), Infra_Has(state, SVL("anchor/file.txt")),
            !Infra_Has(state, SVL("local.txt")), Infra_Has(state, SVL("deep/local.txt")));
  opts.LoadParentIgnores = false;
  Array_Clear(&state.Paths);
  CHECK(Fs_WalkChecked(a, nested, opts, Infra_Visit, &state, &err));
  CHECK_ALL(Infra_Has(state, SVL("parent-only.txt")), !Infra_Has(state, SVL("cross.log")));
  opts.LoadParentIgnores = true;
  Array_Clear(&state.Paths);
  CHECK(Fs_WalkChecked(a, Fs_Join(a, root, SVL("blocked")), opts, Infra_Visit, &state, &err));
  CHECK(state.Paths.Count == 0);

  // An inner repository boundary stops loading ignore files above it.
  Infra_Write(a, root, SVL("nested/.git"));
  Array_Clear(&state.Paths);
  CHECK(Fs_WalkChecked(a, nested, opts, Infra_Visit, &state, &err));
  CHECK_ALL(Infra_Has(state, SVL("parent-only.txt")), !Infra_Has(state, SVL("cross.log")),
            !Infra_Has(state, SVL(".git")));

  CHECK(remove(StrView_DupCStr(a, Fs_Join(a, root, SVL(".git")))) == 0);
  Infra_Write(a, root, SVL(".git/config"));
  opts.IncludeDirs = true;
  Array_Clear(&state.Paths);
  CHECK(Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK_ALL(!Infra_Has(state, SVL(".git")), !Infra_Has(state, SVL(".git/config")));

  volatile i32 abort = 1;
  opts.Abort = &abort;
  Array_Clear(&state.Paths);
  CHECK(!Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK_ALL(state.Paths.Count == 0, StrView_Contains(StrBuf_View(&err), "cancelled"));
  abort = 0;
  state.Cancel = &abort;
  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK_ALL(state.Paths.Count == 1, StrView_Contains(StrBuf_View(&err), "cancelled"));
  opts.Abort = nullptr;
  state.Cancel = nullptr;
  state.Stop = true;
  Array_Clear(&state.Paths);
  StrBuf_Clear(&err);
  CHECK(Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK_ALL(state.Paths.Count == 1, err.Length == 0);
  state.Stop = false;

  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, Fs_Join(a, root, SVL("missing")), opts, Infra_Visit, &state, &err));
  CHECK(err.Length != 0);
  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, Fs_Join(a, root, SVL("keep.log")), opts, Infra_Visit, &state, &err));
  CHECK(err.Length != 0);
  StrView io = Fs_Join(a, root, SVL("io"));
  CHECK(Fs_MkdirP(a, Fs_Join(a, io, SVL("vanish"))));
  opts = FsWalkOptions{};
  opts.IncludeDirs = true;
  state.RemoveDirectory = true;
  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, io, opts, Infra_Visit, &state, &err));
  CHECK(StrView_Contains(StrBuf_View(&err), "directory"));
  state.RemoveDirectory = false;

  CHECK(Fs_MkdirP(a, Fs_Join(a, io, SVL(".gitignore"))));
  opts.RespectGitignore = true;
  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, io, opts, Infra_Visit, &state, &err));
  CHECK(StrView_Contains(StrBuf_View(&err), "not a regular file"));
  CHECK(Infra_RemoveDir(StrView_DupCStr(a, Fs_Join(a, io, SVL(".gitignore")))) == 0);
  StrBuf pattern = StrBuf_Make(a);
  StrBuf filename = StrBuf_Make(a);
  for (u32 i = 0; i < 80; i++) {
    StrBuf_AppendCStr(&pattern, "*a");
    StrBuf_AppendChar(&filename, 'a');
  }
  StrBuf_AppendChar(&pattern, 'b');
  Infra_Write(a, io, SVL(".gitignore"), StrBuf_View(&pattern));
  Infra_Write(a, io, StrBuf_View(&filename));
  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, io, opts, Infra_Visit, &state, &err));
  CHECK(StrView_Contains(StrBuf_View(&err), "complexity limit"));

  StrBuf longPath = StrBuf_Make(a, 4102);
  for (u32 i = 0; i < 4100; i++) StrBuf_AppendChar(&longPath, 'a');
  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, StrBuf_View(&longPath), opts, Infra_Visit, &state, &err));
  CHECK(StrView_Contains(StrBuf_View(&err), "overlong"));
}

#if !defined(_WIN32)
static void TestWalkSpecialFiles(Arena* a, StrView root) {
  Infra_Write(a, root, SVL("regular.txt"));
  StrView pipe = Fs_Join(a, root, SVL("pipe"));
  CHECK(mkfifo(StrView_DupCStr(a, pipe), 0600) == 0);
  StrView loop = Fs_Join(a, root, SVL("loop"));
  StrView link = Fs_Join(a, root, SVL("link"));
  CHECK(symlink(".", StrView_DupCStr(a, loop)) == 0);
  CHECK(symlink("regular.txt", StrView_DupCStr(a, link)) == 0);
  CHECK_ALL(!Fs_Stat(pipe).IsRegular, Fs_Stat(link).IsRegular, Fs_Stat(loop).IsDir);
  bool ok = false;
  Array<FsEntry> entries = Fs_ListDir(a, root, &ok);
  CHECK_ALL(ok, entries.Count == 4);
  for (u32 i = 0; i < entries.Count; i++) {
    const FsEntry& e = entries.Data[i];
    if (StrView_Eq(e.Name, SVL("pipe"))) CHECK_ALL(!e.IsRegular, !e.IsDir, !e.IsSymlink);
    if (StrView_Eq(e.Name, SVL("link")) || StrView_Eq(e.Name, SVL("loop")))
      CHECK_ALL(e.IsSymlink, !e.IsRegular);
    if (StrView_Eq(e.Name, SVL("regular.txt"))) CHECK_ALL(e.IsRegular, !e.IsSymlink);
  }
  InfraWalk state = {};
  state.Mem = a;
  state.Paths = Array_Make<StrView>(a);
  FsWalkOptions opts = {};
  StrBuf err = StrBuf_Make(a);
  CHECK(Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK(state.Paths.Count == 4);
  Array_Clear(&state.Paths);
  opts.SkipLinks = true;
  CHECK(Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK_ALL(state.Paths.Count == 2, !Infra_Has(state, SVL("loop")), !Infra_Has(state, SVL("link")));
  Infra_Write(a, root, SVL(".ignore"), SVL("ignored\n"));
  const char* ignore = StrView_DupCStr(a, Fs_Join(a, root, SVL(".ignore")));
  if (geteuid() != 0) {
    CHECK(chmod(ignore, 0000) == 0);
    opts.RespectIgnoreFiles = true;
    StrBuf_Clear(&err);
    CHECK(!Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
    CHECK(err.Length != 0);
    CHECK(chmod(ignore, 0600) == 0);
  }
  StrView deep = root;
  for (u32 i = 0; i < 129; i++) {
    deep = Fs_Join(a, deep, SVL("d"));
    CHECK(Fs_MkdirP(a, deep));
  }
  StrBuf_Clear(&err);
  CHECK(!Fs_WalkChecked(a, root, opts, Infra_Visit, &state, &err));
  CHECK(StrView_Contains(StrBuf_View(&err), "depth limit"));
}
#endif

static void TestFilesystemTraversal(Arena* a) {
  StrBuf root = StrBuf_Make(a);
  StrBuf_AppendCStr(&root, "out/clanker-infra-walk-");
  StrBuf_AppendU64(&root, MonotonicMillis());
  StrView rules = Fs_Join(a, StrBuf_View(&root), SVL("rules"));
  TestIgnoreRules(a, rules);
  TestCheckedWalk(a, Fs_Join(a, StrBuf_View(&root), SVL("walk")));
#if !defined(_WIN32)
  TestWalkSpecialFiles(a, Fs_Join(a, StrBuf_View(&root), SVL("special")));
#endif
  Infra_RemoveTree(a, StrBuf_View(&root));
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
  TestFilesystemTraversal(&a);
  TestFileLock();
  TestProcessFileLockProbe(&a);
  ArenaRelease(&a);
}
