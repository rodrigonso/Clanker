#include "app/model_cache.h"

#include "base/config_file.h"
#include "base/file_io.h"
#include "json/json.h"
#include "tools/fs.h"

namespace {

StrView CachePath(Arena* arena, StrView agentDirectory, StrView providerId) {
  StrBuf name = StrBuf_Make(arena, providerId.Length + 6);
  StrBuf_Append(&name, providerId);
  StrBuf_AppendCStr(&name, ".json");
  return Fs_Join(arena, Fs_Join(arena, agentDirectory, SVL("models")),
                 StrBuf_View(&name));
}

void WriteModel(StrBuf* out, const Model* model) {
  JsonWriter w;
  JsonWrite_Begin(&w, out);
  JsonWrite_String(&w, "id", model->Id);
  if (model->Name.Length && !StrView_Eq(model->Name, model->Id))
    JsonWrite_String(&w, "name", model->Name);
  if (model->ContextWindow)
    JsonWrite_U64(&w, "contextWindow", model->ContextWindow);
  if (model->ContextWindowOptionCount) {
    JsonWrite_Key(&w, "contextWindowOptions");
    StrBuf_AppendChar(out, '[');
    for (u32 i = 0; i < model->ContextWindowOptionCount; i++) {
      if (i) StrBuf_AppendChar(out, ',');
      StrBuf_AppendU64(out, model->ContextWindowOptions[i]);
    }
    StrBuf_AppendChar(out, ']');
  }
  if (model->Reasoning) JsonWrite_Bool(&w, "reasoning", true);
  if (model->EffortMask) {
    JsonWrite_Key(&w, "efforts");
    StrBuf_AppendChar(out, '[');
    bool first = true;
    for (u8 i = (u8)ModelEffort::Low; i <= (u8)ModelEffort::Ultra; i++) {
      if (!Model_SupportsEffort(model, (ModelEffort)i)) continue;
      if (!first) StrBuf_AppendChar(out, ',');
      first = false;
      Json_WriteString(out, ModelEffort_Name((ModelEffort)i));
    }
    StrBuf_AppendChar(out, ']');
  }
  if (model->DefaultEffort != ModelEffort::None)
    JsonWrite_String(&w, "defaultEffort", ModelEffort_Name(model->DefaultEffort));
  JsonWrite_End(&w);
}

// A positive integral number that fits in u32, else 0.
u32 PositiveU32(const JsonValue* value) {
  if (!value || value->Kind != JsonKind::Number) return 0;
  f64 number = value->Number;
  if (!(number >= 1 && number <= 4294967295.0) || number != (f64)(u64)number)
    return 0;
  return (u32)number;
}

// Fills the catalog fields of `model` from its cache entry. Effort names this
// build does not know are skipped, so a cache written by a newer Clanker still
// applies; anything else malformed rejects the entry.
bool ReadEntry(const JsonValue* entry, Model* model) {
  model->ContextWindow = PositiveU32(Json_Get(entry, "contextWindow"));
  JsonValue* options = Json_Get(entry, "contextWindowOptions");
  if (options) {
    if (!Json_IsArray(options) ||
        options->Count > MODEL_CONTEXT_WINDOW_OPTION_CAPACITY)
      return false;
    for (u32 i = 0; i < options->Count; i++) {
      u32 window = PositiveU32(options->Items[i]);
      if (!window) return false;
      model->ContextWindowOptions[model->ContextWindowOptionCount++] = window;
    }
  }
  model->Reasoning = Json_GetBool(entry, "reasoning", false);
  JsonValue* efforts = Json_Get(entry, "efforts");
  if (Json_IsArray(efforts)) {
    for (u32 i = 0; i < efforts->Count; i++) {
      ModelEffort effort = ModelEffort::None;
      if (Json_IsString(efforts->Items[i]) &&
          ModelEffort_Parse(efforts->Items[i]->Str, &effort))
        model->EffortMask |= ModelEffort_Bit(effort);
    }
  }
  ModelEffort defaultEffort = ModelEffort::None;
  if (ModelEffort_Parse(Json_GetStr(entry, "defaultEffort"), &defaultEffort))
    model->DefaultEffort = defaultEffort;
  return Model_MetadataIsValid(model);
}

}  // namespace

bool ModelCache_Save(StrView agentDirectory, StrView providerId,
                     const Model* models, u32 count) {
  if (!agentDirectory.Length || !providerId.Length) return false;
  Arena scratch = ArenaCreate();
  StrBuf json = StrBuf_Make(&scratch, 16 + count * 128);
  StrBuf_AppendCStr(&json, "{\"models\":[");
  for (u32 i = 0; i < count; i++) {
    if (i) StrBuf_AppendChar(&json, ',');
    WriteModel(&json, &models[i]);
  }
  StrBuf_AppendCStr(&json, "]}");
  // ConfigFile_WriteAtomic creates only the immediate parent directory.
  bool saved =
      File_MakeDir(&scratch, agentDirectory) &&
      ConfigFile_WriteAtomic(&scratch,
                             CachePath(&scratch, agentDirectory, providerId),
                             StrBuf_View(&json));
  ArenaRelease(&scratch);
  return saved;
}

Model ModelCache_CreateModel(Arena* arena, StrView agentDirectory,
                             const ProviderSpec* spec, StrView modelId) {
  Model model = spec->CreateModel(arena, spec, modelId);
  if (!agentDirectory.Length) return model;
  Arena scratch = ArenaCreate();
  StrView content = {};
  JsonValue* root = nullptr;
  if (ConfigFile_ReadAll(&scratch, CachePath(&scratch, agentDirectory, spec->Id),
                         &content) == ConfigFileReadResult::Success)
    root = Json_Parse(&scratch, content);
  JsonValue* entries = Json_Get(root, "models");
  for (u32 i = 0; Json_IsArray(entries) && i < entries->Count; i++) {
    const JsonValue* entry = entries->Items[i];
    if (!StrView_Eq(Json_GetStr(entry, "id"), model.Id)) continue;
    Model cached = model;
    if (ReadEntry(entry, &cached)) {
      StrView name = Json_GetStr(entry, "name");
      if (name.Length) cached.Name = StrView_Dup(arena, name);
      model = cached;
    }
    break;
  }
  ArenaRelease(&scratch);
  return model;
}
