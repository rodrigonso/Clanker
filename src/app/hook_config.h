#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/basics.h"
#include "base/str.h"

// External lifecycle hooks: a table mapping event names to shell commands that
// receive a JSON payload on stdin. Deliberately its own file rather than a key
// in settings.json, because AppSettings_Save rewrites that file from its own
// known keys and would drop anything it does not model.
//
// Schema (the grouped form editors such as Briq already emit):
//   {"hooks": {"<Event>": [{"matcher": "a|b",
//                           "hooks": [{"type": "command",
//                                      "command": "...",
//                                      "timeout": 10}]}]}}
// `matcher` and `timeout` are optional.

constexpr u32 HOOK_DEFAULT_TIMEOUT_MS = 10000;

struct HookCommand {
  StrView Command;
  StrView Matcher;
  u32 TimeoutMs;
};

struct HookEventBinding {
  StrView Event;
  Array<HookCommand> Commands;
};

struct HookConfig {
  Array<HookEventBinding> Events;
  u32 CommandCount;
};

enum class HookConfigLoadResult : u8 {
  Success,
  Missing,
  Malformed,
  ReadFailure,
};

// Default path: <home>/.clank/hooks.json.
StrView HookConfig_DefaultPath(Arena* arena);

// Successful strings are owned by `arena`.
HookConfigLoadResult HookConfig_Load(Arena* arena, StrView path, HookConfig* out);
HookConfigLoadResult HookConfig_Parse(Arena* arena, StrView text, HookConfig* out);

// True when `matcher` accepts `subject`. A matcher is a '|'-separated
// alternation of exact names. Empty, "*", or an empty subject accept anything;
// an empty subject means a non-tool event, which a matcher cannot narrow.
bool HookMatcher_Accepts(StrView matcher, StrView subject);

// Commands registered for `event` whose matcher accepts `subject`, in
// configuration order. Returns the number written to `out`.
u32 HookConfig_Select(const HookConfig* config, StrView event, StrView subject,
                      const HookCommand** out, u32 capacity);
