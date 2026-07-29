#pragma once
#include "agent/tool.h"
#include "llm/types.h"

// Lifecycle events emitted by the agent loop for the UI/REPL. Mirrors pi's
// AgentEvent sequence: agent_start -> turn_start -> message_* -> tool_* ->
// turn_end -> agent_end.
enum class AgentEventKind : u8 {
  AgentStart,
  AgentEnd,
  TurnStart,
  TurnEnd,
  MessageStart,
  MessageUpdate,
  MessageEnd,
  ToolStart,
  ToolUpdate,
  ToolEnd,
  RetryStart,
  RetryEnd,
  QueueUpdate,
};

struct AgentEvent {
  AgentEventKind Kind;
  const Message* Msg;        // message_start/update/end, turn_end
  StreamEvent Stream;        // message_update: the underlying streaming delta
  StrView ToolCallId;        // tool_start/update/end
  StrView ToolName;          // tool_start/update/end
  StrView ToolArgsJson;      // tool_start
  const ToolResult* Result;  // tool_update/tool_end
  bool IsError;              // tool_end
  // retry_start / retry_end (transient provider error backoff)
  u32 RetryAttempt;        // 1-based attempt number
  u32 RetryMaxAttempts;    // configured max retries
  u32 RetryDelayMs;        // backoff before this attempt
  StrView RetryError;      // error text that triggered the retry
  // queue_update: current depth of the agent's message queues
  u32 SteerCount;
  u32 FollowUpCount;
  u32 NextTurnCount;
};

struct AgentSink {
  void* User;
  void (*OnEvent)(void* user, const AgentEvent& ev);
};

inline void AgentSink_Emit(const AgentSink& sink, const AgentEvent& ev) {
  if (sink.OnEvent) sink.OnEvent(sink.User, ev);
}
