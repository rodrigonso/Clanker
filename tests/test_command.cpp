#include "app/command.h"
#include "check.h"

namespace {

struct FakeCommandContext {
  int AuthCalls;
  int ModelCalls;
  int SessionCalls;
  int NameCalls;
  int SessionsCalls;
  int ResumeCalls;
  int TrustCalls;
  int PermissionsCalls;
  int ReloadCalls;
  int DebugCalls;
  int RoundedCalls;
  int SkillCalls;
  int TemplateCalls;
  int Notices;
  int Errors;
  AppCommandHandlerResult AuthResult;
  AppCommandHandlerResult ModelResult;
  AppCommandHandlerResult SkillResult;
  StrView LastNotice;
  StrView LastError;
  StrView LastArguments;
  StrView LastSkillName;
  StrView LastTemplateName;
  StrView TemplateName;
  AppCommandHandlerResult TemplateResult;
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

AppCommandHandlerResult Auth(void* user, const AppCommandUx* ux) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->AuthCalls++;
  return ctx->AuthResult;
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

AppCommandHandlerResult Permissions(void* user, const AppCommandUx* ux,
                                    StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->PermissionsCalls++;
  ctx->LastArguments = arguments;
  if (arguments.Length && !StrView_Eq(arguments, SVL("ask")) &&
      !StrView_Eq(arguments, SVL("auto")) &&
      !StrView_Eq(arguments, SVL("yolo")) &&
      !StrView_Eq(arguments, SVL("reset")))
    return {AppCommandHandlerStatus::Failed,
            SVL("Usage: /permissions [ask|auto|yolo|reset]")};
  return {AppCommandHandlerStatus::Success, SVL("permissions updated")};
}

AppCommandHandlerResult Debug(void* user, const AppCommandUx* ux,
                              StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->DebugCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, SVL("overlay toggled")};
}

AppCommandHandlerResult Rounded(void* user, const AppCommandUx* ux,
                                StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->RoundedCalls++;
  ctx->LastArguments = arguments;
  return {AppCommandHandlerStatus::Success, SVL("rounded corners and bullets off")};
}

AppCommandHandlerResult Skill(void* user, const AppCommandUx* ux, StrView name,
                              StrView arguments) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  ctx->SkillCalls++;
  ctx->LastSkillName = name;
  ctx->LastArguments = arguments;
  return ctx->SkillResult;
}

bool PromptTemplate(void* user, const AppCommandUx* ux, StrView name,
                    StrView arguments, AppCommandHandlerResult* out) {
  (void)ux;
  FakeCommandContext* ctx = (FakeCommandContext*)user;
  if (!StrView_Eq(name, ctx->TemplateName)) return false;
  ctx->TemplateCalls++;
  ctx->LastTemplateName = name;
  ctx->LastArguments = arguments;
  *out = ctx->TemplateResult;
  return true;
}

