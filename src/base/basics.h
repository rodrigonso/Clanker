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
#define MA_TRAP() __fastfail(0)
#else
#define MA_TRAP() __builtin_trap()
#endif

// Fastfail when the (invalid) condition holds. Mirrors the caboose FFI macro:
// FFI(x) traps if x is true, i.e. x describes the disallowed state.
#define FFI(cond)          \
  do {                     \
    if (cond) MA_TRAP();   \
  } while (0)

template <typename T>
inline T MaMin(T a, T b) {
  return a < b ? a : b;
}

// Low-resolution wall clock in milliseconds. Used for message timestamps.
inline u64 NowMillis() { return (u64)time(nullptr) * 1000ull; }
