#include "agent/agent.h"

#include "base/str.h"
#include "base/thread.h"
#include "llm/retry.h"

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
  a.ToolExec = ToolExecution::Parallel;  // pi parity: tool batches run concurrently
  a.MaxParallelTools = 0;                // 0 => default cap
  a.RetryEnabled = true;                 // pi parity: auto-retry transient errors
  a.MaxRetries = 3;
  a.RetryBaseDelayMs = 2000;  // 2s, 4s, 8s exponential backoff
  a.MaxRetryDelayMs = 60000;  // cap any single backoff at 60s
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

// Sleep up to totalMs, polling the abort flag every ~50ms so a user can cancel
// during a retry backoff. Returns true if an abort was observed.
static bool SleepAbortable(u32 totalMs, volatile i32* abort) {
  const u32 slice = 50;
  u32 elapsed = 0;
  while (elapsed < totalMs) {
    if (abort && *abort) return true;
    u32 step = (totalMs - elapsed) < slice ? (totalMs - elapsed) : slice;
    Sleep_Ms(step);
    elapsed += step;
  }
  return abort && *abort;
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

  // Stream the assistant turn, retrying transient provider errors with capped
  // exponential backoff (pi parity). Auth is re-resolved each attempt so an
  // expiring OAuth token refreshes across a backoff. Failed attempts are never
  // appended to the transcript; only the final message is stored below.
  Message assistant = {};
  bool ok = false;
  for (u32 attempt = 0;; attempt++) {
    StreamOptions opts = a->Stream;
    if (a->ResolveAuth) {
      StrView token;
      if (a->ResolveAuth(a->AuthUser, a->Mem, a->Model.Provider, &token)) opts.ApiKey = token;
    }

    assistant = Message{};
    ok = a->Prov.Stream(a->Prov.Ctx, a->Mem, &a->Model, &ctx, &opts, sink, &assistant);

    bool isError = (!ok || assistant.Stop == StopReason::Error);
    bool aborted = (assistant.Stop == StopReason::Aborted);
    // Only retry a clean transient failure: error (not abort), budget left, no
    // content streamed yet (so the UI saw no partial output), and classifiable
    // as retryable from the error text.
    bool canRetry = a->RetryEnabled && isError && !aborted && attempt < a->MaxRetries &&
                    assistant.Content.Count == 0 && IsRetryableError(assistant.ErrorMessage);
    if (!canRetry) break;

    u32 sh = attempt < 20 ? attempt : 20;
    u64 d = (u64)a->RetryBaseDelayMs << sh;  // base * 2^attempt
    if (a->MaxRetryDelayMs && d > a->MaxRetryDelayMs) d = a->MaxRetryDelayMs;
    u32 delay = (u32)d;

    AgentEvent rs = {};
    rs.Kind = AgentEventKind::RetryStart;
    rs.RetryAttempt = attempt + 1;
    rs.RetryMaxAttempts = a->MaxRetries;
    rs.RetryDelayMs = delay;
    rs.RetryError = assistant.ErrorMessage;
    Emit(a, rs);

    bool abortedInSleep = SleepAbortable(delay, a->Stream.Abort);

    AgentEvent re = {};
    re.Kind = AgentEventKind::RetryEnd;
    re.RetryAttempt = attempt + 1;
    Emit(a, re);

    if (abortedInSleep) break;  // user cancelled during backoff; surface the error
  }

  Message* stored = Array_Push(&a->Messages, assistant);
  AgentEvent me = {};
  me.Kind = AgentEventKind::MessageEnd;
  me.Msg = stored;
  Emit(a, me);

  *outAssistant = assistant;
  return ok;
}

// =============================================================================
// Tool batch execution. A single assistant message may contain several tool
// calls. Mirroring pi, the batch runs in parallel by default (each call on a
// small worker pool, isolated into its own arena), or sequentially when the
// agent is configured ToolExecution::Sequential or any called tool is marked
// ToolExecMode::Sequential. Tool-result messages are always appended to the
// transcript in original call order, regardless of completion order.
// =============================================================================

