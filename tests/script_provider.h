#pragma once
#include "base/str.h"
#include "llm/types.h"

// Scripted provider for the offline suite.
//
// Each step produces exactly one assistant message: optional text, then either
// zero or more tool calls or an error. Sequence steps to drive a multi-turn run
// (e.g. step 0 calls a tool, step 1 replies). Running past the last step yields
// an empty assistant message with StopReason::Stop.
//
// This lives under tests/ on purpose: the shipping tree carries no test
// doubles. `Provider` is just a context pointer plus a stream function, so a
// test-owned implementation needs no support from src/.
struct ScriptToolCall {
  StrView Name;
  StrView Id;
  StrView ArgsJson;
};

struct ScriptStep {
  StrView Text;
  const ScriptToolCall* Tools;
  u32 ToolCount;
  StrView Error;    // non-empty => error result (Stop=Error, ErrorMessage=Error)
  StopReason Stop;  // used when the step makes no tool call
  Usage Use;        // reported verbatim, so token thresholds stay deterministic
};

struct ScriptProvider {
  ScriptStep* Steps;
  u32 Count;
  u32 Index;
};

inline bool ScriptProvider_Stream(void* ctx, Arena* arena, const Model* model,
                                  const LlmContext* context, const StreamOptions* options,
                                  StreamSink sink, Message* out) {
  (void)model;
  (void)context;
  (void)options;
  ScriptProvider* script = (ScriptProvider*)ctx;
  ScriptStep step = {};
  if (script->Index < script->Count) {
    step = script->Steps[script->Index];
    script->Index++;
  }

  StreamEvent ev = {};
  ev.Kind = StreamEventKind::Start;
  StreamSink_Emit(sink, ev);

  Message message = Message_AssistantEmpty(arena);
  message.Model = SVL("script");
  message.Provider = SVL("script");
  message.Use = step.Use;

  // Simulate a provider error (e.g. to exercise auto-retry): emit an Error
  // stream event, set Stop=Error plus ErrorMessage, and return false with no
  // content, exactly as a real provider does.
  if (step.Error.Length) {
    message.Stop = StopReason::Error;
    message.ErrorMessage = StrView_Dup(arena, step.Error);
    StreamEvent error = {};
    error.Kind = StreamEventKind::Error;
    StreamSink_Emit(sink, error);
    *out = message;
    return false;
  }

  if (step.Text.Length) {
    u32 index = message.Content.Count;
    StreamEvent start = {};
    start.Kind = StreamEventKind::TextStart;
    start.ContentIndex = index;
    StreamSink_Emit(sink, start);
    StreamEvent delta = {};
    delta.Kind = StreamEventKind::TextDelta;
    delta.ContentIndex = index;
    delta.Delta = step.Text;
    StreamSink_Emit(sink, delta);
    StreamEvent end = {};
    end.Kind = StreamEventKind::TextEnd;
    end.ContentIndex = index;
    StreamSink_Emit(sink, end);
    Array_Push(&message.Content, Content_Text(StrView_Dup(arena, step.Text)));
  }

  if (step.ToolCount) {
    for (u32 i = 0; i < step.ToolCount; i++) {
      u32 index = message.Content.Count;
      StreamEvent start = {};
      start.Kind = StreamEventKind::ToolCallStart;
      start.ContentIndex = index;
      StreamSink_Emit(sink, start);
      StreamEvent end = {};
      end.Kind = StreamEventKind::ToolCallEnd;
      end.ContentIndex = index;
      StreamSink_Emit(sink, end);
      Array_Push(&message.Content,
                 Content_ToolCall(StrView_Dup(arena, step.Tools[i].Id),
                                  StrView_Dup(arena, step.Tools[i].Name),
                                  StrView_Dup(arena, step.Tools[i].ArgsJson)));
    }
    message.Stop = StopReason::ToolUse;
  } else {
    message.Stop = step.Stop;
  }

  StreamEvent done = {};
  done.Kind = StreamEventKind::Done;
  StreamSink_Emit(sink, done);

  *out = message;
  return true;
}

inline Provider ScriptProvider_Make(ScriptProvider* script) {
  Provider provider = {};
  provider.Ctx = script;
  provider.Stream = ScriptProvider_Stream;
  return provider;
}
