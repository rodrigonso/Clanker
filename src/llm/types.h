#pragma once
#include "base/array.h"
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

// =============================================================================
// Core LLM data model (port of pi's packages/ai/src/types.ts).
//
// Memory note: all text is a StrView (a non-owning UTF-8 slice). In a live
// session every StrView points into the session content blob, so messages cost
// only their fixed struct size plus the bytes of text appended once to the blob.
// =============================================================================

enum class Role : u8 { User, Assistant, ToolResult };

enum class ContentKind : u8 { Text, Thinking, ToolCall, Image };

struct Content {
  ContentKind Kind;
  // Text / Thinking
  StrView Text;
  // ToolCall
  StrView ToolId;
  StrView ToolName;
  StrView ToolArgsJson;  // raw JSON object text; tools parse on demand
  // Image
  StrView ImageData;      // base64
  StrView ImageMimeType;  // e.g. "image/png"
  // The provider's own identity for this block, replayed verbatim on the next
  // request so a turn resumes the chain it started. The Responses API keys
  // conversation state on these: a reasoning block carries its whole item JSON
  // (including encrypted_content), a text block its item id, a tool call the id
  // of the function_call item. Empty for blocks Clanker synthesized itself, which
  // are then sent under a generated id instead.
  StrView Signature;
};

// Several structs below carry a `Content` member whose type is also `Content`.
// MSVC accepts that; GCC rejects it ([basic.scope.class] -- a name must mean
// one thing throughout a class), so declare those members through this alias
// and the type name is never re-used as a member name.
using ContentArray = Array<Content>;

inline Content Content_Text(StrView text) {
  Content c = {};
  c.Kind = ContentKind::Text;
  c.Text = text;
  return c;
}
inline Content Content_Thinking(StrView text) {
  Content c = {};
  c.Kind = ContentKind::Thinking;
  c.Text = text;
  return c;
}
inline Content Content_ToolCall(StrView id, StrView name, StrView argsJson) {
  Content c = {};
  c.Kind = ContentKind::ToolCall;
  c.ToolId = id;
  c.ToolName = name;
  c.ToolArgsJson = argsJson;
  return c;
}
inline Content Content_Image(StrView data, StrView mimeType) {
  Content c = {};
  c.Kind = ContentKind::Image;
  c.ImageData = data;
  c.ImageMimeType = mimeType;
  return c;
}

// Deep-copy every text slice into `arena`, so the result outlives whatever
// arena the source pointed into.
inline Content Content_Clone(Arena* arena, const Content& source) {
  Content result = source;
  result.Text = StrView_Dup(arena, source.Text);
  result.ToolId = StrView_Dup(arena, source.ToolId);
  result.ToolName = StrView_Dup(arena, source.ToolName);
  result.ToolArgsJson = StrView_Dup(arena, source.ToolArgsJson);
  result.ImageData = StrView_Dup(arena, source.ImageData);
  result.ImageMimeType = StrView_Dup(arena, source.ImageMimeType);
  result.Signature = StrView_Dup(arena, source.Signature);
  return result;
}

inline Array<Content> Content_CloneArray(Arena* arena, const Array<Content>& source) {
  Array<Content> result =
      Array_Make<Content>(arena, source.Count ? source.Count : 1);
  for (u32 i = 0; i < source.Count; i++)
    Array_Push(&result, Content_Clone(arena, source.Data[i]));
  return result;
}

// Payload of one SSE "data:" line, with the optional space after the colon
// removed. `has` is false for any other line (comments, "event:", blanks).
inline bool Sse_DataPayload(StrView line, StrView* out) {
  if (line.Length < 5 || memcmp(line.Data, "data:", 5) != 0) return false;
  StrView data = {line.Data + 5, line.Length - 5};
  while (data.Length && (data.Data[0] == ' ' || data.Data[0] == '\t')) {
    data.Data++;
    data.Length--;
  }
  *out = data;
  return true;
}

struct Usage {
  u32 Input;
  u32 Output;
  u32 CacheRead;
  u32 Reasoning;
  u32 TotalTokens;
};

enum class StopReason : u8 { Stop, Length, ToolUse, Error, Aborted };

