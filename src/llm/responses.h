#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"
#include "llm/model_list.h"
#include "llm/types.h"
#include "net/http.h"

// OpenAI Responses protocol: `POST <base>/v1/responses` with an SSE stream of
// typed `response.*` events.
//
// This is the only wire protocol Clanker speaks. It is the endpoint OpenAI's
// reasoning models expect for tool use -- chat-completions rejects function
// tools combined with a reasoning effort -- and it is the only one that lets a
// turn hand its reasoning back on the next request, which is what keeps a
// multi-step tool loop coherent.
//
// Request building and SSE parsing are transport-independent so both can be
// covered offline against recorded fixtures; the HTTP glue lives in
// responses.cpp.

// One Responses endpoint. `BaseUrlEnvVar`, when set and non-empty in the
// environment, replaces `BaseUrl` at model-creation time -- which is what lets
// a single build target a gateway or a proxy without a rebuild.
struct ResponsesEndpoint {
  StrView Id;       // provider id recorded on messages and in settings.json
  StrView BaseUrl;  // endpoint root; "/v1/responses" is appended
  const char* BaseUrlEnvVar;
  const Model* FallbackModels;
  u32 FallbackModelCount;
};

// Resolved endpoint root, honoring `BaseUrlEnvVar`. Trailing '/' is stripped so
// appending a path never produces a doubled separator.
StrView ResponsesEndpoint_ResolveBaseUrl(Arena* arena,
                                         const ResponsesEndpoint* endpoint);

// Joins an endpoint root and an API path such as "responses". A root that
// already ends in "/v1" does not get a second one, since that is how most
// gateways publish their address.
StrView Responses_Url(Arena* arena, StrView baseUrl, const char* path);

// Build the streaming request body.
StrBuf Responses_BuildRequest(Arena* arena, const Model* model,
                              const LlmContext* context,
                              const StreamOptions* options);

// One output item being assembled. The stream keys every event by the item's
// `output_index`, so a slot is found by that and nothing else.
struct ResponsesSlot {
  i64 OutputIndex;
  u32 ContentIndex;
  u8 Kind;  // 1 text, 2 reasoning, 3 function call
  StrBuf Value;
  StrView ItemId;
  StrView CallId;
  StrView Name;
  StrView Phase;
  StrView Signature;
  bool Finished;
};

struct ResponsesParse {
  Arena* Mem;
  StrBuf Buffer;
  Message* Msg;
  StreamSink Sink;
  Array<ResponsesSlot> Slots;
  Usage Use;
  StopReason Stop;
  StrView Error;
  bool SawTool;
  bool SawTerminal;
  bool SawError;
  bool Finished;
};

void ResponsesParse_Init(ResponsesParse* state, Arena* arena, Message* message,
                         StreamSink sink);
void ResponsesParse_Feed(ResponsesParse* state, StrView chunk);
// Materializes any item the stream opened but never closed, so a truncated
// response still yields whatever text or arguments arrived. Must be called
// exactly once, after the last Feed, before the message is read.
void ResponsesParse_Finish(ResponsesParse* state);

// True once the stream reported a terminal `response.completed`, `.incomplete`
// or `.failed`. A stream that ends without one was truncated.
inline bool ResponsesParse_SawTerminal(const ResponsesParse* state) {
  return state && state->SawTerminal;
}

Model Responses_Model(Arena* arena, const ResponsesEndpoint* endpoint,
                      StrView modelId);

// Parses the standard `{"data":[{"id":...}]}` catalog. Models whose IDs name a
// family or modality the Responses endpoint cannot serve are skipped.
bool Responses_ParseModelsResponse(Arena* arena,
                                   const ResponsesEndpoint* endpoint,
                                   StrView response, ProviderModelList* out);

typedef HttpResponse (*ResponsesModelsFetchFn)(void* user, Arena* arena,
                                               const HttpRequest* request,
                                               volatile i32* abort);

Provider Responses_Provider();
ProviderModelList Responses_ListModels(Arena* arena,
                                       const ResponsesEndpoint* endpoint,
                                       ProviderAuth auth, volatile i32* abort);
ProviderModelList Responses_ListModelsWithFetch(
    Arena* arena, const ResponsesEndpoint* endpoint, ProviderAuth auth,
    volatile i32* abort, ResponsesModelsFetchFn fetch, void* fetchUser);
