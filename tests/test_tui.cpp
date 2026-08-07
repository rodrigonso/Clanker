#include "agent/harness.h"
#include <string.h>
#include "base/arena.h"
#include "base/array.h"
#include "check.h"
#include "script_provider.h"
#include "session/memory_repository.h"
#include "app/tui_bridge.h"
#include "tui/tui.h"

namespace {

bool g_displayDrainsOk;

// The assistant turn's completion reaches the TUI as the durable record the
// harness appends, not as a transient event.
AgentHarnessEvent AssistantRecordEvent(SessionRecord* record, const Message* message) {
  *record = SessionRecord{};
  record->Kind = SessionRecordKind::Message;
  record->MessageEntry.Value = *message;
  AgentHarnessEvent ev = {};
  ev.Kind = AgentHarnessEventKind::DurableRecord;
  ev.Record = record;
  return ev;
}

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

const UiNode* ComposerNode(const Tui* t) {
  for (u32 i = 0; i < t->Ui.Nodes.Count; i++) {
    const UiNode* node = Ui_GetNode(&t->Ui, i);
    if (node && node->Persistent == &t->InputUi) return node;
  }
  return nullptr;
}

u16 ComposerTop(const Tui* t) {
  const UiNode* node = ComposerNode(t);
  return node ? (u16)node->Rect.Y : 0;
}

u64 FrameHash(const Tui* t) {
  u64 hash = 1469598103934665603ull;
  auto add = [&hash](u64 value) {
    for (u32 i = 0; i < 8; i++) {
      hash ^= (u8)(value >> (i * 8));
      hash *= 1099511628211ull;
    }
  };

  add(t->Scr.Cols);
  add(t->Scr.Rows);
  for (u16 y = 0; y < t->Scr.Rows; y++) {
    for (u16 x = 0; x < t->Scr.Cols; x++) {
      Cell cell = Screen_BackAt(&t->Scr, x, y);
      add(cell.Ch);
      add(cell.St.Fg);
      add(cell.St.Bg);
      add(cell.St.Attr);
    }
  }
  add(t->Scr.CursorVisible);
  add(t->Scr.CursorX);
  add(t->Scr.CursorY);
  return hash;
}

bool ScreenRowContains(const Tui* t, u16 row, const char* needle) {
  u32 length = (u32)strlen(needle);
  if (!length || row >= t->Scr.Rows || length > t->Scr.Cols) return false;
  for (u16 x = 0; x + length <= t->Scr.Cols; x++) {
    bool match = true;
    for (u32 i = 0; i < length; i++) {
      if (Screen_BackAt(&t->Scr, (u16)(x + i), row).Ch !=
          (u8)needle[i]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
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
         t->State == TuiState::Idle && t->Sess.StateTool.Length == 0 &&
         t->QueuedCount == 0 && t->Follow && t->Top == 0 && t->LastTop == 0 &&
         t->LastPaneH == 0 && t->HoveredEntryId == 0 && t->PressedEntryId == 0 &&
         !t->HasMouse && !t->InputUi.Dragging && t->Ui.Captured == 0 &&
         t->Ui.Active == 0 && !t->OperationActive &&
         t->ModalKind == TuiModalKind::None &&
         t->ModalResult == TuiModalResult::None;
}

bool SessionContextPreserved(const Tui* t, const AgentHarness* harness) {
  return StrView_Eq(t->ModelName, SVL("kept-model")) &&
         t->Profile.Effort == ModelEffort::Medium &&
         t->Profile.ContextWindow == 1000000 &&
         StrView_Eq(t->Cwd, SVL("kept-cwd")) &&
         StrView_Eq(t->Branch, SVL("kept-branch")) &&
         t->Agent.User == harness;
}

bool InitTestHarness(Arena* arena, MemoryRepository* memory,
                     AgentHarness* harness, ScriptProvider* script) {
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
  config.ModelInfo.Provider = SVL("script");
  config.ModelInfo.Id = SVL("script-model");
  config.ProviderImpl = ScriptProvider_Make(script);
  config.ToolExec = ToolExecution::Parallel;
  return SessionResult_IsOk(AgentHarness_Init(harness, log, &config));
}

}  // namespace

// SessionMem is a bump allocator, so an entry dropped by the scrollback cap is
// only reclaimed by copying the survivors to a fresh arena. Without compaction
// the arena grew ~6 KB per turn forever, long after the visible transcript had
// stopped growing.
void TestSessionMemoryCompaction(Arena* outer) {
  TuiOpts opts;
  opts.ModelName = SVL("mem");
  opts.Color = false;
  Tui t;
  Tui_InitModel(&t, outer, opts);

  static char body[2048];
  for (u32 i = 0; i < sizeof(body); i++) body[i] = 'a' + (char)(i % 26);
  StrView text = StrView{body, (u32)sizeof(body)};

  Message assistant = {};
  assistant.Kind = Role::Assistant;
  SessionRecord record = {};

  auto runTurn = [&](u32 turn) {
    Tui_AcceptPrompt(&t, SVL("prompt"));
    AgentHarnessEvent delta = {};
    delta.Kind = AgentHarnessEventKind::TransientStream;
    delta.Stream.Kind = StreamEventKind::TextDelta;
    delta.Stream.Delta = text;
    TuiBridge_HarnessEvent(&t, &delta);
    AgentHarnessEvent done = AssistantRecordEvent(&record, &assistant);
    TuiBridge_HarnessEvent(&t, &done);
    AgentHarnessEvent end = {};
    end.Kind = AgentHarnessEventKind::TransientAgentEnd;
    TuiBridge_HarnessEvent(&t, &end);
    DrainDisplay(&t, 1000 + turn * 32ull);
  };

  // Long enough for the scrollback cap to start dropping entries.
  for (u32 turn = 0; turn < 200; turn++) runTurn(turn);
  u32 settledEntries = t.Model.Entries.Count;
  usize before = t.SessionMem.Used;

  // Type something and build recall history: compaction must not eat either.
  Tui_FeedInput(&t, "draft prompt", 12);
  StrView typed = LineEdit_Text(&t.Edit);
  CHECK(StrView_EqCStr(typed, "draft prompt"));

  for (u32 turn = 200; turn < 320; turn++) runTurn(turn);

  // The visible transcript has plateaued, so any further growth is dead weight.
  CHECK(t.Model.Entries.Count == settledEntries);
  usize after = t.SessionMem.Used;
  CHECK(after > before);

  // Compact against the transcript the model is already showing.
  Arena scratch = ArenaCreate();
  Array<Message> transcript = Array_Make<Message>(&scratch, 8);
  for (u32 i = 0; i < 40; i++) {
    Message user = Message_User(&scratch, SVL("prompt"));
    Array_Push(&transcript, user);
    Message reply = Message_AssistantEmpty(&scratch);
    Array_Push(&reply.Content, Content_Text(text));
    Array_Push(&transcript, reply);
  }
  CHECK(Tui_CompactSessionMemory(&t, transcript.Data, transcript.Count));

  // The dead weight is gone and the live set is intact.
  CHECK(t.SessionMem.Used < after / 2);
  CHECK(t.Model.Entries.Count == transcript.Count);
  CHECK(StrView_EqCStr(LineEdit_Text(&t.Edit), "draft prompt"));

  // Compaction released the arena those strings pointed into, so every one of
  // them must be gone. Checking the struct wholesale is what makes this survive
  // someone adding a tenth field.
  const TuiSessionStrings zero = {};
  CHECK(memcmp(&t.Sess, &zero, sizeof(zero)) == 0);

  // Still usable afterwards: the swap rebuilt every pointer into the arena.
  runTurn(320);
  CHECK(t.Model.Entries.Count > transcript.Count);

  // An open modal is displaying SessionMem strings, so the arena must not move.
  StrView options[2] = {SVL("one"), SVL("two")};
  CHECK(Tui_BeginSelection(&t, SVL("pick"), options, 2));
  CHECK(!Tui_SessionMemoryWantsCompaction(&t));
  CHECK(!Tui_CompactSessionMemory(&t, transcript.Data, transcript.Count));

  // ...but once it completes, compaction has to work again. ModalKind used to
  // stay set forever, so confirming a single picker silently disabled
  // reclamation for the rest of the process.
  Feed(&t, "\r");
  u32 picked = 0;
  CHECK(Tui_GetModalResult(&t, &picked, nullptr) == TuiModalResult::Confirmed);
  CHECK(t.ModalKind == TuiModalKind::None);
  for (u32 turn = 321; turn < 400; turn++) runTurn(turn);
  CHECK(Tui_CompactSessionMemory(&t, transcript.Data, transcript.Count));

  // Lag recovery runs on the event pump thread while the main thread is holding
  // StrViews into SessionMem, so it rebuilds without reclaiming.
  for (u32 turn = 400; turn < 480; turn++) runTurn(turn);
  usize beforeReplace = t.SessionMem.Used;
  Tui_ReplaceTranscript(&t, transcript.Data, transcript.Count);
  CHECK(t.SessionMem.Used >= beforeReplace);
  CHECK(t.Model.Entries.Count == transcript.Count);

  ArenaRelease(&scratch);
  Tui_Stop(&t);
}

void TestTui() {
  g_displayDrainsOk = true;
  Arena a = ArenaCreate();
  TestSessionMemoryCompaction(&a);
  TuiOpts o;
  o.ModelName = SVL("test-model");
  o.Color = true;

  Tui t;
  Tui_InitModel(&t, &a, o);

  // Frame hashes cover every cell glyph/style plus cursor state. These fixtures
  // are the visual parity contract for the component-system migration.
  {
    Arena af = ArenaCreate();
    TuiCommandSpec commands[] = {
        {SVL("help"), SVL("h"), SVL("Show help")},
        {SVL("model"), SVL("m"), SVL("Choose model")},
    };
    TuiOpts of;
    of.ModelName = SVL("fixture-model");
    of.Color = true;
    of.Commands = commands;
    of.CommandCount = 2;
    Tui tf;
    Tui_InitModel(&tf, &af, of);
    Tui_SetStartupContext(&tf, SVL("C:\\repo"), SVL("main"));
    LineModel_AddNotice(&tf.Model, EntryKind::Info, SVL("fixture transcript"));
    Feed(&tf, "hello");
    Tui_ComposeFrame(&tf, 20, 12);
    CHECK(FrameHash(&tf) == 16918476410488148328ull);

    LineEdit_Clear(&tf.Edit);
    Feed(&tf, "/");
    Tui_ComposeFrame(&tf, 20, 12);
    CHECK(FrameHash(&tf) == 8675490699267501349ull);

    StrView options[] = {SVL("first"), SVL("second"), SVL("third")};
    CHECK(Tui_BeginSelection(&tf, SVL("Choose"), options, 3, 1));
    Tui_ComposeFrame(&tf, 20, 12);
    CHECK(FrameHash(&tf) == 10760913336260656319ull);

    Tui_ComposeFrame(&tf, 28, 15);
    CHECK(FrameHash(&tf) == 3776490409679530299ull);
    Tui_Stop(&tf);
    ArenaRelease(&af);
  }

  {
    Arena af = ArenaCreate();
    Tui tf;
    InitHeadlessTui(&tf, &af, false);
    LineModel_AddNotice(&tf.Model, EntryKind::Info, SVL("fixture transcript"));
    Feed(&tf, "hello");
    Tui_ComposeFrame(&tf, 20, 12);
    CHECK(FrameHash(&tf) == 15960401383964303925ull);
    Tui_Stop(&tf);
    ArenaRelease(&af);
  }

  // Spinner animation follows elapsed time rather than the number of renders,
  // so streaming and input-driven frames cannot accelerate it.
  {
    Arena spinnerArena = ArenaCreate();
    Tui spinnerTui;
    InitHeadlessTui(&spinnerTui, &spinnerArena, false);
    spinnerTui.State = TuiState::Thinking;
    Tui_ComposeFrame(&spinnerTui, 20, 12, 1000);
    CHECK(spinnerTui.Spinner == 0);
    Tui_ComposeFrame(&spinnerTui, 20, 12, 1016);
    Tui_ComposeFrame(&spinnerTui, 20, 12, 1059);
    CHECK(spinnerTui.Spinner == 0);
    Tui_ComposeFrame(&spinnerTui, 20, 12, 1060);
    CHECK(spinnerTui.Spinner == 1);
    for (u32 i = 0; i < 20; i++)
      Tui_ComposeFrame(&spinnerTui, 20, 12, 1120);
    CHECK(spinnerTui.Spinner == 2);
    spinnerTui.State = TuiState::Idle;
    Tui_ComposeFrame(&spinnerTui, 20, 12, 1180);
    CHECK_ALL(spinnerTui.Spinner == 0, spinnerTui.SpinnerStartMs == 0);
    Tui_Stop(&spinnerTui);
    ArenaRelease(&spinnerArena);
  }

  Message m = {};
  m.Kind = Role::Assistant;

  AgentHarnessEvent ev = {};
  ev.Kind = AgentHarnessEventKind::TransientAgentStart;
  TuiBridge_HarnessEvent(&t, &ev);
  CHECK(t.State == TuiState::Thinking);

  ev = {};
  ev.Kind = AgentHarnessEventKind::TransientRetryStart;
  ev.Retry.Layer = RetryLayer::AssistantTurn;
  ev.Retry.Attempt = 1;
  ev.Retry.MaxAttempts = 3;
  ev.Retry.DelayMs = 250;
  TuiBridge_HarnessEvent(&t, &ev);
  CHECK_ALL(t.State == TuiState::Retry,
            StrView_Contains(t.Sess.RetryStatus, "assistant turn"),
            StrView_Contains(t.Sess.RetryStatus, "250 ms"));
  ev.Kind = AgentHarnessEventKind::TransientRetryEnd;
  TuiBridge_HarnessEvent(&t, &ev);
  CHECK_ALL(t.State == TuiState::Thinking, !t.Sess.RetryStatus.Length);

  // The assistant entry is begun lazily on the first text delta, so a turn that
  // streams no text (tool-only) renders no assistant header at all.
  ev = {};
  ev.Kind = AgentHarnessEventKind::TransientStream;
  ev.Stream.Kind = StreamEventKind::TextDelta;
  ev.Stream.Delta = SVL("Hello ");
  TuiBridge_HarnessEvent(&t, &ev);
  CHECK(Tui_AdvanceStream(&t, 1000));
  CHECK_ALL(t.Model.Entries.Count == 1,
            t.Model.Entries.Data[0].Text.Content.Length ==
                sizeof("Hello ") - 1);
  ev.Stream.Delta = SVL("**world**");
  TuiBridge_HarnessEvent(&t, &ev);
  CHECK(Tui_AdvanceStream(&t, 1016));
  CHECK_ALL(t.Model.Entries.Data[0].Text.Content.Length ==
                sizeof("Hello **world**") - 1,
            t.State == TuiState::Streaming);

  m.Stop = StopReason::ToolUse;
  m.Use.Input = 12;
  m.Use.Output = 7;
  SessionRecord assistantRecord = {};
  ev = AssistantRecordEvent(&assistantRecord, &m);
  TuiBridge_HarnessEvent(&t, &ev);

  ev = {};
  ev.Kind = AgentHarnessEventKind::TransientToolStart;
  ev.Tool.CallId = SVL("shell-1");
  ev.Tool.Name = SVL("shell");
  ev.Tool.ArgumentsJson = SVL("{\"command\":\"ls\"}");
  TuiBridge_HarnessEvent(&t, &ev);
  CHECK(t.State == TuiState::Streaming);
  CHECK(t.Model.Entries.Count == 1);

  ToolResult tr = {};
  tr.Content = Array_Make<Content>(&a, 1);
  Array_Push(&tr.Content, Content_Text(SVL("hello-from-shell")));
  ev = {};
  ev.Kind = AgentHarnessEventKind::TransientToolEnd;
  ev.Tool.CallId = SVL("shell-1");
  ev.Tool.Result = tr;
  TuiBridge_HarnessEvent(&t, &ev);

  ev = {};
  ev.Kind = AgentHarnessEventKind::TransientAgentEnd;
  TuiBridge_HarnessEvent(&t, &ev);
  CHECK_ALL(!t.AgentRunning, t.State == TuiState::Streaming);
  CHECK(Tui_AdvanceStream(&t, 1032));
  CHECK_ALL(t.Model.Entries.Count == 2,
            t.Model.Entries.Data[1].Kind == EntryKind::Tool,
            !t.Model.Entries.Data[1].Tool.Done,
            t.State == TuiState::Tool, Tui_HasPendingDisplay(&t));
  DrainDisplay(&t, 1048);

  // The transcript captured the unlabeled assistant text and compact tool row.
  CHECK_ALL(!AnyRow(&a, &t, "assistant"), AnyRow(&a, &t, "Hello world"),
            AnyRow(&a, &t, " \xe2\x97\xbb  Shell ls"),
            !AnyRow(&a, &t, "hello-from-shell"), t.State == TuiState::Idle,
            t.Use.Output == 7);

  u64 assistantEntryId = t.Model.Entries.Data[0].Id;
  u64 toolEntryId = t.Model.Entries.Data[1].Id;
  CHECK(LineModel_IsEntryCollapsed(&t.Model, toolEntryId));

  // Hover maps screen rows through the last rendered viewport. Separator and
  // non-transcript rows are not interactive.
  Tui_ComposeFrame(&t, 80, 9);
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
  Tui_ComposeFrame(&t, 80, 10);
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
    ScriptProvider script = {};
    MemoryRepository memory = {};
    AgentHarness harness = {};
    CHECK(InitTestHarness(&a2, &memory, &harness, &script));

    TuiOpts o2;
    o2.ModelName = SVL("m");
    o2.Color = false;
    Tui t2;
    Tui_InitModel(&t2, &a2, o2);
    t2.Agent = TuiBridge_AgentHooks(&harness);

    t2.AgentRunning = true;  // a turn is running
    Feed(&t2, "steer me\r");
    CHECK_ALL(harness.Queues.Steering.Count == 1, t2.QCount == 0);
    DrainDisplay(&t2);
    CHECK_ALL(!AnyRow(&a2, &t2, "you"), AnyRow(&a2, &t2, "steer me"),
              t2.Model.Entries.Data[t2.Model.Entries.Count - 1].Text.Timestamp.Length == 5);

    u32 entriesAfterSteer = t2.Model.Entries.Count;
    Feed(&t2, "  /model  \r");
    CHECK_ALL(t2.QCount == 1, harness.Queues.Steering.Count == 1);
    DrainDisplay(&t2);
    CHECK_ALL(t2.Model.Entries.Count == entriesAfterSteer + 1,
              AnyRow(&a2, &t2, "Queued command:"), AnyRow(&a2, &t2, "/model"));

    TuiSubmission submission = {};
    CHECK(Tui_WaitForSubmit(&t2, &submission));
    CHECK_ALL(submission.Kind == TuiSubmissionKind::Command,
              StrView_Eq(submission.Text, SVL("  /model  ")));

    t2.AgentRunning = false;  // between turns
    Feed(&t2, "next prompt\r");
    CHECK_ALL(t2.QCount == 1, harness.Queues.Steering.Count == 1);
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

  // Double Escape clears an idle composer or cancels an active agent turn. The
  // presses may arrive separately or in one terminal read; expired/interrupted
  // sequences re-arm.
  {
    Arena ae = ArenaCreate();
    TuiOpts oe;
    oe.ModelName = SVL("m");
    oe.Color = false;
    Tui te;
    Tui_InitModel(&te, &ae, oe);

    Feed(&te, "draft prompt");
    Feed(&te, "\x1b");
    CHECK_ALL(StrView_Eq(LineEdit_Text(&te.Edit), SVL("draft prompt")),
              te.EscapeArmed);
    Feed(&te, "\x1b");
    CHECK_ALL(*Tui_Abort(&te) == 0, LineEdit_Empty(&te.Edit),
              !te.EscapeArmed);

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
    CHECK_ALL(!te.EscapeArmed,
              StrView_Eq(LineEdit_Text(&te.Edit), SVL("x")));

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
    SessionRecord interruptedRecord = {};
    AgentHarnessEvent event = AssistantRecordEvent(&interruptedRecord, &interrupted);
    TuiBridge_HarnessEvent(&te, &event);
    DrainDisplay(&te);
    CHECK(AnyRow(&ae, &te, "Interrupted by user."));

    Tui_Stop(&te);
    ArenaRelease(&ae);
  }

  // Shift+Enter inserts a newline instead of submitting. It only arrives via
  // the enhanced keyboard protocols requested on terminal entry, which also
  // re-encode Escape, which the cancel path depends on.
  {
    Arena an = ArenaCreate();
    Tui tn;
    InitHeadlessTui(&tn, &an, false);

    Feed(&tn, "one");
    Feed(&tn, "\x1b[13;2u");  // kitty Shift+Enter
    Feed(&tn, "two");
    Feed(&tn, "\x1b[27;2;13~");  // modifyOtherKeys Shift+Enter
    Feed(&tn, "three");
    CHECK_ALL(StrView_Eq(LineEdit_Text(&tn.Edit), SVL("one\ntwo\nthree")),
              tn.QCount == 0);

    // Ctrl+J and Alt+Enter keep working through the new encoding too.
    Feed(&tn, "\x1b[106;5u\x1b[13;3u");
    CHECK_ALL(StrView_Eq(LineEdit_Text(&tn.Edit), SVL("one\ntwo\nthree\n\n")),
              tn.QCount == 0);

    // An unmodified Enter still submits.
    Feed(&tn, "\x1b[13u");
    CHECK_ALL(tn.QCount == 1, LineEdit_Empty(&tn.Edit));
    Feed(&tn, "again\r");
    CHECK_ALL(tn.QCount == 2, LineEdit_Empty(&tn.Edit));

    // Ctrl+Alt+D must stay inert rather than aliasing onto Ctrl+D and quitting.
    Feed(&tn, "\x1b[100;7u");
    CHECK_ALL(!tn.Stop, !tn.Quit);

    // Escape arrives as CSI 27 u once the kitty protocol is active, so the
    // double-Escape composer clear must still fire.
    Feed(&tn, "draft");
    Feed(&tn, "\x1b[27u");
    CHECK_ALL(StrView_Eq(LineEdit_Text(&tn.Edit), SVL("draft")), tn.EscapeArmed);
    Feed(&tn, "\x1b[27u");
    CHECK_ALL(LineEdit_Empty(&tn.Edit), !tn.EscapeArmed);

    Tui_Stop(&tn);
    ArenaRelease(&an);
  }

  // Slash input opens a prefix-filtered command palette. Arrow keys select a
  // command, Enter submits its canonical name, and Escape dismisses the flyout
  // without modifying the composer.
  {
    Arena ap = ArenaCreate();
    TuiCommandSpec commands[] = {
        {SVL("login"), {}, SVL("Sign in")},
        {SVL("model"), {}, SVL("Change model")},
        {SVL("name"), {}, SVL("Name session")},
        {SVL("clear"), {}, SVL("Clear conversation")},
        {SVL("exit"), SVL("quit"), SVL("Exit")},
    };
    TuiOpts op;
    op.ModelName = SVL("m");
    op.Color = false;
    op.Commands = commands;
    op.CommandCount = 5;
    Tui tp;
    Tui_InitModel(&tp, &ap, op);

    Feed(&tp, "/");
    CHECK(StrView_Eq(LineEdit_Text(&tp.Edit), SVL("/")));
    CHECK(tp.CommandPaletteSelected == 0);
    Feed(&tp, "\x1b[B\x1b[B\x1b[B\r");
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
    CHECK_ALL(tp.CommandCount == 5, !tp.CommandPaletteDismissed);
    Feed(&tp, "/c\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/clear")));

    Feed(&tp, "/n\r");
    CHECK(Tui_WaitForSubmit(&tp, &paletteSubmission));
    CHECK(StrView_Eq(paletteSubmission.Text, SVL("/name")));

    Tui_Stop(&tp);
    ArenaRelease(&ap);
  }

  // A durable snapshot hydrates a completed script-provider turn.
  {
    Arena a3 = ArenaCreate();
    ScriptStep step = {};
    step.Text = SVL("Here is the explanation");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {};
    script.Steps = &step;
    script.Count = 1;
    MemoryRepository memory = {};
    AgentHarness harness = {};
    CHECK(InitTestHarness(&a3, &memory, &harness, &script));

    TuiOpts o3;
    o3.ModelName = SVL("m");
    o3.Color = false;
    Tui t3;
    Tui_InitModel(&t3, &a3, o3);
    t3.Agent = TuiBridge_AgentHooks(&harness);

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

    AgentHarnessEvent q = {};
    q.Kind = AgentHarnessEventKind::TransientAgentStart;
    TuiBridge_HarnessEvent(&tq, &q);
    q = {};
    q.Kind = AgentHarnessEventKind::TransientStream;
    q.Stream.Kind = StreamEventKind::TextDelta;
    q.Stream.Delta = SVL("first response");
    TuiBridge_HarnessEvent(&tq, &q);

    Message first = {};
    first.Kind = Role::Assistant;
    first.Stop = StopReason::ToolUse;
    SessionRecord firstRecord = {};
    q = AssistantRecordEvent(&firstRecord, &first);
    TuiBridge_HarnessEvent(&tq, &q);

    char args[] = "{\"command\":\"echo original\"}";
    q = {};
    q.Kind = AgentHarnessEventKind::TransientToolStart;
    q.Tool.CallId = SVL("queued-tool");
    q.Tool.Name = SVL("shell");
    q.Tool.ArgumentsJson = StrView{args, (u32)sizeof(args) - 1};
    TuiBridge_HarnessEvent(&tq, &q);

    char resultText[] = "original result";
    ToolResult result = {};
    result.Content = Array_Make<Content>(&aq, 1);
    Array_Push(&result.Content,
               Content_Text(StrView{resultText, (u32)sizeof(resultText) - 1}));
    q = {};
    q.Kind = AgentHarnessEventKind::TransientToolEnd;
    q.Tool.CallId = SVL("queued-tool");
    q.Tool.Result = result;
    TuiBridge_HarnessEvent(&tq, &q);

    memset(args, 'x', sizeof(args) - 1);
    memset(resultText, 'z', sizeof(resultText) - 1);

    q = {};
    q.Kind = AgentHarnessEventKind::TransientStream;
    q.Stream.Kind = StreamEventKind::TextDelta;
    q.Stream.Delta = SVL("second response");
    TuiBridge_HarnessEvent(&tq, &q);
    Message second = {};
    second.Kind = Role::Assistant;
    second.Stop = StopReason::Stop;
    SessionRecord secondRecord = {};
    q = AssistantRecordEvent(&secondRecord, &second);
    TuiBridge_HarnessEvent(&tq, &q);
    q = {};
    q.Kind = AgentHarnessEventKind::TransientAgentEnd;
    TuiBridge_HarnessEvent(&tq, &q);

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

    AgentHarnessEvent start = {};
    start.Kind = AgentHarnessEventKind::TransientToolStart;
    start.Tool.CallId = SVL("long");
    start.Tool.Name = SVL("shell");
    start.Tool.ArgumentsJson = SVL("{\"command\":\"long-output\"}");
    TuiBridge_HarnessEvent(&t4, &start);

    char output[1700];
    for (u32 i = 0; i < sizeof(output); i++) output[i] = (i == 20) ? '\x1b' : 'x';
    ToolResult longResult = {};
    longResult.Content = Array_Make<Content>(&a4, 1);
    Array_Push(&longResult.Content, Content_Text(StrView{output, (u32)sizeof(output)}));

    AgentHarnessEvent end = {};
    end.Kind = AgentHarnessEventKind::TransientToolEnd;
    end.Tool.CallId = SVL("long");
    end.Tool.Result = longResult;
    TuiBridge_HarnessEvent(&t4, &end);
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
    Tui_ComposeFrame(&t5, 20, 8);

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
    Tui_ComposeFrame(&tc, 20, 12);

    Feed(&tc, "\x1b[<0;2;1M");
    u64 pressedRevision = tc.TranscriptSelectionRevision;
    LineModel_AppendAssistant(&tc.Model, SVL(" update"));
    CHECK(LineModel_DisplayRevision(&tc.Model) == pressedRevision);
    Tui_ComposeFrame(&tc, 20, 12);
    CHECK_ALL(tc.TranscriptDragging,
              tc.PressedEntryId == targetId);
    Feed(&tc, "\x1b[<0;2;1m");
    CHECK_ALL(LineModel_IsEntryCollapsed(&tc.Model, targetId), tc.Follow);

    Feed(&tc, "\x1b[<0;5;3M\x1b[<32;10;3M\x1b[<0;10;3m");
    CHECK(tc.TranscriptSelectionRevision != 0);
    LineModel_AppendAssistant(&tc.Model, SVL(" more"));
    Tui_ComposeFrame(&tc, 20, 12);
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
    Tui_ComposeFrame(&tw, 20, 12);
    Feed(&tw, "\x1b[<0;11;1M\x1b[<32;13;1M\x1b[<0;13;1m");
    u64 whitespaceRevision = tw.TranscriptSelectionRevision;
    CHECK(whitespaceRevision != 0);
    LineModel_AppendAssistant(&tw.Model, SVL(" more"));
    CHECK(LineModel_DisplayRevision(&tw.Model) == whitespaceRevision);
    Tui_ComposeFrame(&tw, 20, 12);
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
    AgentHarnessEvent finish = {};
    finish.Kind = AgentHarnessEventKind::TransientToolEnd;
    finish.Tool.CallId = SVL("early");
    finish.Tool.Result = inserted;
    TuiBridge_HarnessEvent(&t6, &finish);
    DrainDisplay(&t6);
    CHECK(LineModel_RowEntryId(&t6.Model, t6.Top) == completionAnchorId);

    Tui_Stop(&t6);
    ArenaRelease(&a6);
  }

  // Partial tool updates replace the pending preview without completing it;
  // finalization is idempotent for live and durable completion delivery.
  {
    Arena au = ArenaCreate();
    TuiOpts ou;
    ou.ModelName = SVL("m");
    Tui tu;
    Tui_InitModel(&tu, &au, ou);

    AgentHarnessEvent start = {};
    start.Kind = AgentHarnessEventKind::TransientToolStart;
    start.Tool.CallId = SVL("streaming-shell");
    start.Tool.Name = SVL("shell");
    start.Tool.ArgumentsJson = SVL("{\"command\":\"echo\"}");
    TuiBridge_HarnessEvent(&tu, &start);

    ToolResult partial = {};
    partial.Content = Array_Make<Content>(&au, 1);
    Array_Push(&partial.Content, Content_Text(SVL("partial output")));
    AgentHarnessEvent update = {};
    update.Kind = AgentHarnessEventKind::TransientToolUpdate;
    update.Tool.CallId = SVL("streaming-shell");
    update.Tool.Result = partial;
    for (u32 i = 0; i < 100; i++) TuiBridge_HarnessEvent(&tu, &update);
    CHECK_ALL(tu.DisplayEvents.Count == 2,
              tu.ToolUpdateMem.Used <= 1501);
    DrainDisplay(&tu);
    CHECK_ALL(tu.Model.Entries.Count == 1,
              !tu.Model.Entries.Data[0].Tool.Done,
              StrView_Contains(
                  StrBuf_View(&tu.Model.Entries.Data[0].Tool.Preview),
                  "partial output"));
    for (u32 i = 0; i < 10; i++) {
      TuiBridge_HarnessEvent(&tu, &update);
      DrainDisplay(&tu);
    }
    CHECK(tu.ToolUpdateMem.Used <= 1501);

    ToolResult complete = {};
    complete.Content = Array_Make<Content>(&au, 1);
    Array_Push(&complete.Content, Content_Text(SVL("final output")));
    AgentHarnessEvent finish = {};
    finish.Kind = AgentHarnessEventKind::TransientToolEnd;
    finish.Tool.CallId = SVL("streaming-shell");
    finish.Tool.Result = complete;
    TuiBridge_HarnessEvent(&tu, &finish);
    TuiBridge_HarnessEvent(&tu, &finish);
    DrainDisplay(&tu);
    CHECK_ALL(tu.Model.Entries.Data[0].Tool.Done,
              StrView_Contains(
                  StrBuf_View(&tu.Model.Entries.Data[0].Tool.Preview),
                  "final output"));

    AgentHarnessEvent longUpdate = update;
    longUpdate.Tool.CallId = SVL("long-running-shell");
    TuiBridge_HarnessEvent(&tu, &longUpdate);
    DrainDisplay(&tu);
    for (u32 i = 0; i < 10; i++) {
      TuiBridge_HarnessEvent(&tu, &update);
      DrainDisplay(&tu);
      TuiBridge_HarnessEvent(&tu, &finish);
      DrainDisplay(&tu);
    }
    CHECK(tu.ToolUpdateMem.Used <= 3002);

    Tui_Stop(&tu);
    ArenaRelease(&au);
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
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;4;10M\x1b[<32;7;10M\x1b[<0;7;10m");
    CHECK_ALL(LineEdit_HasSelection(&ts.Edit),
              StrView_Eq(LineEdit_SelectedText(&ts.Edit), SVL("bcd")));
    Tui_ComposeFrame(&ts, 20, 12);
    u16 composerTop = ComposerTop(&ts);
    CHECK_ALL(Screen_BackAt(&ts.Scr, 3, composerTop).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 5, composerTop).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 2, composerTop).St.Bg != 238);

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
    Tui_ComposeFrame(&ts, 20, 12);
    CHECK(Screen_BackAt(&ts.Scr, 4, ComposerTop(&ts)).St.Bg == 238);
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
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;3;5M");
    Tui_ComposeFrame(&ts, 20, 12);
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
    Tui_ComposeFrame(&ts, 20, 12);
    CHECK((Screen_BackAt(&ts.Scr, 4, ComposerTop(&ts)).St.Attr &
           ATTR_UNDERLINE) != 0);
    CHECK((Screen_BackAt(&ts.Scr, 3, ComposerTop(&ts)).St.Attr &
           ATTR_UNDERLINE) == 0);

    LineEdit_Clear(&ts.Edit);
    LineModel_BeginAssistant(&ts.Model);
    LineModel_AppendAssistant(&ts.Model, SVL("[link](https://example.com)"));
    LineModel_EndAssistant(&ts.Model);
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;5;1M\x1b[<32;8;1M\x1b[<0;8;1m");
    Tui_ComposeFrame(&ts, 20, 12);
    Style selectedLink = Screen_BackAt(&ts.Scr, 4, 0).St;
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
    Tui_ComposeFrame(&ts, 20, 12);

    Feed(&ts, "\x1b[<0;5;1M\x1b[<32;7;3M\x1b[<0;7;3m");
    CHECK_ALL(ts.TranscriptSelectionRevision != 0, !ts.Follow);
    Tui_ComposeFrame(&ts, 20, 12);
    CHECK_ALL(Screen_BackAt(&ts.Scr, 4, 0).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 0, 1).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 5, 2).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 6, 2).St.Bg == 238,
              Screen_BackAt(&ts.Scr, 7, 2).St.Bg != 238);

    ts.AgentRunning = true;
    ts.AbortFlag = 0;
    Feed(&ts, "\x03");
    CHECK_ALL(ts.AbortFlag == 0,
              StrView_Eq(StrBuf_View(&ts.ClipboardRaw),
                         SVL("alpha\n    bravo\n    cha")),
              ts.TranscriptSelectionRevision != 0);

    Feed(&ts, "\x1b[<0;7;3M\x1b[<32;5;1M\x1b[<0;5;1m");
    Feed(&ts, "\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("alpha\n    bravo\n    cha")));

    // Moving away and back still cancels activation, but leaves no non-empty
    // range, so Ctrl+C falls back to interrupt.
    Feed(&ts, "\x1b[<0;5;1M\x1b[<32;6;1M\x1b[<32;5;1M\x1b[<0;5;1m");
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
    Feed(&ts, "\x1b[<0;5;1M\x1b[<32;6;2M\x1b[<0;6;2m");
    CHECK_ALL(ts.TranscriptSelectionRevision != 0,
              !LineEdit_HasSelection(&ts.Edit));
    Tui_ComposeFrame(&ts, 20, 12);
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
    Tui_ComposeFrame(&ts, 20, 12);
    CHECK(ts.LastPaneH > LineModel_Count(&ts.Model));

    Feed(&ts, "\x1b[<0;20;5M\x1b[<32;5;1M\x1b[<0;5;1m\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("alpha\n    bravo")));

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
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("abcdefghijklmnop"));
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;5;1M\x1b[<32;20;1M\x1b[<0;20;1m\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("abcdefghijklmnop")));
    Feed(&ts, "\x1b[<0;20;1M\x1b[<32;5;1M\x1b[<0;5;1m\x03");
    CHECK(StrView_Eq(StrBuf_View(&ts.ClipboardRaw), SVL("abcdefghijklmnop")));
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
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;11;1M\x1b[<32;10;1M\x1b[<0;10;1m\x03");
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
    Tui_ComposeFrame(&ts, 20, 12);
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
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;1;1M\x1b[<32;2;2M\x1b[<0;2;2m\x03");
    CHECK(StrView_Contains(StrBuf_View(&ts.ClipboardRaw), "\xe2\x80\xa6"));
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // A successful copy confirms on the status row, counting characters rather
  // than bytes, and retires on the compose clock.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = true;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    Feed(&ts, "copy");
    Feed(&ts, "\x1b[1;6D");
    Feed(&ts, "\x03");
    CHECK_ALL(ts.CopyNoticeActive, ts.CopyNoticeChars == 4,
              ts.CopyNoticeExpiryMs == 0);

    // The first frame stamps the deadline and takes over the left status text.
    Tui_ComposeFrame(&ts, 20, 12, 100000);
    u16 statusY = (u16)(ts.Scr.Rows - 1);
    CHECK_ALL(ts.CopyNoticeExpiryMs == 101500,
              ScreenRowContains(&ts, statusY, "copied 4 chars"),
              !ScreenRowContains(&ts, statusY, "ready"),
              Screen_BackAt(&ts.Scr, 1, statusY).Ch == 'c',
              Screen_BackAt(&ts.Scr, 1, statusY).St.Fg == 114);

    // It survives right up to the deadline, then restores the agent state.
    Tui_ComposeFrame(&ts, 20, 12, 101499);
    CHECK_ALL(ts.CopyNoticeActive,
              ScreenRowContains(&ts, statusY, "copied 4 chars"));
    Tui_ComposeFrame(&ts, 20, 12, 101500);
    CHECK_ALL(!ts.CopyNoticeActive, ts.CopyNoticeChars == 0,
              ts.CopyNoticeExpiryMs == 0,
              ScreenRowContains(&ts, statusY, "ready"),
              !ScreenRowContains(&ts, statusY, "copied"));
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Pointer motion keeps the confirmation, the next keystroke retires it, and a
  // Ctrl+C that copies nothing never arms one.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    Feed(&ts, "copy");
    Feed(&ts, "\x1b[1;6D");
    Feed(&ts, "\x03");
    Tui_ComposeFrame(&ts, 20, 12, 100000);
    CHECK(ts.CopyNoticeActive);

    Feed(&ts, "\x1b[<35;6;6M");
    CHECK(ts.CopyNoticeActive);

    Feed(&ts, "x");
    u16 statusY = (u16)(ts.Scr.Rows - 1);
    CHECK(!ts.CopyNoticeActive);
    Tui_ComposeFrame(&ts, 20, 12, 100010);
    CHECK_ALL(ScreenRowContains(&ts, statusY, "ready"),
              !ScreenRowContains(&ts, statusY, "copied"));

    ts.AgentRunning = true;
    ts.AbortFlag = 0;
    Feed(&ts, "\x03");
    CHECK_ALL(ts.AbortFlag == 1, !ts.CopyNoticeActive);
    Tui_Stop(&ts);
    ArenaRelease(&as);
  }

  // Multi-byte selections report characters, not bytes, and singular units.
  {
    Arena as = ArenaCreate();
    TuiOpts os;
    os.ModelName = SVL("m");
    os.Color = false;
    Tui ts;
    Tui_InitModel(&ts, &as, os);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("wide \xe5\xa5\xbdx"));
    Tui_ComposeFrame(&ts, 20, 12, 100000);
    Feed(&ts, "\x1b[<0;11;1M\x1b[<32;10;1M\x1b[<0;10;1m\x03");
    CHECK_ALL(ts.ClipboardRaw.Length == 3, ts.CopyNoticeChars == 1);
    Tui_ComposeFrame(&ts, 20, 12, 100010);
    CHECK(ScreenRowContains(&ts, (u16)(ts.Scr.Rows - 1), "copied 1 char"));
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
    Tui_ComposeFrame(&ts, 20, 12);
    ts.Follow = false;
    ts.Top = 10;
    Tui_ComposeFrame(&ts, 20, 12);

    Feed(&ts, "\x1b[<0;4;4M\x1b[<32;5;7M");
    CHECK(ts.TranscriptSelectionRevision != 0);
    u32 top = ts.Top;
    u32 focus = ts.TranscriptFocusRow;
    Tui_ComposeFrame(&ts, 20, 12, 1000);
    CHECK(ts.Top == top + 1);
    Tui_ComposeFrame(&ts, 20, 12, 1020);
    CHECK(ts.Top == top + 1);
    Tui_ComposeFrame(&ts, 20, 12, 1050);
    CHECK_ALL(ts.Top == top + 2, ts.TranscriptFocusRow > focus, !ts.Follow);
    Feed(&ts, "\x1b[<32;5;1M");
    Tui_ComposeFrame(&ts, 20, 12, 1060);
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
    Tui_ComposeFrame(&ts, 20, 12);
    Feed(&ts, "\x1b[<0;5;1M\x1b[<32;6;2M\x1b[<0;6;2m");
    CHECK(ts.TranscriptSelectionRevision != 0);
    LineModel_AddNotice(&ts.Model, EntryKind::Info, SVL("changed"));
    Tui_ComposeFrame(&ts, 20, 12);
    CHECK(ts.TranscriptSelectionRevision == 0);

    Feed(&ts, "\x1b[<0;5;1M\x1b[<32;6;2M\x1b[<0;6;2m");
    CHECK(ts.TranscriptSelectionRevision != 0);
    Tui_ComposeFrame(&ts, 20, 13);
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
    Tui_ComposeFrame(&tm, 20, 12);
    UiEvent dragStart = {};
    dragStart.Kind = UiEventKind::MouseDown;
    dragStart.X = 2;
    dragStart.Y = ComposerTop(&tm);
    CHECK(Ui_Dispatch(&tm.Ui, dragStart));
    CHECK_ALL(tm.InputUi.Dragging, tm.Ui.Captured != 0);
    CHECK(Tui_BeginSelection(&tm, SVL("Choose"), options, 3));
    CHECK_ALL(!tm.InputUi.Dragging, tm.Ui.Captured == 0);
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

    CHECK(Tui_BeginTextEntry(&tm, SVL("Session name"),
                             SVL("Name (empty clears)"),
                             SVL("Existing title")));
    Feed(&tm,
         "\x7f\x7f\x7f\x7f\x7f\x7f\x7f"
         "\x7f\x7f\x7f\x7f\x7f\x7f\x7f");
    CHECK_ALL(LineEdit_Empty(&tm.ModalEdit),
              Tui_GetModalResult(&tm, nullptr, nullptr) ==
                  TuiModalResult::Pending);
    Feed(&tm, "\r");
    entered = SVL("not-empty");
    result = Tui_GetModalResult(&tm, nullptr, &entered);
    CHECK_ALL(result == TuiModalResult::Confirmed, entered.Length == 0);

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
    oc.Profile = ModelRequestProfile{ModelEffort::Low, 200000};
    oc.Color = false;
    Tui tc;
    Tui_InitModel(&tc, &ac, oc);

    Tui_CommandNotice(&tc, SVL("model discovery unavailable; using fallback"));
    Tui_CommandError(&tc, SVL("settings write failed"));
    DrainDisplay(&tc);
    CHECK_ALL(AnyRow(&ac, &tc, "model discovery unavailable"),
              AnyRow(&ac, &tc, "settings write failed"));

    Tui_SetModelProfile(
        &tc, SVL("after"),
        ModelRequestProfile{ModelEffort::Medium, 1000000});
    CHECK_ALL(StrView_Eq(tc.ModelName, SVL("after")),
              tc.Profile.Effort == ModelEffort::Medium,
              tc.Profile.ContextWindow == 1000000);
    Tui_ComposeFrame(&tc, 80, 12);
    CHECK_ALL(ScreenRowContains(&tc, 11, "after"),
              ScreenRowContains(&tc, 11, "e:medium"),
              ScreenRowContains(&tc, 11, "ctx:1M"));
    Tui_ComposeFrame(&tc, 20, 12);
    CHECK_ALL(ScreenRowContains(&tc, 11, "after"),
              !ScreenRowContains(&tc, 11, "ctx:1M"));

    Tui_BeginOperation(&tc, SVL("login"));
    Tui_SetOperationStatus(&tc, SVL("waiting for approval"));
    CHECK_ALL(tc.OperationActive, StrView_Eq(tc.Sess.OperationLabel, SVL("login")),
              StrView_Eq(tc.Sess.OperationStatus, SVL("waiting for approval")),
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
    orr.Profile = ModelRequestProfile{ModelEffort::Medium, 1000000};
    orr.Color = false;
    Tui resetTui;
    Tui_InitModel(&resetTui, &ar, orr);
    ScriptProvider script = {};
    MemoryRepository memory = {};
    AgentHarness harness = {};
    CHECK(InitTestHarness(&ar, &memory, &harness, &script));
    resetTui.Agent = TuiBridge_AgentHooks(&harness);
    Tui_SetStartupContext(&resetTui, SVL("kept-cwd"), SVL("kept-branch"));

    LineModel_AddNotice(&resetTui.Model, EntryKind::Info, SVL("old transcript"));
    LineEdit_InsertText(&resetTui.Edit, SVL("draft"));
    CHECK(LineEdit_Submit(&resetTui.Edit).Length == 5);
    Feed(&resetTui, "/queued\r");
    CHECK(resetTui.QCount == 1);
    LineEdit_InsertText(&resetTui.Edit, SVL("second draft"));
    Tui_ComposeFrame(&resetTui, 20, 12);
    UiEvent dragStart = {};
    dragStart.Kind = UiEventKind::MouseDown;
    dragStart.X = 2;
    dragStart.Y = ComposerTop(&resetTui);
    CHECK(Ui_Dispatch(&resetTui.Ui, dragStart));
    CHECK_ALL(resetTui.InputUi.Dragging, resetTui.Ui.Captured != 0);
    Tui_CommandNotice(&resetTui, SVL("pending display"));
    StreamReveal_Push(&resetTui.Reveal, SVL("pending reveal"));
    resetTui.Use.Input = 11;
    resetTui.Use.Output = 7;
    resetTui.State = TuiState::Streaming;
    resetTui.Sess.StateTool = SVL("old tool");
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
