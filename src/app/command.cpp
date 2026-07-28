#include "app/command.h"

namespace {

const AppCommandSpec kCommands[] = {
    {AppCommandKind::Login, SVL("login"), {}, SVL("Sign in and choose a provider")},
    {AppCommandKind::Model, SVL("model"), {}, SVL("Change the active provider and model")},
    {AppCommandKind::Clear, SVL("clear"), {}, SVL("Start a new session")},
    {AppCommandKind::Session, SVL("session"), {}, SVL("Show the current session")},
    {AppCommandKind::Sessions, SVL("sessions"), {}, SVL("List project sessions")},
    {AppCommandKind::Resume, SVL("resume"), {}, SVL("Choose a session to resume")},
    {AppCommandKind::Tree, SVL("tree"), {}, SVL("Show the conversation tree")},
    {AppCommandKind::Checkout, SVL("checkout"), {}, SVL("Move to a tree entry")},
    {AppCommandKind::Fork, SVL("fork"), {}, SVL("Fork the current or selected entry")},
    {AppCommandKind::Compact, SVL("compact"), {}, SVL("Compact the current branch")},
    {AppCommandKind::Recover, SVL("recover"), {}, SVL("Recover a suspended operation")},
    {AppCommandKind::Trust, SVL("trust"), {}, SVL("Change saved project trust")},
    {AppCommandKind::Reload, SVL("reload"), {},
     SVL("Reload settings and prompt context")},
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
    case AppCommandKind::Session:
      return SVL("Usage: /session");
    case AppCommandKind::Sessions:
      return SVL("Usage: /sessions");
    case AppCommandKind::Resume:
      return SVL("Usage: /resume [session-id]");
    case AppCommandKind::Tree:
      return SVL("Usage: /tree");
    case AppCommandKind::Checkout:
      return SVL("Usage: /checkout <entry-id>");
    case AppCommandKind::Fork:
      return SVL("Usage: /fork [entry-id]");
    case AppCommandKind::Compact:
      return SVL("Usage: /compact");
    case AppCommandKind::Recover:
      return SVL("Usage: /recover <resume|abort|tool ACTION [result]>");
    case AppCommandKind::Trust:
      return SVL("Usage: /trust");
    case AppCommandKind::Reload:
      return SVL("Usage: /reload");
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

AppCommandHandlerResult RunArgHandler(AppCommandArgHandlerFn fn, void* user,
                                     const AppCommandUx* ux,
                                     StrView arguments) {
  if (!fn)
    return AppCommandHandlerResult{AppCommandHandlerStatus::Unsupported,
                                   SVL("This command is unavailable in this mode.")};
  return fn(user, ux, arguments);
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
    Error(ux, SVL("Unknown slash command."));
    return AppCommandDispatchResult{true, AppLoopAction::Continue};
  }

  bool requiresArguments =
      command.Kind == AppCommandKind::Checkout ||
      command.Kind == AppCommandKind::Recover;
  bool allowsArguments = requiresArguments ||
                         command.Kind == AppCommandKind::Resume ||
                         command.Kind == AppCommandKind::Fork;
  if ((!allowsArguments && command.Arguments.Length) ||
      (requiresArguments && !command.Arguments.Length)) {
    Error(ux, Usage(command.Kind));
    return AppCommandDispatchResult{true, AppLoopAction::Continue};
  }

  if (command.Kind == AppCommandKind::Clear)
    return AppCommandDispatchResult{true, AppLoopAction::ResetSession};
  if (command.Kind == AppCommandKind::Exit)
    return AppCommandDispatchResult{true, AppLoopAction::Exit};

  AppCommandHandlerResult result = {};
  if (command.Kind == AppCommandKind::Login ||
      command.Kind == AppCommandKind::Model) {
    AppCommandHandlerFn fn = nullptr;
    if (handlers)
      fn = command.Kind == AppCommandKind::Login ? handlers->Login
                                                 : handlers->Model;
    result = RunHandler(
        fn, handlers ? handlers->User : nullptr, ux,
        command.Kind == AppCommandKind::Login
            ? SVL("/login is unavailable in this mode.")
            : SVL("/model is unavailable in this mode."));
  } else {
    AppCommandArgHandlerFn fn = nullptr;
    if (handlers) {
      switch (command.Kind) {
        case AppCommandKind::Session: fn = handlers->Session; break;
        case AppCommandKind::Sessions: fn = handlers->Sessions; break;
        case AppCommandKind::Resume: fn = handlers->Resume; break;
        case AppCommandKind::Tree: fn = handlers->Tree; break;
        case AppCommandKind::Checkout: fn = handlers->Checkout; break;
        case AppCommandKind::Fork: fn = handlers->Fork; break;
        case AppCommandKind::Compact: fn = handlers->Compact; break;
        case AppCommandKind::Recover: fn = handlers->Recover; break;
        case AppCommandKind::Trust: fn = handlers->Trust; break;
        case AppCommandKind::Reload: fn = handlers->Reload; break;
        default: break;
      }
    }
    result = RunArgHandler(fn, handlers ? handlers->User : nullptr, ux,
                           command.Arguments);
  }
  ReportHandlerResult(ux, command.Kind, result);
  return AppCommandDispatchResult{true, AppLoopAction::Continue};
}
