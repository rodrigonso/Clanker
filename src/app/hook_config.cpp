#include "app/hook_config.h"

#include "app/config_paths.h"

#include "base/config_file.h"
#include "base/sysinfo.h"
#include "json/json.h"

namespace {

// A command entry is `{"type":"command","command":"...","timeout":N}`. Entries
// of any other type belong to a tool this build does not implement, so they are
// skipped rather than treated as a malformed file.
bool ReadCommandEntry(const JsonValue* entry, StrView matcher, HookCommand* out) {
  if (!Json_IsObject(entry)) return false;

  JsonValue* type = Json_Get(entry, "type");
  if (type && (!Json_IsString(type) || !StrView_Eq(type->Str, SVL("command")))) return false;

  JsonValue* command = Json_Get(entry, "command");
  if (!Json_IsString(command) || command->Str.Length == 0) return false;

  u32 timeoutMs = HOOK_DEFAULT_TIMEOUT_MS;
  JsonValue* timeout = Json_Get(entry, "timeout");
  if (timeout && timeout->Kind == JsonKind::Number && timeout->Number > 0.0 &&
      timeout->Number < 3600.0) {
    timeoutMs = (u32)(timeout->Number * 1000.0);
    // A sub-millisecond timeout truncates to zero, which the exec layer reads
    // as "wait forever" -- the exact opposite of what was asked for.
    if (timeoutMs == 0) timeoutMs = 1;
  }

  out->Command = command->Str;
  out->Matcher = matcher;
  out->TimeoutMs = timeoutMs;
  return true;
}

// One group is `{"matcher":"..","hooks":[...]}`. A bare command entry is also
// accepted so a hand-written file need not nest a single hook.
void ReadGroup(const JsonValue* group, Array<HookCommand>* out) {
  if (!Json_IsObject(group)) return;

  StrView matcher = {};
  JsonValue* matcherValue = Json_Get(group, "matcher");
  if (Json_IsString(matcherValue)) matcher = matcherValue->Str;

  JsonValue* entries = Json_Get(group, "hooks");
  if (!Json_IsArray(entries)) {
    HookCommand direct = {};
    if (ReadCommandEntry(group, matcher, &direct)) Array_Push(out, direct);
    return;
  }

  for (u32 i = 0; i < entries->Count; i++) {
    HookCommand command = {};
    if (ReadCommandEntry(entries->Items[i], matcher, &command)) Array_Push(out, command);
  }
}

}  // namespace

StrView HookConfig_DefaultPath(Arena* arena) {
  return ClankerDataPath(arena, "hooks.json");
}

HookConfigLoadResult HookConfig_Parse(Arena* arena, StrView text, HookConfig* out) {
  *out = HookConfig{};
  out->Events = Array_Make<HookEventBinding>(arena, 8);

  JsonValue* root = Json_Parse(arena, text);
  if (!Json_IsObject(root)) return HookConfigLoadResult::Malformed;

  // The table may sit under "hooks" or be the document itself, so a file
  // written by an editor and one written by hand both load.
  JsonValue* table = Json_Get(root, "hooks");
  if (!table) table = root;
  if (!Json_IsObject(table)) return HookConfigLoadResult::Malformed;

  for (u32 i = 0; i < table->MemberCount; i++) {
    const JsonMember* member = &table->Members[i];
    if (!member->Key.Length || !Json_IsArray(member->Value)) continue;

    HookEventBinding binding = {};
    binding.Event = member->Key;
    binding.Commands = Array_Make<HookCommand>(arena, 4);
    for (u32 j = 0; j < member->Value->Count; j++) {
      ReadGroup(member->Value->Items[j], &binding.Commands);
    }
    if (!binding.Commands.Count) continue;

    out->CommandCount += binding.Commands.Count;
    Array_Push(&out->Events, binding);
  }
  return HookConfigLoadResult::Success;
}

HookConfigLoadResult HookConfig_Load(Arena* arena, StrView path, HookConfig* out) {
  *out = HookConfig{};

  StrView content = {};
  ConfigFileReadResult read = ConfigFile_ReadAll(arena, path, &content);
  if (read == ConfigFileReadResult::Missing) return HookConfigLoadResult::Missing;
  if (read == ConfigFileReadResult::Failure) return HookConfigLoadResult::ReadFailure;
  return HookConfig_Parse(arena, content, out);
}

bool HookMatcher_Accepts(StrView matcher, StrView subject) {
  if (matcher.Length == 0) return true;
  // "*" is how the wider hook ecosystem spells "every tool", and a config
  // copied from there must not silently register commands that never fire.
  if (StrView_Eq(matcher, SVL("*"))) return true;
  // Events with no subject are not tool events, so there is nothing for a
  // matcher to narrow. Testing it would only ever reject, which would turn a
  // stray matcher on SessionStart into a hook that is registered yet dead.
  if (subject.Length == 0) return true;

  u32 start = 0;
  for (u32 i = 0; i <= matcher.Length; i++) {
    if (i != matcher.Length && matcher.Data[i] != '|') continue;
    StrView candidate = {matcher.Data + start, i - start};
    if (candidate.Length &&
        (StrView_Eq(candidate, subject) || StrView_Eq(candidate, SVL("*")))) {
      return true;
    }
    start = i + 1;
  }
  return false;
}

u32 HookConfig_Select(const HookConfig* config, StrView event, StrView subject,
                      const HookCommand** out, u32 capacity) {
  if (!config || !out || !capacity) return 0;

  u32 count = 0;
  for (u32 i = 0; i < config->Events.Count && count < capacity; i++) {
    const HookEventBinding* binding = &config->Events.Data[i];
    if (!StrView_Eq(binding->Event, event)) continue;
    for (u32 j = 0; j < binding->Commands.Count && count < capacity; j++) {
      const HookCommand* command = &binding->Commands.Data[j];
      if (!HookMatcher_Accepts(command->Matcher, subject)) continue;
      out[count++] = command;
    }
  }
  return count;
}
