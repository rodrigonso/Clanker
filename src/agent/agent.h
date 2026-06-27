#pragma once
#include "agent/event.h"
#include "agent/tool.h"
#include "base/array.h"
#include "llm/types.h"

// Stateful agent: owns the transcript (in the session arena), drives the
// turn/tool/steering loop, and emits AgentEvents. Single-threaded; an external
// input thread may set the abort flag and feed steering/follow-up via the poll
// seams (which the loop calls at pi's defined points).
struct Agent {
  Arena* Mem;  // session arena; transcript text lives here
  StrView SystemPrompt;
  Model Model;
  Array<Tool> Tools;
  Array<Message> Messages;

  Provider Prov;
  AgentSink Sink;
  StreamOptions Stream;  // ApiKey, MaxTokens, Abort

  // Optional seams (nullable). Mirrors pi's getSteeringMessages /
  // getFollowUpMessages / beforeToolCall.
  void* User;
  bool (*PollSteering)(void* user, Arena* arena, StrView* outText);
  bool (*PollFollowUp)(void* user, Arena* arena, StrView* outText);
  // Return false to block the tool; write the reason into *outReason.
  bool (*BeforeTool)(void* user, Arena* arena, StrView toolName, StrView argsJson, StrView* outReason);

  // Optional per-call auth resolution (mirrors pi's getApiKey). Called before
  // each provider stream so expiring OAuth tokens refresh mid-session. Returns
  // false to fall back to the static Stream.ApiKey.
  void* AuthUser;
  bool (*ResolveAuth)(void* user, Arena* arena, StrView provider, StrView* outToken);

  bool Streaming;
};

Agent Agent_Make(Arena* arena, Model model, StrView systemPrompt);
void Agent_AddTool(Agent* a, Tool t);

// Add a user message and run the loop to completion.
void Agent_Prompt(Agent* a, StrView userText);

// Run the loop from the current transcript (last message must be user or
// toolResult).
void Agent_Continue(Agent* a);
