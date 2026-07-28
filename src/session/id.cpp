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