enum class ModelEffort : u8 {
  None,
  Low,
  Medium,
  High,
  XHigh,
  Max,
  Ultra,
};

constexpr u8 ModelEffort_Bit(ModelEffort effort) {
  return effort == ModelEffort::None
             ? 0
             : (u8)(1u << ((u8)effort - 1));
}

constexpr u32 MODEL_EFFORT_OPTION_CAPACITY = (u32)ModelEffort::Ultra;
constexpr u8 MODEL_EFFORT_STANDARD_MASK =
    ModelEffort_Bit(ModelEffort::Low) |
    ModelEffort_Bit(ModelEffort::Medium) |
    ModelEffort_Bit(ModelEffort::High);
constexpr u8 MODEL_EFFORT_THROUGH_MAX_MASK =
    MODEL_EFFORT_STANDARD_MASK |
    ModelEffort_Bit(ModelEffort::XHigh) |
    ModelEffort_Bit(ModelEffort::Max);
constexpr u8 MODEL_EFFORT_ALL_MASK =
    MODEL_EFFORT_THROUGH_MAX_MASK |
    ModelEffort_Bit(ModelEffort::Ultra);
constexpr u32 MODEL_CONTEXT_WINDOW_OPTION_CAPACITY = 4;
constexpr u32 MODEL_CONTEXT_WINDOW_LABEL_CAPACITY = 24;

inline StrView ModelEffort_Name(ModelEffort effort) {
  switch (effort) {
    case ModelEffort::Low: return SVL("low");
    case ModelEffort::Medium: return SVL("medium");
    case ModelEffort::High: return SVL("high");
    case ModelEffort::XHigh: return SVL("xhigh");
    case ModelEffort::Max: return SVL("max");
    case ModelEffort::Ultra: return SVL("ultra");
    default: return {};
  }
}

inline bool ModelEffort_Parse(StrView value, ModelEffort* out) {
  if (!out) return false;
  for (u8 i = (u8)ModelEffort::Low; i <= (u8)ModelEffort::Ultra; i++) {
    ModelEffort effort = (ModelEffort)i;
    if (StrView_Eq(value, ModelEffort_Name(effort))) {
      *out = effort;
      return true;
    }
  }
  return false;
}

struct ModelRequestProfile {
  ModelEffort Effort;
  u32 ContextWindow;
};

// One transcript message. A flat struct keyed by Role (role-specific fields are
// unused for other roles). Kept flat for clarity; text lives in the blob.
struct Message {
  Role Kind;
  ContentArray Content;

  // assistant
  StopReason Stop;
  Usage Use;
  StrView ErrorMessage;
  StrView Model;
  StrView Provider;

  // toolResult
  StrView ToolCallId;
  StrView ToolName;
  bool IsError;

  u64 Timestamp;
};

inline Message Message_User(Arena* arena, StrView text) {
  Message m = {};
  m.Kind = Role::User;
  m.Content = Array_Make<Content>(arena, 1);
  Array_Push(&m.Content, Content_Text(text));
  m.Timestamp = NowMillis();
  return m;
}

inline Message Message_AssistantEmpty(Arena* arena) {
  Message m = {};
  m.Kind = Role::Assistant;
  m.Content = Array_Make<Content>(arena, 2);
  m.Stop = StopReason::Stop;
  m.Timestamp = NowMillis();
  return m;
}

inline Message Message_ToolResult(Arena* arena, StrView toolCallId, StrView toolName, StrView text,
                                  bool isError) {
  Message m = {};
  m.Kind = Role::ToolResult;
  m.Content = Array_Make<Content>(arena, 1);
  Array_Push(&m.Content, Content_Text(text));
  m.ToolCallId = toolCallId;
  m.ToolName = toolName;
  m.IsError = isError;
  m.Timestamp = NowMillis();
  return m;
}

// Tool advertised to the model.
struct ToolDef {
  StrView Name;
  StrView Description;
  StrView ParamsJsonSchema;  // JSON Schema object as text
};

// A single provider request snapshot.
struct LlmContext {
  StrView SystemPrompt;
  const Message* Messages;
  u32 MessageCount;
  const ToolDef* Tools;
  u32 ToolCount;
};

