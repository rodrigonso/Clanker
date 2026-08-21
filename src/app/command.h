#pragma once
#include "base/str.h"

enum class AppCommandKind : u8 {
  NotCommand,
  Auth,
  Model,
  Clear,
  Session,
  Name,
  Sessions,
  Resume,
  Tree,
  Checkout,
  Fork,
  Compact,
  Trust,
  Permissions,
  Reload,
  Debug,
  Rounded,
  Skill,
  Exit,
  Unknown,
};

enum class AppCommandArgumentPolicy : u8 {
  None,
  Optional,
  Required,
};

struct AppCommandSpec {
  AppCommandKind Kind;
  StrView Name;
  StrView Alias;
  StrView Description;
  StrView Usage;
  AppCommandArgumentPolicy Arguments;
};

u32 AppCommandSpec_Count();
const AppCommandSpec* AppCommandSpec_At(u32 index);
StrView AppCommand_Usage(AppCommandKind kind);

struct AppCommand {
  AppCommandKind Kind;
  StrView Arguments;
  StrView SkillName;
  // Raw token after `/`, including unknown template names.
  StrView Name;
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
typedef AppCommandHandlerResult (*AppCommandSkillHandlerFn)(
    void* user, const AppCommandUx* ux, StrView name, StrView arguments);
// False leaves the command unresolved.
typedef bool (*AppCommandTemplateHandlerFn)(void* user, const AppCommandUx* ux,
                                            StrView name, StrView arguments,
                                            AppCommandHandlerResult* out);

struct AppCommandHandlers {
  void* User;
  AppCommandHandlerFn Auth;
  AppCommandHandlerFn Model;
  AppCommandArgHandlerFn Session;
  AppCommandArgHandlerFn Name;
  AppCommandArgHandlerFn Sessions;
  AppCommandArgHandlerFn Resume;
  AppCommandArgHandlerFn Tree;
  AppCommandArgHandlerFn Checkout;
  AppCommandArgHandlerFn Fork;
  AppCommandArgHandlerFn Compact;
  AppCommandArgHandlerFn Trust;
  AppCommandArgHandlerFn Permissions;
  AppCommandArgHandlerFn Reload;
  AppCommandArgHandlerFn Debug;
  AppCommandArgHandlerFn Rounded;
  AppCommandSkillHandlerFn Skill;
  AppCommandTemplateHandlerFn PromptTemplate;
};

struct AppCommandDispatchResult {
  // False only for ordinary prompt text. Every slash-prefixed input, including
  // malformed or unknown commands, is handled and must not be sent to the model.
  bool Handled;
  AppLoopAction Action;
};

AppCommandDispatchResult AppCommand_Dispatch(StrView input, const AppCommandHandlers* handlers,
                                             const AppCommandUx* ux);
