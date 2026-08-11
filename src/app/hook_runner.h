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
  // Whether a user is present to answer. Clank's answer to pi's `ctx.hasUI`,
  // which its own permission-gate example uses to fail closed when headless.
  bool Interactive;
};

// Tool-scoped payload fields, meaningful only for the tool events. A
// zero-initialized value renders none of them, which is what every other event
// passes.
struct HookToolInfo {
  StrView Name;
  StrView CallId;
  // Canonical, schema-validated arguments, embedded as a raw JSON object rather
  // than a string. Valid by construction: it is the same text the tool itself
  // is about to receive.
  StrView InputJson;
  // Set when the arguments were dropped to keep the payload inside the stdin
  // pipe bound. `tool_input` is then absent entirely rather than truncated, so
  // a consumer never has to parse half an object.
  bool InputOmitted;
};

struct HookRunner;

constexpr u32 HOOK_SESSION_ID_CAPACITY = 128;
constexpr u32 HOOK_PATH_CAPACITY = 1024;

// Session identity held inline rather than as arena-backed views. A consumer
// snapshots the whole struct by value under its own lock, so a switch can
// rewrite it without invalidating a copy already in flight, and a process that
// switches sessions all day cannot grow one duplicate at a time -- the arena
// this used to allocate from traps on exhaustion rather than failing softly.
struct HookSessionStorage {
  char SessionId[HOOK_SESSION_ID_CAPACITY];
  u32 SessionIdLength;
  char WorkingDirectory[HOOK_PATH_CAPACITY];
  u32 WorkingDirectoryLength;
  char TranscriptPath[HOOK_PATH_CAPACITY];
  u32 TranscriptPathLength;
  bool Interactive;
};

void HookSession_Store(HookSessionStorage* out, const HookSessionInfo* source);

// Views borrow `storage`, so the caller must keep it alive across the payload
// build that consumes them.
HookSessionInfo HookSession_View(const HookSessionStorage* storage);

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

// Payload builder, exposed for tests. `tool` may be null for non-tool events.
void HookRunner_BuildPayload(StrBuf* out, HookEvent event, const HookSessionInfo* session,
                             const HookToolInfo* tool);
