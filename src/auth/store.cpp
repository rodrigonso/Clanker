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

// JSON numbers are doubles; an expiry must be a whole millisecond count that
// survives the round trip exactly.
constexpr f64 MAX_EXACT_TIMESTAMP_MS = 9007199254740992.0;

bool IsTimestamp(JsonValue* v) {
  return v && v->Kind == JsonKind::Number && v->Number >= 0 &&
         v->Number <= MAX_EXACT_TIMESTAMP_MS && (f64)(u64)v->Number == v->Number;
}

bool ParseCred(JsonValue* v, Credential* out) {
  *out = {};
  if (!Json_IsObject(v)) return false;
  for (u32 i = 0; i < v->MemberCount; i++)
    for (u32 j = 0; j < i; j++)
      if (StrView_Eq(v->Members[i].Key, v->Members[j].Key)) return false;
  StrView type = Json_GetStr(v, "type");
  if (StrView_EqCStr(type, "api_key")) {
    JsonValue* key = Json_Get(v, "key");
    if (!key || key->Kind != JsonKind::String) return false;
    out->Kind = CredentialKind::ApiKey;
    out->ApiKey = key->Str;
    return true;
  }
  if (!StrView_EqCStr(type, "oauth")) return false;
  JsonValue* access = Json_Get(v, "access");
  JsonValue* refresh = Json_Get(v, "refresh");
  JsonValue* expires = Json_Get(v, "expires");
  JsonValue* account = Json_Get(v, "accountId");
  if (!Json_IsString(access) || !access->Str.Length || !Json_IsString(refresh) ||
      !refresh->Str.Length || !IsTimestamp(expires) ||
      (account && !Json_IsString(account)))
    return false;
  out->Kind = CredentialKind::OAuth;
  out->AccessToken = access->Str;
  out->RefreshToken = refresh->Str;
  out->ExpiresAtMs = (u64)expires->Number;
  if (account) out->AccountId = account->Str;
  return true;
}

// Rewriting the store must reproduce every member it read, so an entry with
// members this build does not write is refused rather than silently dropped.
u32 SchemaMemberCount(JsonValue* v, const Credential* c) {
  if (c->Kind == CredentialKind::ApiKey) return 2;
  return Json_Get(v, "accountId") ? 5 : 4;
}

bool CredIsWritable(const Credential* c) {
  if (c->Kind == CredentialKind::ApiKey) return true;
  return c->Kind == CredentialKind::OAuth && c->AccessToken.Length &&
         c->RefreshToken.Length && c->ExpiresAtMs <= (u64)MAX_EXACT_TIMESTAMP_MS;
}

void WriteCred(StrBuf* b, const Credential* c) {
  if (c->Kind == CredentialKind::OAuth) {
    StrBuf_AppendCStr(b, "{\"type\":\"oauth\",\"access\":");
    Json_WriteString(b, c->AccessToken);
    StrBuf_AppendCStr(b, ",\"refresh\":");
    Json_WriteString(b, c->RefreshToken);
    StrBuf_AppendCStr(b, ",\"expires\":");
    StrBuf_AppendU64(b, c->ExpiresAtMs);
    if (c->AccountId.Length) {
      StrBuf_AppendCStr(b, ",\"accountId\":");
      Json_WriteString(b, c->AccountId);
    }
    StrBuf_AppendChar(b, '}');
    return;
  }
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
    if (!ParseCred(value, &e.Cred) ||
        value->MemberCount != SchemaMemberCount(value, &e.Cred))
      return false;
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
  if (!cred || !CredIsWritable(cred)) return false;
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
