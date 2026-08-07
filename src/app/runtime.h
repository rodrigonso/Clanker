#pragma once

#include "agent/harness.h"
#include "app/command.h"
#include "app/hook_runner.h"
#include "app/project_trust.h"
#include "app/prompt_context.h"
#include "app/provider_registry.h"
#include "auth/resolve.h"
#include "base/str.h"
#include "base/thread.h"
#include "session/jsonl_repository.h"
#include "session/repository.h"
#include "tools/tools.h"
#include "tui/tui.h"

// Shared state and the renderer-neutral seams every interactive command is
// written against. A command handler never asks whether it is running under the
// TUI or the plain REPL; it goes through the Runtime* calls below, which is what
// lets the handlers live outside main.cpp and be driven by tests.

struct AuthWarmup;
struct HarnessEventPump;

struct AppAuth {
  Arena* Mem;
  StrView Path;
  AuthTokenCache Cache;
  AuthWarmup* Warmup;
};

struct AuthWarmup {
  Thread Worker;
  Arena ResultMem;
  ResolvedAuthToken Result;
  StrView Path;
  StrView Provider;
  const char* EnvVar;
  volatile i32* Abort;
  bool Resolved;
  bool Started;
  bool Joined;
};

// Human-facing consumer of the ordered watch stream. The pump owns event
// ordering and tool-completion dedup; consumers just render what arrives.
struct HarnessEventSink {
  void* User;
  void (*OnEvent)(void* user, const AgentHarnessEvent* ev);
};

inline void HarnessEventSink_Emit(const HarnessEventSink& sink,
                                  const AgentHarnessEvent* ev) {
  if (sink.OnEvent) sink.OnEvent(sink.User, ev);
}

struct InteractiveRuntime {
  Arena* Mem;
  // Scratch for one slash command's user-visible result. Reset at the start of
  // every dispatch, so a command that renders a large report (`/tree` on a big
  // session is tens of KB) costs that once rather than once per invocation for
  // the life of the process. Only values consumed before the next dispatch may
  // live here; anything installed on the runtime or the harness belongs on Mem.
  Arena CommandMem;
  StrView AuthPath;
  StrView SettingsPath;
  const ProviderSpec* Spec;
  Model CurrentModel;
  ModelRequestProfile PreferredProfile;
  AgentHarness* Harness;
  SessionRepository* Sessions;
  // Concrete backend, when there is one, purely so a session switch can report
  // the new durable log path to hooks. Null under `--no-session`.
  JsonlRepository* JsonlSessions;
  StrView ProjectKey;
  StrView WorkingDirectory;
  StrView AgentDirectory;
  StrView TrustPath;
  bool NoSession;
  bool ProjectTrusted;
  bool HasProjectResources;
  bool TrustCoversProjectResources;
  ProjectTrustOverride TrustOverride;
  DefaultProjectTrust DefaultTrust;
  PromptContext* Prompt;
  PromptContextOptions PromptOptions;
  HarnessEventPump* EventPump;
  HarnessEventSink EventSink;
  bool AutoOpen;
  Tui* TuiView;
  AppAuth* Auth;
  AuthWarmup* AuthWarmupState;
  // Where the shell tool writes overflow logs; repointed on session switch.
  ShellToolContext* Shell;
};

// ---- process-wide session state -----------------------------------------
// Exactly one session runs per process, and every reporting site -- startup,
// the prompt choke point, and the event pump thread -- would otherwise have to
// thread the same pointers through unrelated structs.

extern volatile i32 g_abort;
extern AgentHarness* g_activeHarness;
extern HookRunner* g_hooks;

// Runs a prompt with the harness published for the signal handler to cancel.
SessionResult RunHarnessPrompt(AgentHarness* harness, StrView prompt);

// Idempotent, so every exit path can call it without tracking who got there
// first.
void EndHookSession();

// ---- command UX ----------------------------------------------------------

void PlainCommandNotice(void* user, StrView message);
void PlainCommandError(void* user, StrView message);
void TuiCommandNotice(void* user, StrView message);
void TuiCommandError(void* user, StrView message);
void EmitCommandNotice(const AppCommandUx* ux, StrView message);
void EmitCommandError(const AppCommandUx* ux, StrView message);

StrView TrimAscii(StrView value);

enum class RuntimeInputResult : u8 { Confirmed, Back, Cancelled };
enum class RuntimeChoiceStatus : u8 { Selected, Back, Cancelled, Failed };

RuntimeInputResult RuntimeSelect(InteractiveRuntime* rt, StrView title,
                                 const StrView* options, u32 optionCount,
                                 u32* selected, u32 initialSelection = 0);
RuntimeInputResult RuntimeReadText(InteractiveRuntime* rt, Arena* arena,
                                   StrView title, StrView prompt,
                                   StrView initial, StrView* out);

// Long-running command work reports through these so the TUI can show a
// cancellable operation and the plain REPL can fall back to printed notices.
volatile i32* RuntimeBeginOperation(InteractiveRuntime* rt, const AppCommandUx* ux,
                                    StrView label);
void RuntimeSetOperationStatus(InteractiveRuntime* rt, const AppCommandUx* ux,
                               StrView status);
bool RuntimeEndOperation(InteractiveRuntime* rt);

AppCommandHandlerResult CommandResult(SessionResult result, StrView successMessage);
AppCommandHandlerResult RuntimeOwnedFailure(InteractiveRuntime* rt, StrView message);
AppCommandHandlerResult ReturnToCommandPalette(InteractiveRuntime* rt);

// Zero-initializes `rt` and creates the arenas it owns. Every InteractiveRuntime
// must go through this: command results allocate from CommandMem, and an arena
// that was never created traps on its first allocation rather than failing.
void InteractiveRuntime_Init(InteractiveRuntime* rt, Arena* mem);
void InteractiveRuntime_Release(InteractiveRuntime* rt);

// Dispatches one slash command, recycling the previous command's result
// storage first. Every caller must go through this rather than
// AppCommand_Dispatch so CommandMem cannot be left to grow.
AppCommandDispatchResult RuntimeDispatchCommand(InteractiveRuntime* rt, StrView input,
                                                const AppCommandHandlers* handlers,
                                                const AppCommandUx* ux);

// ---- prompt context reporting -------------------------------------------

StrView PromptContextReport(Arena* arena, const PromptContext* context,
                            bool hasProjectResources, bool projectTrusted,
                            StrView heading);
void EmitPromptDiagnostics(const PromptContext* context, const AppCommandUx* ux);
// Startup path, before any renderer exists.
void PrintPromptDiagnostics(const PromptContext* context);