// Static model metadata + resolved auth.
struct Model {
  StrView Id;
  StrView Name;
  StrView Provider;
  StrView BaseUrl;
  u32 ContextWindow;
  u32 MaxTokens;
  bool Reasoning;
  u8 EffortMask;
  ModelEffort DefaultEffort;
  u32 ContextWindowOptions[MODEL_CONTEXT_WINDOW_OPTION_CAPACITY];
  u8 ContextWindowOptionCount;
};

inline bool Model_SupportsEffort(const Model* model, ModelEffort effort) {
  return model && effort != ModelEffort::None &&
         (model->EffortMask & ModelEffort_Bit(effort)) != 0;
}

inline ModelEffort Model_DefaultEffort(const Model* model) {
  return model && Model_SupportsEffort(model, model->DefaultEffort)
             ? model->DefaultEffort
             : ModelEffort::None;
}

inline u32 Model_EffortOptionCount(const Model* model) {
  if (!model) return 0;
  u32 count = 0;
  for (u8 i = (u8)ModelEffort::Low; i <= (u8)ModelEffort::Ultra; i++) {
    if (Model_SupportsEffort(model, (ModelEffort)i)) count++;
  }
  return count;
}

inline bool Model_SupportsContextWindow(const Model* model,
                                        u32 contextWindow) {
  if (!model || contextWindow == 0) return false;
  if (model->ContextWindowOptionCount >
      MODEL_CONTEXT_WINDOW_OPTION_CAPACITY)
    return false;
  if (model->ContextWindowOptionCount) {
    for (u32 i = 0; i < model->ContextWindowOptionCount; i++) {
      if (model->ContextWindowOptions[i] == contextWindow) return true;
    }
    return false;
  }
  return model->ContextWindow == contextWindow;
}

inline bool Model_MetadataIsValid(const Model* model) {
  if (!model ||
      (model->EffortMask & ~MODEL_EFFORT_ALL_MASK) != 0 ||
      (model->DefaultEffort != ModelEffort::None &&
       !Model_SupportsEffort(model, model->DefaultEffort)) ||
      model->ContextWindowOptionCount >
          MODEL_CONTEXT_WINDOW_OPTION_CAPACITY)
    return false;
  if (!model->ContextWindowOptionCount) return true;
  if (!Model_SupportsContextWindow(model, model->ContextWindow))
    return false;
  for (u32 i = 0; i < model->ContextWindowOptionCount; i++) {
    if (!model->ContextWindowOptions[i]) return false;
    for (u32 j = 0; j < i; j++) {
      if (model->ContextWindowOptions[i] ==
          model->ContextWindowOptions[j])
        return false;
    }
  }
  return true;
}

inline ModelRequestProfile Model_DefaultRequestProfile(const Model* model) {
  ModelRequestProfile profile = {};
  if (!model) return profile;
  profile.Effort = Model_DefaultEffort(model);
  profile.ContextWindow = model->ContextWindow;
  return profile;
}

inline bool Model_RequestProfileIsValid(const Model* model,
                                        ModelRequestProfile profile) {
  if (!Model_MetadataIsValid(model)) return false;
  if (profile.Effort != ModelEffort::None &&
      !Model_SupportsEffort(model, profile.Effort))
    return false;
  if (profile.ContextWindow &&
      !Model_SupportsContextWindow(model, profile.ContextWindow))
    return false;
  return true;
}

inline ModelRequestProfile Model_NormalizeRequestPreference(
    const Model* model, ModelRequestProfile requested,
    bool* unsupported = nullptr) {
  ModelRequestProfile normalized = {};
  if (Model_MetadataIsValid(model)) {
    if (Model_EffortOptionCount(model) > 1 &&
        Model_SupportsEffort(model, requested.Effort))
      normalized.Effort = requested.Effort;
    if (model->ContextWindowOptionCount > 1 &&
        Model_SupportsContextWindow(model, requested.ContextWindow))
      normalized.ContextWindow = requested.ContextWindow;
  }
  if (unsupported) {
    *unsupported =
        (requested.Effort != ModelEffort::None &&
         !Model_SupportsEffort(model, requested.Effort)) ||
        (requested.ContextWindow &&
         !Model_SupportsContextWindow(model, requested.ContextWindow));
  }
  return normalized;
}

