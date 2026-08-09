#include "app/command.h"
#include "check.h"

namespace {

struct FakeCommandContext {
  int LoginCalls;
  int ModelCalls;
  int SessionCalls;
  int NameCalls;
  int SessionsCalls;
  int ResumeCalls;
  int TrustCalls;
  int ReloadCalls;
  int DebugCalls;
  int Notices;
  int Errors;
  AppCommandHandlerResult LoginResult;
  AppCommandHandlerResult ModelResult;
  StrView LastNotice;
  StrView LastError;
  StrView LastArguments;
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

AppCommandHandlerResult Session(void* user, const AppCommandUx* ux,
                                StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->SessionCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, SVL("Session shown.")};
}

AppCommandHandlerResult Resume(void* user, const AppCommandUx* ux,
                               StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->ResumeCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, StrView{}};
}

AppCommandHandlerResult Name(void* user, const AppCommandUx* ux,
                             StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->NameCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, SVL("Name updated.")};
}

AppCommandHandlerResult Sessions(void* user, const AppCommandUx* ux,
                                 StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->SessionsCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, StrView{}};
}

AppCommandHandlerResult Trust(void* user, const AppCommandUx* ux,
                              StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->TrustCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, SVL("Trust saved.")};
}

AppCommandHandlerResult Reload(void* user, const AppCommandUx* ux,
                               StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->ReloadCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, SVL("Prompt reloaded.")};
}

AppCommandHandlerResult Debug(void* user, const AppCommandUx* ux,
                              StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->DebugCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, SVL("overlay toggled")};
}

void TestParse() {
  CHECK_ALL(AppCommandSpec_Count() == 15,
            AppCommandSpec_At(AppCommandSpec_Count()) == nullptr,
            AppCommandSpec_At(0)->Kind == AppCommandKind::Login,
            StrView_EqCStr(AppCommandSpec_At(0)->Name, "login"),
            AppCommandSpec_At(0)->Arguments ==
                AppCommandArgumentPolicy::None,
            AppCommandSpec_At(13)->Kind == AppCommandKind::Debug,
            AppCommandSpec_At(13)->Arguments ==
                AppCommandArgumentPolicy::Optional,
            AppCommandSpec_At(14)->Kind == AppCommandKind::Exit,
            StrView_EqCStr(AppCommandSpec_At(14)->Alias, "quit"),
            AppCommandSpec_At(14)->Description.Length != 0,
            StrView_EqCStr(AppCommand_Usage(AppCommandKind::Resume),
                           "Usage: /resume [session-id]"),
            StrView_EqCStr(AppCommand_Usage(AppCommandKind::Debug),
                           "Usage: /debug [on|off]"),
            AppCommand_Usage(AppCommandKind::Unknown).Length == 0);

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
      {SVL("/session"), AppCommandKind::Session},
      {SVL("/name"), AppCommandKind::Name},
      {SVL("/name release work"), AppCommandKind::Name},
      {SVL("/sessions"), AppCommandKind::Sessions},
      {SVL("/sessions all"), AppCommandKind::Sessions},
      {SVL("/resume"), AppCommandKind::Resume},
      {SVL("/resume abc"), AppCommandKind::Resume},
      {SVL("/tree"), AppCommandKind::Tree},
      {SVL("/checkout abc"), AppCommandKind::Checkout},
      {SVL("/fork"), AppCommandKind::Fork},
      {SVL("/compact"), AppCommandKind::Compact},
      {SVL("/trust"), AppCommandKind::Trust},
      {SVL("/reload"), AppCommandKind::Reload},
      {SVL("/debug"), AppCommandKind::Debug},
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
  AppCommandHandlers handlers = {};
  handlers.User = &ctx;
  handlers.Login = Login;
  handlers.Model = Model;
  handlers.Session = Session;
  handlers.Name = Name;
  handlers.Sessions = Sessions;
  handlers.Resume = Resume;
  handlers.Trust = Trust;
  handlers.Reload = Reload;
  handlers.Debug = Debug;

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

  AppCommandDispatchResult session =
      AppCommand_Dispatch(SVL("/session"), &handlers, &ux);
  CHECK_ALL(session.Handled && session.Action == AppLoopAction::Continue,
            ctx.SessionCalls == 1,
            StrView_EqCStr(ctx.LastNotice, "Session shown."));

  AppCommand_Dispatch(SVL("/name Release work"), &handlers, &ux);
  CHECK_ALL(ctx.NameCalls == 1,
            StrView_EqCStr(ctx.LastArguments, "Release work"),
            StrView_EqCStr(ctx.LastNotice, "Name updated."));
  AppCommand_Dispatch(SVL("/name --clear"), &handlers, &ux);
  CHECK_ALL(ctx.NameCalls == 2,
            StrView_EqCStr(ctx.LastArguments, "--clear"));
  AppCommand_Dispatch(SVL("/sessions all"), &handlers, &ux);
  CHECK_ALL(ctx.SessionsCalls == 1,
            StrView_EqCStr(ctx.LastArguments, "all"));

  int noticesBeforeResume = ctx.Notices;
  AppCommandDispatchResult resume =
      AppCommand_Dispatch(SVL("/resume"), &handlers, &ux);
  CHECK_ALL(resume.Handled && resume.Action == AppLoopAction::Continue,
            ctx.ResumeCalls == 1 && ctx.LastArguments.Length == 0,
            ctx.Notices == noticesBeforeResume);
  AppCommand_Dispatch(SVL("/resume abc"), &handlers, &ux);
  CHECK_ALL(ctx.ResumeCalls == 2,
            StrView_EqCStr(ctx.LastArguments, "abc"));

  AppCommandDispatchResult exit = AppCommand_Dispatch(SVL("/exit"), &handlers, &ux);
  CHECK(exit.Handled && exit.Action == AppLoopAction::Exit);
  AppCommandDispatchResult quit = AppCommand_Dispatch(SVL("\t/quit\n"), &handlers, &ux);
  CHECK(quit.Handled && quit.Action == AppLoopAction::Exit);

  AppCommand_Dispatch(SVL("/trust"), &handlers, &ux);
  AppCommand_Dispatch(SVL("/reload"), &handlers, &ux);
  CHECK_ALL(ctx.TrustCalls == 1, ctx.ReloadCalls == 1);

  AppCommand_Dispatch(SVL("/debug on"), &handlers, &ux);
  CHECK_ALL(ctx.DebugCalls == 1, StrView_EqCStr(ctx.LastArguments, "on"),
            StrView_EqCStr(ctx.LastNotice, "overlay toggled"));

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

  AppCommand_Dispatch(SVL("/reload now"), &handlers, &ux);
  CHECK_ALL(ctx.ReloadCalls == 1, ctx.Errors == 5,
            StrView_Contains(ctx.LastError, "Usage: /reload"));

  AppCommandDispatchResult unknown = AppCommand_Dispatch(SVL(" /unknown "), &handlers, &ux);
  CHECK_ALL(unknown.Handled && unknown.Action == AppLoopAction::Continue, ctx.Errors == 6,
            StrView_Contains(ctx.LastError, "Unknown slash command"));
}

