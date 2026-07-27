#include "agent/agent.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/thread.h"
#include "check.h"
#include "llm/faux.h"
#include "llm/retry.h"

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

struct AbortAuthContext {
  volatile i32* SeenAbort;
  u32 Calls;
};

bool AbortDuringAuth(void* user, Arena* arena, StrView provider, volatile i32* abort,
                     StrView* outToken) {
  (void)arena;
  (void)provider;
  (void)outToken;
  AbortAuthContext* ctx = (AbortAuthContext*)user;
  ctx->SeenAbort = abort;
  ctx->Calls++;
  if (abort) *abort = 1;
  return false;
}

u32 CountKind(Recorder* r, AgentEventKind k) {
  u32 c = 0;
  for (u32 i = 0; i < r->Kinds.Count; i++)
    if (r->Kinds.Data[i] == (u8)k) c++;
  return c;
}

bool MessageTextEq(const Agent* agent, u32 index, Role role, const char* text) {
  if (index >= agent->Messages.Count) return false;
  const Message* message = &agent->Messages.Data[index];
  return message->Kind == role && message->Content.Count > 0 &&
         StrView_EqCStr(message->Content.Data[0].Text, text);
}

bool ToolResultEq(const Agent* agent, u32 index, const char* id, const char* text,
                  bool isError = false) {
  if (index >= agent->Messages.Count) return false;
  const Message* message = &agent->Messages.Data[index];
  return message->Kind == Role::ToolResult && message->IsError == isError &&
         StrView_EqCStr(message->ToolCallId, id) && message->Content.Count > 0 &&
         StrView_EqCStr(message->Content.Data[0].Text, text);
}

bool BatchEventsEq(Recorder* recorder, u32 count) {
  return CountKind(recorder, AgentEventKind::ToolStart) == count &&
         CountKind(recorder, AgentEventKind::ToolEnd) == count &&
         recorder->Kinds.Count > 0 &&
         recorder->Kinds.Data[recorder->Kinds.Count - 1] == (u8)AgentEventKind::AgentEnd;
}

// Shared instrument for the parallel/sequential tests: each tool execution bumps
// a live-counter (tracking peak concurrency), sleeps so overlap is observable,
// then echoes its args as a marker. Peak >= 2 proves real concurrency; peak == 1
// proves serial execution. Allocating into the passed-in arena (a per-worker
// arena under parallel execution) also exercises the isolation/relocation path.
struct Concurrency {
  Mutex Lock;
  i32 Active;
  i32 Max;
};

bool OverlapExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort,
                 ToolResult* out, StrBuf* err) {
  (void)id;
  (void)abort;
  (void)err;
  Concurrency* c = (Concurrency*)ctx;
  Mutex_Lock(&c->Lock);
  c->Active++;
  if (c->Active > c->Max) c->Max = c->Active;
  Mutex_Unlock(&c->Lock);

  // Skew completion order: the "A" call finishes last, so a correct loop must
  // still append results in original call order.
  Sleep_Ms(StrView_EqCStr(argsJson, "A") ? 80 : 20);

  Mutex_Lock(&c->Lock);
  c->Active--;
  Mutex_Unlock(&c->Lock);

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrView_Dup(arena, argsJson)));
  return true;
}

// Drives a 3-call batch through one assistant turn, then a final text reply.
void RunBatch(Arena* a, Agent* ag, Concurrency* conc, Recorder* rec, const FauxToolCall* calls,
              u32 callCount, ToolExecMode workMode = ToolExecMode::Default) {
  Tool work = {};
  work.Name = SVL("work");
  work.Description = SVL("overlap probe");
  work.ExecMode = workMode;
  work.Ctx = conc;
  work.Execute = OverlapExec;
  Agent_AddTool(ag, work);

  FauxStep steps[2] = {};
  steps[0].Tools = calls;
  steps[0].ToolCount = callCount;
  steps[1].Text = SVL("ok");
  steps[1].Stop = StopReason::Stop;
  Faux* f = ArenaPushStruct(a, Faux);
  *f = Faux{};
  f->Steps = steps;
  f->Count = 2;
  ag->Prov = Faux_Provider(f);

  rec->Kinds = Array_Make<u8>(a, 64);
  rec->Text = StrBuf_Make(a, 64);
  ag->Sink.User = rec;
  ag->Sink.OnEvent = RecSink;

  Agent_Prompt(ag, SV("go"));
}

