#include "app/hook_runner.h"

#include "base/thread.h"
#include "json/json.h"
#include "tools/exec.h"

namespace {

// A hook is a status notification: dropping one under a burst is strictly
// better than blocking the agent loop that produced it.
constexpr u32 HOOK_QUEUE_CAPACITY = 64;
constexpr u32 HOOK_TOOL_NAME_CAPACITY = 64;
// Long enough that an ordinary run delivers everything it queued, short enough
// that a wedged or spammed hook cannot hold the process open.
constexpr u32 HOOK_SHUTDOWN_DRAIN_MS = 5000;

constexpr u32 HOOK_SESSION_ID_CAPACITY = 128;
constexpr u32 HOOK_PATH_CAPACITY = 1024;

struct HookQueueItem {
  HookEvent Event;
  char ToolName[HOOK_TOOL_NAME_CAPACITY];
  u32 ToolNameLength;
};

// Session identity held inline rather than as arena-backed views. The worker
// snapshots the whole struct by value under the lock, so a switch can rewrite
// it without invalidating a copy already in flight, and a process that switches
// sessions all day cannot grow the runner one duplicate at a time -- the arena
// this used to allocate from traps on exhaustion rather than failing softly.
struct HookSessionStorage {
  char SessionId[HOOK_SESSION_ID_CAPACITY];
  u32 SessionIdLength;
  char WorkingDirectory[HOOK_PATH_CAPACITY];
  u32 WorkingDirectoryLength;
  char TranscriptPath[HOOK_PATH_CAPACITY];
  u32 TranscriptPathLength;
};

// A value that does not fit is reported as absent rather than truncated: half a
// path is worse than no path, because a consumer would act on it.
void StoreField(char* destination, u32 capacity, u32* outLength, StrView value) {
  if (value.Length > capacity) {
    *outLength = 0;
    return;
  }
  if (value.Length) memcpy(destination, value.Data, value.Length);
  *outLength = value.Length;
}

void StoreSession(HookSessionStorage* out, const HookSessionInfo* source) {
  memset(out, 0, sizeof(*out));
  if (!source) return;
  StoreField(out->SessionId, HOOK_SESSION_ID_CAPACITY, &out->SessionIdLength,
             source->SessionId);
  StoreField(out->WorkingDirectory, HOOK_PATH_CAPACITY, &out->WorkingDirectoryLength,
             source->WorkingDirectory);
  StoreField(out->TranscriptPath, HOOK_PATH_CAPACITY, &out->TranscriptPathLength,
             source->TranscriptPath);
}

// Views borrow `storage`, so the caller must keep it alive across the payload
// build that consumes them.
HookSessionInfo ViewSession(const HookSessionStorage* storage) {
  HookSessionInfo out = {};
  out.SessionId = StrView{storage->SessionId, storage->SessionIdLength};
  out.WorkingDirectory = StrView{storage->WorkingDirectory, storage->WorkingDirectoryLength};
  out.TranscriptPath = StrView{storage->TranscriptPath, storage->TranscriptPathLength};
  return out;
}

}  // namespace

struct HookRunner {
  const HookConfig* Config;
  HookSessionStorage Session;

  Mutex Lock;
  CondVar Changed;
  HookQueueItem Items[HOOK_QUEUE_CAPACITY];
  u32 Head;
  u32 Count;
  bool StopRequested;
  // Set when the drain grace period expires, to abandon whatever is queued.
  bool Abandon;
  // Distinguishes a drained queue from one whose last item is still in flight.
  bool Busy;
  // Cuts a running command short. Separate from StopRequested so the terminal
  // event can still run to completion after the worker has stopped.
  volatile i32 Abort;

  Thread Worker;
  bool Started;
};

StrView HookEvent_Name(HookEvent event) {
  switch (event) {
    case HookEvent::SessionStart: return SVL("SessionStart");
    case HookEvent::UserPromptSubmit: return SVL("UserPromptSubmit");
    case HookEvent::PreToolUse: return SVL("PreToolUse");
    case HookEvent::PostToolUse: return SVL("PostToolUse");
    case HookEvent::Stop: return SVL("Stop");
    case HookEvent::StopFailure: return SVL("StopFailure");
    case HookEvent::SessionEnd: return SVL("SessionEnd");
  }
  return StrView{};
}

void HookRunner_BuildPayload(StrBuf* out, HookEvent event, const HookSessionInfo* session,
                             StrView toolName) {
  JsonWriter w;
  JsonWrite_Begin(&w, out);
  JsonWrite_String(&w, "hook_event_name", HookEvent_Name(event));
  if (session) {
    if (session->SessionId.Length) JsonWrite_String(&w, "session_id", session->SessionId);
    if (session->WorkingDirectory.Length)
      JsonWrite_String(&w, "cwd", session->WorkingDirectory);
    if (session->TranscriptPath.Length)
      JsonWrite_String(&w, "transcript_path", session->TranscriptPath);
  }
  if (toolName.Length) JsonWrite_String(&w, "tool_name", toolName);
  JsonWrite_End(&w);
}

