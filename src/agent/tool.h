#pragma once
#include "base/array.h"
#include "base/str.h"
#include "llm/types.h"

// Result returned by a tool on success.
struct ToolResult {
  Array<Content> Content;
  bool Terminate;  // hint the agent to stop after this batch
};

// A tool the agent can execute. `Execute` parses `argsJson`, performs the work,
// and on success fills `out`. On failure it returns false and writes a message
// into `err`; the loop turns that into an isError tool result (mirroring pi's
// "throw on failure" contract).
struct Tool {
  StrView Name;
  StrView Label;
  StrView Description;
  StrView ParamsJsonSchema;
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
