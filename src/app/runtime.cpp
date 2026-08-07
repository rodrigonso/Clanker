#include "app/runtime.h"

#include <stdio.h>

#include "app/plain_select.h"

volatile i32 g_abort = 0;
AgentHarness* g_activeHarness = nullptr;
HookRunner* g_hooks = nullptr;

SessionResult RunHarnessPrompt(AgentHarness* harness, StrView prompt) {
  g_activeHarness = harness;
  SessionResult result = AgentHarness_Run(harness, prompt);
  g_activeHarness = nullptr;
  return result;
}

void EndHookSession() {
  if (!g_hooks) return;
  HookRunner_Shutdown(g_hooks, HookEvent::SessionEnd);
  g_hooks = nullptr;
}

void PlainCommandNotice(void*, StrView message) {
  printf("%.*s\n", (int)message.Length, message.Data);
}

void PlainCommandError(void*, StrView message) {
  fprintf(stderr, "error: %.*s\n", (int)message.Length, message.Data);
}

void TuiCommandNotice(void* user, StrView message) {
  Tui_CommandNotice((Tui*)user, message);
}

void TuiCommandError(void* user, StrView message) {
  Tui_CommandError((Tui*)user, message);
}

void EmitCommandNotice(const AppCommandUx* ux, StrView message) {
  if (message.Length && ux && ux->Notice) ux->Notice(ux->User, message);
}

void EmitCommandError(const AppCommandUx* ux, StrView message) {
  if (message.Length && ux && ux->Error) ux->Error(ux->User, message);
}

static bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

StrView TrimAscii(StrView value) {
  u32 first = 0;
  while (first < value.Length && IsAsciiSpace(value.Data[first])) first++;
  u32 last = value.Length;
  while (last > first && IsAsciiSpace(value.Data[last - 1])) last--;
  return StrView{value.Data ? value.Data + first : nullptr, last - first};
}

RuntimeInputResult RuntimeSelect(InteractiveRuntime* rt, StrView title,
                                 const StrView* options, u32 optionCount,
                                 u32* selected, u32 initialSelection) {
  if (rt->TuiView) {
    TuiModalResult result =
        Tui_Select(rt->TuiView, title, options, optionCount, selected,
                   initialSelection);
    if (result == TuiModalResult::Confirmed)
      return RuntimeInputResult::Confirmed;
    if (result == TuiModalResult::Back) return RuntimeInputResult::Back;
    return RuntimeInputResult::Cancelled;
  }
  return Plain_Select(title, options, optionCount, selected)
             ? RuntimeInputResult::Confirmed
             : RuntimeInputResult::Cancelled;
}

RuntimeInputResult RuntimeReadText(InteractiveRuntime* rt, Arena* arena,
                                   StrView title, StrView prompt,
                                   StrView initial, StrView* out) {
  if (rt->TuiView) {
    StrView value = {};
    TuiModalResult result =
        Tui_ReadText(rt->TuiView, title, prompt, initial, &value);
    if (result == TuiModalResult::Back) return RuntimeInputResult::Back;
    if (result != TuiModalResult::Confirmed)
      return RuntimeInputResult::Cancelled;
    *out = StrView_Dup(arena, value);
    return RuntimeInputResult::Confirmed;
  }

  char buffer[1024];
  StrView value = {};
  if (!Plain_ReadText(prompt, buffer, (u32)sizeof(buffer), &value))
    return RuntimeInputResult::Cancelled;
  *out = StrView_Dup(arena, value);
  return RuntimeInputResult::Confirmed;
}

volatile i32* RuntimeBeginOperation(InteractiveRuntime* rt, const AppCommandUx* ux,
                                    StrView label) {
  g_abort = 0;
  if (rt->TuiView) {
    Tui_BeginOperation(rt->TuiView, label);
    return Tui_OperationAbort(rt->TuiView);
  }
  EmitCommandNotice(ux, label);
  return &g_abort;
}

void RuntimeSetOperationStatus(InteractiveRuntime* rt, const AppCommandUx* ux,
                               StrView status) {
  if (rt->TuiView)
    Tui_SetOperationStatus(rt->TuiView, status);
  else
    EmitCommandNotice(ux, status);
}

