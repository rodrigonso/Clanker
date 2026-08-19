#include "auth/store.h"

#include <stdlib.h>

#include "base/array.h"
#include "base/config_file.h"
#include "json/json.h"

namespace {

struct Entry {
  StrView Id;
  Credential Cred;
};

bool ParseCred(JsonValue* v, Credential* out) {
  *out = {};
  if (!StrView_EqCStr(Json_GetStr(v, "type"), "api_key")) return false;
  out->ApiKey = Json_GetStr(v, "key");
  return true;
}

void WriteCred(StrBuf* b, const Credential* c) {
  StrBuf_AppendCStr(b, "{\"type\":\"api_key\",\"key\":");
  Json_WriteString(b, c->ApiKey);
  StrBuf_AppendChar(b, '}');
}

Array<Entry> LoadEntries(Arena* a, StrView path) {
  Array<Entry> entries = Array_Make<Entry>(a, 8);
  StrView content = {};
  if (ConfigFile_ReadAll(a, path, &content) != ConfigFileReadResult::Success) return entries;
  JsonValue* root = Json_Parse(a, content);
  if (!Json_IsObject(root)) return entries;
  for (u32 i = 0; i < root->MemberCount; i++) {
    Entry e;
    e.Id = root->Members[i].Key;
    if (ParseCred(root->Members[i].Value, &e.Cred)) Array_Push(&entries, e);
  }
  return entries;
}

bool WriteEntries(Arena* a, StrView path, const Array<Entry>* entries) {
  StrBuf out = StrBuf_Make(a, 512);
  StrBuf_AppendChar(&out, '{');
  for (u32 i = 0; i < entries->Count; i++) {
    if (i) StrBuf_AppendChar(&out, ',');
    Json_WriteString(&out, entries->Data[i].Id);
    StrBuf_AppendChar(&out, ':');
    WriteCred(&out, &entries->Data[i].Cred);
  }
  StrBuf_AppendChar(&out, '}');

  return ConfigFile_WriteAtomic(a, path, StrBuf_View(&out));
}

}  // namespace

StrView AuthStore_DefaultPath(Arena* a) {
#if defined(_WIN32)
  const char* home = getenv("USERPROFILE");
#else
  const char* home = getenv("HOME");
#endif
  StrBuf p = StrBuf_Make(a, 128);
  StrBuf_AppendCStr(&p, (home && *home) ? home : ".");
  StrBuf_AppendCStr(&p, "/.clank/auth.json");
  return StrBuf_View(&p);
}

bool AuthStore_Read(Arena* a, StrView path, StrView providerId, Credential* out) {
  Array<Entry> entries = LoadEntries(a, path);
  for (u32 i = 0; i < entries.Count; i++) {
    if (StrView_Eq(entries.Data[i].Id, providerId)) {
      *out = entries.Data[i].Cred;
      return true;
    }
  }
  return false;
}

bool AuthStore_Write(Arena* a, StrView path, StrView providerId, const Credential* cred) {
  Array<Entry> entries = LoadEntries(a, path);
  bool replaced = false;
  for (u32 i = 0; i < entries.Count; i++) {
    if (StrView_Eq(entries.Data[i].Id, providerId)) {
      entries.Data[i].Cred = *cred;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    Entry e;
    e.Id = StrView_Dup(a, providerId);
    e.Cred = *cred;
    Array_Push(&entries, e);
  }
  return WriteEntries(a, path, &entries);
}

bool AuthStore_Delete(Arena* a, StrView path, StrView providerId) {
  Array<Entry> entries = LoadEntries(a, path);
  Array<Entry> kept = Array_Make<Entry>(a, entries.Count + 1);
  for (u32 i = 0; i < entries.Count; i++) {
    if (!StrView_Eq(entries.Data[i].Id, providerId)) Array_Push(&kept, entries.Data[i]);
  }
  return WriteEntries(a, path, &kept);
}
