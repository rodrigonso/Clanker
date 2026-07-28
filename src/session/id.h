#pragma once

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
