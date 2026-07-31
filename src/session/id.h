#pragma once

#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

constexpr u32 DURABLE_ID_BYTE_COUNT = 16;
constexpr u32 DURABLE_ID_CHAR_COUNT = 22;

// Opaque 128-bit identifier encoded as unpadded base64url. The fixed-size,
// value-owned representation keeps durable references independent of arenas.
struct DurableId {
  char Value[DURABLE_ID_CHAR_COUNT + 1];
};

DurableId DurableId_Generate();
DurableId DurableId_FromBytes(const byte bytes[DURABLE_ID_BYTE_COUNT]);
bool DurableId_Parse(StrView text, DurableId* out);
bool DurableId_IsSet(DurableId id);
bool DurableId_Eq(DurableId a, DurableId b);
StrView DurableId_View(const DurableId* id);

// Arena-backed open-addressed hash index from DurableId to a caller-defined
// record position (typically an offset into a parallel records array). Used
// to make parent/target/duplicate-ID/leaf lookups constant time instead of
// linear scans over the whole session log.
struct DurableIdIndexSlot {
  DurableId Id;
  u32 RecordIndex;
  bool Used;
};

struct DurableIdIndex {
  Arena* Mem;
  DurableIdIndexSlot* Slots;
  u32 Capacity;
  u32 Count;
};

u32 DurableIdIndex_Hash(DurableId id);
usize DurableIdIndex_AllocationSize(u32 capacityHint);
void DurableIdIndex_Init(DurableIdIndex* index, Arena* arena, u32 capacityHint);
void DurableIdIndex_Put(DurableIdIndex* index, DurableId id, u32 recordIndex);
bool DurableIdIndex_Find(const DurableIdIndex* index, DurableId id,
                        u32* outRecordIndex);
bool DurableIdIndex_Contains(const DurableIdIndex* index, DurableId id);
