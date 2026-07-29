#include "app/command.h"

namespace {

const AppCommandSpec kCommands[] = {
    {AppCommandKind::Login, SVL("login"), {},
     SVL("Sign in and choose a provider"), SVL("Usage: /login"),
     AppCommandArgumentPolicy::None},
    {AppCommandKind::Model, SVL("model"), {},
     SVL("Change the active model, effort, and context"), SVL("Usage: /model"),
     AppCommandArgumentPolicy::None},
    {AppCommandKind::Clear, SVL("clear"), {}, SVL("Start a new session"),
     SVL("Usage: /clear"), AppCommandArgumentPolicy::None},
    {AppCommandKind::Session, SVL("session"), {},
     SVL("Show the current session"), SVL("Usage: /session"),
     AppCommandArgumentPolicy::None},
    {AppCommandKind::Name, SVL("name"), {},
     SVL("Set or clear the session name"),
     SVL("Usage: /name <title|--clear>"), AppCommandArgumentPolicy::Optional},
    {AppCommandKind::Sessions, SVL("sessions"), {},
     SVL("List current or all sessions"), SVL("Usage: /sessions [all]"),
     AppCommandArgumentPolicy::Optional},
    {AppCommandKind::Resume, SVL("resume"), {},
     SVL("Choose a session to resume"), SVL("Usage: /resume [session-id]"),
     AppCommandArgumentPolicy::Optional},
    {AppCommandKind::Tree, SVL("tree"), {},
     SVL("Show the conversation tree"), SVL("Usage: /tree"),
     AppCommandArgumentPolicy::None},
    {AppCommandKind::Checkout, SVL("checkout"), {},
     SVL("Move to a tree entry"), SVL("Usage: /checkout <entry-id>"),
     AppCommandArgumentPolicy::Required},
    {AppCommandKind::Fork, SVL("fork"), {},
     SVL("Fork the current or selected entry"), SVL("Usage: /fork [entry-id]"),
     AppCommandArgumentPolicy::Optional},
    {AppCommandKind::Compact, SVL("compact"), {},
     SVL("Compact the current branch"), SVL("Usage: /compact"),
     AppCommandArgumentPolicy::None},
    {AppCommandKind::Recover, SVL("recover"), {},
     SVL("Recover a suspended operation"),
     SVL("Usage: /recover <resume|abort|tool ACTION [result]>"),
     AppCommandArgumentPolicy::Required},
    {AppCommandKind::Trust, SVL("trust"), {},
     SVL("Change saved project trust"), SVL("Usage: /trust"),
     AppCommandArgumentPolicy::None},
    {AppCommandKind::Reload, SVL("reload"), {},
     SVL("Reload settings and prompt context"), SVL("Usage: /reload"),
     AppCommandArgumentPolicy::None},
    {AppCommandKind::Exit, SVL("exit"), SVL("quit"),
     SVL("Exit Clank and return to the shell (alias: /quit)"),
     SVL("Usage: /exit (alias: /quit)"), AppCommandArgumentPolicy::None},
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

const AppCommandSpec* FindSpec(AppCommandKind kind) {
  for (u32 i = 0; i < AppCommandSpec_Count(); i++)
    if (kCommands[i].Kind == kind) return &kCommands[i];
  return nullptr;
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

StrView AppCommand_Usage(AppCommandKind kind) {
  const AppCommandSpec* spec = FindSpec(kind);
  return spec ? spec->Usage : StrView{};
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

  const AppCommandSpec* spec = FindSpec(command.Kind);
  bool invalidArguments =
      !spec ||
      (spec->Arguments == AppCommandArgumentPolicy::None &&
       command.Arguments.Length) ||
      (spec->Arguments == AppCommandArgumentPolicy::Required &&
       !command.Arguments.Length);
  if (invalidArguments) {
    Error(ux, spec ? spec->Usage : StrView{});
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
        case AppCommandKind::Name: fn = handlers->Name; break;
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