static const usize WORKER_ARENA_RESERVE = (usize)1 << 30;  // virtual-only; commits lazily
static const u32 DEFAULT_MAX_PARALLEL = 8;

// Deep-copy one content block into `dst` so the transcript owns stable slices
// (used to lift a tool result out of its per-worker arena).
static Content Content_Relocate(Arena* dst, const Content& c) {
  Content r = c;
  r.Text = StrView_Dup(dst, c.Text);
  r.Signature = StrView_Dup(dst, c.Signature);
  r.ToolId = StrView_Dup(dst, c.ToolId);
  r.ToolName = StrView_Dup(dst, c.ToolName);
  r.ToolArgsJson = StrView_Dup(dst, c.ToolArgsJson);
  r.ImageData = StrView_Dup(dst, c.ImageData);
  r.ImageMimeType = StrView_Dup(dst, c.ImageMimeType);
  return r;
}

static Array<Content> ContentArray_Relocate(Arena* dst, const Array<Content>& src) {
  Array<Content> out = Array_Make<Content>(dst, src.Count ? src.Count : 1);
  for (u32 i = 0; i < src.Count; i++) Array_Push(&out, Content_Relocate(dst, src.Data[i]));
  return out;
}

// One resolved tool call plus its captured result.
struct ToolJob {
  const Content* Call;    // the toolCall content (id/name/args)
  Tool* Resolved;         // resolved tool (runnable jobs only)
  volatile i32* Abort;
  bool Runnable;          // false => immediate result already produced (not found / blocked)
  bool HasWorker;         // result lives in Worker and must be relocated
  Arena Worker;           // per-job arena for parallel runs
  Array<Content> Result;  // result content (in Worker if HasWorker, else session arena)
  bool IsError;
  bool Terminate;
};

// Resolve the tool and apply the BeforeTool gate (main thread). Returns true if
// the call should run; false if it produced an immediate error result in `into`.
static bool PrepareTool(Agent* a, ToolJob* job, Arena* into) {
  const Content* tc = job->Call;
  Tool* tool = FindTool(a, tc->ToolName);
  if (!tool) {
    StrBuf e = StrBuf_Make(into, 64);
    StrBuf_AppendCStr(&e, "Tool not found: ");
    StrBuf_Append(&e, tc->ToolName);
    job->Result = Array_Make<Content>(into, 1);
    Array_Push(&job->Result, Content_Text(StrBuf_View(&e)));
    job->IsError = true;
    return false;
  }
  StrView reason = {};
  if (a->BeforeTool && !a->BeforeTool(a->User, a->Mem, tc->ToolName, tc->ToolArgsJson, &reason)) {
    StrBuf e = StrBuf_Make(into, 64);
    StrBuf_AppendCStr(&e, reason.Length ? "" : "Tool blocked");
    StrBuf_Append(&e, reason);
    job->Result = Array_Make<Content>(into, 1);
    Array_Push(&job->Result, Content_Text(StrBuf_View(&e)));
    job->IsError = true;
    return false;
  }
  job->Resolved = tool;
  return true;
}

// Run a prepared tool into `arena` and capture its result. Worker-safe: touches
// only `job` and `arena`, never the session arena, transcript, or event sink.
static void RunToolInto(ToolJob* job, Arena* arena) {
  const Content* tc = job->Call;
  StrBuf err = StrBuf_Make(arena, 64);
  ToolResult r = {};
  r.Content = Array_Make<Content>(arena, 1);
  bool ok = job->Resolved->Execute(job->Resolved->Ctx, arena, tc->ToolId, tc->ToolArgsJson, job->Abort,
                                   &r, &err);
  if (ok) {
    job->Result = r.Content;
    job->Terminate = r.Terminate;
    job->IsError = false;
  } else {
    job->Result = Array_Make<Content>(arena, 1);
    Array_Push(&job->Result, Content_Text(StrView_Dup(arena, StrBuf_View(&err))));
    job->IsError = true;
    job->Terminate = false;
  }
}

