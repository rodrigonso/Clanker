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
  StrView Signature;  // textSignature / thinkingSignature / thoughtSignature
  bool Redacted;      // thinking was redacted by safety filters
  // ToolCall
  StrView ToolId;
  StrView ToolName;
  StrView ToolArgsJson;  // raw JSON object text; tools parse on demand
  // Image
  StrView ImageData;      // base64
  StrView ImageMimeType;  // e.g. "image/png"
};

inline Content Content_Text(StrView text) {
  Content c = {};
  c.Kind = ContentKind::Text;
  c.Text = text;
  return c;
}
inline Content Content_Thinking(StrView text, StrView signature, bool redacted) {
  Content c = {};
  c.Kind = ContentKind::Thinking;
  c.Text = text;
  c.Signature = signature;
  c.Redacted = redacted;
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

struct Usage {
  u32 Input;
  u32 Output;
  u32 CacheRead;
  u32 CacheWrite;
  u32 Reasoning;
  u32 TotalTokens;
};

enum class StopReason : u8 { Stop, Length, ToolUse, Error, Aborted };

// One transcript message. A flat struct keyed by Role (role-specific fields are
// unused for other roles). Kept flat for clarity; text lives in the blob.
struct Message {
  Role Kind;
  Array<Content> Content;

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
  StrView Api;
  StrView BaseUrl;
  u32 ContextWindow;
  u32 MaxTokens;
  bool Reasoning;
};

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

struct StreamOptions {
  StrView ApiKey;
  i32 MaxTokens;            // 0 => use model default
  volatile i32* Abort;     // non-zero => abort in progress
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
