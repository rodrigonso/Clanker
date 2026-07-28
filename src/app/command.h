#pragma once
#include "base/str.h"

enum class AppCommandKind : u8 {
  NotCommand,
  Login,
  Model,
  Clear,
  Session,
  Sessions,
  Resume,
  Tree,
  Checkout,
  Fork,
  Compact,
  Recover,
  Trust,
  Reload,
  Exit,
  Unknown,
};

struct AppCommandSpec {
  AppCommandKind Kind;
  StrView Name;
  StrView Alias;
  StrView Description;
};

u32 AppCommandSpec_Count();
const AppCommandSpec* AppCommandSpec_At(u32 index);

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
typedef AppCommandHandlerResult (*AppCommandArgHandlerFn)(
    void* user, const AppCommandUx* ux, StrView arguments);

struct AppCommandHandlers {
  void* User;
  AppCommandHandlerFn Login;
  AppCommandHandlerFn Model;
  AppCommandArgHandlerFn Session;
  AppCommandArgHandlerFn Sessions;
  AppCommandArgHandlerFn Resume;
  AppCommandArgHandlerFn Tree;
  AppCommandArgHandlerFn Checkout;
  AppCommandArgHandlerFn Fork;
  AppCommandArgHandlerFn Compact;
  AppCommandArgHandlerFn Recover;
  AppCommandArgHandlerFn Trust;
  AppCommandArgHandlerFn Reload;
};

struct AppCommandDispatchResult {
  // False only for ordinary prompt text. Every slash-prefixed input, including
  // malformed or unknown commands, is handled and must not be sent to the model.
  bool Handled;
  AppLoopAction Action;
};

AppCommandDispatchResult AppCommand_Dispatch(StrView input, const AppCommandHandlers* handlers,
                                             const AppCommandUx* ux);