void TestParse() {
  CHECK_ALL(AppCommandSpec_Count() == 18,
            AppCommandSpec_At(AppCommandSpec_Count()) == nullptr,
            AppCommandSpec_At(0)->Kind == AppCommandKind::Auth,
            StrView_EqCStr(AppCommandSpec_At(0)->Name, "auth"),
            AppCommandSpec_At(0)->Alias.Length == 0,
            AppCommandSpec_At(0)->Arguments ==
                AppCommandArgumentPolicy::None,
            AppCommandSpec_At(12)->Kind == AppCommandKind::Permissions,
            StrView_EqCStr(AppCommandSpec_At(12)->Name, "permissions"),
            AppCommandSpec_At(12)->Arguments ==
                AppCommandArgumentPolicy::Optional,
            AppCommandSpec_At(14)->Kind == AppCommandKind::Debug,
            AppCommandSpec_At(14)->Arguments ==
                AppCommandArgumentPolicy::Optional,
            AppCommandSpec_At(15)->Kind == AppCommandKind::Rounded,
            StrView_EqCStr(AppCommandSpec_At(15)->Name, "rounded"),
            AppCommandSpec_At(15)->Arguments ==
                AppCommandArgumentPolicy::Optional,
            AppCommandSpec_At(16)->Kind == AppCommandKind::Skill,
            StrView_EqCStr(AppCommandSpec_At(16)->Name, "skill"),
            AppCommandSpec_At(16)->Arguments ==
                AppCommandArgumentPolicy::Optional,
            AppCommandSpec_At(17)->Kind == AppCommandKind::Exit,
            StrView_EqCStr(AppCommandSpec_At(17)->Alias, "quit"),
            AppCommandSpec_At(17)->Description.Length != 0,
            StrView_EqCStr(AppCommand_Usage(AppCommandKind::Resume),
                           "Usage: /resume [session-id]"),
            StrView_EqCStr(AppCommand_Usage(AppCommandKind::Debug),
                           "Usage: /debug [on|off]"),
            StrView_EqCStr(
                AppCommand_Usage(AppCommandKind::Permissions),
                "Usage: /permissions [ask|auto|yolo|reset]"),
            StrView_EqCStr(AppCommand_Usage(AppCommandKind::Rounded),
                           "Usage: /rounded [on|off]"),
            StrView_EqCStr(AppCommand_Usage(AppCommandKind::Skill),
                           "Usage: /skill:<name> [instructions]"),
            AppCommand_Usage(AppCommandKind::Unknown).Length == 0);

  u32 permissionsIndex = AppCommandSpec_Count();
  for (u32 i = 0; i < AppCommandSpec_Count(); i++) {
    const AppCommandSpec* spec = AppCommandSpec_At(i);
    if (spec->Kind == AppCommandKind::Permissions) permissionsIndex = i;
  }
  CHECK_ALL(permissionsIndex == 12,
            AppCommandSpec_At(permissionsIndex)->Alias.Length == 0,
            AppCommandSpec_At(permissionsIndex)->Arguments ==
                AppCommandArgumentPolicy::Optional,
            StrView_Eq(AppCommand_Parse(SVL("/permissions")).Name,
                       AppCommandSpec_At(permissionsIndex)->Name),
            AppCommand_Parse(SVL("/permissions")).Kind ==
                AppCommandSpec_At(permissionsIndex)->Kind);

  AppCommand plain = AppCommand_Parse(SVL("  hello  "));
  CHECK(plain.Kind == AppCommandKind::NotCommand);

  AppCommand auth = AppCommand_Parse(SVL("\t /auth \r\n"));
  CHECK(auth.Kind == AppCommandKind::Auth);
  CHECK(auth.Arguments.Length == 0);

  AppCommand removed = AppCommand_Parse(SVL("/login"));
  CHECK(removed.Kind == AppCommandKind::Unknown);

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
      {SVL("/compact keep the parser work"), AppCommandKind::Compact},
      {SVL("/trust"), AppCommandKind::Trust},
      {SVL("/permissions"), AppCommandKind::Permissions},
      {SVL("/permissions auto"), AppCommandKind::Permissions},
      {SVL("/permissions reset"), AppCommandKind::Permissions},
      {SVL("/reload"), AppCommandKind::Reload},
      {SVL("/debug"), AppCommandKind::Debug},
      {SVL("/rounded"), AppCommandKind::Rounded},
      {SVL("/rounded on"), AppCommandKind::Rounded},
      {SVL("/rounded off"), AppCommandKind::Rounded},
      {SVL("/exit"), AppCommandKind::Exit},     {SVL("/quit"), AppCommandKind::Exit},
      {SVL("/skill"), AppCommandKind::Skill},
      {SVL("/skill:"), AppCommandKind::Skill},
      {SVL("/skill:review"), AppCommandKind::Skill},
      {SVL("/skill:review go hard"), AppCommandKind::Skill},
      {SVL("/LOGIN"), AppCommandKind::Unknown}, {SVL("/"), AppCommandKind::Unknown},
      {SVL("/wat"), AppCommandKind::Unknown},
  };
  bool parsed = true;
  for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    bool matches = AppCommand_Parse(cases[i].Input).Kind == cases[i].Expected;
    parsed = matches && parsed;
  }
  CHECK(parsed);

  AppCommand skill = AppCommand_Parse(SVL("  /skill:code-review  be terse \n"));
  CHECK_ALL(skill.Kind == AppCommandKind::Skill,
            StrView_EqCStr(skill.SkillName, "code-review"),
            StrView_EqCStr(skill.Arguments, "be terse"));

  AppCommand bareSkill = AppCommand_Parse(SVL("/skill"));
  CHECK_ALL(bareSkill.Kind == AppCommandKind::Skill,
            bareSkill.SkillName.Length == 0);

  AppCommand emptySkillName = AppCommand_Parse(SVL("/skill: do it"));
  CHECK_ALL(emptySkillName.Kind == AppCommandKind::Skill,
            emptySkillName.SkillName.Length == 0,
            StrView_EqCStr(emptySkillName.Arguments, "do it"));

  AppCommand args = AppCommand_Parse(SVL(" /model \t claude-test \n"));
  CHECK_ALL(args.Kind == AppCommandKind::Model,
            StrView_EqCStr(args.Arguments, "claude-test"));

  AppCommand compact = AppCommand_Parse(SVL("  /compact  focus on the parser \n"));
  CHECK_ALL(compact.Kind == AppCommandKind::Compact,
            StrView_EqCStr(compact.Arguments, "focus on the parser"));

  AppCommand bareCompact = AppCommand_Parse(SVL("/compact"));
  CHECK_ALL(bareCompact.Kind == AppCommandKind::Compact,
            bareCompact.Arguments.Length == 0);
}

