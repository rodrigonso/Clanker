#include "app/command.h"

namespace {

const AppCommandSpec kCommands[] = {
    {AppCommandKind::Login, SVL("login"), {}, SVL("Sign in and choose a provider")},
    {AppCommandKind::Model, SVL("model"), {}, SVL("Change the active provider and model")},
    {AppCommandKind::Clear, SVL("clear"), {}, SVL("Clear the current conversation")},
    {AppCommandKind::Exit, SVL("exit"), SVL("quit"),
     SVL("Exit Clank and return to the shell (alias: /quit)")},
};

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

StrView Trim(StrView s) {
  u32 first = 0;
  while (first < s.Length && IsSpace(s.Data[first])) first++;
  u32 last = s.Length;
  while (last > first && IsSpace(s.Data[last - 1])) last--;
  return StrView{s.Data ? s.Data + first : nullptr, last - first};
}

void Notice(const AppCommandUx* ux, StrView message) {
  if (message.Length && ux && ux->Notice) ux->Notice(ux->User, message);
}

void Error(const AppCommandUx* ux, StrView message) {
  if (message.Length && ux && ux->Error) ux->Error(ux->User, message);
}

StrView Usage(AppCommandKind kind) {
  switch (kind) {
    case AppCommandKind::Login:
      return SVL("Usage: /login");
    case AppCommandKind::Model:
      return SVL("Usage: /model");
    case AppCommandKind::Clear:
      return SVL("Usage: /clear");
    case AppCommandKind::Exit:
      return SVL("Usage: /exit (alias: /quit)");
    default:
      return StrView{};
  }
}

AppCommandHandlerResult RunHandler(AppCommandHandlerFn fn, void* user, const AppCommandUx* ux,
                                   StrView unsupported) {
  if (!fn) return AppCommandHandlerResult{AppCommandHandlerStatus::Unsupported, unsupported};
  return fn(user, ux);
}

void ReportHandlerResult(const AppCommandUx* ux, AppCommandKind kind, AppCommandHandlerResult result) {
  if (result.Message.Length) {
    if (result.Status == AppCommandHandlerStatus::Success ||
        result.Status == AppCommandHandlerStatus::Cancelled)
      Notice(ux, result.Message);
    else
      Error(ux, result.Message);
    return;
  }

  if (result.Status == AppCommandHandlerStatus::Cancelled) {
    Notice(ux, kind == AppCommandKind::Login ? SVL("Login cancelled.") : SVL("Model selection cancelled."));
  } else if (result.Status == AppCommandHandlerStatus::Unsupported) {
    Error(ux, kind == AppCommandKind::Login ? SVL("/login is unavailable in this mode.")
                                            : SVL("/model is unavailable in this mode."));
  } else if (result.Status == AppCommandHandlerStatus::Failed) {
    Error(ux, kind == AppCommandKind::Login ? SVL("Login failed.") : SVL("Model selection failed."));
  }
}

}  // namespace

u32 AppCommandSpec_Count() { return sizeof(kCommands) / sizeof(kCommands[0]); }

const AppCommandSpec* AppCommandSpec_At(u32 index) {
  return index < AppCommandSpec_Count() ? &kCommands[index] : nullptr;
}

AppCommand AppCommand_Parse(StrView input) {
  StrView line = Trim(input);
  if (!line.Length || line.Data[0] != '/')
    return AppCommand{AppCommandKind::NotCommand, StrView{}};

  u32 nameEnd = 1;
  while (nameEnd < line.Length && !IsSpace(line.Data[nameEnd])) nameEnd++;
  StrView name{line.Data + 1, nameEnd - 1};
  StrView arguments = Trim(StrView{line.Data + nameEnd, line.Length - nameEnd});

  AppCommandKind kind = AppCommandKind::Unknown;
  for (u32 i = 0; i < AppCommandSpec_Count(); i++) {
    const AppCommandSpec* spec = AppCommandSpec_At(i);
    if (StrView_Eq(name, spec->Name) ||
        (spec->Alias.Length && StrView_Eq(name, spec->Alias))) {
      kind = spec->Kind;
      break;
    }
  }

  return AppCommand{kind, arguments};
}

AppCommandDispatchResult AppCommand_Dispatch(StrView input, const AppCommandHandlers* handlers,
                                             const AppCommandUx* ux) {
  AppCommand command = AppCommand_Parse(input);
  if (command.Kind == AppCommandKind::NotCommand)
    return AppCommandDispatchResult{false, AppLoopAction::Continue};

  if (command.Kind == AppCommandKind::Unknown) {
    Error(ux, SVL("Unknown slash command. Use /login, /model, /clear, or /exit."));
    return AppCommandDispatchResult{true, AppLoopAction::Continue};
  }

  if (command.Arguments.Length) {
    Error(ux, Usage(command.Kind));
    return AppCommandDispatchResult{true, AppLoopAction::Continue};
  }

  if (command.Kind == AppCommandKind::Clear)
    return AppCommandDispatchResult{true, AppLoopAction::ResetSession};
  if (command.Kind == AppCommandKind::Exit)
    return AppCommandDispatchResult{true, AppLoopAction::Exit};

  AppCommandHandlerFn fn = nullptr;
  if (handlers) fn = command.Kind == AppCommandKind::Login ? handlers->Login : handlers->Model;
  AppCommandHandlerResult result =
      RunHandler(fn, handlers ? handlers->User : nullptr, ux,
                 command.Kind == AppCommandKind::Login ? SVL("/login is unavailable in this mode.")
                                                       : SVL("/model is unavailable in this mode."));
  ReportHandlerResult(ux, command.Kind, result);
  return AppCommandDispatchResult{true, AppLoopAction::Continue};
}
