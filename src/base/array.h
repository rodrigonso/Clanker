#pragma once
#include <string.h>

#include "base/arena.h"
#include "base/basics.h"

// Arena-backed growable array. Growth reallocates within the backing arena
// (the old block is abandoned, which is the arena tradeoff); size with a
// sensible initial capacity to avoid churn.
template <typename T>
struct Array {
  T* Data;
  u32 Count;
  u32 Capacity;
  Arena* Backing;
};

namespace ArrayInternal {
template <typename T>
inline void Grow(Array<T>* a, u32 minCapacity) {
  u32 newCap = a->Capacity ? a->Capacity * 2 : 8;
  while (newCap < minCapacity) newCap *= 2;
  T* nd = (T*)ArenaAlloc(a->Backing, sizeof(T) * (usize)newCap, alignof(T));
  if (a->Count) memcpy(nd, a->Data, sizeof(T) * (usize)a->Count);
  a->Data = nd;
  a->Capacity = newCap;
}
}  // namespace ArrayInternal

template <typename T>
inline Array<T> Array_Make(Arena* arena, u32 initialCapacity = 8) {
  if (initialCapacity < 1) initialCapacity = 1;
  Array<T> r;
  r.Backing = arena;
  r.Count = 0;
  r.Capacity = initialCapacity;
  r.Data = (T*)ArenaAlloc(arena, sizeof(T) * (usize)initialCapacity, alignof(T));
  return r;
}

template <typename T>
inline T* Array_Push(Array<T>* a, const T& value) {
  if (a->Count >= a->Capacity) ArrayInternal::Grow(a, a->Count + 1);
  a->Data[a->Count] = value;
  return &a->Data[a->Count++];
}

template <typename T>
inline T* Array_PushEmpty(Array<T>* a) {
  if (a->Count >= a->Capacity) ArrayInternal::Grow(a, a->Count + 1);
  T* slot = &a->Data[a->Count++];
  *slot = T{};
  return slot;
}

template <typename T>
inline T* Array_Get(Array<T>* a, u32 index) {
  return index < a->Count ? &a->Data[index] : nullptr;
}

template <typename T>
inline T* Array_Last(Array<T>* a) {
  return a->Count ? &a->Data[a->Count - 1] : nullptr;
}

template <typename T>
inline T Array_Pop(Array<T>* a) {
  FFI(a->Count == 0);
  return a->Data[--a->Count];
}

template <typename T>
inline void Array_Clear(Array<T>* a) {
  a->Count = 0;
}
