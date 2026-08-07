#pragma once

#include "app/runtime.h"

// Bridges the harness's watch stream to whatever this process is rendering.
//
// A background thread owns the watch and is the single point where the agent's
// events are already in order, so it is also where run-scoped hooks are
// dispatched: reporting Stop from the calling thread would race the pump and
// could land ahead of the run's own last PostToolUse.
struct HarnessEventPump {
  AgentHarnessWatch Watch;
  AgentHarness* Harness;
  Tui* TuiView;
  HarnessEventSink Sink;
  Arena CompletionMem;
  Array<StrView> LiveToolEnds;
  Thread Worker;
  Mutex WatchGuard;
  bool WatchGuardInitialized;
  bool StopRequested;
  volatile i32 Exited;
  bool Started;
  // `--mode json`: write raw watch events as NDJSON instead of forwarding them
  // to the human-facing HarnessEventSink.
  bool JsonOutput;
};

SessionResult StartHarnessEventPump(Arena* arena, AgentHarness* harness,
                                    HarnessEventSink sink, Tui* tuiView,
                                    HarnessEventPump* pump,
                                    AgentHarnessSnapshot* snapshot,
                                    bool jsonOutput = false);
void StopHarnessEventPump(HarnessEventPump* pump);

// Renders the watch stream to stdout for the plain (non-TUI) REPL.
void ReplSink(void* user, const AgentHarnessEvent* ev);

void ReportHarnessResult(SessionResult result);