void TestHandlerOutcomes() {
  FakeCommandContext ctx = {};
  AppCommandUx ux = {&ctx, Notice, Error};
  AppCommandHandlers handlers = {};
  handlers.User = &ctx;
  handlers.Login = Login;
  handlers.Model = Model;

  ctx.LoginResult = {AppCommandHandlerStatus::Cancelled, StrView{}};
  AppCommand_Dispatch(SVL("/login"), &handlers, &ux);
  CHECK(ctx.Notices == 1 && StrView_EqCStr(ctx.LastNotice, "Login cancelled."));

  ctx.ModelResult = {AppCommandHandlerStatus::Failed, SVL("Discovery failed.")};
  AppCommand_Dispatch(SVL("/model"), &handlers, &ux);
  CHECK(ctx.Errors == 1 && StrView_EqCStr(ctx.LastError, "Discovery failed."));

  AppCommandHandlers noHandlers = {};
  AppCommand_Dispatch(SVL("/login"), &noHandlers, &ux);
  CHECK(ctx.Errors == 2 && StrView_Contains(ctx.LastError, "unavailable in this mode"));
  AppCommand_Dispatch(SVL("/model"), &noHandlers, &ux);
  CHECK(ctx.Errors == 3 && StrView_Contains(ctx.LastError, "unavailable in this mode"));

  AppCommandDispatchResult clear = AppCommand_Dispatch(SVL("/clear"), &noHandlers, &ux);
  AppCommandDispatchResult exit = AppCommand_Dispatch(SVL("/exit"), &noHandlers, &ux);
  CHECK_ALL(clear.Action == AppLoopAction::ResetSession,
            exit.Action == AppLoopAction::Exit);
}

}  // namespace

void TestCommand() {
  TestParse();
  TestDispatch();
  TestHandlerOutcomes();
}