namespace {

// `session` is a caller-owned snapshot rather than a read of runner->Session,
// which HookRunner_SetSession may be rewriting concurrently.
void RunOne(HookRunner* runner, Arena* scratch, const HookQueueItem* item,
            const HookSessionInfo* session, volatile i32* abort) {
  StrView toolName = {item->ToolName, item->ToolNameLength};
  // Sized from the whole table, which bounds any single event's selection, so
  // configuring one more command than some fixed cap cannot silently stop it
  // from ever firing.
  u32 capacity = runner->Config->CommandCount;
  if (!capacity) return;
  const HookCommand** commands = ArenaPushArray(scratch, const HookCommand*, capacity);
  u32 count = HookConfig_Select(runner->Config, HookEvent_Name(item->Event), toolName,
                                commands, capacity);
  if (!count) return;

  StrBuf payload = StrBuf_Make(scratch, 512);
  HookRunner_BuildPayload(&payload, item->Event, session, toolName);
  StrView payloadView = StrBuf_View(&payload);

  // Output and exit code carry no meaning: this is a one-way status report, and
  // a broken hook must not become an agent failure.
  for (u32 i = 0; i < count; i++) {
    // Checked between commands as well as inside the wait loop: once shutdown
    // has given up, spawning the rest of an unbounded group only to kill each
    // one is pure latency.
    if (abort && Atomic_LoadI32(abort)) break;
    Exec_RunShellInput(scratch, commands[i]->Command, payloadView, commands[i]->TimeoutMs,
                       abort);
  }
}

void RunHookWorker(void* user) {
  HookRunner* runner = (HookRunner*)user;
  Arena scratch = ArenaCreate((usize)1 << 20);
  for (;;) {
    Mutex_Lock(&runner->Lock);
    while (!runner->Count && !runner->StopRequested) Cond_Wait(&runner->Changed, &runner->Lock);
    if (!runner->Count || runner->Abandon) {
      Mutex_Unlock(&runner->Lock);
      break;
    }
    HookQueueItem item = runner->Items[runner->Head];
    runner->Head = (runner->Head + 1) % HOOK_QUEUE_CAPACITY;
    runner->Count--;
    // Snapshotted by value, not by view: HookRunner_SetSession may rewrite the
    // runner's copy the moment the lock is released.
    HookSessionStorage session = runner->Session;
    runner->Busy = true;
    Mutex_Unlock(&runner->Lock);

    ArenaReset(&scratch);
    HookSessionInfo info = ViewSession(&session);
    RunOne(runner, &scratch, &item, &info, &runner->Abort);

    Mutex_Lock(&runner->Lock);
    runner->Busy = false;
    Cond_Broadcast(&runner->Changed);
    Mutex_Unlock(&runner->Lock);
  }
  ArenaRelease(&scratch);
}

}  // namespace

HookRunner* HookRunner_Create(Arena* arena, const HookConfig* config,
                              const HookSessionInfo* session) {
  if (!arena || !config || !config->CommandCount) return nullptr;

  HookRunner* runner = ArenaPushStruct(arena, HookRunner);
  // memset rather than `*runner = HookRunner{}`: the volatile Abort member
  // makes the implicit copy-assignment ill-formed, and MSVC crashes on it.
  memset(runner, 0, sizeof(*runner));
  runner->Config = config;
  StoreSession(&runner->Session, session);
  Mutex_Init(&runner->Lock);
  Cond_Init(&runner->Changed);

  if (!Thread_Start(&runner->Worker, RunHookWorker, runner)) {
    Cond_Destroy(&runner->Changed);
    Mutex_Destroy(&runner->Lock);
    return nullptr;
  }
  runner->Started = true;
  return runner;
}

void HookRunner_SetSession(HookRunner* runner, const HookSessionInfo* session) {
  if (!runner || !session) return;

  // Copied under the lock so the worker never observes a half-updated identity.
  Mutex_Lock(&runner->Lock);
  StoreSession(&runner->Session, session);
  Mutex_Unlock(&runner->Lock);
}

void HookRunner_Dispatch(HookRunner* runner, HookEvent event, StrView toolName) {
  if (!runner || !runner->Started) return;

  HookQueueItem item = {};
  item.Event = event;
  item.ToolNameLength = toolName.Length < HOOK_TOOL_NAME_CAPACITY ? toolName.Length
                                                                 : HOOK_TOOL_NAME_CAPACITY - 1;
  if (item.ToolNameLength) memcpy(item.ToolName, toolName.Data, item.ToolNameLength);
  item.ToolName[item.ToolNameLength] = '\0';

  Mutex_Lock(&runner->Lock);
  if (!runner->StopRequested && runner->Count < HOOK_QUEUE_CAPACITY) {
    runner->Items[(runner->Head + runner->Count) % HOOK_QUEUE_CAPACITY] = item;
    runner->Count++;
    Cond_Signal(&runner->Changed);
  }
  Mutex_Unlock(&runner->Lock);
}

void HookRunner_Shutdown(HookRunner* runner, HookEvent finalEvent) {
  if (!runner || !runner->Started) return;

  Mutex_Lock(&runner->Lock);
  runner->StopRequested = true;
  Cond_Broadcast(&runner->Changed);
  Mutex_Unlock(&runner->Lock);

  u64 deadline = MonotonicMillis() + HOOK_SHUTDOWN_DRAIN_MS;
  for (;;) {
    Mutex_Lock(&runner->Lock);
    bool idle = !runner->Count && !runner->Busy;
    if (!idle && MonotonicMillis() >= deadline) {
      runner->Abandon = true;
      runner->Abort = 1;
      idle = true;
    }
    Mutex_Unlock(&runner->Lock);
    if (idle) break;
    Sleep_Ms(5);
  }

  Thread_Join(&runner->Worker);
  runner->Started = false;

  // Runs on the caller's thread with cancellation cleared: it is the one report
  // a consumer cannot do without, and is bounded by its own configured timeout.
  runner->Abort = 0;
  HookQueueItem terminal = {};
  terminal.Event = finalEvent;
  HookSessionInfo info = ViewSession(&runner->Session);
  Arena scratch = ArenaCreate((usize)1 << 20);
  RunOne(runner, &scratch, &terminal, &info, nullptr);
  ArenaRelease(&scratch);

  Cond_Destroy(&runner->Changed);
  Mutex_Destroy(&runner->Lock);
}
