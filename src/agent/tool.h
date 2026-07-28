#pragma once
#include "base/array.h"
#include "base/str.h"
#include "llm/types.h"
#include "session/replay.h"

// Result returned by a tool on success.
struct ToolResult {
  Array<Content> Content;
  bool Terminate;  // hint the agent to stop after this batch
};

// How a tool participates in batch execution. `Default` tools may run
// concurrently with others; a `Sequential` tool forces the whole batch to run
// one-at-a-time (mirrors pi's `executionMode === "sequential"`).
enum class ToolExecMode : u8 { Default, Sequential };

// A tool the agent can execute. `Execute` parses `argsJson`, performs the work,
// and on success fills `out`. On failure it returns false and writes a message
// into `err`; the loop turns that into an isError tool result (mirroring pi's
// "throw on failure" contract).
struct Tool {
  StrView Name;
  StrView Label;
  StrView Description;
  StrView ParamsJsonSchema;
  ToolExecMode ExecMode;  // Default => may run in parallel; Sequential => forces serial batch
  ToolReplayPolicy ReplayPolicy;  // zero/default requires explicit recovery reconciliation
  void* Ctx;
  bool (*Execute)(void* ctx, Arena* arena, StrView toolCallId, StrView argsJson, volatile i32* abort,
                  ToolResult* out, StrBuf* err);
};

inline ToolDef Tool_ToDef(const Tool& t) {
  ToolDef d;
  d.Name = t.Name;
  d.Description = t.Description;
  d.ParamsJsonSchema = t.ParamsJsonSchema;
  return d;
}
