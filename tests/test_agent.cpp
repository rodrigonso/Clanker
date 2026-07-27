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

    CHECK(ag.Messages.Count == 6);
    CHECK(ag.Messages.Data[1].Kind == Role::Assistant);
    CHECK(ag.Messages.Data[2].Kind == Role::ToolResult);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].ToolCallId, "c0"));
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "A"));
    CHECK(StrView_EqCStr(ag.Messages.Data[3].ToolCallId, "c1"));
    CHECK(StrView_EqCStr(ag.Messages.Data[3].Content.Data[0].Text, "B"));
    CHECK(StrView_EqCStr(ag.Messages.Data[4].ToolCallId, "c2"));
    CHECK(StrView_EqCStr(ag.Messages.Data[4].Content.Data[0].Text, "C"));
    CHECK(ag.Messages.Data[5].Kind == Role::Assistant);
    CHECK(CountKind(&rec, AgentEventKind::ToolStart) == 3);
    CHECK(CountKind(&rec, AgentEventKind::ToolEnd) == 3);
    CHECK(rec.Kinds.Data[rec.Kinds.Count - 1] == (u8)AgentEventKind::AgentEnd);
    CHECK(conc.Max >= 2);  // proves real concurrency

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

    CHECK(ag.Messages.Count == 6);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "A"));
    CHECK(StrView_EqCStr(ag.Messages.Data[3].Content.Data[0].Text, "B"));
    CHECK(StrView_EqCStr(ag.Messages.Data[4].Content.Data[0].Text, "C"));
    CHECK(conc.Max == 1);  // never overlapped

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

    CHECK(ag.Messages.Count == 6);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "A"));
    CHECK(StrView_EqCStr(ag.Messages.Data[4].Content.Data[0].Text, "C"));
    CHECK(conc.Max == 1);

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

    CHECK(ag.Messages.Count == 6);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "A"));
    CHECK(ag.Messages.Data[2].IsError == false);
    CHECK(ag.Messages.Data[3].IsError == true);
    CHECK(StrView_EqCStr(ag.Messages.Data[3].Content.Data[0].Text, "Tool not found: missing"));
    CHECK(StrView_EqCStr(ag.Messages.Data[4].Content.Data[0].Text, "C"));
    CHECK(conc.Max >= 2);

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
    CHECK(ag.Messages.Count == 4);
    CHECK(ag.Messages.Data[2].Kind == Role::ToolResult);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "OVERRIDDEN"));
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
    CHECK(ag.Messages.Count == 3);
    CHECK(ag.Messages.Data[2].Kind == Role::ToolResult);
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
    CHECK(ag.Messages.Count == 2);
    CHECK(ag.Messages.Data[1].Kind == Role::Assistant);
    CHECK(ag.Messages.Data[1].Stop == StopReason::Stop);
    CHECK(StrView_EqCStr(StrBuf_View(&rec.Text), "recovered"));
    CHECK(CountKind(&rec, AgentEventKind::RetryStart) == 1);
    CHECK(CountKind(&rec, AgentEventKind::RetryEnd) == 1);
    CHECK(f.Index == 2);  // two provider calls: error then success
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
    CHECK(ag.Messages.Data[1].Stop == StopReason::Error);
    CHECK(CountKind(&rec, AgentEventKind::RetryStart) == 0);
    CHECK(f.Index == 1);  // single provider call, no retry
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
    CHECK(ag.Messages.Data[1].Stop == StopReason::Error);
    CHECK(CountKind(&rec, AgentEventKind::RetryStart) == 3);  // 3 retries, 4 calls
    CHECK(f.Index == 4);
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
    CHECK(elapsed < 5000);  // did not actually sleep 100s
    CHECK(CountKind(&rec, AgentEventKind::RetryStart) == 0);
    CHECK(f.Index == 0);
    CHECK(ag.Messages.Data[1].Stop == StopReason::Aborted);
    ArenaRelease(&a);
  }

  // IsRetryableError classifier (port of pi's isRetryableAssistantError).
  {
    CHECK(IsRetryableError(SVL("HTTP 429: overloaded_error")) == true);
    CHECK(IsRetryableError(SVL("overloaded")) == true);
    CHECK(IsRetryableError(SVL("HTTP 503 Service Unavailable")) == true);
    CHECK(IsRetryableError(SVL("network error: connection refused")) == true);
    CHECK(IsRetryableError(SVL("socket hang up")) == true);
    CHECK(IsRetryableError(SVL("HTTP 400: invalid_request_error")) == false);
    CHECK(IsRetryableError(SVL("HTTP 401 unauthorized")) == false);
    CHECK(IsRetryableError(SVL("insufficient_quota")) == false);
    // Non-retryable exclusion wins even when a retryable token is present.
    CHECK(IsRetryableError(SVL("429 but Monthly usage limit reached")) == false);
    CHECK(IsRetryableError(StrView{nullptr, 0}) == false);
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
    CHECK(ag.Messages.Count == 3);
    CHECK(ag.Messages.Data[0].Kind == Role::User);
    CHECK(StrView_EqCStr(ag.Messages.Data[0].Content.Data[0].Text, "queued-first"));
    CHECK(StrView_EqCStr(ag.Messages.Data[1].Content.Data[0].Text, "go"));
    CHECK(ag.Messages.Data[2].Kind == Role::Assistant);
    CHECK(CountKind(&rec, AgentEventKind::QueueUpdate) >= 1);
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
    CHECK(ag.Messages.Count == 3);
    CHECK(StrView_EqCStr(ag.Messages.Data[0].Content.Data[0].Text, "go"));
    CHECK(StrView_EqCStr(ag.Messages.Data[1].Content.Data[0].Text, "steer-pre"));
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
    CHECK(ag.Messages.Count == 5);
    CHECK(ag.Messages.Data[3].Kind == Role::User);
    CHECK(StrView_EqCStr(ag.Messages.Data[3].Content.Data[0].Text, "steered"));
    CHECK(StrView_EqCStr(ag.Messages.Data[4].Content.Data[0].Text, "after"));
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
    CHECK(ag.Messages.Count == 4);
    CHECK(StrView_EqCStr(ag.Messages.Data[1].Content.Data[0].Text, "first"));
    CHECK(ag.Messages.Data[2].Kind == Role::User);
    CHECK(StrView_EqCStr(ag.Messages.Data[2].Content.Data[0].Text, "follow"));
    CHECK(StrView_EqCStr(ag.Messages.Data[3].Content.Data[0].Text, "second"));
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
    CHECK(!Agent_ResetSession(&ag));
    CHECK(ag.Messages.Count == 1);
    CHECK(!Agent_Release(&ag));
    CHECK(ag.QueueInit);

    ag.Streaming = false;
    CHECK(Agent_ResetSession(&ag));
    CHECK(ag.Messages.Count == 0);
    CHECK(ag.Steer.Count == 0);
    CHECK(ag.FollowUp.Count == 0);
    CHECK(ag.NextTurn.Count == 0);
    CHECK(!ag.QueuesDirty);
    CHECK(ag.Stream.Abort == &abort);
    CHECK(abort == 1);
    CHECK(StrView_Eq(ag.Model.Id, SVL("kept-model")));
    CHECK(StrView_Eq(ag.SystemPrompt, SVL("kept-system")));
    CHECK(ag.Tools.Count == 1);
    CHECK(ag.Prov.Stream == provider.Stream);

    Agent_Steer(&ag, SVL("after reset"));
    CHECK(ag.Steer.Count == 1);
    CHECK(Agent_Release(&ag));
    CHECK(!ag.QueueInit);
    CHECK(ag.Steer.Count == 0);
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
    CHECK(auth.Calls == 1);
    CHECK(auth.SeenAbort == &abort);
    CHECK(faux.Index == 0);
    CHECK(ag.Messages.Count == 2);
    CHECK(ag.Messages.Data[1].Kind == Role::Assistant);
    CHECK(ag.Messages.Data[1].Stop == StopReason::Aborted);
    ArenaRelease(&a);
  }
}
