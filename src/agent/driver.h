#pragma once

#include "agent/harness.h"

// Runs a full turn-loop for an accepted user prompt: flushes the next-turn
// queue onto the current head, appends the accepted prompt, flushes the
// steering queue, then loops provider -> tools -> steering-checkpoint ->
// (continue) | follow-up-checkpoint -> (continue) | stop. Returns once the
// run truly stops (success, provider failure/abort, cancellation, or fault).
SessionResult AgentDriver_Run(AgentHarness* harness, StrView prompt);

// Runs one compaction turn: a single provider call summarizing the active
// branch, then appends one Compaction entry replacing prior context.
// `instructions` is appended to the summarization prompt when non-empty and is
// never persisted, so it applies to this compaction only.
SessionResult AgentDriver_Compact(AgentHarness* harness, StrView instructions);

bool AgentDriver_ReviewPermission(
    void* user, Arena* arena, const AgentPermissionReviewRequest* request,
    volatile i32* abort, AgentPermissionReviewResult* out);

// Internal seams shared by harness.cpp and driver.cpp.

// Normalizes a hook's result into a HookError, keeping the hook's own message
// when it supplied one.
inline SessionResult AgentHook_Failure(SessionResult result, const char* fallback) {
  if (SessionResult_IsOk(result)) return result;
  return SessionResult_Error(
      SessionStatus::HookError,
      result.Message.Length ? result.Message : SV(fallback), result.Seq);
}

// Appends records to the session log, then folds each into the harness's own
// maintained Records/Reduction (O(1) per record: never re-validates, clones,
// or reloads history). Notifies watchers of each new durable record.
SessionResult AgentHarness_Append(AgentHarness* harness, SessionRecord* records,
                                  u32 count,
                                  bool rejectIfCancelled = false);

// O(1) copy of the harness's maintained reduction (arena-backed; the copy's
// backing arrays remain valid for as long as the harness lives, since the
// arena never frees an abandoned block -- see base/arena.h).
SessionReduction AgentHarness_ReduceSnapshot(AgentHarness* harness);

// Drains every message currently queued in `*queue` (under StateLock) and
// appends it as a Message record chained from `*ioHead`, in FIFO order.
// Queues are memory-only, so a drained-but-unflushed item is simply gone if
// the process exits first; nothing here is a durable queue record.
// `outMessages`, when given, receives the persisted clone of each appended
// message (harness-lifetime storage) so a run can report what it produced.
SessionResult AgentHarness_FlushQueue(AgentHarness* harness, Array<Message>* queue,
                                      DurableId* ioHead, u32* outFlushedCount,
                                      Array<Message>* outMessages = nullptr);

void AgentHarness_PublishAgentStart(AgentHarness* harness, AgentTurnKind turn);
void AgentHarness_PublishAgentEnd(AgentHarness* harness, AgentTurnKind turn,
                                  AgentTurnOutcome outcome, StrView error);
void AgentHarness_PublishTurnStart(AgentHarness* harness, AgentTurnKind turn);
void AgentHarness_PublishTurnEnd(AgentHarness* harness, AgentTurnKind turn,
                                 AgentTurnOutcome outcome, StrView error);
void AgentHarness_PublishStream(AgentHarness* harness, const StreamEvent* event);
void AgentHarness_PublishToolStart(AgentHarness* harness, StrView toolCallId,
                                   StrView toolName, StrView argumentsJson);
void AgentHarness_PublishToolUpdate(AgentHarness* harness, StrView toolCallId,
                                    StrView toolName, const ToolResult* partial);
void AgentHarness_PublishToolEnd(AgentHarness* harness, StrView toolCallId,
                                 StrView toolName, const ToolResult* result,
                                 bool isError);
void AgentHarness_PublishRetry(AgentHarness* harness, AgentHarnessEventKind kind,
                               RetryLayer layer, u32 attempt, u32 maxAttempts,
                               u32 delayMs, StrView error);
void AgentHarness_PublishPermissionReviewStart(AgentHarness* harness,
                                               StrView toolName,
                                               StrView model);
void AgentHarness_PublishPermissionReviewEnd(
    AgentHarness* harness, StrView toolName, StrView model,
    AgentPermissionReviewOutcome outcome, StrView rationale);
void AgentHarness_PublishQueueUpdate(AgentHarness* harness);
