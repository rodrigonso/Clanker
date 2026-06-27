#include "agent/agent.h"

#include "base/str.h"

// =============================================================================
// Synchronous agent loop. Port of pi's packages/agent/src/agent-loop.ts with a
// memory-first design: the transcript is the single source of truth (identity
// convertToLlm), so there is no per-turn duplication of messages, and streaming
// deltas are forwarded as zero-copy slices rather than whole-message clones.
// =============================================================================

Agent Agent_Make(Arena* arena, Model model, StrView systemPrompt) {
  Agent a = {};
  a.Mem = arena;
  a.SystemPrompt = systemPrompt;
  a.Model = model;
  a.Tools = Array_Make<Tool>(arena, 4);
  a.Messages = Array_Make<Message>(arena, 16);
  return a;
}

void Agent_AddTool(Agent* a, Tool t) { Array_Push(&a->Tools, t); }

static void Emit(Agent* a, const AgentEvent& ev) { AgentSink_Emit(a->Sink, ev); }

static void EmitMessageStartEnd(Agent* a, const Message* m) {
  AgentEvent s = {};
  s.Kind = AgentEventKind::MessageStart;
  s.Msg = m;
  Emit(a, s);
  AgentEvent e = {};
  e.Kind = AgentEventKind::MessageEnd;
  e.Msg = m;
  Emit(a, e);
}

// Forwards streaming deltas from the provider to the agent sink as
// message_update events. Zero-copy: ev.Delta is a slice of the provider buffer.
static void StreamAdapter(void* user, const StreamEvent& ev) {
  Agent* a = (Agent*)user;
  if (ev.Kind == StreamEventKind::TextDelta || ev.Kind == StreamEventKind::ThinkingDelta ||
      ev.Kind == StreamEventKind::ToolCallStart) {
    AgentEvent ae = {};
    ae.Kind = AgentEventKind::MessageUpdate;
    ae.Stream = ev;
    Emit(a, ae);
  }
}

static Tool* FindTool(Agent* a, StrView name) {
  for (u32 i = 0; i < a->Tools.Count; i++) {
    if (StrView_Eq(a->Tools.Data[i].Name, name)) return &a->Tools.Data[i];
  }
  return nullptr;
}

// Streams one assistant response, appends it to the transcript, and returns a
// stable-by-value copy (its Content array lives in the arena).
static bool StreamAssistant(Agent* a, Message* outAssistant) {
  ToolDef* defs = nullptr;
  if (a->Tools.Count) {
    defs = ArenaPushArray(a->Mem, ToolDef, a->Tools.Count);
    for (u32 i = 0; i < a->Tools.Count; i++) defs[i] = Tool_ToDef(a->Tools.Data[i]);
  }
  LlmContext ctx = {};
  ctx.SystemPrompt = a->SystemPrompt;
  ctx.Messages = a->Messages.Data;
  ctx.MessageCount = a->Messages.Count;
  ctx.Tools = defs;
  ctx.ToolCount = a->Tools.Count;

  AgentEvent ms = {};
  ms.Kind = AgentEventKind::MessageStart;
  ms.Msg = nullptr;  // assistant message not built yet
  Emit(a, ms);

  StreamSink sink = {};
  sink.User = a;
  sink.OnEvent = StreamAdapter;

  // Resolve auth per call so OAuth tokens refresh mid-session.
  StreamOptions opts = a->Stream;
  if (a->ResolveAuth) {
    StrView token;
    if (a->ResolveAuth(a->AuthUser, a->Mem, a->Model.Provider, &token)) opts.ApiKey = token;
  }

  Message assistant = {};
  bool ok = a->Prov.Stream(a->Prov.Ctx, a->Mem, &a->Model, &ctx, &opts, sink, &assistant);

  Message* stored = Array_Push(&a->Messages, assistant);
  AgentEvent me = {};
  me.Kind = AgentEventKind::MessageEnd;
  me.Msg = stored;
  Emit(a, me);

  *outAssistant = assistant;
  return ok;
}

