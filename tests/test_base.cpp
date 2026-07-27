#include <string.h>

#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"
#include "base/sysinfo.h"
#include "check.h"

static bool IsSingleLine(StrView text) {
  for (u32 i = 0; i < text.Length; i++) {
    if (text.Data[i] == '\n' || text.Data[i] == '\r') return false;
  }
  return true;
}

void TestArena() {
  Arena a = ArenaCreate();
  void* p1 = ArenaAlloc(&a, 100, 16);
  CHECK(((usize)p1 % 16) == 0);
  usize mark = a.Used;
  void* p2 = ArenaAlloc(&a, 64, 16);
  CHECK((byte*)p2 >= (byte*)p1 + 100);
  ArenaRestore(&a, mark);
  CHECK(a.Used == mark);
  ArenaReset(&a);
  CHECK(a.Used == 0);
  byte* big = (byte*)ArenaAlloc(&a, 5 * 1024 * 1024, 1);
  big[0] = 1;
  big[5 * 1024 * 1024 - 1] = 2;
  CHECK(big[0] == 1 && big[5 * 1024 * 1024 - 1] == 2);
  ArenaRelease(&a);
}

void TestArray() {
  Arena a = ArenaCreate();
  Array<int> arr = Array_Make<int>(&a, 2);
  for (int i = 0; i < 100; i++) Array_Push(&arr, i);
  CHECK_ALL(arr.Count == 100, *Array_Get(&arr, 50) == 50,
            *Array_Last(&arr) == 99);
  CHECK(Array_Pop(&arr) == 99);
  CHECK_ALL(arr.Count == 99, Array_Get(&arr, 999) == nullptr);
  ArenaRelease(&a);
}

void TestStr() {
  Arena a = ArenaCreate();
  StrBuf b = StrBuf_Make(&a, 4);
  StrBuf_AppendCStr(&b, "hello");
  StrBuf_AppendChar(&b, ' ');
  StrBuf_AppendCStr(&b, "world");
  StrBuf_AppendU64(&b, 123);
  CHECK_ALL(strcmp(StrBuf_CStr(&b), "hello world123") == 0, b.Length == 14);
  StrView v = StrBuf_View(&b);
  CHECK_ALL(StrView_EqCStr(v, "hello world123"),
            StrView_Eq(SVL("abc"), SV("abc")),
            !StrView_Eq(SVL("abc"), SV("abd")));

  StrBuf neg = StrBuf_Make(&a, 4);
  StrBuf_AppendI64(&neg, -42);
  CHECK(strcmp(StrBuf_CStr(&neg), "-42") == 0);

  StrView dup = StrView_Dup(&a, SV("copy me"));
  CHECK(StrView_EqCStr(dup, "copy me"));
  ArenaRelease(&a);
}

void TestHostInfo() {
  Arena a = ArenaCreate();

  // Pure HEAD parsing: "ref: refs/heads/<branch>" -> <branch> (slashes kept).
  CHECK_ALL(StrView_EqCStr(Git_BranchFromHead(SVL("ref: refs/heads/main\n")), "main"),
            StrView_EqCStr(
                Git_BranchFromHead(SVL("ref: refs/heads/worktree/swift-mortar-e0e4\n")),
                "worktree/swift-mortar-e0e4"),
            StrView_EqCStr(Git_BranchFromHead(SVL("ref: refs/heads/feature/x")),
                           "feature/x"));
  // Detached HEAD (raw SHA) -> empty.
  CHECK(Git_BranchFromHead(SVL("0123456789abcdef0123456789abcdef01234567\n")).Length == 0);

  // Pure gitdir-file parsing (worktree/submodule indirection), trailing sep stripped.
  CHECK_ALL(StrView_EqCStr(
                Git_GitdirFromFile(SVL("gitdir: C:/repo/.git/worktrees/wt/\n")),
                "C:/repo/.git/worktrees/wt"),
            StrView_EqCStr(Git_GitdirFromFile(SVL("gitdir: ../.git/modules/sub")),
                           "../.git/modules/sub"),
            Git_GitdirFromFile(SVL("not a gitdir file")).Length == 0);

  // Home abbreviation.
  StrView home = SVL("C:\\Users\\rodri");
  CHECK_ALL(
      StrView_EqCStr(Sys_AbbreviatePath(&a, SVL("C:\\Users\\rodri\\.briq\\wt"), home),
                     "~\\.briq\\wt"),
      StrView_EqCStr(Sys_AbbreviatePath(&a, home, home), "~"));
  // A path that only shares a partial final segment must NOT be abbreviated.
  CHECK(StrView_EqCStr(Sys_AbbreviatePath(&a, SVL("C:\\Users\\rodrigo\\x"), home), "C:\\Users\\rodrigo\\x"));
  // Empty home -> unchanged copy.
  CHECK(StrView_EqCStr(Sys_AbbreviatePath(&a, SVL("D:\\proj"), StrView{nullptr, 0}), "D:\\proj"));

  // Live queries: cwd is a non-empty path; the branch lookup must not crash and,
  // when it resolves, returns a clean single-line name (may be empty off-repo).
  StrView cwd = Sys_CurrentDir(&a);
  CHECK(cwd.Length > 0);
  bool hasSep = false;
  for (u32 i = 0; i < cwd.Length; i++)
    if (cwd.Data[i] == '/' || cwd.Data[i] == '\\') hasSep = true;
  CHECK(hasSep);
  StrView branch = Sys_GitBranch(&a, cwd);
  CHECK(IsSingleLine(branch));

  ArenaRelease(&a);
}