bool RuntimeEndOperation(InteractiveRuntime* rt) {
  if (rt->TuiView) return Tui_EndOperation(rt->TuiView);
  return g_abort != 0;
}

AppCommandHandlerResult CommandResult(SessionResult result, StrView successMessage) {
  return AppCommandHandlerResult{
      SessionResult_IsOk(result) ? AppCommandHandlerStatus::Success
                                 : AppCommandHandlerStatus::Failed,
      SessionResult_IsOk(result) ? successMessage : result.Message};
}

AppCommandHandlerResult RuntimeOwnedFailure(InteractiveRuntime* rt, StrView message) {
  return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                 StrView_Dup(rt->Mem, message)};
}

AppCommandHandlerResult ReturnToCommandPalette(InteractiveRuntime* rt) {
  if (rt->TuiView) Tui_OpenCommandPalette(rt->TuiView);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success, StrView{}};
}

StrView PromptContextReport(Arena* arena, const PromptContext* context,
                                   bool hasProjectResources,
                                   bool projectTrusted, StrView heading) {
  StrBuf message = StrBuf_Make(arena, 256);
  StrBuf_Append(&message, heading);
  StrBuf_AppendCStr(&message, "\nSystem prompt: ");
  StrBuf_Append(&message, context->SystemPromptSource.Length
                             ? context->SystemPromptSource
                             : SVL("built-in"));
  StrBuf_AppendCStr(&message, "\nAppend system prompt: ");
  StrBuf_Append(&message, context->AppendPromptSource.Length
                             ? context->AppendPromptSource
                             : SVL("none"));
  StrBuf_AppendCStr(&message, "\nProject prompt resources: ");
  StrBuf_AppendCStr(&message, !hasProjectResources
                                 ? "not present"
                             : projectTrusted ? "enabled"
                                              : "disabled (project untrusted)");
  if (context->ContextFiles.Count) {
    StrBuf_AppendCStr(&message, "\nProject instructions:");
    for (u32 i = 0; i < context->ContextFiles.Count; i++) {
      StrBuf_AppendCStr(&message, "\n- ");
      StrBuf_Append(&message, context->ContextFiles.Data[i].Path);
    }
  } else {
    StrBuf_AppendCStr(&message, "\nProject instructions: none");
  }
  return StrBuf_View(&message);
}

void EmitPromptDiagnostics(const PromptContext* context,
                                  const AppCommandUx* ux) {
  if (!context || !ux) return;
  Arena scratch = ArenaCreate((usize)4 << 20);
  for (u32 i = 0; i < context->Diagnostics.Count; i++) {
    const PromptDiagnostic& diagnostic = context->Diagnostics.Data[i];
    StrBuf message =
        StrBuf_Make(&scratch, diagnostic.Path.Length + diagnostic.Message.Length + 24);
    if (diagnostic.Severity == PromptDiagnosticSeverity::Warning)
      StrBuf_AppendCStr(&message, "warning: ");
    if (diagnostic.Path.Length) {
      StrBuf_Append(&message, diagnostic.Path);
      StrBuf_AppendCStr(&message, ": ");
    }
    StrBuf_Append(&message, diagnostic.Message);
    if (diagnostic.Severity == PromptDiagnosticSeverity::Error)
      EmitCommandError(ux, StrBuf_View(&message));
    else
      EmitCommandNotice(ux, StrBuf_View(&message));
    ArenaReset(&scratch);
  }
  ArenaRelease(&scratch);
}

void PrintPromptDiagnostics(const PromptContext* context) {
  if (!context) return;
  for (u32 i = 0; i < context->Diagnostics.Count; i++) {
    const PromptDiagnostic& diagnostic = context->Diagnostics.Data[i];
    fprintf(stderr, "%s%.*s%s%.*s\n",
            diagnostic.Severity == PromptDiagnosticSeverity::Warning
                ? "warning: "
                : "error: ",
            (int)diagnostic.Path.Length, diagnostic.Path.Data,
            diagnostic.Path.Length ? ": " : "", (int)diagnostic.Message.Length,
            diagnostic.Message.Data);
  }
}
