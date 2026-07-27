#pragma once
#include "base/str.h"

enum class AppCommandKind : u8 {
  NotCommand,
  Login,
  Model,
  Clear,
  Exit,
  Unknown,
};

struct AppCommand {
  AppCommandKind Kind;
  StrView Arguments;
};

// Parses an input line without allocating. Leading/trailing ASCII whitespace is
// ignored, and Arguments excludes whitespace surrounding the argument text.
AppCommand AppCommand_Parse(StrView input);

enum class AppLoopAction : u8 {
  Continue,
  ResetSession,
  Exit,
};

enum class AppCommandHandlerStatus : u8 {
  Success,
  Cancelled,
  Unsupported,
  Failed,
};

struct AppCommandHandlerResult {
  AppCommandHandlerStatus Status;
  // Optional renderer-neutral detail. The dispatcher reports it as a notice for
  // Success/Cancelled and as an error for Unsupported/Failed.
  StrView Message;
};

struct AppCommandUx {
  void* User;
  void (*Notice)(void* user, StrView message);
  void (*Error)(void* user, StrView message);
};

typedef AppCommandHandlerResult (*AppCommandHandlerFn)(void* user, const AppCommandUx* ux);

struct AppCommandHandlers {
  void* User;
  AppCommandHandlerFn Login;
  AppCommandHandlerFn Model;
};

struct AppCommandDispatchResult {
  // False only for ordinary prompt text. Every slash-prefixed input, including
  // malformed or unknown commands, is handled and must not be sent to the model.
  bool Handled;
  AppLoopAction Action;
};

AppCommandDispatchResult AppCommand_Dispatch(StrView input, const AppCommandHandlers* handlers,
                                             const AppCommandUx* ux);
