#pragma once
#include "agent/primitives.h"
#include "app/hook_config.h"
#include "app/hook_runner.h"
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"
#include "tools/exec.h"

// Synchronous PreToolUse evaluation: pi's `beforeToolCall` seam, with the
// policy supplied by external commands rather than by JS extensions. Clanker
// ships no policy of its own, which is the same posture pi takes.
//
// Deliberately separate from HookRunner's fire-and-forget queue. Every other
// event is a notification whose output is ignored; this one is a question whose
// answer decides whether a tool runs at all, so it runs on the caller's thread
// during the clearance phase and reads both stdout and the exit code.
struct HookGate;

// Returns nullptr when no PreToolUse command is registered, so a config without
// one costs nothing and spawns nothing. `config` must outlive the gate; `abort`
// is the run's cancel flag and may be null.
HookGate* HookGate_Create(Arena* arena, const HookConfig* config,
                          const HookSessionInfo* session, volatile i32* abort);

// Replaces the identity reported by later payloads. Required after the session
// is switched or reset. Safe to call while a gate evaluation is in flight.
void HookGate_SetSession(HookGate* gate, const HookSessionInfo* session);

// Binds to AgentToolHooks::BeforeTool with the HookGate as `user`. A null
// `user` allows, so the seam can be installed unconditionally.
void HookGate_BeforeTool(void* user, Arena* arena, StrView toolName, StrView toolCallId,
                         const ToolArgs* args, AgentToolGateDecision* out);

// One command's verdict, exposed for tests. A stdout that parses as a JSON
// object carrying a boolean `block` is the verdict and outranks the exit code;
// otherwise the exit code decides, with the captured stdout as the reason.
// Anything that leaves the answer unknown -- a failed spawn, a timeout, more
// output than can be read -- blocks.
//
// An allowed verdict leaves `out` zeroed. Strings are copied into `arena`,
// which must outlive the decision; `result->Output` may live in scratch.
void HookGate_Decide(Arena* arena, const ExecResult* result, AgentToolGateDecision* out);
