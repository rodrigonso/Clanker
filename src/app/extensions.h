#pragma once

#include "agent/harness_event.h"
#include "agent/hook_chain.h"
#include "base/str.h"

// Features that attach to the agent -- the hooks.json policy, the memory wiki,
// the shell tool's artifact directory -- as self-contained extensions. Each
// owns its harness hooks and also hears the app lifecycle those hooks cannot
// see: which durable session is live, the ordered event stream, and exit.
// One registry per process replaces per-feature globals and the hand-written
// re-pointing a session switch used to do for each of them.

// Identity of the live session. Views are only valid during the call, so an
// extension copies whatever it keeps.
struct AppSessionContext {
  // Empty under `--no-session`, along with the transcript and artifact paths.
  StrView SessionId;
  StrView ProjectKey;
  StrView WorkingDirectory;
  StrView TranscriptPath;
  StrView ArtifactDirectory;
  // Whether a user is present to answer a prompt.
  bool Interactive;
};

// Every callback is optional and receives `User`.
struct AppExtension {
  void* User;
  // `Hooks.User` is ignored; the hooks receive `User` like everything else.
  AgentHarnessHooks Hooks;
  // Main thread, with no run active: once at startup and after every session
  // switch (`/clear`, `/resume`).
  void (*SessionChanged)(void* user, const AppSessionContext* session);
  // The event delivery thread, in the order the agent produced the events.
  void (*OnEvent)(void* user, const AgentHarnessEvent* event);
  // Once, at exit, in registration order.
  void (*Shutdown)(void* user);
};

constexpr u32 APP_EXTENSION_CAPACITY = AGENT_HOOK_CHAIN_CAPACITY;

struct AppExtensions {
  AppExtension Items[APP_EXTENSION_CAPACITY];
  u32 Count;
  AgentHookChain Chain;
  bool ShutDown;
};

// False when the registry is full.
bool AppExtensions_Add(AppExtensions* extensions, const AppExtension* extension);

// The composed hooks for AgentHarnessConfig::Hooks. `extensions` must outlive
// every harness configured with them.
AgentHarnessHooks AppExtensions_Hooks(AppExtensions* extensions);

// Each is a no-op on a null registry, so call sites need no guard.
void AppExtensions_SessionChanged(AppExtensions* extensions,
                                  const AppSessionContext* session);
void AppExtensions_OnEvent(AppExtensions* extensions, const AgentHarnessEvent* event);
// Idempotent, so every exit path can call it.
void AppExtensions_Shutdown(AppExtensions* extensions);
