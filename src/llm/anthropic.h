#pragma once
#include "base/arena.h"
#include "base/str.h"
#include "llm/model_list.h"
#include "llm/types.h"
#include "net/http.h"

// Anthropic Messages API provider. Split into protocol logic (request building
// + SSE parsing, no networking; testable offline) and the HTTP glue.

inline bool Anthropic_IsOAuthToken(StrView token) {
  return StrView_Contains(token, "sk-ant-oat");
}

inline bool Anthropic_UsesOAuth(ProviderAuth auth) {
  return auth.Kind == ProviderAuthKind::OAuth ||
         Anthropic_IsOAuthToken(auth.Token);
}

// ---- protocol (anthropic_proto.cpp) --------------------------------------

// Build the Anthropic /v1/messages streaming request body from a context.
StrBuf Anthropic_BuildRequest(Arena* arena, const Model* model, const LlmContext* ctx,
                              const StreamOptions* opt);

struct AnthropicModelPage {
  Model* Models;
  u32 Count;
  bool HasMore;
  StrView LastId;
  StrView Error;
};

// Parse one GET /v1/models response. The response and all returned metadata are
// copied into `arena`; false means the response did not match the API schema.
bool Anthropic_ParseModelsResponse(Arena* arena, StrView response, AnthropicModelPage* out);

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

// Build a Model entry with Anthropic defaults and known capability metadata.
Model Anthropic_Model(StrView id);
const Model* Anthropic_FallbackModels();
u32 Anthropic_FallbackModelCount();
StrView Anthropic_ContextBeta(const Model* model, const StreamOptions* options);

// Provider that streams over HTTP. Requires libcurl (clank_http).
Provider Anthropic_Provider();

typedef HttpResponse (*AnthropicModelsFetchFn)(void* user, Arena* arena, const HttpRequest* request,
                                               volatile i32* abort);

// Live GET /v1/models discovery. The injected form is used by offline tests.
ProviderModelList Anthropic_ListModels(Arena* arena, ProviderAuth auth,
                                       volatile i32* abort);
ProviderModelList Anthropic_ListModelsWithFetch(Arena* arena, ProviderAuth auth,
                                                volatile i32* abort,
                                                AnthropicModelsFetchFn fetch, void* fetchUser);
