#pragma once

#include "base/array.h"
#include "base/arena.h"
#include "base/str.h"
#include "llm/model_list.h"
#include "llm/types.h"
#include "net/http.h"

// OpenAI Responses protocol. Request building and SSE parsing are kept
// transport-independent so their behavior can be covered offline.
StrBuf OpenAI_BuildRequest(Arena* arena, const Model* model,
                           const LlmContext* context,
                           const StreamOptions* options);

struct OpenAIParseSlot {
  i64 OutputIndex;
  u32 ContentIndex;
  u8 Kind;  // 1 text, 2 reasoning, 3 function call
  StrBuf Value;
  StrView ItemId;
  StrView CallId;
  StrView Name;
  StrView Phase;
  bool Finished;
};

struct OpenAIParse {
  Arena* Mem;
  StrBuf Buffer;
  Message* Msg;
  StreamSink Sink;
  Array<OpenAIParseSlot> Slots;
  Usage Use;
  StopReason Stop;
  StrView Error;
  bool SawTool;
  bool SawTerminal;
  bool SawError;
};

void OpenAIParse_Init(OpenAIParse* state, Arena* arena, Message* message,
                      StreamSink sink);
void OpenAIParse_Feed(OpenAIParse* state, StrView chunk);

Model OpenAI_Model(StrView id);
const Model* OpenAI_FallbackModels();
u32 OpenAI_FallbackModelCount();

// Parses either the public API's {"data":[{"id":...}]} catalog or the rich
// ChatGPT catalog's {"models":[...]} shape.
bool OpenAI_ParseModelsResponse(Arena* arena, StrView response, bool rich,
                                ProviderModelList* out);

typedef HttpResponse (*OpenAIModelsFetchFn)(void* user, Arena* arena,
                                           const HttpRequest* request,
                                           volatile i32* abort);

Provider OpenAI_Provider();
ProviderModelList OpenAI_ListModels(Arena* arena, ProviderAuth auth,
                                    volatile i32* abort);
ProviderModelList OpenAI_ListModelsWithFetch(
    Arena* arena, ProviderAuth auth, volatile i32* abort,
    OpenAIModelsFetchFn fetch, void* fetchUser);
