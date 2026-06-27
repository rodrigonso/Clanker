#pragma once
#include "base/str.h"
#include "llm/types.h"

// Scripted provider for tests (port of pi's faux provider). Each step produces
// one assistant message: optional text, then an optional tool call. Drive a
// multi-turn run by sequencing steps (e.g. step 0 calls a tool, step 1 replies).
struct FauxStep {
  StrView Text;
  StrView ToolName;  // non-empty => emit a tool call
  StrView ToolId;
  StrView ToolArgsJson;
  StopReason Stop;  // used when no tool call
};

struct Faux {
  FauxStep* Steps;
  u32 Count;
  u32 Index;
};

inline bool Faux_Stream(void* ctx, Arena* arena, const Model* model, const LlmContext* context,
                        const StreamOptions* options, StreamSink sink, Message* out) {
  (void)model;
  (void)context;
  (void)options;
  Faux* f = (Faux*)ctx;
  FauxStep step = {};
  if (f->Index < f->Count) {
    step = f->Steps[f->Index];
    f->Index++;
  }

  StreamEvent ev = {};
  ev.Kind = StreamEventKind::Start;
  StreamSink_Emit(sink, ev);

  Message m = Message_AssistantEmpty(arena);
  m.Model = SVL("faux");
  m.Provider = SVL("faux");

  if (step.Text.Length) {
    u32 idx = m.Content.Count;
    StreamEvent ts = {};
    ts.Kind = StreamEventKind::TextStart;
    ts.ContentIndex = idx;
    StreamSink_Emit(sink, ts);
    StreamEvent td = {};
    td.Kind = StreamEventKind::TextDelta;
    td.ContentIndex = idx;
    td.Delta = step.Text;
    StreamSink_Emit(sink, td);
    StreamEvent te = {};
    te.Kind = StreamEventKind::TextEnd;
    te.ContentIndex = idx;
    StreamSink_Emit(sink, te);
    Array_Push(&m.Content, Content_Text(StrView_Dup(arena, step.Text)));
  }

  if (step.ToolName.Length) {
    u32 idx = m.Content.Count;
    StreamEvent cs = {};
    cs.Kind = StreamEventKind::ToolCallStart;
    cs.ContentIndex = idx;
    StreamSink_Emit(sink, cs);
    StreamEvent ce = {};
    ce.Kind = StreamEventKind::ToolCallEnd;
    ce.ContentIndex = idx;
    StreamSink_Emit(sink, ce);
    Array_Push(&m.Content, Content_ToolCall(StrView_Dup(arena, step.ToolId),
                                            StrView_Dup(arena, step.ToolName),
                                            StrView_Dup(arena, step.ToolArgsJson)));
    m.Stop = StopReason::ToolUse;
  } else {
    m.Stop = step.Stop;
  }

  StreamEvent done = {};
  done.Kind = StreamEventKind::Done;
  StreamSink_Emit(sink, done);

  *out = m;
  return true;
}

inline Provider Faux_Provider(Faux* f) {
  Provider p = {};
  p.Ctx = f;
  p.Stream = Faux_Stream;
  return p;
}
