#include "auth/store.h"

#include <stdio.h>
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
    out->IdToken = Json_GetStr(v, "idToken");
    out->AccountId = Json_GetStr(v, "accountId");
    out->IsFedRamp = Json_GetBool(v, "isFedRamp", false);
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
    if (c->IdToken.Length) {
      StrBuf_AppendCStr(b, ",\"idToken\":");
      Json_WriteString(b, c->IdToken);
    }
    if (c->AccountId.Length) {
      StrBuf_AppendCStr(b, ",\"accountId\":");
      Json_WriteString(b, c->AccountId);
    }
    if (c->IsFedRamp)
      StrBuf_AppendCStr(b, ",\"isFedRamp\":true");
    StrBuf_AppendChar(b, '}');
  }
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
  StrView path = StrBuf_View(&p);

  FILE* current = fopen(StrBuf_CStr(&p), "rb");
  if (current) {
    fclose(current);
    return path;
  }

  StrBuf legacy = StrBuf_Make(a, 128);
  StrBuf_AppendCStr(&legacy, (home && *home) ? home : ".");
  StrBuf_AppendCStr(&legacy, "/.mini-agent/auth.json");
  FILE* old = fopen(StrBuf_CStr(&legacy), "rb");
  if (!old) return path;
  fclose(old);

  ConfigFile_EnsureParentDir(a, path);
  if (rename(StrBuf_CStr(&legacy), StrBuf_CStr(&p)) == 0) return path;

  fprintf(stderr, "warning: failed to migrate credentials to %s; using %s\n", StrBuf_CStr(&p),
          StrBuf_CStr(&legacy));
  return StrBuf_View(&legacy);
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
