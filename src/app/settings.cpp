#include "app/settings.h"

#include "base/config_file.h"
#include "base/sysinfo.h"
#include "json/json.h"

namespace {

// Optional positive token count. Absent leaves `*io` at its default; present
// but not a positive JSON-safe integer is a hard parse failure.
bool ReadTokenCount(JsonValue* object, const char* key, u32* io) {
  JsonValue* value = Json_Get(object, key);
  if (!value) return true;
  if (value->Kind != JsonKind::Number || value->Number < 1.0 ||
      value->Number > 4294967295.0)
    return false;
  u32 parsed = (u32)value->Number;
  if ((f64)parsed != value->Number) return false;
  *io = parsed;
  return true;
}

}  // namespace

StrView AppSettings_DefaultPath(Arena* a) {
  StrView home = Sys_HomeDir(a);
  StrBuf path = StrBuf_Make(a, home.Length + 24);
  if (home.Length > 0)
    StrBuf_Append(&path, home);
  else
    StrBuf_AppendChar(&path, '.');
  StrBuf_AppendCStr(&path, "/.clank/settings.json");
  return StrBuf_View(&path);
}

AppSettingsLoadResult AppSettings_Load(Arena* a, StrView path, AppSettings* out) {
  *out = AppSettings{};
  StrView content = {};
  ConfigFileReadResult read = ConfigFile_ReadAll(a, path, &content);
  if (read == ConfigFileReadResult::Missing) return AppSettingsLoadResult::Missing;
  if (read == ConfigFileReadResult::Failure) return AppSettingsLoadResult::ReadFailure;

  JsonValue* root = Json_Parse(a, content);
  if (!Json_IsObject(root)) return AppSettingsLoadResult::Malformed;
  JsonValue* provider = Json_Get(root, "provider");
  JsonValue* model = Json_Get(root, "model");
  if (!Json_IsString(provider) || !Json_IsString(model) || provider->Str.Length == 0 || model->Str.Length == 0) {
    return AppSettingsLoadResult::Malformed;
  }
  DefaultProjectTrust defaultTrust = DefaultProjectTrust::Ask;
  JsonValue* trust = Json_Get(root, "defaultProjectTrust");
  if (trust &&
      (!Json_IsString(trust) ||
       !DefaultProjectTrust_Parse(trust->Str, &defaultTrust)))
    return AppSettingsLoadResult::Malformed;
  ModelRequestProfile profile = {};
  JsonValue* effortValue = Json_Get(root, "effort");
  if (effortValue) {
    if (!Json_IsString(effortValue) || !effortValue->Str.Length ||
        !ModelEffort_Parse(effortValue->Str, &profile.Effort))
      return AppSettingsLoadResult::Malformed;
  }
  JsonValue* contextValue = Json_Get(root, "contextWindow");
  if (contextValue) {
    if (contextValue->Kind != JsonKind::Number ||
        contextValue->Number < 1.0 ||
        contextValue->Number > 4294967295.0)
      return AppSettingsLoadResult::Malformed;
    profile.ContextWindow = (u32)contextValue->Number;
    if ((f64)profile.ContextWindow != contextValue->Number)
      return AppSettingsLoadResult::Malformed;
  }

  AgentCompactionPolicy compaction = AgentCompactionPolicy_Default();
  JsonValue* compactionValue = Json_Get(root, "compaction");
  if (compactionValue) {
    if (!Json_IsObject(compactionValue)) return AppSettingsLoadResult::Malformed;
    JsonValue* enabled = Json_Get(compactionValue, "enabled");
    if (enabled) {
      if (enabled->Kind != JsonKind::Bool) return AppSettingsLoadResult::Malformed;
      compaction.Enabled = enabled->Bool;
    }
    if (!ReadTokenCount(compactionValue, "reserveTokens", &compaction.ReserveTokens) ||
        !ReadTokenCount(compactionValue, "keepRecentTokens", &compaction.KeepRecentTokens))
      return AppSettingsLoadResult::Malformed;
  }

  out->Provider = provider->Str;
  out->Model = model->Str;
  out->DefaultTrust = defaultTrust;
  out->Profile = profile;
  out->Compaction = compaction;
  return AppSettingsLoadResult::Success;
}

bool AppSettings_Save(Arena* a, StrView path, const AppSettings* settings) {
  StrView trust = DefaultProjectTrust_Name(settings->DefaultTrust);
  StrView effort = ModelEffort_Name(settings->Profile.Effort);
  if (!trust.Length ||
      (settings->Profile.Effort != ModelEffort::None && !effort.Length))
    return false;
  StrBuf content = StrBuf_Make(
      a, settings->Provider.Length + settings->Model.Length + trust.Length +
             effort.Length + 128);
  StrBuf_AppendCStr(&content, "{\"provider\":");
  Json_WriteString(&content, settings->Provider);
  StrBuf_AppendCStr(&content, ",\"model\":");
  Json_WriteString(&content, settings->Model);
  StrBuf_AppendCStr(&content, ",\"defaultProjectTrust\":");
  Json_WriteString(&content, trust);
  if (settings->Profile.Effort != ModelEffort::None) {
    StrBuf_AppendCStr(&content, ",\"effort\":");
    Json_WriteString(&content, effort);
  }
  if (settings->Profile.ContextWindow) {
    StrBuf_AppendCStr(&content, ",\"contextWindow\":");
    StrBuf_AppendU64(&content, settings->Profile.ContextWindow);
  }
  // Normalize on write: a zero token count would round-trip into a file the
  // strict loader rejects, leaving the user with unreadable settings.
  u32 reserveTokens = settings->Compaction.ReserveTokens
                          ? settings->Compaction.ReserveTokens
                          : AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS;
  u32 keepRecentTokens = settings->Compaction.KeepRecentTokens
                             ? settings->Compaction.KeepRecentTokens
                             : AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS;
  StrBuf_AppendCStr(&content, ",\"compaction\":{\"enabled\":");
  StrBuf_AppendCStr(&content, settings->Compaction.Enabled ? "true" : "false");
  StrBuf_AppendCStr(&content, ",\"reserveTokens\":");
  StrBuf_AppendU64(&content, reserveTokens);
  StrBuf_AppendCStr(&content, ",\"keepRecentTokens\":");
  StrBuf_AppendU64(&content, keepRecentTokens);
  StrBuf_AppendChar(&content, '}');
  StrBuf_AppendChar(&content, '}');
  return ConfigFile_WriteAtomic(a, path, StrBuf_View(&content));
}
