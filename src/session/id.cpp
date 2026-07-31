#include "session/id.h"

#include <string.h>

#include "crypto/random.h"

DurableId DurableId_FromBytes(const byte bytes[DURABLE_ID_BYTE_COUNT]) {
  static const char* chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  DurableId id = {};
  u32 input = 0;
  u32 output = 0;
  while (input + 3 <= DURABLE_ID_BYTE_COUNT) {
    u32 n = ((u32)bytes[input] << 16) | ((u32)bytes[input + 1] << 8) |
            (u32)bytes[input + 2];
    id.Value[output++] = chars[(n >> 18) & 63];
    id.Value[output++] = chars[(n >> 12) & 63];
    id.Value[output++] = chars[(n >> 6) & 63];
    id.Value[output++] = chars[n & 63];
    input += 3;
  }
  u32 remaining = DURABLE_ID_BYTE_COUNT - input;
  if (remaining == 1) {
    u32 n = (u32)bytes[input] << 16;
    id.Value[output++] = chars[(n >> 18) & 63];
    id.Value[output++] = chars[(n >> 12) & 63];
  } else if (remaining == 2) {
    u32 n = ((u32)bytes[input] << 16) | ((u32)bytes[input + 1] << 8);
    id.Value[output++] = chars[(n >> 18) & 63];
    id.Value[output++] = chars[(n >> 12) & 63];
    id.Value[output++] = chars[(n >> 6) & 63];
  }
  FFI(output != DURABLE_ID_CHAR_COUNT);
  id.Value[output] = '\0';
  return id;
}

DurableId DurableId_Generate() {
  byte bytes[DURABLE_ID_BYTE_COUNT];
  RandomBytes(bytes, DURABLE_ID_BYTE_COUNT);
  return DurableId_FromBytes(bytes);
}

bool DurableId_Parse(StrView text, DurableId* out) {
  if (!out || text.Length != DURABLE_ID_CHAR_COUNT) return false;
  for (u32 i = 0; i < text.Length; i++) {
    char c = text.Data[i];
    bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!valid) return false;
  }
  DurableId id = {};
  memcpy(id.Value, text.Data, DURABLE_ID_CHAR_COUNT);
  id.Value[DURABLE_ID_CHAR_COUNT] = '\0';
  *out = id;
  return true;
}

bool DurableId_IsSet(DurableId id) { return id.Value[0] != '\0'; }

bool DurableId_Eq(DurableId a, DurableId b) {
  return memcmp(a.Value, b.Value, sizeof(a.Value)) == 0;
}

StrView DurableId_View(const DurableId* id) {
  if (!id || !DurableId_IsSet(*id)) return StrView{"", 0};
  return StrView{id->Value, DURABLE_ID_CHAR_COUNT};
}

u32 DurableIdIndex_Hash(DurableId id) {
  // FNV-1a over the fixed-width base64url character payload.
  u32 hash = 2166136261u;
  for (u32 i = 0; i < DURABLE_ID_CHAR_COUNT; i++) {
    hash ^= (u8)id.Value[i];
    hash *= 16777619u;
  }
  return hash;
}

namespace {
u32 DurableIdIndex_Capacity(u32 capacityHint) {
  FFI(capacityHint > (1u << 30));
  u32 cap = 16;
  u64 required = (u64)capacityHint * 2;
  while ((u64)cap < required) cap *= 2;
  return cap;
}
}  // namespace

usize DurableIdIndex_AllocationSize(u32 capacityHint) {
  return sizeof(DurableIdIndexSlot) *
         (usize)DurableIdIndex_Capacity(capacityHint);
}

void DurableIdIndex_Init(DurableIdIndex* index, Arena* arena, u32 capacityHint) {
  *index = DurableIdIndex{};
  index->Mem = arena;
  u32 cap = DurableIdIndex_Capacity(capacityHint);
  index->Capacity = cap;
  index->Slots = (DurableIdIndexSlot*)ArenaAlloc(
      arena, sizeof(DurableIdIndexSlot) * (usize)cap, alignof(DurableIdIndexSlot));
  memset(index->Slots, 0, sizeof(DurableIdIndexSlot) * (usize)cap);
}

namespace {
void DurableIdIndex_Rehash(DurableIdIndex* index, u32 minCapacity) {
  DurableIdIndexSlot* oldSlots = index->Slots;
  u32 oldCapacity = index->Capacity;
  u32 newCapacity = oldCapacity ? oldCapacity * 2 : 16;
  while (newCapacity < minCapacity) newCapacity *= 2;
  index->Slots = (DurableIdIndexSlot*)ArenaAlloc(
      index->Mem, sizeof(DurableIdIndexSlot) * (usize)newCapacity,
      alignof(DurableIdIndexSlot));
  memset(index->Slots, 0, sizeof(DurableIdIndexSlot) * (usize)newCapacity);
  index->Capacity = newCapacity;
  index->Count = 0;
  for (u32 i = 0; i < oldCapacity; i++) {
    if (oldSlots[i].Used)
      DurableIdIndex_Put(index, oldSlots[i].Id, oldSlots[i].RecordIndex);
  }
}
}  // namespace

void DurableIdIndex_Put(DurableIdIndex* index, DurableId id, u32 recordIndex) {
  if (!index->Capacity) DurableIdIndex_Init(index, index->Mem, 16);
  if ((u64)(index->Count + 1) * 10 >= (u64)index->Capacity * 7)
    DurableIdIndex_Rehash(index, index->Capacity * 2);
  u32 mask = index->Capacity - 1;
  u32 slot = DurableIdIndex_Hash(id) & mask;
  for (;;) {
    if (!index->Slots[slot].Used) {
      index->Slots[slot].Used = true;
      index->Slots[slot].Id = id;
      index->Slots[slot].RecordIndex = recordIndex;
      index->Count++;
      return;
    }
    if (DurableId_Eq(index->Slots[slot].Id, id)) {
      index->Slots[slot].RecordIndex = recordIndex;
      return;
    }
    slot = (slot + 1) & mask;
  }
}

bool DurableIdIndex_Find(const DurableIdIndex* index, DurableId id,
                        u32* outRecordIndex) {
  if (!index || !index->Capacity) return false;
  u32 mask = index->Capacity - 1;
  u32 slot = DurableIdIndex_Hash(id) & mask;
  for (u32 probes = 0; probes < index->Capacity; probes++) {
    if (!index->Slots[slot].Used) return false;
    if (DurableId_Eq(index->Slots[slot].Id, id)) {
      if (outRecordIndex) *outRecordIndex = index->Slots[slot].RecordIndex;
      return true;
    }
    slot = (slot + 1) & mask;
  }
  return false;
}

bool DurableIdIndex_Contains(const DurableIdIndex* index, DurableId id) {
  return DurableIdIndex_Find(index, id, nullptr);
}
