#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;
using i64 = int64_t;
using i32 = int32_t;
using i16 = int16_t;
using i8 = int8_t;
using f32 = float;
using f64 = double;
using byte = uint8_t;
using usize = size_t;

#if defined(_MSC_VER)
#include <intrin.h>
#define CLANKER_TRAP() __fastfail(0)
#else
#define CLANKER_TRAP() __builtin_trap()
#endif

// Fastfail when the (invalid) condition holds. Mirrors the caboose FFI macro:
// FFI(x) traps if x is true, i.e. x describes the disallowed state.
#define FFI(cond)             \
  do {                        \
    if (cond) CLANKER_TRAP(); \
  } while (0)

template <typename T>
inline T ClankerMin(T a, T b) {
  return a < b ? a : b;
}

// Unsigned addition that clamps at the accumulator's maximum instead of
// wrapping. Used for token and byte totals, where wrapping would silently
// turn a huge total into a tiny one. The addend is converted to the
// accumulator type first, so it must not be the wider of the two.
template <typename T, typename U>
inline T AddSaturated(T a, U b) {
  T limit = (T)~(T)0;
  T addend = (T)b;
  return limit - a < addend ? limit : (T)(a + addend);
}

// Wall clock in milliseconds since the Unix epoch. Backs message timestamps,
// session CreatedAt, and credential expiry, so the unit has to mean what it
// says: this was `time(nullptr) * 1000` and only ever moved in 1000 ms steps.
//
// Wall clock, not monotonic -- it can jump when the system clock is adjusted.
// Use MonotonicMillis() from base/thread.h to measure a duration.
u64 NowMillis();