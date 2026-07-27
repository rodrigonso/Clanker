#pragma once
#include "agent/event.h"
#include "agent/tool.h"
#include "base/array.h"
#include "base/thread.h"
#include "llm/types.h"

// How a batch of tool calls in one assistant message is executed. `Parallel`
// (default, mirrors pi) runs them concurrently on a small worker pool, each into
// its own arena, then appends results in original order. `Sequential` runs them
// one-at-a-time. A batch also falls back to sequential if any called tool is
// marked ToolExecMode::Sequential.
enum class ToolExecution : u8 { Parallel, Sequential };

// Mutable tool outcome passed to the AfterTool hook. The hook may replace any
// field in place (a left-unchanged field keeps its value), mirroring pi's
// field-by-field afterToolCall override of content/isError/terminate.
struct ToolOutcome {
  Array<Content> Content;
  bool IsError;
  bool Terminate;
};

// Fixed-capacity FIFO of message text. The stored StrViews point into
// producer-owned memory (e.g. the TUI line buffer) and stay valid until the
// agent thread drains them.
struct MsgQueue {
  StrView Items[64];
  u32 Head;
  u32 Tail;
  u32 Count;
};

// Stateful agent: owns the transcript (in the session arena), drives the
// turn/tool/steering loop, and emits AgentEvents. Single-threaded loop; an
// external input thread may set the abort flag and push steering / follow-up /
// next-turn messages via the thread-safe Agent_Steer/FollowUp/NextTurn API.
struct Agent {
  Arena* Mem;  // session arena; transcript text lives here
  StrView SystemPrompt;
  Model Model;
  Array<Tool> Tools;
  Array<Message> Messages;

  Provider Prov;
  AgentSink Sink;
  StreamOptions Stream;  // ApiKey, MaxTokens, Abort

  // Tool batch execution policy. Defaults to Parallel (pi parity).
  ToolExecution ToolExec;
  u32 MaxParallelTools;  // 0 => default cap

  // Provider auto-retry on transient errors (pi parity). Defaults: enabled,
  // MaxRetries=3, base 2s exponential backoff capped at 60s.
  bool RetryEnabled;
  u32 MaxRetries;
  u32 RetryBaseDelayMs;
  u32 MaxRetryDelayMs;

  // Owned message queues (pi parity), guarded by QueueLock. Producers push via
  // Agent_Steer/FollowUp/NextTurn (thread-safe); the loop drains them on the
  // agent thread at pi's defined points, duping text into Mem. QueueInit guards
  // lazy/explicit Mutex initialization. QueuesDirty flags a pending enqueue so a
  // queue_update is emitted at the next turn boundary.
  Mutex QueueLock;
  bool QueueInit;
  MsgQueue Steer;
  MsgQueue FollowUp;
  MsgQueue NextTurn;
  bool QueuesDirty;

  // Optional seams (nullable). Mirrors pi's beforeToolCall / afterToolCall.
  void* User;
  // Return false to block the tool; write the reason into *outReason.
  bool (*BeforeTool)(void* user, Arena* arena, StrView toolName, StrView argsJson, StrView* outReason);
  // Optional. Called after a tool runs, before its result is recorded, with the
  // executed outcome in *io. May rewrite io->Content / io->IsError / io->Terminate
  // in place (mirrors pi's afterToolCall). Runs on the agent thread, in original
  // call order; allocate replacements from `arena`.
  void (*AfterTool)(void* user, Arena* arena, StrView toolName, StrView argsJson, ToolOutcome* io);

  // Optional per-call auth resolution (mirrors pi's getApiKey). Called before
  // each provider stream so expiring OAuth tokens refresh mid-session. Returns
  // false to fall back to the static Stream.ApiKey.
  void* AuthUser;
  bool (*ResolveAuth)(void* user, Arena* arena, StrView provider, volatile i32* abort,
                      StrView* outToken);

  bool Streaming;
};

Agent Agent_Make(Arena* arena, Model model, StrView systemPrompt);
void Agent_AddTool(Agent* a, Tool t);

// Initialize the message queues (Mutex). Idempotent. Call once on the final
// Agent address before pushing while idle (the loop also inits lazily).
void Agent_InitQueues(Agent* a);

// Clear conversation-only state at an idle boundary while preserving the
// provider, model, tools, hooks, retry policy, and stream configuration. Returns
// false if a turn is active. Transcript allocations remain in Mem until the
// host arena is released; this avoids invalidating preserved configuration that
// currently shares that arena.
bool Agent_ResetSession(Agent* a);

// Final idle-only cleanup for Agent-owned synchronization. Returns false if a
// turn is active. The host still owns Mem and all provider/model/tool storage.
bool Agent_Release(Agent* a);

// Thread-safe message producers (pi parity: steer / followUp / nextTurn). Text
// must stay valid until the agent drains it. No-op until queues are initialized.
void Agent_Steer(Agent* a, StrView text);     // inject before the next assistant response
void Agent_FollowUp(Agent* a, StrView text);  // continue the run after it would stop
void Agent_NextTurn(Agent* a, StrView text);  // prepend at the start of the next run

// Add a user message and run the loop to completion.
void Agent_Prompt(Agent* a, StrView userText);