inline ModelRequestProfile Model_ResolveRequestProfile(
    const Model* model, ModelRequestProfile requested) {
  ModelRequestProfile resolved = requested;
  ModelRequestProfile defaults = Model_DefaultRequestProfile(model);
  if (resolved.Effort == ModelEffort::None) resolved.Effort = defaults.Effort;
  if (resolved.ContextWindow == 0)
    resolved.ContextWindow = defaults.ContextWindow;
  return resolved;
}

inline ModelRequestProfile Model_ResolveRequestPreference(
    const Model* model, ModelRequestProfile requested) {
  return Model_ResolveRequestProfile(
      model, Model_NormalizeRequestPreference(model, requested));
}

inline u32 ModelContextWindow_Format(char* buffer, u32 capacity,
                                     u32 tokens) {
  if (!buffer || !capacity) return 0;
  u32 value = tokens;
  char suffix = 0;
  if (tokens >= 1000000 && tokens % 1000000 == 0) {
    value = tokens / 1000000;
    suffix = 'M';
  } else if (tokens >= 1000 && tokens % 1000 == 0) {
    value = tokens / 1000;
    suffix = 'K';
  }
  char digits[16];
  u32 count = 0;
  if (!value) digits[count++] = '0';
  while (value && count < sizeof(digits)) {
    digits[count++] = (char)('0' + value % 10);
    value /= 10;
  }
  u32 length = 0;
  while (count && length + 1 < capacity)
    buffer[length++] = digits[--count];
  if (suffix && length + 1 < capacity) buffer[length++] = suffix;
  buffer[length] = '\0';
  return length;
}

// -----------------------------------------------------------------------------
// Streaming protocol (port of AssistantMessageEvent). Replaces JS async
// iterators with a push callback. The provider builds the final assistant
// Message and emits deltas as it goes.
// -----------------------------------------------------------------------------
enum class StreamEventKind : u8 {
  Start,
  TextStart,
  TextDelta,
  TextEnd,
  ThinkingStart,
  ThinkingDelta,
  ThinkingEnd,
  ToolCallStart,
  ToolCallDelta,
  ToolCallEnd,
  Done,
  Error,
};

struct StreamEvent {
  StreamEventKind Kind;
  u32 ContentIndex;
  StrView Delta;  // for *_delta events; zero-copy slice of provider buffer
};

struct StreamSink {
  void* User;
  void (*OnEvent)(void* user, const StreamEvent& ev);
};

inline void StreamSink_Emit(const StreamSink& sink, const StreamEvent& ev) {
  if (sink.OnEvent) sink.OnEvent(sink.User, ev);
}

struct HttpRetryPolicy;  // llm/retry.h

struct ProviderAuth {
  StrView Token;  // empty only when the endpoint allows anonymous access
};

enum class LlmToolChoice : u8 { Auto, NamedFunction };

// Wire-level controls shared by providers that advertise function tools.
// Zero initialization preserves the normal request shape: automatic tool
// choice, parallel calls enabled, and non-strict schemas.
struct LlmRequestControls {
  LlmToolChoice ToolChoice;
  StrView ToolName;  // used when ToolChoice is NamedFunction
  bool StrictTools;
  bool DisableParallelToolCalls;
};

struct StreamOptions {
  ProviderAuth Auth;
  i32 MaxTokens;            // 0 => use model default
  ModelRequestProfile Profile;
  LlmRequestControls Request;
  volatile i32* Abort;     // non-zero => abort in progress
  // Opt-in transport-level HTTP retry policy for this request (transport
  // failures and HTTP 408/409/429/5xx). Null (the default) disables it, so
  // provider behavior is unchanged unless a caller explicitly sets this.
  const HttpRetryPolicy* Retry;
};

// A provider streams one assistant turn. Emits StreamEvents to `sink`, writes
// the final assistant Message (allocated from `arena`) into `out`. Returns true
// on a normal completion; false means an error was produced (still in `out`,
// with Stop == Error/Aborted and ErrorMessage set).
struct Provider {
  void* Ctx;
  bool (*Stream)(void* ctx, Arena* arena, const Model* model, const LlmContext* context,
                 const StreamOptions* options, StreamSink sink, Message* out);
};
