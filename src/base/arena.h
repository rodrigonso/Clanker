#pragma once
#include "base/basics.h"

// Cross-platform bump allocator. Reserves a large virtual range up front and
// commits pages lazily as `Used` grows, so allocations never relocate for the
// arena's lifetime (pointers into the arena stay stable). Single-threaded.
struct Arena {
  byte* Base;       // Base address of the reserved range
  usize Reserved;   // Total virtual bytes reserved
  usize Committed;  // Bytes currently committed (backed by pages)
  usize Used;       // Bytes handed out
};

// Default reservation: 1 GiB of virtual address space (not committed).
Arena ArenaCreate(usize reserveBytes = (usize)1 << 30);

void* ArenaAlloc(Arena* a, usize size, usize align);
void ArenaReset(Arena* a);            // Roll Used back to 0, keep committed pages
void ArenaRestore(Arena* a, usize pos);  // Roll Used back to a saved mark

// Hand committed pages above `keepBytes` back to the OS, keeping the
// reservation so the arena stays usable and grows again on demand.
//
// Deliberately separate from ArenaReset rather than folded into it: reset runs
// per streamed token, per rendered frame, per layout and per shell chunk, and a
// decommit syscall on those paths would cost far more than the pages are worth.
// Call this only where an arena is known to have spiked and then gone idle.
//
// Traps if `Used` is above `keepBytes` -- that would be decommitting live data.
void ArenaTrim(Arena* a, usize keepBytes);

void ArenaRelease(Arena* a);          // Release the whole reservation

#define ArenaPushStruct(a, T) (T*)ArenaAlloc((a), sizeof(T), alignof(T))
#define ArenaPushArray(a, T, n) (T*)ArenaAlloc((a), sizeof(T) * (usize)(n), alignof(T))
