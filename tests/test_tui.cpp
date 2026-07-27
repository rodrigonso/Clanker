#include "agent/agent.h"
#include "agent/event.h"
#include "base/arena.h"
#include "base/array.h"
#include "check.h"
#include "llm/faux.h"
#include "term/tui.h"

namespace {

bool AnyRow(Arena* a, Tui* t, const char* needle) {
  for (u32 i = 0; i < LineModel_Count(&t->Model); i++) {
    const SpanLine* sl = LineModel_At(&t->Model, i);
    StrBuf b = StrBuf_Make(a, 64);
    for (u32 j = 0; j < sl->Spans.Count; j++) StrBuf_Append(&b, sl->Spans.Data[j].Text);
    if (StrView_Contains(StrBuf_View(&b), needle)) return true;
  }
  return false;
}

// Feed a NUL-terminated byte sequence through the controller's input path.
void Feed(Tui* t, const char* s) {
  u32 n = 0;
  while (s[n]) n++;
  Tui_FeedInput(t, s, n);
}

void DrainDisplay(Tui* t, u64 startMs = 1000) {
  u32 frames = 0;
  while (Tui_HasPendingDisplay(t) && frames < 10000) {
    Tui_AdvanceStream(t, startMs);
    startMs += 16;
    frames++;
  }
  CHECK(!Tui_HasPendingDisplay(t));
}

}  // namespace