// Executes one tool call, appends a toolResult message, and updates allTerminate.
static void ExecuteTool(Agent* a, const Content* tc, bool* allTerminate) {
  AgentEvent s = {};
  s.Kind = AgentEventKind::ToolStart;
  s.ToolCallId = tc->ToolId;
  s.ToolName = tc->ToolName;
  s.ToolArgsJson = tc->ToolArgsJson;
  Emit(a, s);

  ToolResult result = {};
  result.Content = Array_Make<Content>(a->Mem, 1);
  bool isError = false;

  Tool* tool = FindTool(a, tc->ToolName);
  if (!tool) {
    StrBuf e = StrBuf_Make(a->Mem, 64);
    StrBuf_AppendCStr(&e, "Tool not found: ");
    StrBuf_Append(&e, tc->ToolName);
    Array_Push(&result.Content, Content_Text(StrBuf_View(&e)));
    isError = true;
  } else {
    StrView reason = {};
    bool blocked = a->BeforeTool && !a->BeforeTool(a->User, a->Mem, tc->ToolName, tc->ToolArgsJson, &reason);
    if (blocked) {
      StrBuf e = StrBuf_Make(a->Mem, 64);
      StrBuf_AppendCStr(&e, reason.Length ? "" : "Tool blocked");
      StrBuf_Append(&e, reason);
      Array_Push(&result.Content, Content_Text(StrBuf_View(&e)));
      isError = true;
    } else {
      StrBuf err = StrBuf_Make(a->Mem, 64);
      ToolResult r = {};
      r.Content = Array_Make<Content>(a->Mem, 1);
      bool ok = tool->Execute(tool->Ctx, a->Mem, tc->ToolId, tc->ToolArgsJson, a->Stream.Abort, &r, &err);
      if (ok) {
        result = r;
      } else {
        Array_Push(&result.Content, Content_Text(StrBuf_View(&err)));
        isError = true;
      }
    }
  }

  if (!result.Terminate) *allTerminate = false;

  AgentEvent e = {};
  e.Kind = AgentEventKind::ToolEnd;
  e.ToolCallId = tc->ToolId;
  e.ToolName = tc->ToolName;
  e.Result = &result;
  e.IsError = isError;
  Emit(a, e);

  Message tr = {};
  tr.Kind = Role::ToolResult;
  tr.Content = result.Content;
  tr.ToolCallId = tc->ToolId;
  tr.ToolName = tc->ToolName;
  tr.IsError = isError;
  tr.Timestamp = NowMillis();
  Message* stored = Array_Push(&a->Messages, tr);
  EmitMessageStartEnd(a, stored);
}

static bool PollSteering(Agent* a, StrView* out) {
  return a->PollSteering && a->PollSteering(a->User, a->Mem, out);
}
static bool PollFollowUp(Agent* a, StrView* out) {
  return a->PollFollowUp && a->PollFollowUp(a->User, a->Mem, out);
}

static void RunLoop(Agent* a, const StrView* promptText) {
  a->Streaming = true;

  AgentEvent start = {};
  start.Kind = AgentEventKind::AgentStart;
  Emit(a, start);

  AgentEvent turn = {};
  turn.Kind = AgentEventKind::TurnStart;
  Emit(a, turn);

  if (promptText) {
    Message um = Message_User(a->Mem, StrView_Dup(a->Mem, *promptText));
    Message* stored = Array_Push(&a->Messages, um);
    EmitMessageStartEnd(a, stored);
  }

  bool firstTurn = true;
  StrView pending = {};
  bool hasPending = PollSteering(a, &pending);

  while (true) {
    bool hasMoreToolCalls = true;
    while (hasMoreToolCalls || hasPending) {
      if (!firstTurn) {
        AgentEvent ts = {};
        ts.Kind = AgentEventKind::TurnStart;
        Emit(a, ts);
      } else {
        firstTurn = false;
      }

      if (hasPending) {
        Message um = Message_User(a->Mem, StrView_Dup(a->Mem, pending));
        Message* stored = Array_Push(&a->Messages, um);
        EmitMessageStartEnd(a, stored);
        hasPending = false;
      }

      Message assistant = {};
      bool ok = StreamAssistant(a, &assistant);

      if (!ok || assistant.Stop == StopReason::Error || assistant.Stop == StopReason::Aborted) {
        AgentEvent te = {};
        te.Kind = AgentEventKind::TurnEnd;
        te.Msg = &assistant;
        Emit(a, te);
        AgentEvent ae = {};
        ae.Kind = AgentEventKind::AgentEnd;
        Emit(a, ae);
        a->Streaming = false;
        return;
      }

      hasMoreToolCalls = false;
      bool anyTool = false;
      bool allTerminate = true;
      for (u32 i = 0; i < assistant.Content.Count; i++) {
        const Content* c = &assistant.Content.Data[i];
        if (c->Kind != ContentKind::ToolCall) continue;
        anyTool = true;
        ExecuteTool(a, c, &allTerminate);
      }
      if (anyTool && !allTerminate) hasMoreToolCalls = true;

      AgentEvent te = {};
      te.Kind = AgentEventKind::TurnEnd;
      te.Msg = &assistant;
      Emit(a, te);

      hasPending = PollSteering(a, &pending);
    }

    if (PollFollowUp(a, &pending)) {
      hasPending = true;
      continue;
    }
    break;
  }

  AgentEvent end = {};
  end.Kind = AgentEventKind::AgentEnd;
  Emit(a, end);
  a->Streaming = false;
}

void Agent_Prompt(Agent* a, StrView userText) { RunLoop(a, &userText); }

void Agent_Continue(Agent* a) {
  FFI(a->Messages.Count == 0);
  Message* last = Array_Last(&a->Messages);
  FFI(last->Kind == Role::Assistant);
  RunLoop(a, nullptr);
}
