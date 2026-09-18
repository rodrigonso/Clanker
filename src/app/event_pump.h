#pragma once

#include "app/runtime.h"

// Presentation delivery, including ordered run-hook dispatch. Machine output
// uses the synchronous observer below, not this deliberately lossy watch.
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
  bool Started;
};

// Replaces the TUI transcript synchronously before starting event delivery;
// other consumers subscribe without copying history.
SessionResult StartHarnessEventPump(AgentHarness* harness,
                                    HarnessEventSink sink, Tui* tuiView,
                                    HarnessEventPump* pump);
void StopHarnessEventPump(HarnessEventPump* pump);

struct HarnessJsonWriter {
  void* User;
  // Consumes a complete newline-terminated line before returning, including
  // flushing any buffered output. False means delivery failed.
  bool (*Write)(void* user, StrView line);
};

struct HarnessJsonOutput {
  AgentHarness* Harness;
  Arena Scratch;
  HarnessJsonWriter Writer;
  SessionResult Failure;
  bool Started;
};

// Start/finish only while idle. A null writer uses stdout. No watch, queue, or
// worker is allocated; stdout backpressure reaches the producer directly.
SessionResult StartHarnessJsonOutput(AgentHarness* harness,
                                     HarnessJsonOutput* output,
                                     HarnessJsonWriter writer = {});
// Detaches, writes the final result, and releases scratch. The returned verdict
// includes encoding/write/flush failures and determines the process exit code.
SessionResult FinishHarnessJsonOutput(HarnessJsonOutput* output,
                                      SessionResult result);

// Renders the watch stream to stdout for the plain (non-TUI) REPL.
void ReplSink(void* user, const AgentHarnessEvent* ev);

void ReportHarnessResult(SessionResult result);
