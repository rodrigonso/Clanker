#include "auth/store.h"

#include <stdio.h>
#include <stdlib.h>

#include "base/array.h"
#include "json/json.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace {

struct Entry {
  StrView Id;
  Credential Cred;
};

StrView ReadFileAll(Arena* a, StrView path) {
  StrBuf p = StrBuf_Make(a, path.Length + 1);
  StrBuf_Append(&p, path);
  FILE* f = fopen(StrBuf_CStr(&p), "rb");
  if (!f) return StrView{nullptr, 0};
  StrBuf b = StrBuf_Make(a, 4096);
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) StrBuf_Append(&b, StrView{buf, (u32)n});
  fclose(f);
  return StrBuf_View(&b);
}

bool ParseCred(JsonValue* v, Credential* out) {
  *out = {};
  StrView type = Json_GetStr(v, "type");
  if (StrView_EqCStr(type, "api_key")) {
    out->Kind = CredKind::ApiKey;
    out->ApiKey = Json_GetStr(v, "key");
    return true;
  }
  if (StrView_EqCStr(type, "oauth")) {
    out->Kind = CredKind::OAuth;
    out->Access = Json_GetStr(v, "access");
    out->Refresh = Json_GetStr(v, "refresh");
    out->ExpiresMs = (u64)Json_GetInt(v, "expires", 0);
    return true;
  }
  return false;
}

void WriteCred(StrBuf* b, const Credential* c) {
  if (c->Kind == CredKind::ApiKey) {
    StrBuf_AppendCStr(b, "{\"type\":\"api_key\",\"key\":");
    Json_WriteString(b, c->ApiKey);
    StrBuf_AppendChar(b, '}');
  } else {
    StrBuf_AppendCStr(b, "{\"type\":\"oauth\",\"access\":");
    Json_WriteString(b, c->Access);
    StrBuf_AppendCStr(b, ",\"refresh\":");
    Json_WriteString(b, c->Refresh);
    StrBuf_AppendCStr(b, ",\"expires\":");
    StrBuf_AppendU64(b, c->ExpiresMs);
    StrBuf_AppendChar(b, '}');
  }
}

Array<Entry> LoadEntries(Arena* a, StrView path) {
  Array<Entry> entries = Array_Make<Entry>(a, 8);
  StrView content = ReadFileAll(a, path);
  if (!content.Data) return entries;
  JsonValue* root = Json_Parse(a, content);
  if (!Json_IsObject(root)) return entries;
  for (u32 i = 0; i < root->MemberCount; i++) {
    Entry e;
    e.Id = root->Members[i].Key;
    if (ParseCred(root->Members[i].Value, &e.Cred)) Array_Push(&entries, e);
  }
  return entries;
}

void EnsureParentDir(Arena* a, StrView path) {
  i32 slash = -1;
  for (u32 i = 0; i < path.Length; i++)
    if (path.Data[i] == '/' || path.Data[i] == '\\') slash = (i32)i;
  if (slash <= 0) return;
  StrBuf d = StrBuf_Make(a, (u32)slash + 1);
  StrBuf_Append(&d, StrView{path.Data, (u32)slash});
#if defined(_WIN32)
  _mkdir(StrBuf_CStr(&d));
#else
  mkdir(StrBuf_CStr(&d), 0700);
#endif
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

  EnsureParentDir(a, path);
  StrBuf pz = StrBuf_Make(a, path.Length + 1);
  StrBuf_Append(&pz, path);
  StrBuf tmp = StrBuf_Make(a, path.Length + 8);
  StrBuf_Append(&tmp, path);
  StrBuf_AppendCStr(&tmp, ".tmp");

  FILE* f = fopen(StrBuf_CStr(&tmp), "wb");
  if (!f) return false;
  fwrite(out.Data, 1, out.Length, f);
  fclose(f);

  remove(StrBuf_CStr(&pz));  // Windows rename will not overwrite
  if (rename(StrBuf_CStr(&tmp), StrBuf_CStr(&pz)) != 0) return false;
#if !defined(_WIN32)
  chmod(StrBuf_CStr(&pz), 0600);
#endif
  return true;
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
  StrBuf_AppendCStr(&p, "/.mini-agent/auth.json");
  return StrBuf_View(&p);
}

bool AuthStore_Read(Arena* a, StrView path, StrView providerId, Credential* out) {
  StrView content = ReadFileAll(a, path);
  if (!content.Data) return false;
  JsonValue* root = Json_Parse(a, content);
  if (!Json_IsObject(root)) return false;
  for (u32 i = 0; i < root->MemberCount; i++) {
    if (StrView_Eq(root->Members[i].Key, providerId)) return ParseCred(root->Members[i].Value, out);
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