void TestDispatch() {
  FakeCommandContext ctx = {};
  ctx.AuthResult = {AppCommandHandlerStatus::Success, SVL("Authenticated.")};
  ctx.ModelResult = {AppCommandHandlerStatus::Success, SVL("Model selected.")};
  ctx.SkillResult = {AppCommandHandlerStatus::Success, StrView{}};
  AppCommandUx ux = {&ctx, Notice, Error};
  AppCommandHandlers handlers = {};
  handlers.User = &ctx;
  handlers.Auth = Auth;
  handlers.Model = Model;
  handlers.Session = Session;
  handlers.Name = Name;
  handlers.Sessions = Sessions;
  handlers.Resume = Resume;
  handlers.Trust = Trust;
  handlers.Permissions = Permissions;
  handlers.Reload = Reload;
  handlers.Debug = Debug;
  handlers.Rounded = Rounded;
  handlers.Skill = Skill;

  AppCommandDispatchResult plain = AppCommand_Dispatch(SVL("ordinary prompt"), &handlers, &ux);
  CHECK_ALL(!plain.Handled, plain.Action == AppLoopAction::Continue);

  AppCommandDispatchResult auth = AppCommand_Dispatch(SVL(" /auth "), &handlers, &ux);
  CHECK_ALL(auth.Handled && auth.Action == AppLoopAction::Continue,
            ctx.AuthCalls == 1 && ctx.ModelCalls == 0,
            ctx.Notices == 1 && StrView_EqCStr(ctx.LastNotice, "Authenticated."));

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
  AppCommand_Dispatch(SVL("/permissions auto"), &handlers, &ux);
  CHECK_ALL(ctx.PermissionsCalls == 1,
            StrView_EqCStr(ctx.LastArguments, "auto"),
            StrView_EqCStr(ctx.LastNotice, "permissions updated"));
  AppCommand_Dispatch(SVL("/reload"), &handlers, &ux);
  CHECK_ALL(ctx.TrustCalls == 1, ctx.ReloadCalls == 1);

  AppCommand_Dispatch(SVL("/debug on"), &handlers, &ux);
  CHECK_ALL(ctx.DebugCalls == 1, StrView_EqCStr(ctx.LastArguments, "on"),
            StrView_EqCStr(ctx.LastNotice, "overlay toggled"));

  AppCommand_Dispatch(SVL("/rounded off"), &handlers, &ux);
  CHECK_ALL(ctx.RoundedCalls == 1, StrView_EqCStr(ctx.LastArguments, "off"),
            StrView_EqCStr(ctx.LastNotice, "rounded corners and bullets off"));
  AppCommand_Dispatch(SVL("/rounded"), &handlers, &ux);
  CHECK_ALL(ctx.RoundedCalls == 2, ctx.LastArguments.Length == 0);

  int calls = ctx.AuthCalls + ctx.ModelCalls;
  AppCommandDispatchResult args = AppCommand_Dispatch(SVL("/auth anthropic"), &handlers, &ux);
  CHECK_ALL(args.Handled && args.Action == AppLoopAction::Continue,
            ctx.AuthCalls + ctx.ModelCalls == calls,
            ctx.Errors == 1 && StrView_EqCStr(ctx.LastError, "Usage: /auth"));

  AppCommandDispatchResult modelArgs = AppCommand_Dispatch(SVL("/model claude-test"), &handlers, &ux);
  CHECK_ALL(modelArgs.Handled && modelArgs.Action == AppLoopAction::Continue,
            ctx.AuthCalls + ctx.ModelCalls == calls,
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

  AppCommand_Dispatch(SVL("/permissions nope"), &handlers, &ux);
  CHECK_ALL(ctx.PermissionsCalls == 2, ctx.Errors == 6,
            StrView_Contains(ctx.LastError, "Usage: /permissions"));

  AppCommandDispatchResult unknown = AppCommand_Dispatch(SVL(" /unknown "), &handlers, &ux);
  CHECK_ALL(unknown.Handled && unknown.Action == AppLoopAction::Continue, ctx.Errors == 7,
            StrView_Contains(ctx.LastError, "Unknown slash command"));

  AppCommandDispatchResult skill =
      AppCommand_Dispatch(SVL("/skill:code-review be terse"), &handlers, &ux);
  CHECK_ALL(skill.Handled && skill.Action == AppLoopAction::Continue,
            ctx.SkillCalls == 1,
            StrView_EqCStr(ctx.LastSkillName, "code-review"),
            StrView_EqCStr(ctx.LastArguments, "be terse"), ctx.Errors == 7);

  AppCommandDispatchResult bareSkill =
      AppCommand_Dispatch(SVL("/skill"), &handlers, &ux);
  CHECK_ALL(bareSkill.Handled && bareSkill.Action == AppLoopAction::Continue,
            ctx.SkillCalls == 1, ctx.Errors == 8,
            StrView_Contains(ctx.LastError, "Usage: /skill:<name>"));

  AppCommand_Dispatch(SVL("/skill: do it"), &handlers, &ux);
  CHECK_ALL(ctx.SkillCalls == 1, ctx.Errors == 9,
            StrView_Contains(ctx.LastError, "Usage: /skill:<name>"));

  ctx.SkillResult = {AppCommandHandlerStatus::Failed, SVL("Unknown skill: nope")};
  AppCommand_Dispatch(SVL("/skill:nope"), &handlers, &ux);
  CHECK_ALL(ctx.SkillCalls == 2, ctx.Errors == 10,
            StrView_EqCStr(ctx.LastError, "Unknown skill: nope"));

  AppCommandHandlers noSkill = handlers;
  noSkill.Skill = nullptr;
  AppCommand_Dispatch(SVL("/skill:code-review"), &noSkill, &ux);
  CHECK_ALL(ctx.SkillCalls == 2, ctx.Errors == 11,
            StrView_Contains(ctx.LastError, "Skills are unavailable"));

  ctx.TemplateName = SVL("ship");
  ctx.TemplateResult = {AppCommandHandlerStatus::Success, StrView{}};
  handlers.PromptTemplate = PromptTemplate;
  AppCommandDispatchResult tmpl =
      AppCommand_Dispatch(SVL("  /ship  fast  now "), &handlers, &ux);
  CHECK_ALL(tmpl.Handled && tmpl.Action == AppLoopAction::Continue,
            ctx.TemplateCalls == 1, ctx.Errors == 11,
            StrView_EqCStr(ctx.LastTemplateName, "ship"),
            StrView_EqCStr(ctx.LastArguments, "fast  now"));

  AppCommandDispatchResult declined =
      AppCommand_Dispatch(SVL("/nosuchthing"), &handlers, &ux);
  CHECK_ALL(declined.Handled, ctx.TemplateCalls == 1, ctx.Errors == 12,
            StrView_Contains(ctx.LastError, "Unknown slash command"));

  ctx.TemplateResult = {AppCommandHandlerStatus::Failed, SVL("Template broke.")};
  AppCommand_Dispatch(SVL("/ship"), &handlers, &ux);
  CHECK_ALL(ctx.TemplateCalls == 2, ctx.Errors == 13,
            StrView_EqCStr(ctx.LastError, "Template broke."));

  ctx.TemplateName = SVL("reload");
  int callsBeforeBuiltin = ctx.TemplateCalls;
  AppCommand_Dispatch(SVL("/reload"), &handlers, &ux);
  CHECK_ALL(ctx.TemplateCalls == callsBeforeBuiltin, ctx.ReloadCalls == 2);

  ctx.TemplateName = SVL("");
  int callsBeforeBare = ctx.TemplateCalls;
  AppCommand_Dispatch(SVL("/"), &handlers, &ux);
  CHECK_ALL(ctx.TemplateCalls == callsBeforeBare, ctx.Errors == 14,
            StrView_Contains(ctx.LastError, "Unknown slash command"));
}

void TestHandlerOutcomes() {
  FakeCommandContext ctx = {};
  AppCommandUx ux = {&ctx, Notice, Error};
  AppCommandHandlers handlers = {};
  handlers.User = &ctx;
  handlers.Auth = Auth;
  handlers.Model = Model;

  ctx.AuthResult = {AppCommandHandlerStatus::Cancelled, StrView{}};
  AppCommand_Dispatch(SVL("/auth"), &handlers, &ux);
  CHECK(ctx.Notices == 1 && StrView_EqCStr(ctx.LastNotice, "Authentication cancelled."));

  ctx.ModelResult = {AppCommandHandlerStatus::Failed, SVL("Discovery failed.")};
  AppCommand_Dispatch(SVL("/model"), &handlers, &ux);
  CHECK(ctx.Errors == 1 && StrView_EqCStr(ctx.LastError, "Discovery failed."));

  AppCommandHandlers noHandlers = {};
  AppCommand_Dispatch(SVL("/auth"), &noHandlers, &ux);
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
