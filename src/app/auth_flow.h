#pragma once

#include "app/runtime.h"
#include "app/settings.h"

// Credential resolution and the interactive provider/model pickers built on it.
// Everything here is renderer-neutral and drives the TUI or the plain REPL
// through the Runtime* seams.

// Startup configuration: the settings file, or its defaults when the file is
// missing or unusable, plus the provider and model they resolve to.
struct RuntimeSelection {
  const ProviderSpec* Spec;
  // The configured model, or the command-line override.
  StrView ModelId;
  // Profile is cleared when the configured provider is unknown; on a
  // malformed file only the reviewer diagnostics are kept.
  AppSettings Settings;
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

// Reports whether the active provider has usable credentials, as a notice or an
// error depending on `asError`.
bool RuntimeCheckAuth(InteractiveRuntime* rt, const AppCommandUx* ux, bool asError);

AppCommandHandlerResult RuntimeEnterApiKey(
    InteractiveRuntime* rt, StrView credentialId, StrView title,
    StrView category, const AppCommandUx* ux);

// Installs a provider/model selection on the live harness and persists it.
bool ActivateRuntimeSelection(InteractiveRuntime* rt, const ProviderSpec* spec,
                              const Model& model, ModelRequestProfile preference,
                              const AppCommandUx* ux, bool* persisted = nullptr);
StrView SelectionMessage(InteractiveRuntime* rt, const char* prefix);

AppCommandHandlerResult HandleModelCommand(void* user, const AppCommandUx* ux);
AppCommandHandlerResult HandleAuthCommand(void* user, const AppCommandUx* ux);

// `clanker --logout <provider>`: removes the stored credential without entering
// the TUI.
int RunCommandLineLogout(Arena* arena, StrView authPath, const char* providerId);
