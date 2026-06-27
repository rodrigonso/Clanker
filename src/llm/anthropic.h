#pragma once
#include "base/arena.h"
#include "base/str.h"
#include "llm/types.h"

// Anthropic Messages API provider. Split into protocol logic (request building
// + SSE parsing, no networking; testable offline) and the HTTP glue.

// ---- protocol (anthropic_proto.cpp) --------------------------------------

// Build the Anthropic /v1/messages streaming request body from a context.
StrBuf Anthropic_BuildRequest(Arena* arena, const Model* model, const LlmContext* ctx,
                              const StreamOptions* opt);

// Incremental SSE parser state. Feed raw response bytes; it builds `Msg` (an
// assistant message whose Content array lives in `arena`) and emits StreamEvents.
struct AnthropicParse {
  Arena* Mem;
  StrBuf Buffer;  // accumulates raw bytes, processed line by line
  Message* Msg;
  StreamSink Sink;
  StrBuf CurText;  // current block text / thinking / tool-input JSON
  StrBuf CurSig;   // current thinking signature
  i32 CurType;     // 0 none, 1 text, 2 thinking, 3 tool_use
  StrView CurToolId;
  StrView CurToolName;
  bool SawError;
  StrView ErrMsg;
  StopReason Stop;
  Usage Use;
};

void AnthropicParse_Init(AnthropicParse* st, Arena* arena, Message* msg, StreamSink sink);
void AnthropicParse_Feed(AnthropicParse* st, StrView chunk);

// ---- provider (anthropic.cpp) --------------------------------------------

// Build a Model entry with Anthropic defaults (base URL, context window).
Model Anthropic_Model(StrView id);

// Provider that streams over HTTP. Requires libcurl (ma_http).
Provider Anthropic_Provider();
