#include "app/hook_extension.h"

#include <stdio.h>

namespace {

void BeforeTool(void* user, Arena* arena, StrView toolName, StrView toolCallId,
                const ToolArgs* args, AgentToolGateDecision* out) {
  HookGate_BeforeTool(((HookExtension*)user)->Gate, arena, toolName, toolCallId,
                      args, out);
}

void SessionChanged(void* user, const AppSessionContext* session) {
  HookExtension* hooks = (HookExtension*)user;
  HookSessionInfo info = {};
  info.SessionId = session->SessionId;
  info.WorkingDirectory = session->WorkingDirectory;
  info.TranscriptPath = session->TranscriptPath;
  info.Interactive = session->Interactive;
  HookGate_SetSession(hooks->Gate, &info);
  if (!hooks->Runner) return;
  HookRunner_SetSession(hooks->Runner, &info);
  // Announced as a fresh start on every switch, so a consumer tracks the new
  // session rather than silently reattributing it to the old one.
  HookRunner_Dispatch(hooks->Runner, HookEvent::SessionStart, StrView{});
}

// Dispatched from ordered event delivery, never around Run on the caller:
// Stop must follow the run's last PostToolUse. PreToolUse belongs to the gate.
void OnEvent(void* user, const AgentHarnessEvent* event) {
  HookRunner* runner = ((HookExtension*)user)->Runner;
  switch (event->Kind) {
    case AgentHarnessEventKind::TransientToolEnd:
      HookRunner_Dispatch(runner, HookEvent::PostToolUse, event->Tool.Name);
      break;
    case AgentHarnessEventKind::TransientAgentStart:
      if (event->Turn.Kind == AgentTurnKind::Run)
        HookRunner_Dispatch(runner, HookEvent::UserPromptSubmit, StrView{});
      break;
    case AgentHarnessEventKind::TransientAgentEnd:
      if (event->Turn.Kind == AgentTurnKind::Run)
        HookRunner_Dispatch(runner,
                            event->Turn.Outcome == AgentTurnOutcome::Failed
                                ? HookEvent::StopFailure
                                : HookEvent::Stop,
                            StrView{});
      break;
    default:
      break;
  }
}

void Shutdown(void* user) {
  HookExtension* hooks = (HookExtension*)user;
  if (!hooks->Runner) return;
  HookRunner_Shutdown(hooks->Runner, HookEvent::SessionEnd);
  hooks->Runner = nullptr;
}

}  // namespace

void HookExtension_Init(HookExtension* hooks, Arena* arena, StrView path,
                        bool durable, bool interactive, volatile i32* abort) {
  *hooks = HookExtension{};
  HookConfigLoadResult loaded = HookConfig_Load(arena, path, &hooks->Config);
  if (loaded == HookConfigLoadResult::Malformed ||
      loaded == HookConfigLoadResult::ReadFailure) {
    fprintf(stderr, "warning: %s %.*s; hooks are disabled\n",
            loaded == HookConfigLoadResult::Malformed
                ? "hooks file is malformed:"
                : "could not read hooks file:",
            (int)path.Length, path.Data);
    hooks->Config = HookConfig{};
  }
  // Identity arrives with the first SessionChanged, before any event fires.
  HookSessionInfo unannounced = {};
  unannounced.Interactive = interactive;
  hooks->Gate = HookGate_Create(arena, &hooks->Config, &unannounced, abort);
  if (durable) hooks->Runner = HookRunner_Create(arena, &hooks->Config, &unannounced);
}

AppExtension HookExtension_Make(HookExtension* hooks) {
  AppExtension extension = {};
  extension.User = hooks;
  if (hooks->Gate) extension.Hooks.BeforeTool = BeforeTool;
  extension.SessionChanged = SessionChanged;
  if (hooks->Runner) {
    extension.OnEvent = OnEvent;
    extension.Shutdown = Shutdown;
  }
  return extension;
}
