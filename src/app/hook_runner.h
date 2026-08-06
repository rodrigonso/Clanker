#pragma once
#include "app/hook_config.h"
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"
#include "base/thread.h"

// Reports agent lifecycle transitions to the external commands configured in
// hooks.json. Dispatch is queued onto a worker thread: a hook is an arbitrary
// user command, and a slow one must never stall the agent loop or the TUI
// redraw.

enum class HookEvent : u8 {
  SessionStart,
  UserPromptSubmit,
  PreToolUse,
  PostToolUse,
  Stop,
  StopFailure,
  SessionEnd,
};

StrView HookEvent_Name(HookEvent event);

// Session identity echoed in every payload. A consumer uses TranscriptPath to
// find the durable log without having to re-derive the project key. Values are
// copied into fixed inline storage, so a field longer than about 1 KiB is
// reported as absent rather than truncated -- half a path is worse than none.
struct HookSessionInfo {
  StrView SessionId;
  StrView WorkingDirectory;
  StrView TranscriptPath;
};

struct HookRunner;

// Returns nullptr when the config registers no commands, so a build with no
// hooks configured spawns no thread and costs nothing. `config` must outlive
// the runner.
HookRunner* HookRunner_Create(Arena* arena, const HookConfig* config,
                              const HookSessionInfo* session);

// Replaces the identity reported by later payloads. Required after the session
// is switched or reset, which changes the session ID, the transcript path, and
// possibly the working directory. Safe to call while the worker is running.
void HookRunner_SetSession(HookRunner* runner, const HookSessionInfo* session);

// Queues one event. `toolName` is the matcher subject and is only meaningful
// for PreToolUse / PostToolUse. Never blocks: if the queue is full the event is
// dropped, because a status report must not throttle the run that produced it.
// Safe to call with a null runner.
//
// Callers should dispatch run-scoped events from the harness watch stream
// rather than from around AgentHarness_Run: that stream is the only place the
// agent's events are already ordered, so reporting Stop from the calling thread
// can land it ahead of the run's own last PostToolUse.
void HookRunner_Dispatch(HookRunner* runner, HookEvent event, StrView toolName);

// Gives the queued backlog a bounded chance to finish, cancels whatever is
// still outstanding once that grace period expires, joins the worker, and only
// then reports `finalEvent` synchronously and uncancelled.
//
// Running the terminal event inline is what makes its delivery unconditional:
// queueing it would leave it subject to the same overflow drop as every other
// event, which is exactly what happens when hooks are slow.
void HookRunner_Shutdown(HookRunner* runner, HookEvent finalEvent);

// Payload builder, exposed for tests.
void HookRunner_BuildPayload(StrBuf* out, HookEvent event, const HookSessionInfo* session,
                             StrView toolName);
