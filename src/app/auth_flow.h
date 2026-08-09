#pragma once

#include "app/runtime.h"

// Credential resolution and the interactive provider/model pickers built on it.
//
// Auth resolution has a warmup path because the first prompt of a session would
// otherwise block on a token refresh: StartAuthWarmup resolves in the
// background while the user is still typing, and the prompt choke point joins
// it. Everything here is renderer-neutral and drives the TUI or the plain REPL
// through the Runtime* seams.

struct RuntimeSelection {
  const ProviderSpec* Spec;
  StrView ModelId;
  ModelRequestProfile RequestedProfile;
  DefaultProjectTrust DefaultTrust;
  AgentCompactionPolicy Compaction;
  AgentRetryPolicy Retry;
  bool Rounded = true;
  Array<StrView> Skills;
  Array<StrView> PromptTemplates;
};

void AppAuth_Invalidate(AppAuth* auth);

// AgentResolveAuthFn for AgentHarnessConfig; `user` is an AppAuth*.
bool AppResolveAuth(void* user, Arena* arena, StrView provider, volatile i32* abort,
                    ProviderAuth* out);

// Resolves credentials before a prompt is accepted, so a failure is reported as
// a command error rather than a mid-turn provider fault.
bool ResolvePromptAuth(AgentHarness* harness, volatile i32* abort);

// Human-readable "how to authenticate this provider" text.
StrView AuthGuidance(Arena* arena, const ProviderSpec* spec, bool interactive);

RuntimeSelection LoadRuntimeSelection(Arena* arena, StrView settingsPath,
                                      const char* modelOverride);

bool StartAuthWarmup(AuthWarmup* warmup, AppAuth* auth, const ProviderSpec* spec,
                     volatile i32* abort);
void FinishRuntimeAuthWarmup(InteractiveRuntime* rt);
void CancelRuntimeAuthWarmup(InteractiveRuntime* rt);

// Reports whether the active provider has usable credentials, as a notice or an
// error depending on `asError`.
bool RuntimeCheckAuth(InteractiveRuntime* rt, const AppCommandUx* ux, bool asError);

// Installs a provider/model selection on the live harness and persists it.
bool ActivateRuntimeSelection(InteractiveRuntime* rt, const ProviderSpec* spec,
                              const Model& model, ModelRequestProfile preference,
                              const AppCommandUx* ux, bool* persisted = nullptr);
StrView SelectionMessage(InteractiveRuntime* rt, const char* prefix);

AppCommandHandlerResult HandleModelCommand(void* user, const AppCommandUx* ux);
AppCommandHandlerResult HandleLoginCommand(void* user, const AppCommandUx* ux);

// `clank --login <provider>` / `--logout <provider>`: one-shot, non-interactive
// entry points that reuse the same OAuth flow the slash command drives.
int RunCommandLineLogin(Arena* arena, StrView authPath, const char* providerId,
                        bool autoOpen);
int RunCommandLineLogout(Arena* arena, StrView authPath, const char* providerId);