// Append a tool result to the transcript and emit its events (main thread, in
// original call order). Relocates a worker-arena result into the session arena.
static void FinalizeTool(Agent* a, ToolJob* job, bool* allTerminate) {
  const Content* tc = job->Call;
  Array<Content> content = job->HasWorker ? ContentArray_Relocate(a->Mem, job->Result) : job->Result;
  bool isError = job->IsError;
  bool terminate = job->Terminate;

  // afterToolCall: let the host rewrite the outcome before it is recorded.
  if (a->AfterTool) {
    ToolOutcome io = {};
    io.Content = content;
    io.IsError = isError;
    io.Terminate = terminate;
    a->AfterTool(a->User, a->Mem, tc->ToolName, tc->ToolArgsJson, &io);
    content = io.Content;
    isError = io.IsError;
    terminate = io.Terminate;
  }

  if (!terminate) *allTerminate = false;

  ToolResult tr = {};
  tr.Content = content;
  tr.Terminate = terminate;

  AgentEvent e = {};
  e.Kind = AgentEventKind::ToolEnd;
  e.ToolCallId = tc->ToolId;
  e.ToolName = tc->ToolName;
  e.Result = &tr;
  e.IsError = isError;
  Emit(a, e);

  Message m = {};
  m.Kind = Role::ToolResult;
  m.Content = content;
  m.ToolCallId = tc->ToolId;
  m.ToolName = tc->ToolName;
  m.IsError = isError;
  m.Timestamp = NowMillis();
  Message* stored = Array_Push(&a->Messages, m);
  EmitMessageStartEnd(a, stored);
}

// Bounded worker pool: threads pull runnable jobs from a shared index, each
// executing into its own arena. No shared mutable state beyond the index.
struct ToolPool {
  ToolJob** Jobs;  // runnable jobs
  u32 Count;
  u32 Next;
  Mutex Lock;
};

static void ToolWorker(void* arg) {
  ToolPool* p = (ToolPool*)arg;
  for (;;) {
    Mutex_Lock(&p->Lock);
    u32 i = p->Next;
    if (i < p->Count) p->Next = i + 1;
    Mutex_Unlock(&p->Lock);
    if (i >= p->Count) break;
    RunToolInto(p->Jobs[i], &p->Jobs[i]->Worker);
  }
}

