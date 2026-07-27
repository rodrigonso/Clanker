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

struct BlockingSelect {
  Tui* Controller;
  const StrView* Options;
  u32 OptionCount;
  u32 Selected;
  bool Confirmed;
};

void RunBlockingSelect(void* user) {
  BlockingSelect* select = (BlockingSelect*)user;
  select->Confirmed =
      Tui_Select(select->Controller, SVL("Blocking"), select->Options, select->OptionCount,
                 &select->Selected) == TuiModalResult::Confirmed;
}

void InitHeadlessTui(Tui* t, Arena* a, bool color) {
  TuiOpts opts = {};
  opts.ModelName = SVL("m");
  opts.Color = color;
  Tui_InitModel(t, a, opts);
}

u16 ScrollbarColumn(const Tui* t) { return (u16)(t->Scr.Cols - 1); }

u32 ScrollMaxTop(const Tui* t) {
  u32 count = LineModel_Count(&t->Model);
  return count > t->LastPaneH ? count - t->LastPaneH : 0;
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

  // The scrollbar appears only on overflow, reserves the transcript's rightmost
  // column, maps track clicks, and keeps dragging outside the scrollbar column.
  {
    Arena as = ArenaCreate();
    Tui ts;
    InitHeadlessTui(&ts, &as, false);
    for (u32 i = 0; i < 18; i++)
      LineModel_AddNotice(&ts.Model, EntryKind::Info,
                          SVL("scroll row with enough text to wrap when narrowed"));

    Tui_ComposeFrame(&ts, 20, 12);
    u16 scrollbarX = ScrollbarColumn(&ts);
    CHECK(ts.ScrollbarVisible);
    CHECK(ts.Model.Width == 19);
    CHECK(ts.LastPaneH == 7);
    CHECK(scrollbarX == 19);
    CHECK(ts.LastScrollbarThumbH > 0 && ts.LastScrollbarThumbH < ts.LastPaneH);
    CHECK(ts.LastScrollbarThumbTop + ts.LastScrollbarThumbH == ts.LastPaneH);
    CHECK(Screen_BackAt(&ts.Scr, scrollbarX, ts.LastScrollbarThumbTop).Ch ==
          0x2595);
    CHECK((Screen_BackAt(&ts.Scr, scrollbarX, ts.LastScrollbarThumbTop).St.Attr &
           ATTR_BOLD) != 0);
    u16 trackY =
        ts.LastScrollbarThumbTop == 0 ? ts.LastScrollbarThumbH : (u16)0;
    CHECK(Screen_BackAt(&ts.Scr, scrollbarX, trackY).Ch == 0x2595);
    CHECK((Screen_BackAt(&ts.Scr, scrollbarX, trackY).St.Attr & ATTR_DIM) != 0);

    // Clicking the track centers the thumb near the clicked row and cannot
    // toggle the transcript entry under that screen row.
    u64 clickedId = LineModel_RowEntryId(&ts.Model, ts.LastTop + 3);
    CHECK(clickedId != 0);
    Feed(&ts, "\x1b[<0;20;4M\x1b[<0;20;4m");
    CHECK(!LineModel_IsEntryCollapsed(&ts.Model, clickedId));
    CHECK(!ts.ScrollbarDragging);
    CHECK(!ts.Follow);
    Tui_ComposeFrame(&ts, 20, 12);
    u16 thumbCenter =
        (u16)(ts.LastScrollbarThumbTop + ts.LastScrollbarThumbH / 2);
    CHECK(thumbCenter >= 2 && thumbCenter <= 4);

    // Hovering the reserved column is never transcript hover.
    Feed(&ts, "\x1b[<35;20;4M");
    CHECK(ts.HoveredEntryId == 0);

    // A top track click clamps to the beginning. Dragging the thumb to the
    // bottom continues after the pointer leaves the scrollbar column.
    Feed(&ts, "\x1b[<0;20;1M\x1b[<0;20;1m");
    CHECK(ts.Top == 0);
    CHECK(!ts.Follow);
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;20;1M\x1b[<32;10;7M\x1b[<0;10;7m");
    CHECK(!ts.ScrollbarDragging);
    CHECK(ts.Follow);
    CHECK(ts.Top == ScrollMaxTop(&ts));

    // The release position itself is applied even if no final motion event is
    // delivered.
    Feed(&ts, "\x1b[<0;20;1M\x1b[<0;20;1m");
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;20;1M\x1b[<0;10;7m");
    CHECK(ts.Follow);
    CHECK(ts.Top == ScrollMaxTop(&ts));

    // A detached logical top remains anchored when a resize rewraps the model.
    u64 resizeAnchorId = ts.Model.Entries.Data[6].Id;
    u32 resizeAnchorRow = 0;
    CHECK(LineModel_FindEntryRows(&ts.Model, resizeAnchorId, &resizeAnchorRow, nullptr));
    ts.Follow = false;
    ts.Top = resizeAnchorRow;
    Tui_ComposeFrame(&ts, 20, 12);
    Tui_ComposeFrame(&ts, 14, 12);
    CHECK(ts.ScrollbarVisible);
    CHECK(ts.Model.Width == 13);
    CHECK(LineModel_RowEntryId(&ts.Model, ts.Top) == resizeAnchorId);
    Tui_ComposeFrame(&ts, 14, 10);
    CHECK(ts.LastPaneH == 5);
    CHECK(LineModel_RowEntryId(&ts.Model, ts.Top) == resizeAnchorId);
    Tui_ComposeFrame(&ts, 5, 5);
    Tui_ComposeFrame(&ts, 14, 10);
    CHECK(LineModel_RowEntryId(&ts.Model, ts.Top) == resizeAnchorId);

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Collapsing enough content to fit removes the scrollbar and restores the
  // full transcript width. Tiny terminals also clear all scrollbar state.
  {
    Arena as = ArenaCreate();
    Tui ts;
    InitHeadlessTui(&ts, &as, true);
    LineModel_AddNotice(
        &ts.Model, EntryKind::Info,
        SVL("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"));
    u64 entryId = ts.Model.Entries.Data[0].Id;

    Tui_ComposeFrame(&ts, 20, 12);
    u16 scrollbarX = ScrollbarColumn(&ts);
    CHECK(ts.ScrollbarVisible);
    CHECK(Screen_BackAt(&ts.Scr, scrollbarX, ts.LastScrollbarThumbTop).St.Fg == 250);
    u16 colorTrackY =
        ts.LastScrollbarThumbTop == 0 ? ts.LastScrollbarThumbH : (u16)0;
    CHECK(Screen_BackAt(&ts.Scr, scrollbarX, colorTrackY).St.Fg == 238);
    ts.Follow = false;
    ts.Top = 0;
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;2;1M\x1b[<0;2;1m");
    CHECK(LineModel_IsEntryCollapsed(&ts.Model, entryId));
    Tui_ComposeFrame(&ts, 20, 12);
    CHECK(!ts.ScrollbarVisible);
    CHECK(ts.Model.Width == 20);
    CHECK(ts.LastScrollbarThumbH == 0);

    Tui_ComposeFrame(&ts, 5, 5);
    CHECK(!ts.ScrollbarVisible);
    CHECK(!ts.ScrollbarDragging);
    CHECK(ts.LastPaneH == 0);

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Resizing from a narrower overflowing layout rechecks the full width, so a
  // one-column wrap boundary cannot leave a history-dependent scrollbar.
  {
    Arena as = ArenaCreate();
    Tui ts;
    InitHeadlessTui(&ts, &as, true);
    LineModel_AddNotice(
        &ts.Model, EntryKind::Info,
        SVL("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));

    u16 boundaryCols = 0;
    for (u16 cols = 7; cols <= 80; cols++) {
      LineModel_SetWidth(&ts.Model, cols);
      u32 fullCount = LineModel_Count(&ts.Model);
      LineModel_SetWidth(&ts.Model, (u16)(cols - 1));
      u32 narrowCount = LineModel_Count(&ts.Model);
      if (fullCount <= 7 && narrowCount > 7) {
        boundaryCols = cols;
        break;
      }
    }
    CHECK(boundaryCols != 0);
    if (boundaryCols != 0) {
      Tui_ComposeFrame(&ts, (u16)(boundaryCols - 1), 12);
      CHECK(ts.ScrollbarVisible);
      Tui_ComposeFrame(&ts, boundaryCols, 12);
      CHECK(!ts.ScrollbarVisible);
      CHECK(ts.Model.Width == boundaryCols);
    }

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Submit routing: commands always queue for the main thread, with a notice when
  // queued behind an active turn. Ordinary text steers an active turn or waits
  // for application preflight before an idle prompt is rendered.
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
    CHECK(t2.QCount == 0);
    DrainDisplay(&t2);
    CHECK(!AnyRow(&a2, &t2, "you"));
    CHECK(AnyRow(&a2, &t2, "steer me"));
    CHECK(t2.Model.Entries.Data[t2.Model.Entries.Count - 1].Text.Timestamp.Length == 5);

    u32 entriesAfterSteer = t2.Model.Entries.Count;
    Feed(&t2, "  /model  \r");
    CHECK(t2.QCount == 1);
    CHECK(ag.Steer.Count == 1);
    DrainDisplay(&t2);
    CHECK(t2.Model.Entries.Count == entriesAfterSteer + 1);
    CHECK(AnyRow(&a2, &t2, "Queued command:"));
    CHECK(AnyRow(&a2, &t2, "/model"));

    TuiSubmission submission = {};
    CHECK(Tui_WaitForSubmit(&t2, &submission));
    CHECK(submission.Kind == TuiSubmissionKind::Command);
    CHECK(StrView_Eq(submission.Text, SVL("  /model  ")));

    t2.AgentRunning = false;  // between turns
    Feed(&t2, "next prompt\r");
    CHECK(t2.QCount == 1);
    CHECK(ag.Steer.Count == 1);  // unchanged
    CHECK(Tui_WaitForSubmit(&t2, &submission));
    CHECK(submission.Kind == TuiSubmissionKind::Prompt);
    CHECK(StrView_Eq(submission.Text, SVL("next prompt")));
    DrainDisplay(&t2);
    CHECK(!AnyRow(&a2, &t2, "next prompt"));
    CHECK(Tui_AcceptPrompt(&t2, submission.Text));
    DrainDisplay(&t2);
    CHECK(AnyRow(&a2, &t2, "next prompt"));
    t2.AgentRunning = false;

    u32 entriesAfterPrompt = t2.Model.Entries.Count;
    Feed(&t2, "/unknown\r");
    CHECK(t2.QCount == 1);
    CHECK(Tui_WaitForSubmit(&t2, &submission));
    CHECK(submission.Kind == TuiSubmissionKind::Command);
    CHECK(StrView_Eq(submission.Text, SVL("/unknown")));
    DrainDisplay(&t2);
    CHECK(t2.Model.Entries.Count == entriesAfterPrompt);
    CHECK(!AnyRow(&a2, &t2, "/unknown"));

    for (u32 i = 0; i < 64; i++) Feed(&t2, "queued prompt\r");
    CHECK(t2.QCount == 64);
    Feed(&t2, "/exit\r");
    CHECK(t2.QCount == 64);
    DrainDisplay(&t2);
    CHECK(AnyRow(&a2, &t2, "Submission queue is full"));

    Tui_Stop(&t2);
    ArenaRelease(&a2);
  }

  // Slash input opens a prefix-filtered command palette. Arrow keys select a
  // command, Enter submits its canonical name, and Escape dismisses the flyout
  // without modifying the composer.
  {
    Arena ap = ArenaCreate();
    TuiCommandSpec commands[] = {
        {SVL("login"), {}, SVL("Sign in")},
        {SVL("model"), {}, SVL("Change model")},
        {SVL("clear"), {}, SVL("Clear conversation")},
        {SVL("exit"), SVL("quit"), SVL("Exit")},
    };
    TuiOpts op;
    op.ModelName = SVL("m");
    op.Color = false;
    op.Commands = commands;
    op.CommandCount = 4;
    Tui tp;
    Tui_InitModel(&tp, &ap, op);

    Feed(&tp, "/");
    CHECK(StrView_Eq(LineEdit_Text(&tp.Edit), SVL("/")));
    CHECK(tp.CommandPaletteSelected == 0);
    Feed(&tp, "\x1b[B\x1b[B\r");
    TuiSubmission paletteSubmission = {};
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(paletteSubmission.Kind == TuiSubmissionKind::Command);
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/clear")));

    Feed(&tp, "/q\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/exit")));

    Feed(&tp, "/unknown\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/unknown")));

    Feed(&tp, "/");
    Feed(&tp, "\x1b");
    CHECK(tp.CommandPaletteDismissed);
    CHECK(StrView_Eq(LineEdit_Text(&tp.Edit), SVL("/")));
    Feed(&tp, "m\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/model")));

    CHECK(Tui_ClearSession(&tp));
    CHECK(tp.CommandCount == 4);
    CHECK(!tp.CommandPaletteDismissed);
    Feed(&tp, "/c\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/clear")));

    Tui_Stop(&tp);
    ArenaRelease(&ap);
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

  // Modal selectors route navigation/confirmation away from the composer and
  // preserve a cancellable manual text-entry path.
  {
    Arena am = ArenaCreate();
    TuiOpts om;
    om.ModelName = SVL("old-model");
    om.Color = false;
    Tui tm;
    Tui_InitModel(&tm, &am, om);

    StrView options[] = {SVL("first"), SVL("second"), SVL("third")};
    CHECK(Tui_BeginSelection(&tm, SVL("Choose"), options, 3));
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Pending);
    Feed(&tm, "\x1b[B\x1b[B\r");
    u32 selected = 0;
    CHECK(Tui_GetModalResult(&tm, &selected, nullptr) == TuiModalResult::Confirmed);
    CHECK(selected == 2);
    CHECK(LineEdit_Empty(&tm.Edit));

    CHECK(Tui_BeginSelection(&tm, SVL("Cancel"), options, 3, 1));
    Feed(&tm, "\x1b");
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Cancelled);

    CHECK(Tui_BeginSelection(&tm, SVL("Back"), options, 3, 1));
    Feed(&tm, "\x7f");
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Back);

    CHECK(Tui_BeginTextEntry(&tm, SVL("Model"), SVL("Enter model ID")));
    Feed(&tm, "claude-manual\r");
    StrView entered = {};
    CHECK(Tui_GetModalResult(&tm, nullptr, &entered) == TuiModalResult::Confirmed);
    CHECK(StrView_Eq(entered, SVL("claude-manual")));

    CHECK(Tui_BeginTextEntry(&tm, SVL("Model"), SVL("Enter model ID"),
                             SVL("x")));
    Feed(&tm, "\x7f");
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Pending);
    CHECK(LineEdit_Empty(&tm.ModalEdit));
    Feed(&tm, "\x7f");
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Back);

    Tui_OpenCommandPalette(&tm);
    CHECK(StrView_Eq(LineEdit_Text(&tm.Edit), SVL("/")));
    CHECK(tm.ModalKind == TuiModalKind::None);
    CHECK(tm.ModalResult == TuiModalResult::None);

    BlockingSelect blocking = {};
    blocking.Controller = &tm;
    blocking.Options = options;
    blocking.OptionCount = 3;
    Thread waiter = {};
    CHECK(Thread_Start(&waiter, RunBlockingSelect, &blocking));
    for (u32 i = 0; i < 100 &&
                    Tui_GetModalResult(&tm, nullptr, nullptr) != TuiModalResult::Pending;
         i++)
      Sleep_Ms(1);
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Pending);
    Feed(&tm, "\x1b[B\r");
    Thread_Join(&waiter);
    CHECK(blocking.Confirmed);
    CHECK(blocking.Selected == 1);

    Tui_Stop(&tm);
    ArenaRelease(&am);
  }

  // Command notices/errors, model status, and operation cancellation are
  // thread-safe state updates. Ctrl+C cancels an operation without exiting.
  {
    Arena ac = ArenaCreate();
    TuiOpts oc;
    oc.ModelName = SVL("before");
    oc.Color = false;
    Tui tc;
    Tui_InitModel(&tc, &ac, oc);

    Tui_CommandNotice(&tc, SVL("model discovery unavailable; using fallback"));
    Tui_CommandError(&tc, SVL("settings write failed"));
    DrainDisplay(&tc);
    CHECK(AnyRow(&ac, &tc, "model discovery unavailable"));
    CHECK(AnyRow(&ac, &tc, "settings write failed"));

    Tui_SetModelName(&tc, SVL("after"));
    CHECK(StrView_Eq(tc.ModelName, SVL("after")));

    Tui_BeginOperation(&tc, SVL("login"));
    Tui_SetOperationStatus(&tc, SVL("waiting for approval"));
    CHECK(tc.OperationActive);
    CHECK(StrView_Eq(tc.OperationLabel, SVL("login")));
    CHECK(StrView_Eq(tc.OperationStatus, SVL("waiting for approval")));
    CHECK(*Tui_OperationAbort(&tc) == 0);
    Feed(&tc, "\x03");
    CHECK(*Tui_OperationAbort(&tc) == 1);
    CHECK(!tc.Quit);
    CHECK(!tc.Stop);
    CHECK(Tui_EndOperation(&tc));
    CHECK(!tc.OperationActive);
    Tui_BeginOperation(&tc, SVL("models"));
    CHECK(*Tui_OperationAbort(&tc) == 0);
    StrView stopOptions[] = {SVL("one"), SVL("two")};
    CHECK(Tui_BeginSelection(&tc, SVL("Stop"), stopOptions, 2));
    Feed(&tc, "\x04");
    CHECK(*Tui_OperationAbort(&tc) == 1);
    CHECK(tc.Quit);
    CHECK(tc.Stop);
    CHECK(Tui_GetModalResult(&tc, nullptr, nullptr) == TuiModalResult::Cancelled);
    CHECK(Tui_EndOperation(&tc));
    Tui_BeginOperation(&tc, SVL("must not restart"));
    CHECK(!tc.OperationActive);
    CHECK(*Tui_OperationAbort(&tc) == 1);
    CHECK(!Tui_BeginSelection(&tc, SVL("Stopped"), stopOptions, 2));

    Tui_Stop(&tc);
    ArenaRelease(&ac);
  }

  // Session clear reclaims the resettable arena and returns all conversation/UI
  // state to idle while preserving terminal context, agent binding, and model.
  {
    Arena ar = ArenaCreate();
    TuiOpts orr;
    orr.ModelName = SVL("kept-model");
    orr.Color = false;
    Tui resetTui;
    Tui_InitModel(&resetTui, &ar, orr);
    Agent ag = Agent_Make(&ar, Model{}, SVL("sys"));
    Agent_InitQueues(&ag);
    resetTui.Ag = &ag;
    resetTui.Cwd = SVL("kept-cwd");
    resetTui.Branch = SVL("kept-branch");

    LineModel_AddNotice(&resetTui.Model, EntryKind::Info, SVL("old transcript"));
    LineEdit_InsertText(&resetTui.Edit, SVL("draft"));
    CHECK(LineEdit_Submit(&resetTui.Edit).Length == 5);
    Feed(&resetTui, "/queued\r");
    CHECK(resetTui.QCount == 1);
    LineEdit_InsertText(&resetTui.Edit, SVL("second draft"));
    Tui_CommandNotice(&resetTui, SVL("pending display"));
    StreamReveal_Push(&resetTui.Reveal, SVL("pending reveal"));
    resetTui.Use.Input = 11;
    resetTui.Use.Output = 7;
    resetTui.State = TuiState::Streaming;
    resetTui.StateTool = SVL("old tool");
    resetTui.TurnStartMs = 99;
    resetTui.QueuedCount = 3;
    resetTui.Top = 4;
    resetTui.LastTop = 3;
    resetTui.LastPaneH = 10;
    resetTui.Follow = false;
    resetTui.HoveredEntryId = 4;
    resetTui.PressedEntryId = 4;
    resetTui.HasMouse = true;
    Tui_BeginOperation(&resetTui, SVL("old operation"));
    StrView options[] = {SVL("one"), SVL("two")};
    CHECK(Tui_BeginSelection(&resetTui, SVL("old modal"), options, 2));
    usize usedBeforeClear = resetTui.SessionMem.Used;

    resetTui.AgentRunning = true;
    CHECK(!Tui_ClearSession(&resetTui));
    CHECK(resetTui.Model.Entries.Count != 0);
    resetTui.AgentRunning = false;

    CHECK(Agent_ResetSession(&ag));
    CHECK(Tui_ClearSession(&resetTui));
    CHECK(resetTui.SessionMem.Base != nullptr);
    CHECK(resetTui.SessionMem.Used > 0);
    CHECK(resetTui.SessionMem.Used < usedBeforeClear);
    CHECK(resetTui.Model.Entries.Count == 0);
    CHECK(LineModel_Count(&resetTui.Model) == 0);
    CHECK(LineEdit_Empty(&resetTui.Edit));
    CHECK(resetTui.Edit.History.Count == 0);
    CHECK(LineEdit_Empty(&resetTui.ModalEdit));
    CHECK(resetTui.ModalEdit.History.Count == 0);
    CHECK(resetTui.QCount == 1);
    CHECK(!Tui_HasPendingDisplay(&resetTui));
    CHECK(resetTui.DisplayHead == 0);
    CHECK(resetTui.Use.Input == 0 && resetTui.Use.Output == 0);
    CHECK(resetTui.State == TuiState::Idle);
    CHECK(resetTui.StateTool.Length == 0);
    CHECK(resetTui.TurnStartMs == 0);
    CHECK(resetTui.QueuedCount == 0);
    CHECK(resetTui.Follow);
    CHECK(resetTui.Top == 0 && resetTui.LastTop == 0 && resetTui.LastPaneH == 0);
    CHECK(resetTui.HoveredEntryId == 0 && resetTui.PressedEntryId == 0);
    CHECK(!resetTui.HasMouse);
    CHECK(!resetTui.OperationActive);
    CHECK(resetTui.ModalKind == TuiModalKind::None);
    CHECK(resetTui.ModalResult == TuiModalResult::None);
    CHECK(StrView_Eq(resetTui.ModelName, SVL("kept-model")));
    CHECK(StrView_Eq(resetTui.Cwd, SVL("kept-cwd")));
    CHECK(StrView_Eq(resetTui.Branch, SVL("kept-branch")));
    CHECK(resetTui.Ag == &ag);

    TuiSubmission preserved = {};
    CHECK(Tui_WaitForSubmit(&resetTui, &preserved));
    CHECK(preserved.Kind == TuiSubmissionKind::Command);
    CHECK(StrView_Eq(preserved.Text, SVL("/queued")));

    LineEdit_InsertText(&resetTui.Edit, SVL("new prompt"));
    CHECK(Tui_BeginSelection(&resetTui, SVL("new modal"), options, 2));
    Feed(&resetTui, "\r");
    CHECK(Tui_GetModalResult(&resetTui, nullptr, nullptr) == TuiModalResult::Confirmed);
    Tui_EndOperation(&resetTui);
    CHECK(Agent_Release(&ag));
    Tui_Stop(&resetTui);
    CHECK(!resetTui.SessionUp);
    CHECK(!resetTui.SyncUp);
    Tui_Stop(&resetTui);  // final cleanup is idempotent
    ArenaRelease(&ar);
  }

  Tui_Stop(&t);
  ArenaRelease(&a);
}
