#include "agent/event.h"
#include "agent/harness.h"
#include "base/arena.h"
#include "base/array.h"
#include "check.h"
#include "llm/faux.h"
#include "session/memory_repository.h"
#include "term/tui.h"

namespace {

bool g_displayDrainsOk;

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
  g_displayDrainsOk = !Tui_HasPendingDisplay(t) && g_displayDrainsOk;
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

bool ScrollbarRendered(const Tui* t, u16 x) {
  u16 trackY = t->LastScrollbarThumbTop == 0 ? t->LastScrollbarThumbH : (u16)0;
  return t->ScrollbarVisible && t->Model.Width == 19 && t->LastPaneH == 6 && x == 19 &&
         t->LastScrollbarThumbH > 0 && t->LastScrollbarThumbH < t->LastPaneH &&
         t->LastScrollbarThumbTop + t->LastScrollbarThumbH == t->LastPaneH &&
         Screen_BackAt(&t->Scr, x, t->LastScrollbarThumbTop).Ch == 0x2595 &&
         (Screen_BackAt(&t->Scr, x, t->LastScrollbarThumbTop).St.Attr & ATTR_BOLD) != 0 &&
         Screen_BackAt(&t->Scr, x, trackY).Ch == 0x2595 &&
         (Screen_BackAt(&t->Scr, x, trackY).St.Attr & ATTR_DIM) != 0;
}

bool SessionUiCleared(const Tui* t, usize usedBefore) {
  return t->SessionMem.Base != nullptr && t->SessionMem.Used > 0 &&
         t->SessionMem.Used < usedBefore && t->Model.Entries.Count == 0 &&
         LineModel_Count(&t->Model) == 0 && LineEdit_Empty(&t->Edit) &&
         t->Edit.History.Count == 0 && LineEdit_Empty(&t->ModalEdit) &&
         t->ModalEdit.History.Count == 0 && !Tui_HasPendingDisplay(t) &&
         t->DisplayHead == 0 && t->Use.Input == 0 && t->Use.Output == 0 &&
         t->State == TuiState::Idle && t->StateTool.Length == 0 && t->TurnStartMs == 0 &&
         t->QueuedCount == 0 && t->Follow && t->Top == 0 && t->LastTop == 0 &&
         t->LastPaneH == 0 && t->HoveredEntryId == 0 && t->PressedEntryId == 0 &&
         !t->HasMouse && !t->OperationActive && t->ModalKind == TuiModalKind::None &&
         t->ModalResult == TuiModalResult::None;
}

bool SessionContextPreserved(const Tui* t, const AgentHarness* harness) {
  return StrView_Eq(t->ModelName, SVL("kept-model")) &&
         StrView_Eq(t->Cwd, SVL("kept-cwd")) &&
         StrView_Eq(t->Branch, SVL("kept-branch")) &&
         t->Harness == harness;
}

bool InitTestHarness(Arena* arena, MemoryRepository* memory,
                     AgentHarness* harness, Faux* faux) {
  MemoryRepository_Init(memory);
  SessionRepository repository = MemoryRepository_AsRepository(memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("tui-test");
  create.WorkingDirectory = SVL("C:\\tui-test");
  SessionLog log = {};
  SessionInfo info = {};
  if (!SessionResult_IsOk(
          repository.Create(repository.Ctx, arena, &create, &log, &info)))
    return false;
  AgentHarnessConfig config = {};
  config.SystemPrompt = SVL("sys");
  config.ModelInfo.Provider = SVL("faux");
  config.ModelInfo.Id = SVL("faux-model");
  config.ProviderImpl = Faux_Provider(faux);
  config.ToolExec = ToolExecution::Parallel;
  return SessionResult_IsOk(AgentHarness_Init(harness, log, &config));
}

}  // namespace

void TestTui() {
  g_displayDrainsOk = true;
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
  CHECK_ALL(t.Model.Entries.Count == 1,
            t.Model.Entries.Data[0].Text.Content.Length == 1);
  ev.Stream.Delta = SVL("**world**");
  Tui_AgentEvent(&t, ev);
  CHECK(Tui_AdvanceStream(&t, 1016));
  CHECK_ALL(t.Model.Entries.Data[0].Text.Content.Length < sizeof("Hello **world**") - 1,
            t.State == TuiState::Streaming);

  m.Stop = StopReason::ToolUse;
  m.Use.Input = 12;
  m.Use.Output = 7;
  ev = {};
  ev.Kind = AgentEventKind::MessageEnd;
  ev.Msg = &m;
  Tui_AgentEvent(&t, ev);

  ev = {};
  ev.Kind = AgentEventKind::ToolStart;
  ev.ToolCallId = SVL("shell-1");
  ev.ToolName = SVL("shell");
  ev.ToolArgsJson = SVL("{\"command\":\"ls\"}");
  Tui_AgentEvent(&t, ev);
  CHECK(t.State == TuiState::Streaming);
  CHECK(t.Model.Entries.Count == 1);

  ToolResult tr = {};
  tr.Content = Array_Make<Content>(&a, 1);
  Array_Push(&tr.Content, Content_Text(SVL("hello-from-shell")));
  ev = {};
  ev.Kind = AgentEventKind::ToolEnd;
  ev.ToolCallId = SVL("shell-1");
  ev.Result = &tr;
  Tui_AgentEvent(&t, ev);

  ev = {};
  ev.Kind = AgentEventKind::AgentEnd;
  Tui_AgentEvent(&t, ev);
  CHECK_ALL(!t.AgentRunning, t.State == TuiState::Streaming);
  DrainDisplay(&t, 1032);

  // The transcript captured the unlabeled assistant text and compact tool row.
  CHECK_ALL(!AnyRow(&a, &t, "assistant"), AnyRow(&a, &t, "Hello world"),
            AnyRow(&a, &t, "\xe2\x96\xa0  Shell ls"),
            !AnyRow(&a, &t, "hello-from-shell"), t.State == TuiState::Idle,
            t.Use.Output == 7);

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
  CHECK(AnyRow(&a, &t, "hello-from-shell"));
  t.LastPaneH = 4;
  Feed(&t, "\x1b[<35;2;4M");  // expanded preview row is content, not a button
  CHECK(t.HoveredEntryId == 0);
  Feed(&t, "\x1b[<0;2;4M\x1b[<0;2;4m");
  CHECK(!LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  Feed(&t, "\x1b[<0;2;3M\x1b[<0;2;1m");
  CHECK(!LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  Feed(&t, "\x1b[<0;2;3M\x1b[<0;2;3m");
  CHECK(LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  CHECK(!AnyRow(&a, &t, "hello-from-shell"));

  // Ctrl+O expands all when any tool is collapsed, then collapses an
  // all-expanded tool set.
  Feed(&t, "\x0f");
  CHECK(!LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  CHECK(AnyRow(&a, &t, "hello-from-shell"));
  Feed(&t, "\x0f");
  CHECK(LineModel_IsEntryCollapsed(&t.Model, toolEntryId));
  CHECK(!AnyRow(&a, &t, "hello-from-shell"));

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

  // A blank row separates the transcript viewport from the sticky cwd/branch
  // context line above the composer.
  {
    Arena as = ArenaCreate();
    Tui ts;
    InitHeadlessTui(&ts, &as, false);
    LineModel_AddNotice(&ts.Model, EntryKind::Info,
                        SVL("one\ntwo\nthree\nfour\nfive\nsix\nseven"));
    Tui_SetStartupContext(&ts, SVL("cwd"), SVL("branch"));

    Tui_ComposeFrame(&ts, 20, 12);
    bool gapBlank = true;
    for (u16 x = 0; x < ts.Scr.Cols; x++) {
      if (Screen_BackAt(&ts.Scr, x, ts.LastPaneH).Ch != 0) gapBlank = false;
    }
    CHECK_ALL(ts.LastPaneH == 6, gapBlank,
              Screen_BackAt(&ts.Scr, 1, (u16)(ts.LastPaneH + 1)).Ch == 'c');

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

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
    CHECK(ScrollbarRendered(&ts, scrollbarX));

    // Clicking the track centers the thumb near the clicked row and cannot
    // toggle the transcript entry under that screen row.
    u64 clickedId = LineModel_RowEntryId(&ts.Model, ts.LastTop + 3);
    CHECK(clickedId != 0);
    Feed(&ts, "\x1b[<0;20;4M\x1b[<0;20;4m");
    CHECK_ALL(!LineModel_IsEntryCollapsed(&ts.Model, clickedId),
              !ts.ScrollbarDragging, !ts.Follow);
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
    CHECK_ALL(ts.Top == 0, !ts.Follow);
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;20;1M\x1b[<32;10;7M\x1b[<0;10;7m");
    CHECK_ALL(!ts.ScrollbarDragging, ts.Follow, ts.Top == ScrollMaxTop(&ts));

    // The release position itself is applied even if no final motion event is
    // delivered.
    Feed(&ts, "\x1b[<0;20;1M\x1b[<0;20;1m");
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;20;1M\x1b[<0;10;7m");
    CHECK_ALL(ts.Follow, ts.Top == ScrollMaxTop(&ts));

    // A detached logical top remains anchored when a resize rewraps the model.
    u64 resizeAnchorId = ts.Model.Entries.Data[6].Id;
    u32 resizeAnchorRow = 0;
    CHECK(LineModel_FindEntryRows(&ts.Model, resizeAnchorId, &resizeAnchorRow, nullptr));
    ts.Follow = false;
    ts.Top = resizeAnchorRow;
    Tui_ComposeFrame(&ts, 20, 12);
    Tui_ComposeFrame(&ts, 14, 12);
    CHECK_ALL(ts.ScrollbarVisible, ts.Model.Width == 13,
              LineModel_RowEntryId(&ts.Model, ts.Top) == resizeAnchorId);
    Tui_ComposeFrame(&ts, 14, 10);
    CHECK_ALL(ts.LastPaneH == 4,
              LineModel_RowEntryId(&ts.Model, ts.Top) == resizeAnchorId);
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
    CHECK_ALL(ts.ScrollbarVisible,
              Screen_BackAt(&ts.Scr, scrollbarX, ts.LastScrollbarThumbTop).St.Fg == 250);
    u16 colorTrackY =
        ts.LastScrollbarThumbTop == 0 ? ts.LastScrollbarThumbH : (u16)0;
    CHECK(Screen_BackAt(&ts.Scr, scrollbarX, colorTrackY).St.Fg == 238);
    ts.Follow = false;
    ts.Top = 0;
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;2;1M\x1b[<0;2;1m");
    CHECK(LineModel_IsEntryCollapsed(&ts.Model, entryId));
    Tui_ComposeFrame(&ts, 20, 12);
    CHECK_ALL(!ts.ScrollbarVisible, ts.Model.Width == 20,
              ts.LastScrollbarThumbH == 0);

    Tui_ComposeFrame(&ts, 5, 5);
    CHECK_ALL(!ts.ScrollbarVisible, !ts.ScrollbarDragging, ts.LastPaneH == 0);

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
      if (fullCount <= 6 && narrowCount > 6) {
        boundaryCols = cols;
        break;
      }
    }
    CHECK(boundaryCols != 0);
    if (boundaryCols != 0) {
      Tui_ComposeFrame(&ts, (u16)(boundaryCols - 1), 12);
      CHECK(ts.ScrollbarVisible);
      Tui_ComposeFrame(&ts, boundaryCols, 12);
      CHECK_ALL(!ts.ScrollbarVisible, ts.Model.Width == boundaryCols);
    }

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Submit routing: commands always queue for the main thread, with a notice when
  // queued behind an active turn. Ordinary text steers an active turn or waits
  // for application preflight before an idle prompt is rendered.
  {
    Arena a2 = ArenaCreate();
    Faux faux = {};
    MemoryRepository memory = {};
    AgentHarness harness = {};
    CHECK(InitTestHarness(&a2, &memory, &harness, &faux));

    TuiOpts o2;
    o2.ModelName = SVL("m");
    o2.Color = false;
    Tui t2;
    Tui_InitModel(&t2, &a2, o2);
    t2.Harness = &harness;

    t2.AgentRunning = true;  // a turn is running
    Feed(&t2, "steer me\r");
    SessionReduction queued = {};
    CHECK(AgentHarness_Snapshot(&a2, &harness, &queued).Status ==
          SessionStatus::Ok);
    CHECK_ALL(queued.PendingQueueItems.Count == 1, t2.QCount == 0);
    DrainDisplay(&t2);
    CHECK_ALL(!AnyRow(&a2, &t2, "you"), AnyRow(&a2, &t2, "steer me"),
              t2.Model.Entries.Data[t2.Model.Entries.Count - 1].Text.Timestamp.Length == 5);

    u32 entriesAfterSteer = t2.Model.Entries.Count;
    Feed(&t2, "  /model  \r");
    CHECK_ALL(t2.QCount == 1, queued.PendingQueueItems.Count == 1);
    DrainDisplay(&t2);
    CHECK_ALL(t2.Model.Entries.Count == entriesAfterSteer + 1,
              AnyRow(&a2, &t2, "Queued command:"), AnyRow(&a2, &t2, "/model"));

    TuiSubmission submission = {};
    CHECK(Tui_WaitForSubmit(&t2, &submission));
    CHECK_ALL(submission.Kind == TuiSubmissionKind::Command,
              StrView_Eq(submission.Text, SVL("  /model  ")));

    t2.AgentRunning = false;  // between turns
    Feed(&t2, "next prompt\r");
    CHECK_ALL(t2.QCount == 1, queued.PendingQueueItems.Count == 1);
    CHECK(Tui_WaitForSubmit(&t2, &submission));
    CHECK_ALL(submission.Kind == TuiSubmissionKind::Prompt,
              StrView_Eq(submission.Text, SVL("next prompt")));
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
    CHECK_ALL(submission.Kind == TuiSubmissionKind::Command,
              StrView_Eq(submission.Text, SVL("/unknown")));
    DrainDisplay(&t2);
    CHECK_ALL(t2.Model.Entries.Count == entriesAfterPrompt,
              !AnyRow(&a2, &t2, "/unknown"));

    for (u32 i = 0; i < 64; i++) Feed(&t2, "queued prompt\r");
    CHECK(t2.QCount == 64);
    Feed(&t2, "/exit\r");
    CHECK(t2.QCount == 64);
    DrainDisplay(&t2);
    CHECK(AnyRow(&a2, &t2, "Submission queue is full"));

    Tui_Stop(&t2);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    MemoryRepository_Destroy(&memory);
    ArenaRelease(&a2);
  }

  // Double Escape cancels only an active agent turn. The presses may arrive
  // separately or in one terminal read; expired/interrupted sequences re-arm.
  {
    Arena ae = ArenaCreate();
    TuiOpts oe;
    oe.ModelName = SVL("m");
    oe.Color = false;
    Tui te;
    Tui_InitModel(&te, &ae, oe);

    Feed(&te, "\x1b\x1b");
    CHECK(*Tui_Abort(&te) == 0);
    CHECK(!te.EscapeArmed);

    te.AgentRunning = true;
    Feed(&te, "\x1b");
    CHECK(*Tui_Abort(&te) == 0);
    CHECK(te.EscapeArmed);
    te.LastEscapeMs = MonotonicMillis() - 501;
    Feed(&te, "\x1b");
    CHECK(*Tui_Abort(&te) == 0);
    CHECK(te.EscapeArmed);

    Feed(&te, "x");
    CHECK(!te.EscapeArmed);
    Feed(&te, "\x1b");
    CHECK(*Tui_Abort(&te) == 0);
    Feed(&te, "\x1b");
    CHECK(*Tui_Abort(&te) == 1);
    CHECK(!te.EscapeArmed);

    *Tui_Abort(&te) = 0;
    Feed(&te, "\x1b\x1b");
    CHECK(*Tui_Abort(&te) == 1);

    *Tui_Abort(&te) = 0;
    te.AgentRunning = false;
    Tui_BeginOperation(&te, SVL("preflight"));
    Feed(&te, "\x1b\x1b");
    CHECK(*Tui_OperationAbort(&te) == 0);
    CHECK(!Tui_EndOperation(&te));

    Message interrupted = {};
    interrupted.Kind = Role::Assistant;
    interrupted.Stop = StopReason::Aborted;
    AgentEvent event = {};
    event.Kind = AgentEventKind::MessageEnd;
    event.Msg = &interrupted;
    Tui_AgentEvent(&te, event);
    DrainDisplay(&te);
    CHECK(AnyRow(&ae, &te, "Interrupted by user."));

    Tui_Stop(&te);
    ArenaRelease(&ae);
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
    CHECK_ALL(paletteSubmission.Kind == TuiSubmissionKind::Command,
              StrView_Eq(paletteSubmission.Text, SVL("/clear")));

    Feed(&tp, "/q\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/exit")));

    Feed(&tp, "/unknown\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/unknown")));

    Feed(&tp, "/");
    tp.AgentRunning = true;
    Feed(&tp, "\x1b");
    CHECK_ALL(tp.CommandPaletteDismissed,
              StrView_Eq(LineEdit_Text(&tp.Edit), SVL("/")));
    CHECK(*Tui_Abort(&tp) == 0);
    Feed(&tp, "\x1b");
    CHECK(*Tui_Abort(&tp) == 1);
    tp.AgentRunning = false;
    *Tui_Abort(&tp) = 0;
    Feed(&tp, "m\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/model")));

    CHECK(Tui_ClearSession(&tp));
    CHECK_ALL(tp.CommandCount == 4, !tp.CommandPaletteDismissed);
    Feed(&tp, "/c\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/clear")));

    Tui_Stop(&tp);
    ArenaRelease(&ap);
  }

  // A durable snapshot hydrates a completed faux-provider turn.
  {
    Arena a3 = ArenaCreate();
    FauxStep step = {};
    step.Text = SVL("Here is the explanation");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    MemoryRepository memory = {};
    AgentHarness harness = {};
    CHECK(InitTestHarness(&a3, &memory, &harness, &faux));

    TuiOpts o3;
    o3.ModelName = SVL("m");
    o3.Color = false;
    Tui t3;
    Tui_InitModel(&t3, &a3, o3);
    t3.Harness = &harness;

    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SV("ask"))));
    AgentHarnessSnapshot snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_SnapshotFull(&a3, &harness, &snapshot)));
    Tui_HydrateTranscript(&t3, snapshot.Transcript.Data,
                          snapshot.Transcript.Count);
    u32 hydratedEntries = t3.Model.Entries.Count;
    Tui_ReplaceTranscript(&t3, snapshot.Transcript.Data,
                          snapshot.Transcript.Count);

    CHECK_ALL(!AnyRow(&a3, &t3, "assistant"),
              AnyRow(&a3, &t3, "Here is the explanation"),
              t3.Model.Entries.Count == hydratedEntries,
              t3.State == TuiState::Idle);

    Tui_Stop(&t3);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    MemoryRepository_Destroy(&memory);
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
    q.ToolName = SVL("shell");
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
    CHECK_ALL(tq.Model.Entries.Count == 3,
              tq.Model.Entries.Data[0].Kind == EntryKind::Assistant,
              tq.Model.Entries.Data[1].Kind == EntryKind::Tool,
              tq.Model.Entries.Data[2].Kind == EntryKind::Assistant);
    CHECK_ALL(AnyRow(&aq, &tq, "first response"), AnyRow(&aq, &tq, "echo original"),
              AnyRow(&aq, &tq, "second response"),
              StrView_Eq(StrBuf_View(&tq.Model.Entries.Data[1].Tool.Preview),
                         SVL("original result")),
              tq.State == TuiState::Idle);

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
    start.ToolName = SVL("shell");
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

    CHECK_ALL(
        t4.Model.Entries.Count == 1,
        t4.Model.Entries.Data[0].Tool.Preview.Length == 1500,
        !StrView_Contains(StrBuf_View(&t4.Model.Entries.Data[0].Tool.Preview), "\x1b"));

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

    t5.Follow = true;
    Feed(&t5, "\x1b[<0;2;2M\x1b[<0;2;2m");
    CHECK(t5.Follow);
    Feed(&t5, "\x1b[<0;2;1M\x1b[<32;2;2M\x1b[<32;2;1M\x1b[<0;2;1m");
    CHECK_ALL(!LineModel_IsEntryCollapsed(&t5.Model, noticeId), !t5.Follow);
    t5.Follow = true;
    Feed(&t5, "\x1b[<0;2;1M\x1b[<0;2;1m");
    CHECK_ALL(LineModel_IsEntryCollapsed(&t5.Model, noticeId), t5.Follow);

    Tui_Stop(&t5);
    ArenaRelease(&a5);
  }

  // Streaming updates preserve transcript selection coordinates and stationary
  // click activation while the assistant response is still growing.
  {
    Arena ac = ArenaCreate();
    TuiOpts oc;
    oc.ModelName = SVL("m");
    oc.Color = false;
    Tui tc;
    Tui_InitModel(&tc, &ac, oc);
    LineModel_AddNotice(&tc.Model, EntryKind::Info, SVL("click target"));
    u64 targetId = tc.Model.Entries.Data[0].Id;
    LineModel_BeginAssistant(&tc.Model);
    LineModel_AppendAssistant(&tc.Model, SVL("stream"));
    Tui_TestCompose(&tc, 20, 12);

    Feed(&tc, "\x1b[<0;2;1M");
    u64 pressedRevision = tc.TranscriptSelectionRevision;
    LineModel_AppendAssistant(&tc.Model, SVL(" update"));
    CHECK(LineModel_DisplayRevision(&tc.Model) == pressedRevision);
    Tui_TestCompose(&tc, 20, 12);
    CHECK_ALL(tc.DragDomain == TuiDragDomain::Transcript,
              tc.PressedEntryId == targetId);
    Feed(&tc, "\x1b[<0;2;1m");
    CHECK_ALL(LineModel_IsEntryCollapsed(&tc.Model, targetId), tc.Follow);

    Feed(&tc, "\x1b[<0;4;3M\x1b[<32;9;3M\x1b[<0;9;3m");
    CHECK(tc.TranscriptSelectionRevision != 0);
    LineModel_AppendAssistant(&tc.Model, SVL(" more"));
    Tui_TestCompose(&tc, 20, 12);
    CHECK(tc.TranscriptSelectionRevision != 0);
    Feed(&tc, "\x03");
    CHECK(StrView_Eq(StrBuf_View(&tc.ClipboardRaw), SVL("stream")));

    Tui_Stop(&tc);
    ArenaRelease(&ac);
  }

  // A stable same-row stream append preserves text selections, but invalidates
  // a selection if newly rendered text replaces cells selected as whitespace.
  {
    Arena aw = ArenaCreate();
    TuiOpts ow;
    ow.ModelName = SVL("m");
    ow.Color = false;
    Tui tw;
    Tui_InitModel(&tw, &aw, ow);
    LineModel_BeginAssistant(&tw.Model);
    LineModel_AppendAssistant(&tw.Model, SVL("stream"));
    Tui_TestCompose(&tw, 20, 12);
    Feed(&tw, "\x1b[<0;11;1M\x1b[<32;13;1M\x1b[<0;13;1m");
    u64 whitespaceRevision = tw.TranscriptSelectionRevision;
    CHECK(whitespaceRevision != 0);
    LineModel_AppendAssistant(&tw.Model, SVL(" more"));
    CHECK(LineModel_DisplayRevision(&tw.Model) == whitespaceRevision);
    Tui_TestCompose(&tw, 20, 12);
    CHECK(tw.TranscriptSelectionRevision == 0);
    Tui_Stop(&tw);
    ArenaRelease(&aw);
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
    LineModel_AddTool(&t6.Model, SVL("early"), SVL("shell"), SVL("{\"command\":\"echo\"}"));
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

  // Composer mouse placement/dragging uses the last rendered wrapped layout,
  // and keyboard motion extends or collapses the editor selection.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = true;
    Tui ts;
    Tui_InitModel(&ts, &as, os);

    Feed(&ts, "abcdef");
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;4;10M\x1b[<32;7;10M\x1b[<0;7;10m");
    CHECK_ALL(LineEdit_HasSelection(&ts.Edit),
              StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("bcd")));
    Tui_TestCompose(&ts, 20, 12);
    CHECK_ALL(Screen_BackAt(&ts.Scr, 3, ts.LastInputTop).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 5, ts.LastInputTop).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 2, ts.LastInputTop).St.Bg != 238);

    Feed(&ts, "\x1b[<0;5;10M\x1b[<0;5;10m");
    CHECK_ALL(LineEdit_Cursor(&ts.Edit) == 2, !LineEdit_HasSelection(&ts.Edit));

    LineEdit_Clear(&ts.Edit);
    Feed(&ts, "one two");
    Feed(&ts, "\x1b[1;6D");  // Shift+Ctrl+Left
    CHECK(StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("two")));
    Feed(&ts, "\x1b[D");  // unshifted Left collapses to the selection start
    CHECK_ALL(!LineEdit_HasSelection(&ts.Edit), LineEdit_Cursor(&ts.Edit) == 4);
    Feed(&ts, "\x1b[1;2C");  // Shift+Right
    CHECK(StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("t")));

    LineEdit_Clear(&ts.Edit);
    Feed(&ts, "ab\ncde");
    Feed(&ts, "\x1b[1;2A");  // Shift+Up
    CHECK(StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("\ncde")));
    Tui_TestCompose(&ts, 20, 12);
    CHECK(Screen_BackAt(&ts.Scr, 4, ts.LastInputTop).St.Bg == 238);
    Feed(&ts, "\x1b[1;2B");  // Shift+Down
    CHECK(!LineEdit_HasSelection(&ts.Edit));
    Feed(&ts, "\x1b[1;2H");  // Shift+Home
    CHECK(StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("cde")));
    Feed(&ts, "\x1b[1;2D");  // Shift+Left
    CHECK(StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("\ncde")));
    Feed(&ts, "\x1b[1;2C");
    CHECK(StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("cde")));
    Feed(&ts, "\x1b[1;2F");  // Shift+End
    CHECK(!LineEdit_HasSelection(&ts.Edit));

    LineEdit_Clear(&ts.Edit);
    Feed(&ts, "hello");
    Feed(&ts, "\x1b[1;6D");
    Feed(&ts, "z");
    CHECK(StrView_Eq(LineEdit_Text(&ts.Edit), SVL("z")));
    Feed(&ts, "\x1b[1;2D");
    Feed(&ts, "\x1b[200~paste\ntext\x1b[201~");
    CHECK(StrView_Eq(LineEdit_Text(&ts.Edit), SVL("paste\ntext")));

    // Copying a composer selection wins over the interrupt path and preserves
    // both the selection and editor state.
    Feed(&ts, "\x1b[1;6D");
    ts.AgentRunning = true;
    ts.AbortFlag = 0;
    Feed(&ts, "\x03");
    CHECK_ALL(ts.AbortFlag == 0,
              StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("text")),
              StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("text")),
              StrView_Eq(LineEdit_Text(&ts.Edit), SVL("paste\ntext")));

    // Moving the cursor on mouse-down must not change the viewport origin used
    // to map later drag rows in a scrolled composer.
    LineEdit_Clear(&ts.Edit);
    Feed(&ts, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9");
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;3;5M");
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<32;4;10M\x1b[<0;4;10m");
    CHECK(StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("4\n5\n6\n7\n8\n9")));

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Composer highlighting uses the same minimum-one-cell scalar widths as
  // layout and rendering, including combining marks.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    Feed(&ts, "a\xcc\x81" "b");
    Feed(&ts, "\x1b[1;2D");
    Tui_TestCompose(&ts, 20, 12);
    CHECK((Screen_BackAt(&ts.Scr, 4, ts.LastInputTop).St.Attr & ATTR_UNDERLINE) != 0);
    CHECK((Screen_BackAt(&ts.Scr, 3, ts.LastInputTop).St.Attr & ATTR_UNDERLINE) == 0);

    LineEdit_Clear(&ts.Edit);
    LineModel_BeginAssistant(&ts.Model);
    LineModel_AppendAssistant(&ts.Model, SVL("[link](https://example.com)"));
    LineModel_EndAssistant(&ts.Model);
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;4;1M\x1b[<32;7;1M\x1b[<0;7;1m");
    Tui_TestCompose(&ts, 20, 12);
    Style selectedLink = Screen_BackAt(&ts.Scr, 3, 0).St;
    CHECK((selectedLink.Attr & ATTR_UNDERLINE) != 0);
    CHECK((selectedLink.Attr & ATTR_REVERSE) != 0);
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Transcript selections serialize the visual rows (including indentation),
  // work in either direction, and highlight every selected row range.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = true;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("alpha\nbravo\ncharlie"));
    Tui_TestCompose(&ts, 20, 12);

    Feed(&ts, "\x1b[<0;4;1M\x1b[<32;6;3M\x1b[<0;6;3m");
    CHECK_ALL(ts.TranscriptSelectionRevision != 0, !ts.Follow);
    Tui_TestCompose(&ts, 20, 12);
    CHECK_ALL(Screen_BackAt(&ts.Scr, 3, 0).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 0, 1).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 4, 2).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 5, 2).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 6, 2).St.Bg != 238);

    ts.AgentRunning = true;
    ts.AbortFlag = 0;
    Feed(&ts, "\x03");
    CHECK_ALL(ts.AbortFlag == 0,
              StrView_Eq(StrBuf_View(&ts.ClipboardRaw),
                         SVL("alpha\n   bravo\n   cha")),
              ts.TranscriptSelectionRevision != 0);

    Feed(&ts, "\x1b[<0;6;3M\x1b[<32;4;1M\x1b[<0;4;1m");
    Feed(&ts, "\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("alpha\n   bravo\n   cha")));

    // Moving away and back still cancels activation, but leaves no non-empty
    // range, so Ctrl+C falls back to interrupt.
    Feed(&ts, "\x1b[<0;4;1M\x1b[<32;5;1M\x1b[<32;4;1M\x1b[<0;4;1m");
    CHECK(ts.TranscriptSelectionRevision == 0);
    ts.AbortFlag = 0;
    Feed(&ts, "\x03");
    CHECK(ts.AbortFlag == 1);

    // Typing moves focus to the composer and clears transcript selection.
    ts.AbortFlag = 0;
    Feed(&ts, "x");
    CHECK_ALL(ts.TranscriptSelectionRevision == 0,
              StrView_Eq(LineEdit_Text(&ts.Edit), SVL("x")));
    Feed(&ts, "\x1b[1;2D");
    CHECK(LineEdit_HasSelection(&ts.Edit));
    Feed(&ts, "\x1b[<0;4;1M\x1b[<32;5;2M\x1b[<0;5;2m");
    CHECK_ALL(ts.TranscriptSelectionRevision != 0,
              !LineEdit_HasSelection(&ts.Edit));
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;3;10M\x1b[<32;4;10M\x1b[<0;4;10m");
    CHECK_ALL(LineEdit_HasSelection(&ts.Edit),
              ts.TranscriptSelectionRevision == 0);

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // A transcript drag can begin in blank pane space below the rendered rows.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("alpha\nbravo"));
    Tui_TestCompose(&ts, 20, 12);
    CHECK(ts.LastPaneH > LineModel_Count(&ts.Model));

    Feed(&ts, "\x1b[<0;20;5M\x1b[<32;4;1M\x1b[<0;4;1m\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("alpha\n   bravo")));

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // The pointed transcript cell is included at both drag endpoints, including
  // the terminal's rightmost cell in forward and reverse selections.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("abcdefghijklmnopq"));
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;4;1M\x1b[<32;20;1M\x1b[<0;20;1m\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("abcdefghijklmnopq")));
    Feed(&ts, "\x1b[<0;20;1M\x1b[<32;4;1M\x1b[<0;4;1m\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("abcdefghijklmnopq")));
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Cell ranges snap outwards around width-two glyphs.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("wide \xe5\xa5\xbdx"));
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;10;1M\x1b[<32;9;1M\x1b[<0;9;1m\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("\xe5\xa5\xbd")));
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Full visual-row copy includes right metadata and excludes sticky/status
  // chrome because only LineModel display rows are serialized.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("model-chrome");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddUser(&ts.Model, SVL("copy me\nnext"), 1000);
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;1;1M\x1b[<32;2;2M\x1b[<0;2;2m\x03");
    StrView copied = StrBuf_View(&ts.ClipboardRaw);
    char timestamp[6];
    memcpy(timestamp, ts.Model.Entries.Data[0].Text.Timestamp.Data, 5);
    timestamp[5] = '\0';
    CHECK_ALL(StrView_Contains(copied, "copy me"), StrView_Contains(copied, timestamp),
              !StrView_Contains(copied, "ready"),
              !StrView_Contains(copied, "model-chrome"));
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Hidden collapsed content is copied from the same visual-layout iterator as
  // rendering, including the ellipsis run.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddTool(&ts.Model, SVL("copy-tool"), SVL("shell"),
                      SVL("{\"command\":\"echo hi\"}"));
    CHECK(LineModel_FinishTool(&ts.Model, SVL("copy-tool"), SVL("result"),
                               SVL("hidden preview"), false));
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("after"));
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;1;1M\x1b[<32;2;2M\x1b[<0;2;2m\x03");
    CHECK(StrView_Contains(StrBuf_View(&ts.ClipboardRaw), "\xe2\x80\xa6"));
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Edge dragging auto-scrolls at a controlled cadence and extends the
  // absolute transcript focus across pages while remaining detached.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    for (u32 i = 0; i < 30; i++)
      LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("scroll row"));
    Tui_TestCompose(&ts, 20, 12);
    ts.Follow = false;
    ts.Top = 10;
    Tui_TestCompose(&ts, 20, 12);

    Feed(&ts, "\x1b[<0;4;4M\x1b[<32;5;7M");
    CHECK(ts.TranscriptSelectionRevision != 0);
    u32 top = ts.Top;
    u32 focus = ts.TranscriptFocusRow;
    Tui_TestCompose(&ts, 20, 12, 1000);
    CHECK(ts.Top == top + 1);
    Tui_TestCompose(&ts, 20, 12, 1020);
    CHECK(ts.Top == top + 1);
    Tui_TestCompose(&ts, 20, 12, 1050);
    CHECK_ALL(ts.Top == top + 2, ts.TranscriptFocusRow > focus, !ts.Follow);
    Feed(&ts, "\x1b[<32;5;1M");
    Tui_TestCompose(&ts, 20, 12, 1060);
    CHECK(ts.Top == top + 1);
    Feed(&ts, "\x1b[<0;5;1m");

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Display structure changes and terminal resizes invalidate absolute-row
  // transcript selections before they can be rendered or copied.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("one\ntwo"));
    Tui_TestCompose(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;4;1M\x1b[<32;5;2M\x1b[<0;5;2m");
    CHECK(ts.TranscriptSelectionRevision != 0);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("changed"));
    Tui_TestCompose(&ts, 20, 12);
    CHECK(ts.TranscriptSelectionRevision == 0);

    Feed(&ts, "\x1b[<0;4;1M\x1b[<32;5;2M\x1b[<0;5;2m");
    CHECK(ts.TranscriptSelectionRevision != 0);
    Tui_TestCompose(&ts, 20, 13);
    CHECK(ts.TranscriptSelectionRevision == 0);

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // With no selection, Ctrl+C keeps the exact interrupt/clear/quit fallback.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    Feed(&ts, "keep");
    ts.AgentRunning = true;
    Feed(&ts, "\x03");
    CHECK_ALL(ts.AbortFlag == 1,
              StrView_Eq(LineEdit_Text(&ts.Edit), SVL("keep")), !ts.Quit);

    ts.AgentRunning = false;
    ts.AbortFlag = 0;
    Feed(&ts, "\x03");
    CHECK_ALL(LineEdit_Empty(&ts.Edit), !ts.Quit);
    Feed(&ts, "\x03");
    CHECK_ALL(ts.Quit, ts.Stop);

    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Clipboard output queued immediately before shutdown is emitted once by the
  // render shutdown path before terminal restoration.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    Feed(&ts, "copy");
    Feed(&ts, "\x1b[1;6D");
    Feed(&ts, "\x03\x04");
    CHECK_ALL(ts.Stop, StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("copy")));

    struct ClipboardCapture {
      StrBuf Bytes;
      u32 Writes;
    };
    ClipboardCapture capture = {};
    capture.Bytes = StrBuf_Make(&as, 64);
    auto captureWrite = [](void* user, const char* data, u32 n) {
      ClipboardCapture* capture = (ClipboardCapture*)user;
      StrBuf_Append(&capture->Bytes, StrView{data, n});
      capture->Writes++;
    };
    CHECK(Tui_TestDrainShutdown(&ts, captureWrite, &capture));
    CHECK_ALL(capture.Writes == 1,
              StrView_Eq(StrBuf_View(&capture.Bytes),
                         SVL("\x1b]52;c;Y29weQ==\x07")),
              ts.ClipboardRaw.Length == 0);
    CHECK(Tui_TestDrainShutdown(&ts, captureWrite, &capture));
    CHECK(capture.Writes == 1);

    Tui_Stop(&ts);
    ArenaRelease(&as);
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
    TuiModalResult result = Tui_GetModalResult(&tm, &selected, nullptr);
    CHECK_ALL(result == TuiModalResult::Confirmed, selected == 2,
              LineEdit_Empty(&tm.Edit));

    CHECK(Tui_BeginSelection(&tm, SVL("Cancel"), options, 3, 1));
    Feed(&tm, "\x1b");
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Cancelled);

    CHECK(Tui_BeginSelection(&tm, SVL("Back"), options, 3, 1));
    Feed(&tm, "\x7f");
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Back);

    CHECK(Tui_BeginTextEntry(&tm, SVL("Model"), SVL("Enter model ID")));
    Feed(&tm, "claude-manual\r");
    StrView entered = {};
    result = Tui_GetModalResult(&tm, nullptr, &entered);
    CHECK_ALL(result == TuiModalResult::Confirmed,
              StrView_Eq(entered, SVL("claude-manual")));

    CHECK(Tui_BeginTextEntry(&tm, SVL("Model"), SVL("Enter model ID"),
                             SVL("x")));
    Feed(&tm, "\x7f");
    CHECK_ALL(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Pending,
              LineEdit_Empty(&tm.ModalEdit));
    Feed(&tm, "\x7f");
    CHECK(Tui_GetModalResult(&tm, nullptr, nullptr) == TuiModalResult::Back);

    Tui_OpenCommandPalette(&tm);
    CHECK_ALL(StrView_Eq(LineEdit_Text(&tm.Edit), SVL("/")),
              tm.ModalKind == TuiModalKind::None,
              tm.ModalResult == TuiModalResult::None);

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
    CHECK_ALL(blocking.Confirmed, blocking.Selected == 1);

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
    CHECK_ALL(AnyRow(&ac, &tc, "model discovery unavailable"),
              AnyRow(&ac, &tc, "settings write failed"));

    Tui_SetModelName(&tc, SVL("after"));
    CHECK(StrView_Eq(tc.ModelName, SVL("after")));

    Tui_BeginOperation(&tc, SVL("login"));
    Tui_SetOperationStatus(&tc, SVL("waiting for approval"));
    CHECK_ALL(tc.OperationActive, StrView_Eq(tc.OperationLabel, SVL("login")),
              StrView_Eq(tc.OperationStatus, SVL("waiting for approval")),
              *Tui_OperationAbort(&tc) == 0);
    Feed(&tc, "\x03");
    CHECK_ALL(*Tui_OperationAbort(&tc) == 1, !tc.Quit, !tc.Stop);
    CHECK(Tui_EndOperation(&tc));
    CHECK(!tc.OperationActive);
    Tui_BeginOperation(&tc, SVL("models"));
    CHECK(*Tui_OperationAbort(&tc) == 0);
    StrView stopOptions[] = {SVL("one"), SVL("two")};
    CHECK(Tui_BeginSelection(&tc, SVL("Stop"), stopOptions, 2));
    Feed(&tc, "\x04");
    CHECK_ALL(*Tui_OperationAbort(&tc) == 1, tc.Quit, tc.Stop,
              Tui_GetModalResult(&tc, nullptr, nullptr) == TuiModalResult::Cancelled);
    CHECK(Tui_EndOperation(&tc));
    Tui_BeginOperation(&tc, SVL("must not restart"));
    bool selectionStarted = Tui_BeginSelection(&tc, SVL("Stopped"), stopOptions, 2);
    CHECK_ALL(!tc.OperationActive, *Tui_OperationAbort(&tc) == 1,
              !selectionStarted);

    Tui_Stop(&tc);
    ArenaRelease(&ac);
  }

  // Session clear reclaims the resettable arena and returns all conversation/UI
  // state to idle while preserving terminal context, harness binding, and model.
  {
    Arena ar = ArenaCreate();
    TuiOpts orr;
    orr.ModelName = SVL("kept-model");
    orr.Color = false;
    Tui resetTui;
    Tui_InitModel(&resetTui, &ar, orr);
    Faux faux = {};
    MemoryRepository memory = {};
    AgentHarness harness = {};
    CHECK(InitTestHarness(&ar, &memory, &harness, &faux));
    resetTui.Harness = &harness;
    Tui_SetStartupContext(&resetTui, SVL("kept-cwd"), SVL("kept-branch"));

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
    bool clearedWhileRunning = Tui_ClearSession(&resetTui);
    CHECK_ALL(!clearedWhileRunning, resetTui.Model.Entries.Count != 0);
    resetTui.AgentRunning = false;

    CHECK(Tui_ClearSession(&resetTui));
    CHECK(resetTui.QCount == 1);
    CHECK(SessionUiCleared(&resetTui, usedBeforeClear));
    CHECK(SessionContextPreserved(&resetTui, &harness));

    TuiSubmission preserved = {};
    CHECK(Tui_WaitForSubmit(&resetTui, &preserved));
    CHECK_ALL(preserved.Kind == TuiSubmissionKind::Command,
              StrView_Eq(preserved.Text, SVL("/queued")));

    LineEdit_InsertText(&resetTui.Edit, SVL("new prompt"));
    CHECK(Tui_BeginSelection(&resetTui, SVL("new modal"), options, 2));
    Feed(&resetTui, "\r");
    CHECK(Tui_GetModalResult(&resetTui, nullptr, nullptr) == TuiModalResult::Confirmed);
    Tui_EndOperation(&resetTui);
    Tui_Stop(&resetTui);
    CHECK_ALL(!resetTui.SessionUp, !resetTui.SyncUp);
    Tui_Stop(&resetTui);  // final cleanup is idempotent
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    MemoryRepository_Destroy(&memory);
    ArenaRelease(&ar);
  }

  // Startup context is copied into controller-owned storage, and input wakes
  // are distinguished so the renderer may bypass its stream-frame throttle.
  {
    Arena as = ArenaCreate();
    Tui startup;
    InitHeadlessTui(&startup, &as, false);
    char cwd[] = "startup-cwd";
    char branch[] = "startup-branch";
    Tui_SetStartupContext(&startup, SV(cwd), SV(branch));
    cwd[0] = 'x';
    branch[0] = 'x';
    CHECK_ALL(StrView_Eq(startup.Cwd, SVL("startup-cwd")),
              StrView_Eq(startup.Branch, SVL("startup-branch")),
              !startup.InputDirty);
    Feed(&startup, "a");
    CHECK_ALL(startup.InputDirty,
              StrView_Eq(LineEdit_Text(&startup.Edit), SVL("a")));
    Tui_Stop(&startup);
    ArenaRelease(&as);
  }

  CHECK(g_displayDrainsOk);
  Tui_Stop(&t);
  ArenaRelease(&a);
}
