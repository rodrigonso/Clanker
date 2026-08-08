#pragma once
#include "base/array.h"
#include "base/str.h"
#include "json/json.h"
#include "json/schema.h"
#include "llm/types.h"

// Result returned by a tool on success.
struct ToolResult {
  ContentArray Content;
  bool Terminate;  // hint the agent to stop after this batch
};

struct ToolArgs {
  const JsonValue* Root;
  StrView Json;
};

inline const JsonValue* ToolArgs_Get(const ToolArgs* args, const char* key) {
  return args ? Json_Get(args->Root, key) : nullptr;
}
inline StrView ToolArgs_GetStr(const ToolArgs* args, const char* key) {
  return args ? Json_GetStr(args->Root, key) : StrView{};
}

inline i64 ToolArgs_GetInt(const ToolArgs* args, const char* key, i64 fallback) {
  return args ? Json_GetInt(args->Root, key, fallback) : fallback;
}


struct ToolUpdateSink {
  void* User;
  bool* Accepting;
  void (*OnUpdate)(void* user, const ToolResult* partial);
};

inline void ToolUpdateSink_Emit(const ToolUpdateSink* sink,
                                const ToolResult* partial) {
  if (sink && sink->OnUpdate && (!sink->Accepting || *sink->Accepting))
    sink->OnUpdate(sink->User, partial);
}

// How a tool participates in batch execution. `Default` tools may run
// concurrently with others; a `Sequential` tool forces the whole batch to run
// one-at-a-time (mirrors pi's `executionMode === "sequential"`).
enum class ToolExecMode : u8 { Default, Sequential };

// A tool the agent can execute. The harness prepares and validates `ToolArgs`
// before `Execute`; failures become isError tool results.
struct Tool {
  StrView Name;
  StrView Label;
  StrView Description;
  // System-prompt contributions, mirroring pi's `promptSnippet` /
  // `promptGuidelines`. `Description` is the model-facing schema text; these
  // are the prose the prompt builder renders. A tool is listed under
  // "Available tools" only when it supplies a `PromptSnippet`. Prefer
  // `Tool_SetPromptGuidelines` over assigning the pointer and count directly.
  StrView PromptSnippet;
  const StrView* PromptGuidelines;
  u32 PromptGuidelineCount;
  StrView ParamsJsonSchema;
  ToolExecMode ExecMode;  // Default => may run in parallel; Sequential => forces serial batch
  void* Ctx;
  bool (*PrepareArguments)(void* ctx, Arena* arena, JsonValue* input,
                           JsonValue** out, StrBuf* err);
  bool (*Execute)(void* ctx, Arena* arena, StrView toolCallId,
                  const ToolArgs* args, volatile i32* abort,
                  const ToolUpdateSink* updates, ToolResult* out, StrBuf* err);
};

struct ToolRuntime {
  const Tool* Definition;
  JsonSchema Schema;
};

// Binds a guideline array to a tool so the pointer and its count cannot drift.
template <usize N>
inline void Tool_SetPromptGuidelines(Tool* tool,
                                     const StrView (&guidelines)[N]) {
  tool->PromptGuidelines = guidelines;
  tool->PromptGuidelineCount = (u32)N;
}

inline ToolDef Tool_ToDef(const Tool& t) {
  ToolDef d;
  d.Name = t.Name;
  d.Description = t.Description;
  d.ParamsJsonSchema = t.ParamsJsonSchema;
  return d;
}
