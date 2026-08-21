#include "app/settings.h"

#include "app/config_paths.h"

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

bool ReadStringArray(Arena* arena, JsonValue* object, const char* key,
                     Array<StrView>* out) {
  *out = {};
  JsonValue* value = Json_Get(object, key);
  if (!value) return true;
  if (!Json_IsArray(value)) return false;
  if (value->Count) *out = Array_Make<StrView>(arena, value->Count);
  for (u32 i = 0; i < value->Count; i++) {
    JsonValue* entry = value->Items[i];
    if (!Json_IsString(entry) || !entry->Str.Length) return false;
    Array_Push(out, entry->Str);
  }
  return true;
}

void WriteStringArray(StrBuf* content, const char* key,
                      const Array<StrView>* values) {
  if (!values->Count) return;
  StrBuf_AppendCStr(content, ",\"");
  StrBuf_AppendCStr(content, key);
  StrBuf_AppendCStr(content, "\":[");
  for (u32 i = 0; i < values->Count; i++) {
    if (i) StrBuf_AppendChar(content, ',');
    Json_WriteString(content, values->Data[i]);
  }
  StrBuf_AppendChar(content, ']');
}

}  // namespace

StrView AppSettings_DefaultPath(Arena* a) {
  return ClankerDataPath(a, "settings.json");
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

  AgentRetryPolicy retry = AgentRetryPolicy_Default();
  JsonValue* retryValue = Json_Get(root, "retry");
  if (retryValue) {
    if (!Json_IsObject(retryValue)) return AppSettingsLoadResult::Malformed;
    JsonValue* enabled = Json_Get(retryValue, "enabled");
    if (enabled) {
      if (enabled->Kind != JsonKind::Bool) return AppSettingsLoadResult::Malformed;
      retry.Enabled = enabled->Bool;
    }
    // A zero retry count is meaningful here (retry off), unlike the token
    // counts above, so it is read directly rather than through ReadTokenCount.
    JsonValue* maxRetries = Json_Get(retryValue, "maxRetries");
    if (maxRetries) {
      if (maxRetries->Kind != JsonKind::Number || maxRetries->Number < 0.0 ||
          maxRetries->Number > 1000.0)
        return AppSettingsLoadResult::Malformed;
      retry.MaxRetries = (u32)maxRetries->Number;
      if ((f64)retry.MaxRetries != maxRetries->Number)
        return AppSettingsLoadResult::Malformed;
    }
    if (!ReadTokenCount(retryValue, "baseDelayMs", &retry.BaseDelayMs) ||
        !ReadTokenCount(retryValue, "maxDelayMs", &retry.MaxDelayMs))
      return AppSettingsLoadResult::Malformed;
  }

  AgentTurnLimitPolicy turnLimit = AgentTurnLimitPolicy_Default();
  JsonValue* turnLimitValue = Json_Get(root, "turnLimit");
  if (turnLimitValue) {
    if (!Json_IsObject(turnLimitValue)) return AppSettingsLoadResult::Malformed;
    JsonValue* enabled = Json_Get(turnLimitValue, "enabled");
    if (enabled) {
      if (enabled->Kind != JsonKind::Bool) return AppSettingsLoadResult::Malformed;
      turnLimit.Enabled = enabled->Bool;
    }
    // Zero is meaningful (no cap), so this is read directly rather than through
    // ReadTokenCount's positive-integer check.
    JsonValue* maxTurns = Json_Get(turnLimitValue, "maxTurns");
    if (maxTurns) {
      if (maxTurns->Kind != JsonKind::Number || maxTurns->Number < 0.0 ||
          maxTurns->Number > 100000.0)
        return AppSettingsLoadResult::Malformed;
      turnLimit.MaxTurns = (u32)maxTurns->Number;
      if ((f64)turnLimit.MaxTurns != maxTurns->Number)
        return AppSettingsLoadResult::Malformed;
    }
  }

  bool rounded = true;
  JsonValue* roundedValue = Json_Get(root, "rounded");
  if (roundedValue) {
    if (roundedValue->Kind != JsonKind::Bool)
      return AppSettingsLoadResult::Malformed;
    rounded = roundedValue->Bool;
  }

  Array<StrView> skills = {};
  Array<StrView> promptTemplates = {};
  if (!ReadStringArray(a, root, "skills", &skills) ||
      !ReadStringArray(a, root, "promptTemplates", &promptTemplates))
    return AppSettingsLoadResult::Malformed;

  AgentPermissionMode permissionMode = AgentPermissionMode::Auto;
  JsonValue* permissionModeValue = Json_Get(root, "permissionMode");
  if (permissionModeValue) {
    if (!Json_IsString(permissionModeValue) ||
        !AgentPermissionMode_Parse(permissionModeValue->Str, &permissionMode))
      return AppSettingsLoadResult::Malformed;
  }

  StrView permissionReviewerModel = AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL;
  JsonValue* permissionReviewerValue =
      Json_Get(root, "permissionReviewerModel");
  if (permissionReviewerValue) {
    if (!Json_IsString(permissionReviewerValue) ||
        !permissionReviewerValue->Str.Length)
      return AppSettingsLoadResult::Malformed;
    permissionReviewerModel = permissionReviewerValue->Str;
  }

  bool yoloAcknowledged = false;
  JsonValue* yoloAcknowledgedValue = Json_Get(root, "yoloAcknowledged");
  if (yoloAcknowledgedValue) {
    if (yoloAcknowledgedValue->Kind != JsonKind::Bool)
      return AppSettingsLoadResult::Malformed;
    yoloAcknowledged = yoloAcknowledgedValue->Bool;
  }

  out->Provider = provider->Str;
  out->Model = model->Str;
  out->DefaultTrust = defaultTrust;
  out->Profile = profile;
  out->Compaction = compaction;
  out->Retry = retry;
  out->TurnLimit = turnLimit;
  out->PermissionMode = permissionMode;
  out->PermissionReviewerModel = permissionReviewerModel;
  out->YoloAcknowledged = yoloAcknowledged;
  out->Rounded = rounded;
  out->Skills = skills;
  out->PromptTemplates = promptTemplates;
  return AppSettingsLoadResult::Success;
}

