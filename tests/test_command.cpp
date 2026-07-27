#include "app/command.h"
#include "check.h"

namespace {

struct FakeCommandContext {
  int LoginCalls;
  int ModelCalls;
  int Notices;
  int Errors;
  AppCommandHandlerResult LoginResult;
  AppCommandHandlerResult ModelResult;
  StrView LastNotice;
  StrView LastError;
};

void Notice(void* user, StrView message) {
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->Notices++;
  ctx->LastNotice = message;
}

void Error(void* user, StrView message) {
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->Errors++;
  ctx->LastError = message;
}

AppCommandHandlerResult Login(void* user, const AppCommandUx* ux) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->LoginCalls++;
  return ctx->LoginResult;
}

AppCommandHandlerResult Model(void* user, const AppCommandUx* ux) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->ModelCalls++;
  return ctx->ModelResult;
}

void TestParse() {
  CHECK_ALL(AppCommandSpec_Count() == 4,
            AppCommandSpec_At(AppCommandSpec_Count()) == nullptr,
            AppCommandSpec_At(0)->Kind == AppCommandKind::Login,
            StrView_EqCStr(AppCommandSpec_At(0)->Name, "login"),
            AppCommandSpec_At(3)->Kind == AppCommandKind::Exit,
            StrView_EqCStr(AppCommandSpec_At(3)->Alias, "quit"),
            AppCommandSpec_At(3)->Description.Length != 0);

  AppCommand plain = AppCommand_Parse(SVL("  hello  "));
  CHECK(plain.Kind == AppCommandKind::NotCommand);

  AppCommand login = AppCommand_Parse(SVL("\t /login \r\n"));
  CHECK(login.Kind == AppCommandKind::Login);
  CHECK(login.Arguments.Length == 0);

  struct ParseCase {
    StrView Input;
    AppCommandKind Expected;
  };
  ParseCase cases[] = {
      {SVL("/model"), AppCommandKind::Model},   {SVL("/clear"), AppCommandKind::Clear},
      {SVL("/exit"), AppCommandKind::Exit},     {SVL("/quit"), AppCommandKind::Exit},
      {SVL("/LOGIN"), AppCommandKind::Unknown}, {SVL("/"), AppCommandKind::Unknown},
      {SVL("/wat"), AppCommandKind::Unknown},
  };
  bool parsed = true;
  for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    bool matches = AppCommand_Parse(cases[i].Input).Kind == cases[i].Expected;
    parsed = matches && parsed;
  }
  CHECK(parsed);

  AppCommand args = AppCommand_Parse(SVL(" /model \t claude-test \n"));
  CHECK_ALL(args.Kind == AppCommandKind::Model,
            StrView_EqCStr(args.Arguments, "claude-test"));
}

