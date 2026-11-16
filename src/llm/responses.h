#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"
#include "llm/model_list.h"
#include "llm/types.h"
#include "net/http.h"

// The Responses endpoints Clanker speaks: `POST <base>/responses` with an SSE
// stream of typed `response.*` events.
//
// This is the only wire protocol Clanker speaks. OpenRouter translates it for
// upstream vendors while preserving function tools and reasoning items, which
// lets a turn hand its reasoning back on the next request and keeps a
// multi-step tool loop coherent. The ChatGPT backend that serves Codex for
// ChatGPT subscriptions speaks the same events under a different layout; the
// endpoint's dialect selects that layout.
//
// Request building and SSE parsing are transport-independent so both can be
// covered offline against recorded fixtures; the HTTP glue lives in
// responses.cpp.

// OpenRouter is the public Responses API shape: `<base>/v1/responses`, the
// `{"data":[...]}` catalog, and a bearer key. ChatGptCodex is
// `<base>/codex/responses`, scoped to a ChatGPT account by header, with its
// own model listing; it rejects `max_output_tokens` and requires
// `instructions`.
enum class ResponsesDialect : u8 { OpenRouter, ChatGptCodex };

// `BaseUrlEnvVar`, when set and non-empty in the environment, replaces
// `BaseUrl` at model-creation time for a compatible mirror.
struct ResponsesEndpoint {
  StrView Id;       // provider id recorded on messages and in settings.json
  StrView BaseUrl;  // endpoint root; the dialect's path layout is joined
  const char* BaseUrlEnvVar;
  ResponsesDialect Dialect;
};

// Sent as `client_version` when listing Codex models. The backend lists only
// the models a Codex client at least this new can drive, so it tracks the
// Codex CLI release whose request shape Clanker matches.
#define RESPONSES_CODEX_CLIENT_VERSION "0.169.0"

// Resolved endpoint root, honoring `BaseUrlEnvVar`. Trailing '/' is stripped so
// appending a path never produces a doubled separator.
StrView ResponsesEndpoint_ResolveBaseUrl(Arena* arena,
                                         const ResponsesEndpoint* endpoint);

// Joins an endpoint root and an API path such as "responses". A root that
// already ends in "/v1" does not get a second one, since that is how most
// gateways publish their address.
StrView Responses_Url(Arena* arena, StrView baseUrl, const char* path);

// Joins a ChatGPT backend root and a Codex API path such as "responses" under
// "/codex/". A root that already ends in "/codex" is not given a second one.
StrView Responses_CodexUrl(Arena* arena, StrView baseUrl, const char* path);

// Build the streaming request body.
StrBuf Responses_BuildRequest(
    Arena* arena, const Model* model, const LlmContext* context,
    const StreamOptions* options,
    ResponsesDialect dialect = ResponsesDialect::OpenRouter);

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

// Parses the ChatGPT backend's `{"models":[{"slug":...}]}` listing. Only
// models the backend marks for listing are kept, in its priority order.
bool Responses_ParseCodexModelsResponse(Arena* arena,
                                        const ResponsesEndpoint* endpoint,
                                        StrView response,
                                        ProviderModelList* out);

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
