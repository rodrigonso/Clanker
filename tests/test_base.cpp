#include <string.h>

#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"
#include "check.h"

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
  CHECK(arr.Count == 100);
  CHECK(*Array_Get(&arr, 50) == 50);
  CHECK(*Array_Last(&arr) == 99);
  CHECK(Array_Pop(&arr) == 99);
  CHECK(arr.Count == 99);
  CHECK(Array_Get(&arr, 999) == nullptr);
  ArenaRelease(&a);
}

void TestStr() {
  Arena a = ArenaCreate();
  StrBuf b = StrBuf_Make(&a, 4);
  StrBuf_AppendCStr(&b, "hello");
  StrBuf_AppendChar(&b, ' ');
  StrBuf_AppendCStr(&b, "world");
  StrBuf_AppendU64(&b, 123);
  CHECK(strcmp(StrBuf_CStr(&b), "hello world123") == 0);
  CHECK(b.Length == 14);
  StrView v = StrBuf_View(&b);
  CHECK(StrView_EqCStr(v, "hello world123"));
  CHECK(StrView_Eq(SVL("abc"), SV("abc")));
  CHECK(!StrView_Eq(SVL("abc"), SV("abd")));

  StrBuf neg = StrBuf_Make(&a, 4);
  StrBuf_AppendI64(&neg, -42);
  CHECK(strcmp(StrBuf_CStr(&neg), "-42") == 0);

  StrView dup = StrView_Dup(&a, SV("copy me"));
  CHECK(StrView_EqCStr(dup, "copy me"));
  ArenaRelease(&a);
}
