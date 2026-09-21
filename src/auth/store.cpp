#include "auth/store.h"

#include "base/array.h"
#include "base/config_file.h"
#include "base/sysinfo.h"
#include "json/json.h"

namespace {

struct Entry {
  StrView Id;
  Credential Cred;
};

bool CompleteStoreDocument(StrView content) {
  // The shared parser accepts unterminated empty containers. Bound nesting and
  // require closed strings/containers before inspecting the credential schema.
  char stack[16] = {};
  u32 depth = 0;
  bool quoted = false;
  for (u32 i = 0; i < content.Length; i++) {
    char c = content.Data[i];
    if (quoted) {
      if ((u8)c < 0x20) return false;
      if (c == '\\') {
        if (++i == content.Length) return false;
      } else if (c == '"') {
        quoted = false;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == '{' || c == '[') {
      if (depth == sizeof(stack)) return false;
      stack[depth++] = c;
    } else if (c == '}' || c == ']') {
      if (!depth || stack[--depth] != (c == '}' ? '{' : '[')) return false;
    } else if ((u8)c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
      return false;
    }
  }
  return !quoted && depth == 0;
}

bool ParseCred(JsonValue* v, Credential* out) {
  *out = {};
  if (!Json_IsObject(v)) return false;
  for (u32 i = 0; i < v->MemberCount; i++)
    for (u32 j = 0; j < i; j++)
      if (StrView_Eq(v->Members[i].Key, v->Members[j].Key)) return false;
  if (!StrView_EqCStr(Json_GetStr(v, "type"), "api_key")) return false;
  JsonValue* key = Json_Get(v, "key");
  if (!key || key->Kind != JsonKind::String) return false;
  out->ApiKey = Json_GetStr(v, "key");
  return true;
}

void WriteCred(StrBuf* b, const Credential* c) {
  StrBuf_AppendCStr(b, "{\"type\":\"api_key\",\"key\":");
  Json_WriteString(b, c->ApiKey);
  StrBuf_AppendChar(b, '}');
}

bool LoadEntries(Arena* a, StrView path, Array<Entry>* entries) {
  *entries = Array_Make<Entry>(a, 8);
  StrView content = {};
  ConfigFileReadResult loaded = ConfigFile_ReadAll(a, path, &content);
  if (loaded == ConfigFileReadResult::Missing) return true;
  if (loaded != ConfigFileReadResult::Success || !CompleteStoreDocument(content))
    return false;
  JsonValue* root = Json_Parse(a, content);
  if (!Json_IsObject(root)) return false;
  for (u32 i = 0; i < root->MemberCount; i++) {
    Entry e = {};
    e.Id = root->Members[i].Key;
    JsonValue* value = root->Members[i].Value;
    if (!ParseCred(value, &e.Cred) || value->MemberCount != 2) return false;
    for (u32 j = 0; j < entries->Count; j++)
      if (StrView_Eq(entries->Data[j].Id, e.Id)) return false;
    Array_Push(entries, e);
  }
  return true;
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
  StrView home = Sys_HomeDir(a);
  StrBuf p = StrBuf_Make(a, home.Length + 24);
  StrBuf_Append(&p, home.Length ? home : SVL("."));
  StrBuf_AppendCStr(&p, "/.clanker/auth.json");
  return StrBuf_View(&p);
}

bool AuthStore_Read(Arena* a, StrView path, StrView providerId, Credential* out) {
  *out = {};
  StrView content = {};
  if (ConfigFile_ReadAll(a, path, &content) != ConfigFileReadResult::Success)
    return false;
  if (!CompleteStoreDocument(content)) return false;
  JsonValue* root = Json_Parse(a, content);
  if (!Json_IsObject(root)) return false;
  JsonValue* credential = nullptr;
  for (u32 i = 0; i < root->MemberCount; i++) {
    if (!StrView_Eq(root->Members[i].Key, providerId)) continue;
    if (credential) return false;
    credential = root->Members[i].Value;
  }
  return ParseCred(credential, out);
}

bool AuthStore_Write(Arena* a, StrView path, StrView providerId, const Credential* cred) {
  Array<Entry> entries = {};
  if (!LoadEntries(a, path, &entries)) return false;
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
  Array<Entry> entries = {};
  if (!LoadEntries(a, path, &entries)) return false;
  Array<Entry> kept = Array_Make<Entry>(a, entries.Count + 1);
  for (u32 i = 0; i < entries.Count; i++) {
    if (!StrView_Eq(entries.Data[i].Id, providerId)) Array_Push(&kept, entries.Data[i]);
  }
  return WriteEntries(a, path, &kept);
}
