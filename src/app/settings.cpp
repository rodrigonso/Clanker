#include "app/settings.h"

#include "base/config_file.h"
#include "base/sysinfo.h"
#include "json/json.h"

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

  out->Provider = provider->Str;
  out->Model = model->Str;
  out->DefaultTrust = defaultTrust;
  return AppSettingsLoadResult::Success;
}

bool AppSettings_Save(Arena* a, StrView path, const AppSettings* settings) {
  StrView trust = DefaultProjectTrust_Name(settings->DefaultTrust);
  if (!trust.Length) return false;
  StrBuf content = StrBuf_Make(
      a, settings->Provider.Length + settings->Model.Length + trust.Length + 72);
  StrBuf_AppendCStr(&content, "{\"provider\":");
  Json_WriteString(&content, settings->Provider);
  StrBuf_AppendCStr(&content, ",\"model\":");
  Json_WriteString(&content, settings->Model);
  StrBuf_AppendCStr(&content, ",\"defaultProjectTrust\":");
  Json_WriteString(&content, trust);
  StrBuf_AppendChar(&content, '}');
  return ConfigFile_WriteAtomic(a, path, StrBuf_View(&content));
}