bool AppSettings_Save(Arena* a, StrView path, const AppSettings* settings) {
  StrView trust = DefaultProjectTrust_Name(settings->DefaultTrust);
  StrView effort = ModelEffort_Name(settings->Profile.Effort);
  StrView permissionMode = AgentPermissionMode_Name(settings->PermissionMode);
  if (!trust.Length ||
      !permissionMode.Length ||
      !settings->PermissionReviewerModel.Length ||
      (settings->Profile.Effort != ModelEffort::None && !effort.Length))
    return false;
  StrBuf content = StrBuf_Make(
      a, settings->Provider.Length + settings->Model.Length + trust.Length +
             effort.Length + settings->PermissionReviewerModel.Length + 192);
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
  // Same normalization as compaction: a zero delay would not survive the
  // strict loader's positive-integer check on the way back in.
  u32 baseDelayMs = settings->Retry.BaseDelayMs
                        ? settings->Retry.BaseDelayMs
                        : AGENT_RETRY_DEFAULT_BASE_DELAY_MS;
  u32 maxDelayMs = settings->Retry.MaxDelayMs
                       ? settings->Retry.MaxDelayMs
                       : AGENT_RETRY_DEFAULT_MAX_DELAY_MS;
  StrBuf_AppendCStr(&content, ",\"retry\":{\"enabled\":");
  StrBuf_AppendCStr(&content, settings->Retry.Enabled ? "true" : "false");
  StrBuf_AppendCStr(&content, ",\"maxRetries\":");
  StrBuf_AppendU64(&content, settings->Retry.MaxRetries);
  StrBuf_AppendCStr(&content, ",\"baseDelayMs\":");
  StrBuf_AppendU64(&content, baseDelayMs);
  StrBuf_AppendCStr(&content, ",\"maxDelayMs\":");
  StrBuf_AppendU64(&content, maxDelayMs);
  StrBuf_AppendChar(&content, '}');
  StrBuf_AppendCStr(&content, ",\"turnLimit\":{\"enabled\":");
  StrBuf_AppendCStr(&content, settings->TurnLimit.Enabled ? "true" : "false");
  StrBuf_AppendCStr(&content, ",\"maxTurns\":");
  StrBuf_AppendU64(&content, settings->TurnLimit.MaxTurns);
  StrBuf_AppendChar(&content, '}');
  StrBuf_AppendCStr(&content, ",\"permissionMode\":");
  Json_WriteString(&content, permissionMode);
  StrBuf_AppendCStr(&content, ",\"permissionReviewerModel\":");
  Json_WriteString(&content, settings->PermissionReviewerModel);
  StrBuf_AppendCStr(&content, ",\"yoloAcknowledged\":");
  StrBuf_AppendCStr(&content, settings->YoloAcknowledged ? "true" : "false");
  StrBuf_AppendCStr(&content, ",\"rounded\":");
  StrBuf_AppendCStr(&content, settings->Rounded ? "true" : "false");
  WriteStringArray(&content, "skills", &settings->Skills);
  WriteStringArray(&content, "promptTemplates", &settings->PromptTemplates);
  StrBuf_AppendChar(&content, '}');
  return ConfigFile_WriteAtomic(a, path, StrBuf_View(&content));
}
