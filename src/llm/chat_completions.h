#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"
#include "llm/model_list.h"
#include "llm/types.h"
#include "net/http.h"

// OpenAI-compatible Chat Completions protocol: `POST <base>/v1/chat/completions`
// with an SSE stream of `choices[].delta` frames.
//
// This is the only wire protocol Clank speaks. Everything that differs between
// vendors is data (a ChatEndpoint), not code, so any server implementing the
// OpenAI shape is reachable without a gateway in front of it.
//
// Request building and SSE parsing are transport-independent so both can be
// covered offline against recorded fixtures; the HTTP glue lives in
// chat_completions.cpp.

// One OpenAI-compatible endpoint. `BaseUrlEnvVar`, when set and non-empty in
// the environment, replaces `BaseUrl` at model-creation time -- which is what
// lets a single build target a local server or an aggregator without a rebuild.
struct ChatEndpoint {
  StrView Id;       // provider id recorded on messages and in settings.json
  StrView BaseUrl;  // endpoint root; "/v1/chat/completions" is appended
  const char* BaseUrlEnvVar;
  const Model* FallbackModels;
  u32 FallbackModelCount;
  // Local servers (Ollama, llama.cpp, LM Studio) authenticate nobody. When set,
  // a missing credential is not an error and no Authorization header is sent.
  bool AllowAnonymous;
};

// Resolved endpoint root, honoring `BaseUrlEnvVar`. Trailing '/' is stripped so
// appending a path never produces a doubled separator.
StrView ChatEndpoint_ResolveBaseUrl(Arena* arena, const ChatEndpoint* endpoint);

// Joins an endpoint root and an API path such as "chat/completions". A root
// that already ends in "/v1" does not get a second one, since that is how most
// aggregators and local servers publish their address.
StrView ChatCompletions_Url(Arena* arena, StrView baseUrl, const char* path);

// Build the streaming request body. Never emits provider-specific extensions:
// thinking blocks are dropped (chat-completions standardizes no reasoning
// round-trip) and effort is clamped to the portable low/medium/high triple.
StrBuf ChatCompletions_BuildRequest(Arena* arena, const Model* model,
                                    const LlmContext* context,
                                    const StreamOptions* options);

// One tool call assembled across `delta.tool_calls[]` frames. Servers key the
// frames by `index`; `id` and `function.name` usually arrive only on the first.
struct ChatToolSlot {
  i64 Index;
  u32 ContentIndex;
  StrView Id;
  StrBuf Name;
  StrBuf Arguments;
  bool Finished;
};

struct ChatParse {
  Arena* Mem;
  StrBuf Buffer;
  Message* Msg;
  StreamSink Sink;
  StrBuf Text;
  StrBuf Reasoning;
  Array<ChatToolSlot> Tools;
  Usage Use;
  StopReason Stop;
  StrView Error;
  // Content indices are handed out as blocks open, since a chat-completions
  // stream never announces how many blocks it will produce. Finish then walks
  // indices in order, so the materialized content array is always in the same
  // order the stream opened its blocks -- no assumption that reasoning
  // precedes text, which not every server honors.
  u32 NextContentIndex;
  u32 TextIndex;
  u32 ReasoningIndex;
  bool HasText;
  bool HasReasoning;
  bool SawError;
  bool SawDone;
  bool SawFinish;
  bool Finished;
};

void ChatParse_Init(ChatParse* state, Arena* arena, Message* message,
                    StreamSink sink);
void ChatParse_Feed(ChatParse* state, StrView chunk);
// Closes any block still open and materializes the assistant message's content
// in reasoning -> text -> tool-call order. Must be called exactly once, after
// the last Feed, before the message is read.
void ChatParse_Finish(ChatParse* state);

// True once the stream produced something usable: a terminal `finish_reason`,
// or `[DONE]`. A stream that ends without either was truncated.
inline bool ChatParse_SawTerminal(const ChatParse* state) {
  return state && (state->SawFinish || state->SawDone);
}

Model ChatCompletions_Model(Arena* arena, const ChatEndpoint* endpoint,
                            StrView modelId);

// Parses the standard `{"data":[{"id":...}]}` catalog. Models whose IDs name a
// non-conversational modality (embeddings, audio, image, moderation) are
// skipped, since they cannot serve an agent turn.
bool ChatCompletions_ParseModelsResponse(Arena* arena,
                                         const ChatEndpoint* endpoint,
                                         StrView response,
                                         ProviderModelList* out);

typedef HttpResponse (*ChatModelsFetchFn)(void* user, Arena* arena,
                                          const HttpRequest* request,
                                          volatile i32* abort);

Provider ChatCompletions_Provider();
ProviderModelList ChatCompletions_ListModels(Arena* arena,
                                             const ChatEndpoint* endpoint,
                                             ProviderAuth auth,
                                             volatile i32* abort);
ProviderModelList ChatCompletions_ListModelsWithFetch(
    Arena* arena, const ChatEndpoint* endpoint, ProviderAuth auth,
    volatile i32* abort, ChatModelsFetchFn fetch, void* fetchUser);