// Execute every tool call in the assistant message. Sets *anyTool and updates
// *allTerminate (false unless every tool succeeded with Terminate set).
static void ExecuteToolBatch(Agent* a, const Message* assistant, bool* allTerminate, bool* anyTool) {
  u32 n = 0;
  for (u32 i = 0; i < assistant->Content.Count; i++)
    if (assistant->Content.Data[i].Kind == ContentKind::ToolCall) n++;
  if (n == 0) return;
  *anyTool = true;

  ToolJob* jobs = ArenaPushArray(a->Mem, ToolJob, n);
  u32 j = 0;
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    const Content* c = &assistant->Content.Data[i];
    if (c->Kind != ContentKind::ToolCall) continue;
    jobs[j] = ToolJob{};
    jobs[j].Call = c;
    jobs[j].Abort = a->Stream.Abort;
    j++;
  }

  // Phase A: emit tool_start + prepare (resolve + gate) for each call, in order.
  bool sequential = (a->ToolExec == ToolExecution::Sequential);
  for (u32 i = 0; i < n; i++) {
    ToolJob* job = &jobs[i];
    AgentEvent s = {};
    s.Kind = AgentEventKind::ToolStart;
    s.ToolCallId = job->Call->ToolId;
    s.ToolName = job->Call->ToolName;
    s.ToolArgsJson = job->Call->ToolArgsJson;
    Emit(a, s);
    job->Runnable = PrepareTool(a, job, a->Mem);
    if (job->Runnable && job->Resolved->ExecMode == ToolExecMode::Sequential) sequential = true;
  }

  u32 runnableCount = 0;
  for (u32 i = 0; i < n; i++)
    if (jobs[i].Runnable) runnableCount++;

  // Phase B/C: run. Spawn threads only when >=2 tools can actually run at once.
  if (!sequential && runnableCount >= 2) {
    ToolJob** run = ArenaPushArray(a->Mem, ToolJob*, runnableCount);
    u32 r = 0;
    for (u32 i = 0; i < n; i++) {
      if (!jobs[i].Runnable) continue;
      jobs[i].Worker = ArenaCreate(WORKER_ARENA_RESERVE);
      jobs[i].HasWorker = true;
      run[r++] = &jobs[i];
    }
    u32 cap = a->MaxParallelTools ? a->MaxParallelTools : DEFAULT_MAX_PARALLEL;
    u32 nthreads = runnableCount < cap ? runnableCount : cap;
    ToolPool pool = {};
    pool.Jobs = run;
    pool.Count = runnableCount;
    pool.Next = 0;
    Mutex_Init(&pool.Lock);
    Thread* threads = ArenaPushArray(a->Mem, Thread, nthreads);
    for (u32 t = 0; t < nthreads; t++) Thread_Start(&threads[t], ToolWorker, &pool);
    for (u32 t = 0; t < nthreads; t++) Thread_Join(&threads[t]);
    Mutex_Destroy(&pool.Lock);
  } else {
    // Sequential / single-tool fast path: run inline into the session arena.
    for (u32 i = 0; i < n; i++)
      if (jobs[i].Runnable) RunToolInto(&jobs[i], a->Mem);
  }

  // Phase D: finalize in original order, then release worker arenas.
  for (u32 i = 0; i < n; i++) {
    FinalizeTool(a, &jobs[i], allTerminate);
    if (jobs[i].HasWorker) ArenaRelease(&jobs[i].Worker);
  }
}

void Agent_InitQueues(Agent* a) {
  if (a->QueueInit) return;
  Mutex_Init(&a->QueueLock);
  a->Steer = MsgQueue{};
  a->FollowUp = MsgQueue{};
  a->NextTurn = MsgQueue{};
  a->QueuesDirty = false;
  a->QueueInit = true;
}

static void QueuePushLocked(MsgQueue* q, StrView text) {
  if (q->Count >= 64) return;  // drop on overflow
  q->Items[q->Tail] = text;
  q->Tail = (q->Tail + 1) % 64;
  q->Count++;
}

static void QueuePush(Agent* a, MsgQueue* q, StrView text) {
  if (!a->QueueInit) return;
  Mutex_Lock(&a->QueueLock);
  QueuePushLocked(q, text);
  a->QueuesDirty = true;
  Mutex_Unlock(&a->QueueLock);
}

void Agent_Steer(Agent* a, StrView text) { QueuePush(a, &a->Steer, text); }
void Agent_FollowUp(Agent* a, StrView text) { QueuePush(a, &a->FollowUp, text); }
void Agent_NextTurn(Agent* a, StrView text) { QueuePush(a, &a->NextTurn, text); }

// Drain a queue into a session-arena array of duped StrViews. Thread-safe: the
// brief critical section only copies the views out; the arena dup happens after
// unlocking (agent thread only). Returns count; *out points to the duped views.
static u32 DrainQueue(Agent* a, MsgQueue* q, StrView** out) {
  StrView tmp[64];
  Mutex_Lock(&a->QueueLock);
  u32 n = q->Count;
  for (u32 i = 0; i < n; i++) {
    tmp[i] = q->Items[q->Head];
    q->Head = (q->Head + 1) % 64;
  }
  q->Count = 0;
  Mutex_Unlock(&a->QueueLock);
  if (n == 0) {
    *out = nullptr;
    return 0;
  }
  StrView* arr = ArenaPushArray(a->Mem, StrView, n);
  for (u32 i = 0; i < n; i++) arr[i] = StrView_Dup(a->Mem, tmp[i]);
  *out = arr;
  return n;
}

