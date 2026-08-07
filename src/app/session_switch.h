#pragma once

#include "app/runtime.h"

// Switching the live harness onto a different session log, and the project-trust
// decisions that gate it.
//
// A switch is not a fresh start: the event pump, shell tool, prompt context and
// trust state all point at the old session and have to be moved together, and
// any step can fail with the harness already half-moved. RuntimeSwitchToLog owns
// that sequencing and rolls back rather than leaving a mixed state.

// One project-trust menu, shared by startup, `/trust`, and cross-project
// resume. The session-only choices apply to the running process instead of
// being written to the trust store.
enum class TrustChoice : u8 {
  TrustCurrent,
  TrustParent,
  TrustSession,
  UntrustCurrent,
  UntrustSession,
};

constexpr u32 TRUST_CHOICE_CAPACITY = 5;

struct TrustDecision {
  bool Trusted;
  bool Persist;  // false for the session-only choices
  StrView DecisionPath;
  StrView RemovePath;  // a parent-scoped decision supersedes any child entry
};

struct SessionOptionList {
  SessionInfo** Sessions;
  StrView* Options;
  u32 Count;
};

// Writes the menu into `outOptions`/`outChoices` (each at least
// TRUST_CHOICE_CAPACITY long) and returns the number of entries. `/trust`
// exists to persist a decision, so it omits the session-only choices.
u32 BuildTrustChoices(Arena* arena, StrView parent, bool includeSessionOnly,
                      StrView* outOptions, TrustChoice* outChoices);
TrustDecision ResolveTrustChoice(TrustChoice choice, StrView cwd, StrView parent);

SessionResult ResolveRuntimeProjectTrust(InteractiveRuntime* rt, Arena* scratch,
                                         StrView workingDirectory, bool* trusted,
                                         bool* hasResources);

// Startup trust prompt. Returns false when the user declined to continue.
bool ResolveStartupProjectTrust(Arena* arena, StrView trustPath,
                                StrView workingDirectory,
                                DefaultProjectTrust defaultTrust,
                                ProjectTrustOverride overrideValue, bool interactive,
                                bool* trusted, bool* hasResources);

// Restarts the watch/event pump against the harness's current session.
bool RuntimeRestartWatch(InteractiveRuntime* rt, const AppCommandUx* ux);
void RuntimeUpdateShellDirectory(InteractiveRuntime* rt, StrView projectKey,
                                 DurableId sessionId);
bool RuntimeRestorePreviousSession(InteractiveRuntime* rt, DurableId previousSessionId,
                                   const AgentHarnessConfig* config,
                                   const AppCommandUx* ux);
bool RuntimeRollbackSessionSwitch(InteractiveRuntime* rt,
                                  StrView previousWorkingDirectory,
                                  DurableId previousSessionId,
                                  const AgentHarnessConfig* previousConfig,
                                  const AppCommandUx* ux);
SessionResult RuntimeSwitchToLog(InteractiveRuntime* rt, SessionLog target,
                                 const SessionInfo* targetInfo,
                                 const SessionReduction* targetState,
                                 const AppCommandUx* ux);

// ---- session listing / formatting ---------------------------------------

StrView SessionTitle(const SessionInfo* info);
StrView SessionStatusText(const SessionInfo* info);
Array<StrView> SessionDetailLines(Arena* arena, const SessionInfo* info,
                                  StrView storage, StrView status);
StrView FormatSessionOption(Arena* arena, const SessionInfo* info,
                            DurableId currentSessionId, bool allProjects,
                            bool includeFullId, u64 nowMs, StrView home);
SessionOptionList BuildSessionOptions(Arena* arena, Array<SessionInfo>* items,
                                      DurableId currentSessionId, bool allProjects,
                                      bool includeFullId, bool availableOnly,
                                      u64 nowMs, StrView home);
SessionResult CurrentSessionId(Arena* arena, InteractiveRuntime* rt, DurableId* out);

// ---- session storage paths ----------------------------------------------

StrView SessionDataRoot(Arena* arena);
StrView SessionProjectKey(Arena* arena, StrView workingDirectory);