// afterToolCall hooks for tests: replace content, flip success->error, or force
// early termination of the batch.
void AfterRewrite(void* user, Arena* arena, StrView toolName, StrView argsJson, ToolOutcome* io) {
  (void)user;
  (void)toolName;
  (void)argsJson;
  io->Content = Array_Make<Content>(arena, 1);
  Array_Push(&io->Content, Content_Text(SVL("OVERRIDDEN")));
}

void AfterFlipError(void* user, Arena* arena, StrView toolName, StrView argsJson, ToolOutcome* io) {
  (void)user;
  (void)arena;
  (void)toolName;
  (void)argsJson;
  io->IsError = true;
}

void AfterTerminate(void* user, Arena* arena, StrView toolName, StrView argsJson, ToolOutcome* io) {
  (void)user;
  (void)arena;
  (void)toolName;
  (void)argsJson;
  io->Terminate = true;
}

// Tool that steers its own agent mid-run (ctx = Agent*).
bool PokeSteerExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort,
                   ToolResult* out, StrBuf* err) {
  (void)id;
  (void)argsJson;
  (void)abort;
  (void)err;
  Agent_Steer((Agent*)ctx, SVL("steered"));
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("poked")));
  return true;
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

    CHECK_ALL(ag.Messages.Count == 4, ag.Messages.Data[0].Kind == Role::User,
              ag.Messages.Data[1].Kind == Role::Assistant,
              ToolResultEq(&ag, 2, "call_1", "echo:{\"x\":1}"),
              ag.Messages.Data[3].Kind == Role::Assistant);
    CHECK_ALL(StrView_EqCStr(StrBuf_View(&rec.Text), "done!"),
              rec.Kinds.Data[0] == (u8)AgentEventKind::AgentStart,
              rec.Kinds.Data[rec.Kinds.Count - 1] == (u8)AgentEventKind::AgentEnd,
              HasKind(&rec, AgentEventKind::ToolStart),
              HasKind(&rec, AgentEventKind::ToolEnd));
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
    CHECK_ALL(ag.Messages.Count == 4, ToolResultEq(&ag, 2, "c1", "boom", true));
    ArenaRelease(&a);
  }

  // Parallel tool batch: 3 calls to one tool run concurrently; results are
  // appended in original order despite skewed completion times.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));  // ToolExec defaults to Parallel

    Concurrency conc = {};
    Mutex_Init(&conc.Lock);
    FauxToolCall calls[3] = {};
    calls[0].Name = SVL("work"); calls[0].Id = SVL("c0"); calls[0].ArgsJson = SVL("A");
    calls[1].Name = SVL("work"); calls[1].Id = SVL("c1"); calls[1].ArgsJson = SVL("B");
    calls[2].Name = SVL("work"); calls[2].Id = SVL("c2"); calls[2].ArgsJson = SVL("C");

    Recorder rec;
    RunBatch(&a, &ag, &conc, &rec, calls, 3);

    CHECK_ALL(ag.Messages.Count == 6, ag.Messages.Data[1].Kind == Role::Assistant,
              ToolResultEq(&ag, 2, "c0", "A"), ToolResultEq(&ag, 3, "c1", "B"),
              ToolResultEq(&ag, 4, "c2", "C"),
              ag.Messages.Data[5].Kind == Role::Assistant);
    CHECK_ALL(BatchEventsEq(&rec, 3), conc.Max >= 2);  // proves real concurrency

    Mutex_Destroy(&conc.Lock);
    ArenaRelease(&a);
  }

  // Sequential execution policy: the same batch never overlaps, order preserved.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    ag.ToolExec = ToolExecution::Sequential;

    Concurrency conc = {};
    Mutex_Init(&conc.Lock);
    FauxToolCall calls[3] = {};
    calls[0].Name = SVL("work"); calls[0].Id = SVL("c0"); calls[0].ArgsJson = SVL("A");
    calls[1].Name = SVL("work"); calls[1].Id = SVL("c1"); calls[1].ArgsJson = SVL("B");
    calls[2].Name = SVL("work"); calls[2].Id = SVL("c2"); calls[2].ArgsJson = SVL("C");

    Recorder rec;
    RunBatch(&a, &ag, &conc, &rec, calls, 3);

    CHECK_ALL(ag.Messages.Count == 6, ToolResultEq(&ag, 2, "c0", "A"),
              ToolResultEq(&ag, 3, "c1", "B"), ToolResultEq(&ag, 4, "c2", "C"),
              conc.Max == 1);  // never overlapped

    Mutex_Destroy(&conc.Lock);
    ArenaRelease(&a);
  }

  // A tool marked Sequential forces the whole batch serial under the default
  // Parallel policy (mirrors pi's executionMode === "sequential").
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));  // Parallel policy

    Concurrency conc = {};
    Mutex_Init(&conc.Lock);
    FauxToolCall calls[3] = {};
    calls[0].Name = SVL("work"); calls[0].Id = SVL("c0"); calls[0].ArgsJson = SVL("A");
    calls[1].Name = SVL("work"); calls[1].Id = SVL("c1"); calls[1].ArgsJson = SVL("B");
    calls[2].Name = SVL("work"); calls[2].Id = SVL("c2"); calls[2].ArgsJson = SVL("C");

    Recorder rec;
    RunBatch(&a, &ag, &conc, &rec, calls, 3, ToolExecMode::Sequential);

    CHECK_ALL(ag.Messages.Count == 6, ToolResultEq(&ag, 2, "c0", "A"),
              ToolResultEq(&ag, 4, "c2", "C"), conc.Max == 1);

    Mutex_Destroy(&conc.Lock);
    ArenaRelease(&a);
  }

  // Parallel batch with an unknown tool: the not-found error is finalized in its
  // original position alongside the concurrently-run tools.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));

    Concurrency conc = {};
    Mutex_Init(&conc.Lock);
    FauxToolCall calls[3] = {};
    calls[0].Name = SVL("work");    calls[0].Id = SVL("c0"); calls[0].ArgsJson = SVL("A");
    calls[1].Name = SVL("missing"); calls[1].Id = SVL("c1"); calls[1].ArgsJson = SVL("B");
    calls[2].Name = SVL("work");    calls[2].Id = SVL("c2"); calls[2].ArgsJson = SVL("C");

    Recorder rec;
    RunBatch(&a, &ag, &conc, &rec, calls, 3);

    CHECK_ALL(ag.Messages.Count == 6, ToolResultEq(&ag, 2, "c0", "A"),
              ToolResultEq(&ag, 3, "c1", "Tool not found: missing", true),
              ToolResultEq(&ag, 4, "c2", "C"), conc.Max >= 2);

    Mutex_Destroy(&conc.Lock);
    ArenaRelease(&a);
  }

  // afterToolCall: hook replaces the tool result content before it is recorded.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Execute = EchoExec;
    Agent_AddTool(&ag, echo);
    ag.AfterTool = AfterRewrite;

    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("echo");
    steps[0].ToolId = SVL("c1");
    steps[0].ToolArgsJson = SVL("{}");
    steps[1].Text = SVL("done");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Agent_Prompt(&ag, SV("go"));
    CHECK_ALL(ag.Messages.Count == 4, ToolResultEq(&ag, 2, "c1", "OVERRIDDEN"));
    ArenaRelease(&a);
  }

  // afterToolCall: hook flips a successful result into an error.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Execute = EchoExec;
    Agent_AddTool(&ag, echo);
    ag.AfterTool = AfterFlipError;

    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("echo");
    steps[0].ToolId = SVL("c1");
    steps[0].ToolArgsJson = SVL("{}");
    steps[1].Text = SVL("done");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Agent_Prompt(&ag, SV("go"));
    CHECK(ag.Messages.Data[2].IsError == true);
    ArenaRelease(&a);
  }

  // afterToolCall: hook forces early termination, so no further assistant turn.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Execute = EchoExec;
    Agent_AddTool(&ag, echo);
    ag.AfterTool = AfterTerminate;

    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("echo");
    steps[0].ToolId = SVL("c1");
    steps[0].ToolArgsJson = SVL("{}");
    steps[1].Text = SVL("should-not-run");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Agent_Prompt(&ag, SV("go"));
    // user, assistant(toolcall), toolResult — loop stops (terminate); step 1 unused.
    CHECK_ALL(ag.Messages.Count == 3, ag.Messages.Data[2].Kind == Role::ToolResult);
    ArenaRelease(&a);
  }

  // Auto-retry: a retryable provider error is retried, then succeeds.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    ag.RetryBaseDelayMs = 1;  // keep the suite fast
    ag.MaxRetryDelayMs = 4;

    FauxStep steps[2] = {};
    steps[0].Error = SVL("HTTP 429: overloaded_error");
    steps[1].Text = SVL("recovered");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Recorder rec;
    rec.Kinds = Array_Make<u8>(&a, 32);
    rec.Text = StrBuf_Make(&a, 32);
    ag.Sink.User = &rec;
    ag.Sink.OnEvent = RecSink;

    Agent_Prompt(&ag, SV("go"));
    // Only the final (successful) assistant turn is recorded.
    CHECK_ALL(ag.Messages.Count == 2, ag.Messages.Data[1].Kind == Role::Assistant,
              ag.Messages.Data[1].Stop == StopReason::Stop,
              StrView_EqCStr(StrBuf_View(&rec.Text), "recovered"),
              CountKind(&rec, AgentEventKind::RetryStart) == 1,
              CountKind(&rec, AgentEventKind::RetryEnd) == 1,
              f.Index == 2);  // two provider calls: error then success
    ArenaRelease(&a);
  }

  // Auto-retry: a non-retryable error is not retried.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    ag.RetryBaseDelayMs = 1;

    FauxStep steps[1] = {};
    steps[0].Error = SVL("HTTP 400: invalid_request_error");
    Faux f = {};
    f.Steps = steps;
    f.Count = 1;
    ag.Prov = Faux_Provider(&f);

    Recorder rec;
    rec.Kinds = Array_Make<u8>(&a, 32);
    rec.Text = StrBuf_Make(&a, 32);
    ag.Sink.User = &rec;
    ag.Sink.OnEvent = RecSink;

    Agent_Prompt(&ag, SV("go"));
    CHECK_ALL(ag.Messages.Data[1].Stop == StopReason::Error,
              CountKind(&rec, AgentEventKind::RetryStart) == 0,
              f.Index == 1);  // single provider call, no retry
    ArenaRelease(&a);
  }

  // Auto-retry: retryable errors that never recover exhaust the retry budget.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    ag.RetryBaseDelayMs = 1;
    ag.MaxRetryDelayMs = 4;
    ag.MaxRetries = 3;

    FauxStep steps[4] = {};
    for (u32 i = 0; i < 4; i++) steps[i].Error = SVL("HTTP 503 service unavailable");
    Faux f = {};
    f.Steps = steps;
    f.Count = 4;
    ag.Prov = Faux_Provider(&f);

    Recorder rec;
    rec.Kinds = Array_Make<u8>(&a, 32);
    rec.Text = StrBuf_Make(&a, 32);
    ag.Sink.User = &rec;
    ag.Sink.OnEvent = RecSink;

    Agent_Prompt(&ag, SV("go"));
    CHECK_ALL(ag.Messages.Data[1].Stop == StopReason::Error,
              CountKind(&rec, AgentEventKind::RetryStart) == 3,
              f.Index == 4);  // 3 retries, 4 calls
    ArenaRelease(&a);
  }

  // Auto-retry: a pre-set abort prevents the provider request entirely so
  // cancellation stays responsive.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    ag.RetryBaseDelayMs = 100000;  // would be a very long sleep if not aborted
    ag.MaxRetryDelayMs = 100000;
    volatile i32 abort = 1;  // already aborted
    ag.Stream.Abort = &abort;

    FauxStep steps[2] = {};
    steps[0].Error = SVL("HTTP 503 service unavailable");
    steps[1].Text = SVL("should-not-run");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Recorder rec;
    rec.Kinds = Array_Make<u8>(&a, 32);
    rec.Text = StrBuf_Make(&a, 32);
    ag.Sink.User = &rec;
    ag.Sink.OnEvent = RecSink;

    u64 t0 = NowMillis();
    Agent_Prompt(&ag, SV("go"));
    u64 elapsed = NowMillis() - t0;
    CHECK_ALL(elapsed < 5000,  // did not actually sleep 100s
              CountKind(&rec, AgentEventKind::RetryStart) == 0, f.Index == 0,
              ag.Messages.Data[1].Stop == StopReason::Aborted);
    ArenaRelease(&a);
  }

  // IsRetryableError classifier (port of pi's isRetryableAssistantError).
  {
    struct RetryableCase {
      StrView Error;
      bool Expected;
    };
    RetryableCase cases[] = {
        {SVL("HTTP 429: overloaded_error"), true},
        {SVL("overloaded"), true},
        {SVL("HTTP 503 Service Unavailable"), true},
        {SVL("network error: connection refused"), true},
        {SVL("socket hang up"), true},
        {SVL("HTTP 400: invalid_request_error"), false},
        {SVL("HTTP 401 unauthorized"), false},
        {SVL("insufficient_quota"), false},
        {SVL("429 but Monthly usage limit reached"), false},
        {StrView{nullptr, 0}, false},
    };
    bool classified = true;
    for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      bool matches = IsRetryableError(cases[i].Error) == cases[i].Expected;
      classified = matches && classified;
    }
    CHECK(classified);
  }

  // nextTurn: queued messages are prepended at the start of the next run.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Agent_InitQueues(&ag);
    Agent_NextTurn(&ag, SVL("queued-first"));

    FauxStep steps[1] = {};
    steps[0].Text = SVL("ok");
    steps[0].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 1;
    ag.Prov = Faux_Provider(&f);

    Recorder rec;
    rec.Kinds = Array_Make<u8>(&a, 32);
    rec.Text = StrBuf_Make(&a, 32);
    ag.Sink.User = &rec;
    ag.Sink.OnEvent = RecSink;

    Agent_Prompt(&ag, SV("go"));
    // user(queued-first), user(go), assistant(ok)
    CHECK_ALL(ag.Messages.Count == 3, MessageTextEq(&ag, 0, Role::User, "queued-first"),
              MessageTextEq(&ag, 1, Role::User, "go"),
              ag.Messages.Data[2].Kind == Role::Assistant,
              CountKind(&rec, AgentEventKind::QueueUpdate) >= 1);
    ArenaRelease(&a);
  }

  // Steering queued before the run is injected before the first response.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Agent_InitQueues(&ag);
    Agent_Steer(&ag, SVL("steer-pre"));

    FauxStep steps[1] = {};
    steps[0].Text = SVL("ok");
    steps[0].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 1;
    ag.Prov = Faux_Provider(&f);

    Agent_Prompt(&ag, SV("go"));
    // user(go), user(steer-pre), assistant(ok)
    CHECK_ALL(ag.Messages.Count == 3, MessageTextEq(&ag, 0, Role::User, "go"),
              MessageTextEq(&ag, 1, Role::User, "steer-pre"));
    ArenaRelease(&a);
  }

  // A tool steers mid-run: the steering message is injected before the next turn.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Agent_InitQueues(&ag);

    Tool poke = {};
    poke.Name = SVL("poke");
    poke.Ctx = &ag;
    poke.Execute = PokeSteerExec;
    Agent_AddTool(&ag, poke);

    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("poke");
    steps[0].ToolId = SVL("c1");
    steps[0].ToolArgsJson = SVL("{}");
    steps[1].Text = SVL("after");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Agent_Prompt(&ag, SV("go"));
    // user(go), assistant(toolcall), toolResult(poked), user(steered), assistant(after)
    CHECK_ALL(ag.Messages.Count == 5, MessageTextEq(&ag, 3, Role::User, "steered"),
              MessageTextEq(&ag, 4, Role::Assistant, "after"));
    ArenaRelease(&a);
  }

  // followUp queued before the run continues the agent after it would stop.
  {
    Arena a = ArenaCreate();
    Model model = {};
    Agent ag = Agent_Make(&a, model, SV("sys"));
    Agent_InitQueues(&ag);
    Agent_FollowUp(&ag, SVL("follow"));

    FauxStep steps[2] = {};
    steps[0].Text = SVL("first");
    steps[0].Stop = StopReason::Stop;
    steps[1].Text = SVL("second");
    steps[1].Stop = StopReason::Stop;
    Faux f = {};
    f.Steps = steps;
    f.Count = 2;
    ag.Prov = Faux_Provider(&f);

    Agent_Prompt(&ag, SV("go"));
    // user(go), assistant(first), user(follow), assistant(second)
    CHECK_ALL(ag.Messages.Count == 4, MessageTextEq(&ag, 1, Role::Assistant, "first"),
              MessageTextEq(&ag, 2, Role::User, "follow"),
              MessageTextEq(&ag, 3, Role::Assistant, "second"));
    ArenaRelease(&a);
  }

  // Idle session reset clears transcript/queues/runtime flags while preserving
  // provider/model/tools/config, and final release destroys the queue mutex.
  {
    Arena a = ArenaCreate();
    Model model = {};
    model.Id = SVL("kept-model");
    Agent ag = Agent_Make(&a, model, SV("kept-system"));
    Agent_InitQueues(&ag);
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Execute = EchoExec;
    Agent_AddTool(&ag, echo);
    Provider provider = ag.Prov;
    volatile i32 abort = 1;
    ag.Stream.Abort = &abort;
    ag.Messages.Count = 1;
    ag.Messages.Data[0] = Message_User(&a, SVL("old message"));
    Agent_Steer(&ag, SVL("steer"));
    Agent_FollowUp(&ag, SVL("follow"));
    Agent_NextTurn(&ag, SVL("next"));

    ag.Streaming = true;
    bool resetWhileStreaming = Agent_ResetSession(&ag);
    CHECK_ALL(!resetWhileStreaming, ag.Messages.Count == 1);
    bool releaseWhileStreaming = Agent_Release(&ag);
    CHECK_ALL(!releaseWhileStreaming, ag.QueueInit);

    ag.Streaming = false;
    CHECK(Agent_ResetSession(&ag));
    CHECK_ALL(ag.Messages.Count == 0, ag.Steer.Count == 0, ag.FollowUp.Count == 0,
              ag.NextTurn.Count == 0, !ag.QueuesDirty);
    CHECK_ALL(ag.Stream.Abort == &abort, abort == 1,
              StrView_Eq(ag.Model.Id, SVL("kept-model")),
              StrView_Eq(ag.SystemPrompt, SVL("kept-system")), ag.Tools.Count == 1,
              ag.Prov.Stream == provider.Stream);

    Agent_Steer(&ag, SVL("after reset"));
    CHECK(ag.Steer.Count == 1);
    CHECK(Agent_Release(&ag));
    CHECK_ALL(!ag.QueueInit, ag.Steer.Count == 0);
    ArenaRelease(&a);
  }

  // Cancellation raised during per-stream auth resolution prevents the provider
  // request and records an aborted assistant turn.
  {
    Arena a = ArenaCreate();
    Model model = {};
    model.Provider = SVL("faux");
    Agent ag = Agent_Make(&a, model, SVL("sys"));
    volatile i32 abort = 0;
    ag.Stream.Abort = &abort;
    AbortAuthContext auth = {};
    ag.AuthUser = &auth;
    ag.ResolveAuth = AbortDuringAuth;

    FauxStep step = {};
    step.Text = SVL("must not stream");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    ag.Prov = Faux_Provider(&faux);

    Agent_Prompt(&ag, SVL("go"));
    CHECK_ALL(auth.Calls == 1, auth.SeenAbort == &abort, faux.Index == 0,
              ag.Messages.Count == 2, ag.Messages.Data[1].Kind == Role::Assistant,
              ag.Messages.Data[1].Stop == StopReason::Aborted);
    ArenaRelease(&a);
  }
}