// Emit a queue_update snapshot (counts). Always on the agent thread, never while
// holding QueueLock, so the (single-threaded) sink can't deadlock with a producer.
static void EmitQueueUpdate(Agent* a) {
  if (!a->QueueInit) return;
  Mutex_Lock(&a->QueueLock);
  u32 s = a->Steer.Count, f = a->FollowUp.Count, nt = a->NextTurn.Count;
  a->QueuesDirty = false;
  Mutex_Unlock(&a->QueueLock);
  AgentEvent ev = {};
  ev.Kind = AgentEventKind::QueueUpdate;
  ev.SteerCount = s;
  ev.FollowUpCount = f;
  ev.NextTurnCount = nt;
  Emit(a, ev);
}

static bool QueuesDirty(Agent* a) {
  Mutex_Lock(&a->QueueLock);
  bool d = a->QueuesDirty;
  Mutex_Unlock(&a->QueueLock);
  return d;
}

// Append a batch of user messages to the transcript, emitting message events.
static void InjectUserMessages(Agent* a, const StrView* texts, u32 count) {
  for (u32 i = 0; i < count; i++) {
    Message um = Message_User(a->Mem, texts[i]);  // already duped into Mem by DrainQueue
    Message* stored = Array_Push(&a->Messages, um);
    EmitMessageStartEnd(a, stored);
  }
}

static void RunLoop(Agent* a, const StrView* promptText) {
  if (!a->QueueInit) Agent_InitQueues(a);
  a->Streaming = true;

  AgentEvent start = {};
  start.Kind = AgentEventKind::AgentStart;
  Emit(a, start);

  AgentEvent turn = {};
  turn.Kind = AgentEventKind::TurnStart;
  Emit(a, turn);

  // nextTurn queue: messages queued for the start of this run, prepended before
  // the prompt (pi parity, agent-harness.ts:538-543).
  StrView* nextTurn = nullptr;
  u32 nextTurnN = DrainQueue(a, &a->NextTurn, &nextTurn);
  InjectUserMessages(a, nextTurn, nextTurnN);

  if (promptText) {
    Message um = Message_User(a->Mem, StrView_Dup(a->Mem, *promptText));
    Message* stored = Array_Push(&a->Messages, um);
    EmitMessageStartEnd(a, stored);
  }
  if (nextTurnN || QueuesDirty(a)) EmitQueueUpdate(a);

  bool firstTurn = true;

  // Steering messages typed while waiting are injected before the first response.
  StrView* pending = nullptr;
  u32 pendingN = DrainQueue(a, &a->Steer, &pending);

  while (true) {
    bool hasMoreToolCalls = true;
    while (hasMoreToolCalls || pendingN > 0) {
      if (!firstTurn) {
        AgentEvent ts = {};
        ts.Kind = AgentEventKind::TurnStart;
        Emit(a, ts);
      } else {
        firstTurn = false;
      }

      if (pendingN > 0) {
        InjectUserMessages(a, pending, pendingN);
        EmitQueueUpdate(a);
        pendingN = 0;
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
      ExecuteToolBatch(a, &assistant, &allTerminate, &anyTool);
      if (anyTool && !allTerminate) hasMoreToolCalls = true;

      AgentEvent te = {};
      te.Kind = AgentEventKind::TurnEnd;
      te.Msg = &assistant;
      Emit(a, te);

      pendingN = DrainQueue(a, &a->Steer, &pending);
      if (pendingN || QueuesDirty(a)) EmitQueueUpdate(a);
    }

    // Agent would stop. Check for follow-up messages that continue the run.
    pendingN = DrainQueue(a, &a->FollowUp, &pending);
    if (pendingN > 0) {
      EmitQueueUpdate(a);
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
