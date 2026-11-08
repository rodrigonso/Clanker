#pragma once

#include "app/extensions.h"
#include "app/hook_config.h"
#include "app/hook_gate.h"
#include "app/hook_runner.h"

// hooks.json as an extension: the synchronous PreToolUse gate on the harness,
// and the lifecycle notifications from ordered event delivery. With no hooks
// configured it fills no slot, starts no thread, and spawns nothing.
struct HookExtension {
  HookConfig Config;
  // Null under `--no-session`, which has no transcript to report and so
  // reports no lifecycle events, or when no command is configured.
  HookRunner* Runner;
  // Null when no PreToolUse command is configured. Unlike the runner it
  // survives `--no-session`: a policy deciding whether a tool may run has
  // nothing to do with whether the transcript is durable.
  HookGate* Gate;
};

// Loads `path`. A missing file disables hooks quietly; a malformed or
// unreadable one disables them with a warning on stderr, since a typo must not
// switch off every hook in silence. `abort` cancels a running gate command.
void HookExtension_Init(HookExtension* hooks, Arena* arena, StrView path,
                        bool durable, bool interactive, volatile i32* abort);

AppExtension HookExtension_Make(HookExtension* hooks);
