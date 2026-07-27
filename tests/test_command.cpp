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
  AppCommand plain = AppCommand_Parse(SVL("  hello  "));
  CHECK(plain.Kind == AppCommandKind::NotCommand);

  AppCommand login = AppCommand_Parse(SVL("\t /login \r\n"));
  CHECK(login.Kind == AppCommandKind::Login);
  CHECK(login.Arguments.Length == 0);

  CHECK(AppCommand_Parse(SVL("/model")).Kind == AppCommandKind::Model);
  CHECK(AppCommand_Parse(SVL("/clear")).Kind == AppCommandKind::Clear);
  CHECK(AppCommand_Parse(SVL("/exit")).Kind == AppCommandKind::Exit);
  CHECK(AppCommand_Parse(SVL("/quit")).Kind == AppCommandKind::Exit);

  AppCommand args = AppCommand_Parse(SVL(" /model \t claude-test \n"));
  CHECK(args.Kind == AppCommandKind::Model);
  CHECK(StrView_EqCStr(args.Arguments, "claude-test"));

  CHECK(AppCommand_Parse(SVL("/LOGIN")).Kind == AppCommandKind::Unknown);
  CHECK(AppCommand_Parse(SVL("/")).Kind == AppCommandKind::Unknown);
  CHECK(AppCommand_Parse(SVL("/wat")).Kind == AppCommandKind::Unknown);
}

void TestDispatch() {
  FakeCommandContext ctx = {};
  ctx.LoginResult = {AppCommandHandlerStatus::Success, SVL("Logged in.")};
  ctx.ModelResult = {AppCommandHandlerStatus::Success, SVL("Model selected.")};
  AppCommandUx ux = {&ctx, Notice, Error};
  AppCommandHandlers handlers = {&ctx, Login, Model};

  AppCommandDispatchResult plain = AppCommand_Dispatch(SVL("ordinary prompt"), &handlers, &ux);
  CHECK(!plain.Handled);
  CHECK(plain.Action == AppLoopAction::Continue);

  AppCommandDispatchResult login = AppCommand_Dispatch(SVL(" /login "), &handlers, &ux);
  CHECK(login.Handled && login.Action == AppLoopAction::Continue);
  CHECK(ctx.LoginCalls == 1 && ctx.ModelCalls == 0);
  CHECK(ctx.Notices == 1 && StrView_EqCStr(ctx.LastNotice, "Logged in."));

  AppCommandDispatchResult model = AppCommand_Dispatch(SVL("/model"), &handlers, &ux);
  CHECK(model.Handled && model.Action == AppLoopAction::Continue);
  CHECK(ctx.ModelCalls == 1);
  CHECK(ctx.Notices == 2 && StrView_EqCStr(ctx.LastNotice, "Model selected."));

  AppCommandDispatchResult clear = AppCommand_Dispatch(SVL("/clear"), &handlers, &ux);
  CHECK(clear.Handled && clear.Action == AppLoopAction::ResetSession);

  AppCommandDispatchResult exit = AppCommand_Dispatch(SVL("/exit"), &handlers, &ux);
  CHECK(exit.Handled && exit.Action == AppLoopAction::Exit);
  AppCommandDispatchResult quit = AppCommand_Dispatch(SVL("\t/quit\n"), &handlers, &ux);
  CHECK(quit.Handled && quit.Action == AppLoopAction::Exit);

  int calls = ctx.LoginCalls + ctx.ModelCalls;
  AppCommandDispatchResult args = AppCommand_Dispatch(SVL("/login anthropic"), &handlers, &ux);
  CHECK(args.Handled && args.Action == AppLoopAction::Continue);
  CHECK(ctx.LoginCalls + ctx.ModelCalls == calls);
  CHECK(ctx.Errors == 1 && StrView_EqCStr(ctx.LastError, "Usage: /login"));

  AppCommandDispatchResult modelArgs = AppCommand_Dispatch(SVL("/model claude-test"), &handlers, &ux);
  CHECK(modelArgs.Handled && modelArgs.Action == AppLoopAction::Continue);
  CHECK(ctx.LoginCalls + ctx.ModelCalls == calls);
  CHECK(ctx.Errors == 2 && StrView_EqCStr(ctx.LastError, "Usage: /model"));

  AppCommandDispatchResult clearArgs = AppCommand_Dispatch(SVL("/clear now"), &handlers, &ux);
  CHECK(clearArgs.Handled && clearArgs.Action == AppLoopAction::Continue);
  CHECK(ctx.Errors == 3 && StrView_EqCStr(ctx.LastError, "Usage: /clear"));

  AppCommandDispatchResult quitArgs = AppCommand_Dispatch(SVL("/quit now"), &handlers, &ux);
  CHECK(quitArgs.Handled && quitArgs.Action == AppLoopAction::Continue);
  CHECK(ctx.Errors == 4 && StrView_Contains(ctx.LastError, "Usage: /exit"));

  AppCommandDispatchResult unknown = AppCommand_Dispatch(SVL(" /unknown "), &handlers, &ux);
  CHECK(unknown.Handled && unknown.Action == AppLoopAction::Continue);
  CHECK(ctx.Errors == 5);
  CHECK(StrView_Contains(ctx.LastError, "Unknown slash command"));
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
  CHECK(clear.Action == AppLoopAction::ResetSession);
  CHECK(exit.Action == AppLoopAction::Exit);
}

}  // namespace

void TestCommand() {
  TestParse();
  TestDispatch();
  TestHandlerOutcomes();
}
