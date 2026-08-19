#include "base/arena.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static const usize ARENA_COMMIT_CHUNK = (usize)64 * 1024;

static usize RoundUp(usize value, usize granularity) {
  return (value + (granularity - 1)) & ~(granularity - 1);
}

static void ArenaCommitTo(Arena* a, usize targetUsed) {
  usize target = RoundUp(targetUsed, ARENA_COMMIT_CHUNK);
  if (target > a->Reserved) target = a->Reserved;
  if (target <= a->Committed) return;
#if defined(_WIN32)
  void* p = VirtualAlloc(a->Base + a->Committed, target - a->Committed, MEM_COMMIT, PAGE_READWRITE);
  if (!p) CLANKER_TRAP();
#else
  if (mprotect(a->Base + a->Committed, target - a->Committed, PROT_READ | PROT_WRITE) != 0) CLANKER_TRAP();
#endif
  a->Committed = target;
}

Arena ArenaCreate(usize reserveBytes) {
  Arena a = {};
  usize reserve = RoundUp(reserveBytes, ARENA_COMMIT_CHUNK);
#if defined(_WIN32)
  a.Base = (byte*)VirtualAlloc(nullptr, reserve, MEM_RESERVE, PAGE_READWRITE);
  if (!a.Base) CLANKER_TRAP();
#else
  void* p = mmap(nullptr, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) CLANKER_TRAP();
  a.Base = (byte*)p;
#endif
  a.Reserved = reserve;
  a.Committed = 0;
  a.Used = 0;
  return a;
}

void* ArenaAlloc(Arena* a, usize size, usize align) {
  usize mask = align - 1;
  usize pos = (a->Used + mask) & ~mask;
  usize newUsed = pos + size;
  FFI(newUsed > a->Reserved);
  if (newUsed > a->Committed) ArenaCommitTo(a, newUsed);
  a->Used = newUsed;
  return a->Base + pos;
}

void ArenaReset(Arena* a) { a->Used = 0; }

void ArenaTrim(Arena* a, usize keepBytes) {
  if (!a->Base) return;
  // Only ever safe once the arena's contents are dead: this hands the pages
  // back to the OS, so anything still pointing into them is reading unmapped
  // memory afterwards.
  FFI(a->Used > keepBytes);
  // Compared before rounding: RoundUp overflows to 0 near SIZE_MAX, which would
  // decommit the whole arena for a keep that covers all of it.
  if (keepBytes >= a->Committed) return;
  usize keep = RoundUp(keepBytes, ARENA_COMMIT_CHUNK);
  if (keep >= a->Committed) return;
#if defined(_WIN32)
  // Decommit leaves the range reserved, so Base stays valid and ArenaCommitTo
  // can commit it again.
  if (!VirtualFree(a->Base + keep, a->Committed - keep, MEM_DECOMMIT)) CLANKER_TRAP();
#else
  // MADV_DONTNEED drops the physical pages; PROT_NONE is what actually restores
  // the "not committed" state this arena's commit model is written against, so
  // ArenaCommitTo's mprotect on the next growth is not a no-op.
  if (mprotect(a->Base + keep, a->Committed - keep, PROT_NONE) != 0) CLANKER_TRAP();
  madvise(a->Base + keep, a->Committed - keep, MADV_DONTNEED);
#endif
  a->Committed = keep;
}

void ArenaRestore(Arena* a, usize pos) {
  FFI(pos > a->Used);
  a->Used = pos;
}

void ArenaRelease(Arena* a) {
  if (!a->Base) return;
#if defined(_WIN32)
  VirtualFree(a->Base, 0, MEM_RELEASE);
#else
  munmap(a->Base, a->Reserved);
#endif
  *a = Arena{};
}