void TestDispatch() {
  FakeCommandContext ctx = {};
  ctx.LoginResult = {AppCommandHandlerStatus::Success, SVL("Logged in.")};
  ctx.ModelResult = {AppCommandHandlerStatus::Success, SVL("Model selected.")};
  AppCommandUx ux = {&ctx, Notice, Error};
  AppCommandHandlers handlers = {&ctx, Login, Model};

  AppCommandDispatchResult plain = AppCommand_Dispatch(SVL("ordinary prompt"), &handlers, &ux);
  CHECK_ALL(!plain.Handled, plain.Action == AppLoopAction::Continue);

  AppCommandDispatchResult login = AppCommand_Dispatch(SVL(" /login "), &handlers, &ux);
  CHECK_ALL(login.Handled && login.Action == AppLoopAction::Continue,
            ctx.LoginCalls == 1 && ctx.ModelCalls == 0,
            ctx.Notices == 1 && StrView_EqCStr(ctx.LastNotice, "Logged in."));

  AppCommandDispatchResult model = AppCommand_Dispatch(SVL("/model"), &handlers, &ux);
  CHECK_ALL(model.Handled && model.Action == AppLoopAction::Continue, ctx.ModelCalls == 1,
            ctx.Notices == 2 && StrView_EqCStr(ctx.LastNotice, "Model selected."));

  AppCommandDispatchResult clear = AppCommand_Dispatch(SVL("/clear"), &handlers, &ux);
  CHECK(clear.Handled && clear.Action == AppLoopAction::ResetSession);

  AppCommandDispatchResult exit = AppCommand_Dispatch(SVL("/exit"), &handlers, &ux);
  CHECK(exit.Handled && exit.Action == AppLoopAction::Exit);
  AppCommandDispatchResult quit = AppCommand_Dispatch(SVL("\t/quit\n"), &handlers, &ux);
  CHECK(quit.Handled && quit.Action == AppLoopAction::Exit);

  int calls = ctx.LoginCalls + ctx.ModelCalls;
  AppCommandDispatchResult args = AppCommand_Dispatch(SVL("/login anthropic"), &handlers, &ux);
  CHECK_ALL(args.Handled && args.Action == AppLoopAction::Continue,
            ctx.LoginCalls + ctx.ModelCalls == calls,
            ctx.Errors == 1 && StrView_EqCStr(ctx.LastError, "Usage: /login"));

  AppCommandDispatchResult modelArgs = AppCommand_Dispatch(SVL("/model claude-test"), &handlers, &ux);
  CHECK_ALL(modelArgs.Handled && modelArgs.Action == AppLoopAction::Continue,
            ctx.LoginCalls + ctx.ModelCalls == calls,
            ctx.Errors == 2 && StrView_EqCStr(ctx.LastError, "Usage: /model"));

  AppCommandDispatchResult clearArgs = AppCommand_Dispatch(SVL("/clear now"), &handlers, &ux);
  CHECK_ALL(clearArgs.Handled && clearArgs.Action == AppLoopAction::Continue,
            ctx.Errors == 3 && StrView_EqCStr(ctx.LastError, "Usage: /clear"));

  AppCommandDispatchResult quitArgs = AppCommand_Dispatch(SVL("/quit now"), &handlers, &ux);
  CHECK_ALL(quitArgs.Handled && quitArgs.Action == AppLoopAction::Continue,
            ctx.Errors == 4 && StrView_Contains(ctx.LastError, "Usage: /exit"));

  AppCommandDispatchResult unknown = AppCommand_Dispatch(SVL(" /unknown "), &handlers, &ux);
  CHECK_ALL(unknown.Handled && unknown.Action == AppLoopAction::Continue, ctx.Errors == 5,
            StrView_Contains(ctx.LastError, "Unknown slash command"));
}

void TestHandlerOutcomes() {
  FakeCommandContext ctx = {};
  AppCommandUx ux = {&ctx, Notice, Error};
  AppCommandHandlers handlers = {&ctx, Login, Model};

  ctx.LoginResult = {AppCommandHandlerStatus::Cancelled, StrView{}};
  AppCommand_Dispatch(SVL("/login"), &handlers, &ux);
  CHECK(ctx.Notices == 1 && StrView_EqCStr(ctx.LastNotice, "Login cancelled."));

  ctx.ModelResult = {AppCommandHandlerStatus::Failed, SVL("Discovery failed.")};
  AppCommand_Dispatch(SVL("/model"), &handlers, &ux);
  CHECK(ctx.Errors == 1 && StrView_EqCStr(ctx.LastError, "Discovery failed."));

  AppCommandHandlers faux = {};
  AppCommand_Dispatch(SVL("/login"), &faux, &ux);
  CHECK(ctx.Errors == 2 && StrView_Contains(ctx.LastError, "unavailable in this mode"));
  AppCommand_Dispatch(SVL("/model"), &faux, &ux);
  CHECK(ctx.Errors == 3 && StrView_Contains(ctx.LastError, "unavailable in this mode"));

  AppCommandDispatchResult clear = AppCommand_Dispatch(SVL("/clear"), &faux, &ux);
  AppCommandDispatchResult exit = AppCommand_Dispatch(SVL("/exit"), &faux, &ux);
  CHECK_ALL(clear.Action == AppLoopAction::ResetSession,
            exit.Action == AppLoopAction::Exit);
}

}  // namespace

void TestCommand() {
  TestParse();
  TestDispatch();
  TestHandlerOutcomes();
}
