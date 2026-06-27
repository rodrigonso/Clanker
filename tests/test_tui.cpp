#include "agent/agent.h"
#include "agent/event.h"
#include "base/arena.h"
#include "base/array.h"
#include "check.h"
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

  ev = {};
  ev.Kind = AgentEventKind::MessageStart;
  ev.Msg = &m;
  Tui_AgentEvent(&t, ev);

  ev = {};
  ev.Kind = AgentEventKind::MessageUpdate;
  ev.Stream.Kind = StreamEventKind::TextDelta;
  ev.Stream.Delta = SVL("Hello ");
  Tui_AgentEvent(&t, ev);
  ev.Stream.Delta = SVL("**world**");
  Tui_AgentEvent(&t, ev);
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
  ev.ToolName = SVL("bash");
  ev.ToolArgsJson = SVL("{\"command\":\"ls\"}");
  Tui_AgentEvent(&t, ev);
  CHECK(t.State == TuiState::Tool);

  ToolResult tr = {};
  tr.Content = Array_Make<Content>(&a, 1);
  Array_Push(&tr.Content, Content_Text(SVL("hello-from-bash")));
  ev = {};
  ev.Kind = AgentEventKind::ToolEnd;
  ev.Result = &tr;
  Tui_AgentEvent(&t, ev);

  ev = {};
  ev.Kind = AgentEventKind::AgentEnd;
  Tui_AgentEvent(&t, ev);

  // The transcript captured the assistant text (markdown stripped), the tool
  // call, and the tool result.
  CHECK(AnyRow(&a, &t, "assistant"));
  CHECK(AnyRow(&a, &t, "Hello world"));
  CHECK(AnyRow(&a, &t, "bash"));
  CHECK(AnyRow(&a, &t, "hello-from-bash"));
  CHECK(t.State == TuiState::Idle);
  CHECK(t.Use.Output == 7);

  // No queued prompt input pending.
  CHECK(t.QCount == 0);

  // Mouse-wheel scrolling. Seed enough rows to scroll, then drive wheel events
  // through the input path. Wheel-up detaches follow and moves the viewport up;
  // enough wheel-down returns to the bottom and re-enables follow.
  for (u32 i = 0; i < 40; i++)
    LineModel_AddSimple(&t.Model, EntryKind::Info, StrView{nullptr, 0}, SVL("filler"));
  t.LastPaneH = 10;
  t.Follow = true;
  Feed(&t, "\x1b[<64;1;1M");  // wheel up
  CHECK(!t.Follow);
  u32 afterUp = t.Top;
  Feed(&t, "\x1b[<64;1;1M");  // wheel up again
  CHECK(t.Top < afterUp);
  for (u32 i = 0; i < 20; i++) Feed(&t, "\x1b[<65;1;1M");  // wheel down to the bottom
  CHECK(t.Follow);

  // A non-wheel mouse event (click) is ignored and does not change scroll state.
  t.Follow = true;
  Feed(&t, "\x1b[<0;2;3M");
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

    t2.State = TuiState::Tool;  // a turn is running
    Feed(&t2, "steer me\r");
    CHECK(ag.Steer.Count == 1);
    CHECK(t2.QCount == 0);  // did not land in the idle prompt queue

    t2.State = TuiState::Idle;  // between turns
    Feed(&t2, "next prompt\r");
    CHECK(t2.QCount == 1);
    CHECK(ag.Steer.Count == 1);  // unchanged

    ArenaRelease(&a2);
  }

  ArenaRelease(&a);
}