void TestTui() {
  Arena a = ArenaCreate();
  TuiOpts o;
  o.ModelName = SVL("test-model");
  o.Color = true;

  Tui t;
  Tui_InitModel(&t, &a, o);

  Message m = {};
  m.Kind = Role::Assistant;

  AgentEvent ev = {};
  ev.Kind = AgentEventKind::AgentStart;
  Tui_AgentEvent(&t, ev);
  CHECK(t.State == TuiState::Thinking);

  // Mirror the real agent loop: message_start for the assistant turn carries a
  // not-yet-built message (null), so the entry must be begun lazily on the first
  // text delta. (Passing &m here previously masked that the streamed assistant
  // text was being dropped in the TUI.)
  ev = {};
  ev.Kind = AgentEventKind::MessageStart;
  ev.Msg = nullptr;
  Tui_AgentEvent(&t, ev);

  ev = {};
  ev.Kind = AgentEventKind::MessageUpdate;
  ev.Stream.Kind = StreamEventKind::TextDelta;
  ev.Stream.Delta = SVL("Hello ");
  Tui_AgentEvent(&t, ev);
  CHECK(Tui_AdvanceStream(&t, 1000));
  CHECK(t.Model.Entries.Count == 1);
  CHECK(t.Model.Entries.Data[0].Text.Content.Length == 1);
  ev.Stream.Delta = SVL("**world**");
  Tui_AgentEvent(&t, ev);
  CHECK(Tui_AdvanceStream(&t, 1016));
  CHECK(t.Model.Entries.Data[0].Text.Content.Length < sizeof("Hello **world**") - 1);
  CHECK(t.State == TuiState::Streaming);

  m.Stop = StopReason::ToolUse;
  m.Use.Input = 12;
  m.Use.Output = 7;
  ev = {};
  ev.Kind = AgentEventKind::MessageEnd;
  ev.Msg = &m;
  Tui_AgentEvent(&t, ev);

  ev = {};
  ev.Kind = AgentEventKind::ToolStart;
  ev.ToolCallId = SVL("bash-1");
  ev.ToolName = SVL("bash");
  ev.ToolArgsJson = SVL("{\"command\":\"ls\"}");
  Tui_AgentEvent(&t, ev);
  CHECK(t.State == TuiState::Streaming);
  CHECK(t.Model.Entries.Count == 1);

  ToolResult tr = {};
  tr.Content = Array_Make<Content>(&a, 1);
  Array_Push(&tr.Content, Content_Text(SVL("hello-from-bash")));
  ev = {};
  ev.Kind = AgentEventKind::ToolEnd;
  ev.ToolCallId = SVL("bash-1");
  ev.Result = &tr;
  Tui_AgentEvent(&t, ev);

  ev = {};
  ev.Kind = AgentEventKind::AgentEnd;
  Tui_AgentEvent(&t, ev);
  CHECK(!t.AgentRunning);
  CHECK(t.State == TuiState::Streaming);
  DrainDisplay(&t, 1032);

  // The transcript captured the unlabeled assistant text and compact tool row.
  CHECK(!AnyRow(&a, &t, "assistant"));
  CHECK(AnyRow(&a, &t, "Hello world"));
  CHECK(AnyRow(&a, &t, "\xe2\x96\xa0  ls"));
  CHECK(!AnyRow(&a, &t, "hello-from-bash"));
  CHECK(t.State == TuiState::Idle);
  CHECK(t.Use.Output == 7);

  u64 assistantEntryId = t.Model.Entries.Data[0].Id;
  u64 toolEntryId = t.Model.Entries.Data[1].Id;
  CHECK(LineModel_IsEntryCollapsed(&t.Model, toolEntryId));

  // Hover maps screen rows through the last rendered viewport. Separator and
  // non-transcript rows are not interactive.
  t.LastTop = 0;
  t.LastPaneH = 3;
  Feed(&t, "\x1b[<35;50;1M");  // assistant row; x does not limit the button
  CHECK(t.HoveredEntryId == assistantEntryId);
  Feed(&t, "\x1b[<35;2;2M");  // separator
  CHECK(t.HoveredEntryId == 0);
  Feed(&t, "\x1b[<35;2;3M");  // tool summary
  CHECK(t.HoveredEntryId == toolEntryId);
  Feed(&t, "\x1b[<35;2;4M");  // below transcript pane
  CHECK(t.HoveredEntryId == 0);

  // Press/release on the same logical turn toggles it. Releasing on another
  // turn cancels activation.
  Feed(&t, "\x1b[<0;2;3M\x1b[<0;2;3m");
  CHECK(!LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  CHECK(AnyRow(&a, &t, "hello-from-bash"));
  t.LastPaneH = 4;
  Feed(&t, "\x1b[<35;2;4M");  // expanded preview row is content, not a button
  CHECK(t.HoveredEntryId == 0);
  Feed(&t, "\x1b[<0;2;4M\x1b[<0;2;4m");
  CHECK(!LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  Feed(&t, "\x1b[<0;2;3M\x1b[<0;2;1m");
  CHECK(!LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  Feed(&t, "\x1b[<0;2;3M\x1b[<0;2;3m");
  CHECK(LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  CHECK(!AnyRow(&a, &t, "hello-from-bash"));

  // Ctrl+O expands all when any tool is collapsed, then collapses an
  // all-expanded tool set.
  Feed(&t, "\x0f");
  CHECK(!LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  CHECK(AnyRow(&a, &t, "hello-from-bash"));
  Feed(&t, "\x0f");
  CHECK(LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  CHECK(!AnyRow(&a, &t, "hello-from-bash"));

  // No queued prompt input pending.
  CHECK(t.QCount == 0);

  // Mouse-wheel scrolling. Seed enough rows to scroll, then drive wheel events
  // through the input path. Wheel-up detaches follow and moves the viewport up;
  // enough wheel-down returns to the bottom and re-enables follow.
  for (u32 i = 0; i < 40; i++)
    LineModel_AddNotice(&t.Model, EntryKind::Info, SVL("filler"));

  // Global expansion above the viewport keeps the same logical top anchor.
  u64 anchorId = t.Model.Entries.Data[10].Id;
  u32 anchorRow = 0;
  CHECK(LineModel_FindEntryRows(&t.Model, anchorId, &anchorRow, nullptr));
  t.LastPaneH = 10;
  t.Top = anchorRow;
  t.LastTop = anchorRow;
  t.Follow = false;
  Feed(&t, "\x0f");
  CHECK(LineModel_RowEntryId(&t.Model, t.Top) == anchorId);
  Feed(&t, "\x0f");
  CHECK(LineModel_RowEntryId(&t.Model, t.Top) == anchorId);

  t.LastPaneH = 10;
  t.Follow = true;
  Feed(&t, "\x1b[<64;1;1M");  // wheel up
  CHECK(!t.Follow);
  u32 afterUp = t.Top;
  Feed(&t, "\x1b[<64;1;1M");  // wheel up again
  CHECK(t.Top < afterUp);
  for (u32 i = 0; i < 20; i++) Feed(&t, "\x1b[<65;1;1M");  // wheel down to the bottom
  CHECK(t.Follow);

  // Submit routing: while a turn is running, a submitted line steers the agent;
  // while idle, it becomes the next prompt (Tui_WaitForSubmit queue).
  {
    Arena a2 = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a2, model, SV("sys"));
    Agent_InitQueues(&ag);

    TuiOpts o2;
    o2.ModelName = SVL("m");
    o2.Color = false;
    Tui t2;
    Tui_InitModel(&t2, &a2, o2);
    t2.Ag = &ag;

    t2.AgentRunning = true;  // a turn is running
    Feed(&t2, "steer me\r");
    CHECK(ag.Steer.Count == 1);
    CHECK(t2.QCount == 0);  // did not land in the idle prompt queue
    DrainDisplay(&t2);
    CHECK(!AnyRow(&a2, &t2, "you"));
    CHECK(AnyRow(&a2, &t2, "steer me"));
    CHECK(t2.Model.Entries.Data[t2.Model.Entries.Count - 1].Text.Timestamp.Length == 5);

    t2.AgentRunning = false;  // between turns
    Feed(&t2, "next prompt\r");
    CHECK(t2.QCount == 1);
    CHECK(ag.Steer.Count == 1);  // unchanged

    Tui_Stop(&t2);
    ArenaRelease(&a2);
  }

  // End-to-end: a real agent turn (faux provider) must surface the assistant's
  // streamed text in the transcript after the display queue drains.
  {
    Arena a3 = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a3, model, SV("sys"));
    Agent_InitQueues(&ag);

    FauxStep step = {};
    step.Text = SVL("Here is the explanation");
    step.Stop = StopReason::Stop;
    Faux* f = ArenaPushStruct(&a3, Faux);
    *f = Faux{};
    f->Steps = &step;
    f->Count = 1;
    ag.Prov = Faux_Provider(f);

    TuiOpts o3;
    o3.ModelName = SVL("m");
    o3.Color = false;
    Tui t3;
    Tui_InitModel(&t3, &a3, o3);
    ag.Sink.User = &t3;
    ag.Sink.OnEvent = Tui_AgentEvent;

    Agent_Prompt(&ag, SV("ask"));
    DrainDisplay(&t3);

    CHECK(!AnyRow(&a3, &t3, "assistant"));
    CHECK(AnyRow(&a3, &t3, "Here is the explanation"));
    CHECK(t3.State == TuiState::Idle);

    Tui_Stop(&t3);
    ArenaRelease(&a3);
  }

  // The agent may complete tools and start another assistant turn before the
  // display catches up. Deferred events retain their data and visual order.
  {
    Arena aq = ArenaCreate();
    TuiOpts oq;
    oq.ModelName = SVL("m");
    oq.Color = false;
    Tui tq;
    Tui_InitModel(&tq, &aq, oq);

    AgentEvent q = {};
    q.Kind = AgentEventKind::AgentStart;
    Tui_AgentEvent(&tq, q);
    q = {};
    q.Kind = AgentEventKind::MessageUpdate;
    q.Stream.Kind = StreamEventKind::TextDelta;
    q.Stream.Delta = SVL("first response");
    Tui_AgentEvent(&tq, q);

    Message first = {};
    first.Kind = Role::Assistant;
    first.Stop = StopReason::ToolUse;
    q = {};
    q.Kind = AgentEventKind::MessageEnd;
    q.Msg = &first;
    Tui_AgentEvent(&tq, q);

    char args[] = "{\"command\":\"echo original\"}";
    q = {};
    q.Kind = AgentEventKind::ToolStart;
    q.ToolCallId = SVL("queued-tool");
    q.ToolName = SVL("bash");
    q.ToolArgsJson = StrView{args, (u32)sizeof(args) - 1};
    Tui_AgentEvent(&tq, q);

    char resultText[] = "original result";
    ToolResult result = {};
    result.Content = Array_Make<Content>(&aq, 1);
    Array_Push(&result.Content,
               Content_Text(StrView{resultText, (u32)sizeof(resultText) - 1}));
    q = {};
    q.Kind = AgentEventKind::ToolEnd;
    q.ToolCallId = SVL("queued-tool");
    q.Result = &result;
    Tui_AgentEvent(&tq, q);

    memset(args, 'x', sizeof(args) - 1);
    memset(resultText, 'z', sizeof(resultText) - 1);

    q = {};
    q.Kind = AgentEventKind::MessageUpdate;
    q.Stream.Kind = StreamEventKind::TextDelta;
    q.Stream.Delta = SVL("second response");
    Tui_AgentEvent(&tq, q);
    Message second = {};
    second.Kind = Role::Assistant;
    second.Stop = StopReason::Stop;
    q = {};
    q.Kind = AgentEventKind::MessageEnd;
    q.Msg = &second;
    Tui_AgentEvent(&tq, q);
    q = {};
    q.Kind = AgentEventKind::AgentEnd;
    Tui_AgentEvent(&tq, q);

    CHECK(tq.Model.Entries.Count == 0);
    DrainDisplay(&tq);
    CHECK(tq.Model.Entries.Count == 3);
    CHECK(tq.Model.Entries.Data[0].Kind == EntryKind::Assistant);
    CHECK(tq.Model.Entries.Data[1].Kind == EntryKind::Tool);
    CHECK(tq.Model.Entries.Data[2].Kind == EntryKind::Assistant);
    CHECK(AnyRow(&aq, &tq, "first response"));
    CHECK(AnyRow(&aq, &tq, "echo original"));
    CHECK(AnyRow(&aq, &tq, "second response"));
    CHECK(StrView_Eq(StrBuf_View(&tq.Model.Entries.Data[1].Tool.Preview),
                     SVL("original result")));
    CHECK(tq.State == TuiState::Idle);

    Tui_Stop(&tq);
    ArenaRelease(&aq);
  }

  // Tool output entering the TUI remains sanitized and capped at 1,500 bytes.
  {
    Arena a4 = ArenaCreate();
    TuiOpts o4;
    o4.ModelName = SVL("m");
    o4.Color = true;
    Tui t4;
    Tui_InitModel(&t4, &a4, o4);

    AgentEvent start = {};
    start.Kind = AgentEventKind::ToolStart;
    start.ToolCallId = SVL("long");
    start.ToolName = SVL("bash");
    start.ToolArgsJson = SVL("{\"command\":\"long-output\"}");
    Tui_AgentEvent(&t4, start);

    char output[1700];
    for (u32 i = 0; i < sizeof(output); i++) output[i] = (i == 20) ? '\x1b' : 'x';
    ToolResult longResult = {};
    longResult.Content = Array_Make<Content>(&a4, 1);
    Array_Push(&longResult.Content, Content_Text(StrView{output, (u32)sizeof(output)}));

    AgentEvent end = {};
    end.Kind = AgentEventKind::ToolEnd;
    end.ToolCallId = SVL("long");
    end.Result = &longResult;
    Tui_AgentEvent(&t4, end);
    DrainDisplay(&t4);

    CHECK(t4.Model.Entries.Count == 1);
    CHECK(t4.Model.Entries.Data[0].Tool.Preview.Length == 1500);
    CHECK(!StrView_Contains(StrBuf_View(&t4.Model.Entries.Data[0].Tool.Preview), "\x1b"));

    Tui_Stop(&t4);
    ArenaRelease(&a4);
  }

  // Only the first visual row of a turn is a button. Continuation content does
  // not hover or click, and dragging off the first row cancels activation.
  {
    Arena a5 = ArenaCreate();
    TuiOpts o5;
    o5.ModelName = SVL("m");
    o5.Color = true;
    Tui t5;
    Tui_InitModel(&t5, &a5, o5);
    LineModel_AddNotice(&t5.Model, EntryKind::Info, SVL("first row\ncontent row"));
    u64 noticeId = t5.Model.Entries.Data[0].Id;
    t5.LastTop = 0;
    t5.LastPaneH = 2;

    Feed(&t5, "\x1b[<35;2;1M");
    CHECK(t5.HoveredEntryId == noticeId);
    Feed(&t5, "\x1b[<35;2;2M");
    CHECK(t5.HoveredEntryId == 0);

    Feed(&t5, "\x1b[<0;2;1M\x1b[<32;2;2M\x1b[<32;2;1M\x1b[<0;2;1m");
    CHECK(!LineModel_IsEntryCollapsed(&t5.Model, noticeId));
    Feed(&t5, "\x1b[<0;2;1M\x1b[<0;2;1m");
    CHECK(LineModel_IsEntryCollapsed(&t5.Model, noticeId));

    Tui_Stop(&t5);
    ArenaRelease(&a5);
  }

  // Tool completion can insert an expanded preview above a detached viewport;
  // the logical top entry remains anchored.
  {
    Arena a6 = ArenaCreate();
    TuiOpts o6;
    o6.ModelName = SVL("m");
    o6.Color = true;
    Tui t6;
    Tui_InitModel(&t6, &a6, o6);
    LineModel_AddTool(&t6.Model, SVL("early"), SVL("bash"), SVL("{\"command\":\"echo\"}"));
    u64 toolId = t6.Model.Entries.Data[0].Id;
    CHECK(LineModel_ToggleEntry(&t6.Model, toolId));
    for (u32 i = 0; i < 12; i++)
      LineModel_AddNotice(&t6.Model, EntryKind::Info, SVL("later"));
    u64 completionAnchorId = t6.Model.Entries.Data[6].Id;
    u32 completionAnchorRow = 0;
    CHECK(
        LineModel_FindEntryRows(&t6.Model, completionAnchorId, &completionAnchorRow, nullptr));
    t6.Top = completionAnchorRow;
    t6.LastTop = completionAnchorRow;
    t6.LastPaneH = 4;
    t6.Follow = false;

    ToolResult inserted = {};
    inserted.Content = Array_Make<Content>(&a6, 1);
    Array_Push(&inserted.Content, Content_Text(SVL("preview one\npreview two")));
    AgentEvent finish = {};
    finish.Kind = AgentEventKind::ToolEnd;
    finish.ToolCallId = SVL("early");
    finish.Result = &inserted;
    Tui_AgentEvent(&t6, finish);
    DrainDisplay(&t6);
    CHECK(LineModel_RowEntryId(&t6.Model, t6.Top) == completionAnchorId);

    Tui_Stop(&t6);
    ArenaRelease(&a6);
  }

  Tui_Stop(&t);
  ArenaRelease(&a);
}
