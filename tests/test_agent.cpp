#include "agent/agent.h"
#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "llm/faux.h"

namespace {

struct Recorder {
  Array<u8> Kinds;
  StrBuf Text;
};

void RecSink(void* user, const AgentEvent& ev) {
  Recorder* r = (Recorder*)user;
  Array_Push(&r->Kinds, (u8)ev.Kind);
  if (ev.Kind == AgentEventKind::MessageUpdate && ev.Stream.Kind == StreamEventKind::TextDelta) {
    StrBuf_Append(&r->Text, ev.Stream.Delta);
  }
}

bool EchoExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort, ToolResult* out,
              StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)abort;
  (void)err;
  StrBuf b = StrBuf_Make(arena, 32);
  StrBuf_AppendCStr(&b, "echo:");
  StrBuf_Append(&b, argsJson);
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&b)));
  return true;
}

bool FailExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort, ToolResult* out,
              StrBuf* err) {
  (void)ctx;
  (void)arena;
  (void)id;
  (void)argsJson;
  (void)abort;
  (void)out;
  StrBuf_AppendCStr(err, "boom");
  return false;
}

bool HasKind(Recorder* r, AgentEventKind k) {
  for (u32 i = 0; i < r->Kinds.Count; i++)
    if (r->Kinds.Data[i] == (u8)k) return true;
  return false;
}

}  // namespace

void TestAgent() {
  // Multi-turn: assistant calls echo tool, then replies with text.
  {
    Arena a = ArenaCreate();
    Model model = {};
    model.Id = SVL("faux");
    model.Provider = SVL("faux");
    Agent ag = Agent_Make(&a, model, SV("You are a test."));

    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("echo args");
    echo.Execute = EchoExec;
    Agent_AddTool(&ag, echo);

    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("echo");
    steps[0].ToolId = SVL("call_1");
    steps[0].ToolArgsJson = SVL("{\"x\":1}");
    steps[1].Text = SVL("done!");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Recorder rec;
    rec.Kinds = Array_Make<u8>(&a, 64);
    rec.Text = StrBuf_Make(&a, 64);
    ag.Sink.User = &rec;
    ag.Sink.OnEvent = RecSink;

    Agent_Prompt(&ag, SV("hello"));

    CHECK(ag.Messages.Count == 4);
    CHECK(ag.Messages.Data[0].Kind == Role::User);
    CHECK(ag.Messages.Data[1].Kind == Role::Assistant);
    CHECK(ag.Messages.Data[2].Kind == Role::ToolResult);
    CHECK(ag.Messages.Data[3].Kind == Role::Assistant);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "echo:{\"x\":1}"));
    CHECK(StrView_EqCStr(StrBuf_View(&rec.Text), "done!"));
    CHECK(rec.Kinds.Data[0] == (u8)AgentEventKind::AgentStart);
    CHECK(rec.Kinds.Data[rec.Kinds.Count - 1] == (u8)AgentEventKind::AgentEnd);
    CHECK(HasKind(&rec, AgentEventKind::ToolStart));
    CHECK(HasKind(&rec, AgentEventKind::ToolEnd));
    ArenaRelease(&a);
  }

  // Tool failure becomes an isError tool result, and the loop continues.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Tool fail = {};
    fail.Name = SVL("fail");
    fail.Execute = FailExec;
    Agent_AddTool(&ag, fail);

    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("fail");
    steps[0].ToolId = SVL("c1");
    steps[0].ToolArgsJson = SVL("{}");
    steps[1].Text = SVL("recovered");
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Agent_Prompt(&ag, SV("go"));
    CHECK(ag.Messages.Count == 4);
    CHECK(ag.Messages.Data[2].Kind == Role::ToolResult);
    CHECK(ag.Messages.Data[2].IsError == true);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "boom"));
    ArenaRelease(&a);
  }
}
