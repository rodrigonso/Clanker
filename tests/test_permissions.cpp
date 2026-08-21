#include "agent/permissions.h"
#include "agent/harness.h"

#include "base/arena.h"
#include "base/thread.h"
#include "check.h"
#include "json/json.h"
#include "script_provider.h"
#include "session/memory_repository.h"

namespace {

struct PermissionTestState {
  u32 Reviews;
  AgentPermissionReviewDecision ReviewDecision;
  bool ReviewUnavailable;
  StrView Rationale;
  AgentPermissionEngine* ChangeModeEngine;
  AgentPermissionMode ChangeModeTo;
  AgentPermissionMode ChangeModeAfter;
  bool ChangeModeTwice;
  bool ChangeModeOnlyOnce;
  bool ModeChanged;
  u32 Prompts;
  u32 PromptChoice;
  bool PromptAvailable;
};

bool Review(void* user, Arena* arena,
            const AgentPermissionReviewRequest* request, volatile i32*,
            AgentPermissionReviewResult* out) {
  PermissionTestState* state = (PermissionTestState*)user;
  state->Reviews++;
  CHECK_ALL(StrView_Eq(request->ToolName, SVL("shell")),
            StrView_Contains(request->Action, "shell"),
            StrView_Contains(request->TrustedUserText, "trusted request"));
  if (state->ChangeModeEngine &&
      (!state->ChangeModeOnlyOnce || !state->ModeChanged)) {
    AgentPermissionEngine_SetMode(state->ChangeModeEngine,
                                  state->ChangeModeTo);
    if (state->ChangeModeTwice)
      AgentPermissionEngine_SetMode(state->ChangeModeEngine,
                                    state->ChangeModeAfter);
    state->ModeChanged = true;
  }
  out->Decision = state->ReviewDecision;
  out->Rationale = StrView_Dup(arena, state->Rationale);
  return !state->ReviewUnavailable;
}

bool Ask(void* user, Arena* arena, const AgentAskRequest* request,
         AgentAskResponse* out) {
  PermissionTestState* state = (PermissionTestState*)user;
  state->Prompts++;
  CHECK_ALL(request->ChoiceCount == 3,
            StrView_Eq(request->Category, SVL("Permission")),
            StrView_Contains(request->Question, "shell"),
            !StrView_Contains(request->Question, "\n"));
  if (!state->PromptAvailable) return false;
  out->Status = AgentAskStatus::Answered;
  out->ChoiceIndex = state->PromptChoice;
  out->Answer = StrView_Dup(arena, request->Choices[state->PromptChoice]);
  return true;
}

Tool SensitiveTool() {
  Tool tool = {};
  tool.Name = SVL("shell");
  tool.PermissionEffect = ToolPermissionEffect::Sensitive;
  return tool;
}

AgentPermissionGate Gate(AgentPermissionEngine* engine, Arena* arena) {
  static Message messages[1];
  messages[0] = Message_User(arena, SVL("trusted request"));
  AgentPermissionGate gate = {};
  gate.Engine = engine;
  gate.NewMessages = messages;
  gate.NewMessageCount = 1;
  return gate;
}

ToolArgs Args(Arena* arena, StrView json) {
  ToolArgs args = {};
  args.Json = json;
  args.Root = Json_Parse(arena, json);
  return args;
}

ToolArgs IndexedArgs(Arena* arena, u32 index) {
  StrBuf json = StrBuf_Make(arena, 32);
  StrBuf_AppendCStr(&json, "{\"command\":\"grant-");
  StrBuf_AppendU64(&json, index);
  StrBuf_AppendCStr(&json, "\"}");
  return Args(arena, StrBuf_View(&json));
}

StrView RequestId(Arena* arena, StrView denial) {
  JsonValue* root = Json_Parse(arena, denial);
  return Json_GetStr(root, "permission_request_id");
}

bool OversizedGrantKey(void*, Arena* arena, const ToolArgs*, StrView,
                       ToolPermissionInfo* out, StrBuf*) {
  *out = {};
  out->Action = SVL("Run shell with an oversized grant key");
  StrBuf key = StrBuf_Make(arena, 8194);
  for (u32 i = 0; i < 8193; i++) StrBuf_AppendChar(&key, 'k');
  out->GrantKey = StrBuf_View(&key);
  return true;
}

bool ConcurrentAlwaysAsk(void*, Arena*, const AgentAskRequest*,
                         AgentAskResponse* out) {
  out->Status = AgentAskStatus::Answered;
  out->ChoiceIndex = 1;
  out->Answer = SVL("Always for this session");
  return true;
}

struct DecisionThread {
  AgentPermissionEngine* Engine;
  const Tool* Tool;
  StrView ArgsJson;
  volatile i32* Abort;
  AgentPermissionDecision Decision;
  Arena Arena;
};

void DecideThread(void* user) {
  DecisionThread* thread = (DecisionThread*)user;
  thread->Arena = ArenaCreate((usize)1 << 20);
  ToolArgs args = Args(&thread->Arena, thread->ArgsJson);
  AgentPermissionGate gate = {};
  gate.Engine = thread->Engine;
  gate.Abort = thread->Abort;
  AgentPermission_Decide(&thread->Arena, &gate, thread->Tool, &args,
                         &thread->Decision);
}

struct AbortReviewState {
  volatile i32 Entered;
};

bool AbortReview(void* user, Arena*, const AgentPermissionReviewRequest*,
                 volatile i32* abort, AgentPermissionReviewResult*) {
  AbortReviewState* state = (AbortReviewState*)user;
  Atomic_StoreI32(&state->Entered, 1);
  while (!abort || Atomic_LoadI32(abort) == 0) Sleep_Ms(1);
  return false;
}

void TestModeHelpers() {
  AgentPermissionMode mode = AgentPermissionMode::Yolo;
  CHECK(AgentPermissionMode_Parse(SVL("ask"), &mode));
  CHECK(mode == AgentPermissionMode::Ask);
  CHECK(AgentPermissionMode_Parse(SVL("auto"), &mode));
  CHECK(mode == AgentPermissionMode::Auto);
  CHECK(AgentPermissionMode_Parse(SVL("yolo"), &mode));
  CHECK(mode == AgentPermissionMode::Yolo);
  CHECK_ALL(!AgentPermissionMode_Parse(SVL("always"), &mode),
            StrView_Eq(AgentPermissionMode_Name(AgentPermissionMode::Ask),
                       SVL("ask")),
            AgentPermissionMode_Next(AgentPermissionMode::Ask) ==
                AgentPermissionMode::Auto,
            AgentPermissionMode_Next(AgentPermissionMode::Auto) ==
                AgentPermissionMode::Yolo,
            AgentPermissionMode_Next(AgentPermissionMode::Yolo) ==
                AgentPermissionMode::Ask);
}

void TestModesAndRecovery() {
  Arena arena = ArenaCreate();
  PermissionTestState state = {};
  state.PromptAvailable = true;
  AgentAskUser ask = {&state, Ask};
  AgentPermissionReviewer reviewer = {&state, Review};
  AgentPermissionEngine engine = {};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Auto,
                             SVL("C:\\work"), &ask, reviewer);
  AgentPermissionEngine_BeginRun(&engine);

  Tool tool = SensitiveTool();
  ToolArgs args = Args(&arena, SVL("{\"command\":\"danger\"}"));
  AgentPermissionGate gate = Gate(&engine, &arena);

  state.ReviewDecision = AgentPermissionReviewDecision::Allow;
  state.Rationale = SVL("ordinary requested work");
  AgentPermissionDecision decision = {};
  AgentPermission_Decide(&arena, &gate, &tool, &args, &decision);
  CHECK_ALL(!decision.Block, state.Reviews == 1);
  AgentPermission_Decide(&arena, &gate, &tool, &args, &decision);
  CHECK_ALL(!decision.Block, state.Reviews == 1);

  ToolArgs deniedArgs = Args(&arena, SVL("{\"command\":\"other\"}"));
  state.ReviewDecision = AgentPermissionReviewDecision::Ask;
  state.Rationale = SVL("needs approval");
  AgentPermission_Decide(&arena, &gate, &tool, &deniedArgs, &decision);
  StrView requestId = RequestId(&arena, decision.Reason);
  CHECK_ALL(decision.Block, requestId.Length == DURABLE_ID_CHAR_COUNT,
            state.Reviews == 2);

  state.PromptChoice = 0;
  StrView resolved = {};
  StrBuf error = StrBuf_Make(&arena, 128);
  StrView ignored = {};
  StrBuf invalidError = StrBuf_Make(&arena, 128);
  CHECK(!AgentPermission_ResolveRequest(
      &engine, &arena, SVL("not-a-real-id"), nullptr, &ignored,
      &invalidError));
  CHECK(AgentPermission_ResolveRequest(&engine, &arena, requestId, nullptr,
                                       &resolved, &error));
  CHECK(StrView_Contains(resolved, "granted once"));
  StrBuf reusedError = StrBuf_Make(&arena, 128);
  CHECK(!AgentPermission_ResolveRequest(&engine, &arena, requestId, nullptr,
                                        &ignored, &reusedError));
  ToolArgs alteredArgs = Args(&arena, SVL("{\"command\":\"other changed\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &alteredArgs, &decision);
  CHECK(decision.Block);
  AgentPermission_Decide(&arena, &gate, &tool, &deniedArgs, &decision);
  CHECK(!decision.Block);
  AgentPermission_Decide(&arena, &gate, &tool, &deniedArgs, &decision);
  CHECK(decision.Block);

  AgentPermissionEngine_SetMode(&engine, AgentPermissionMode::Yolo);
  ToolArgs yoloArgs = Args(&arena, SVL("{\"command\":\"unrestricted\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &yoloArgs, &decision);
  CHECK_ALL(!decision.Block, state.Reviews == 4);

  AgentPermissionEngine_SetMode(&engine, AgentPermissionMode::Ask);
  state.PromptChoice = 1;
  ToolArgs grantedArgs = Args(&arena, SVL("{\"command\":\"remember\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &grantedArgs, &decision);
  CHECK_ALL(!decision.Block, AgentPermissionEngine_GrantCount(&engine) == 1);
  u32 promptCount = state.Prompts;
  AgentPermission_Decide(&arena, &gate, &tool, &grantedArgs, &decision);
  CHECK_ALL(!decision.Block, state.Prompts == promptCount);

  AgentPermissionEngine_Reset(&engine);
  CHECK_ALL(AgentPermissionEngine_GetMode(&engine) ==
                AgentPermissionMode::Ask,
            AgentPermissionEngine_GrantCount(&engine) == 0);

  AgentPermissionEngine_EndRun(&engine);
  AgentPermissionEngine_BeginRun(&engine);
  StrBuf staleError = StrBuf_Make(&arena, 128);
  CHECK(!AgentPermission_ResolveRequest(&engine, &arena, requestId, nullptr,
                                        &ignored, &staleError));
  AgentPermissionEngine_EndRun(&engine);
  AgentPermissionEngine_Close(&engine);
  ArenaRelease(&arena);
}

void TestLiveModeChangeAndFastPaths() {
  Arena arena = ArenaCreate();
  PermissionTestState state = {};
  AgentAskUser ask = {&state, Ask};
  AgentPermissionReviewer reviewer = {&state, Review};
  AgentPermissionEngine engine = {};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Auto,
                             SVL("C:\\work"), &ask, reviewer);
  AgentPermissionEngine_BeginRun(&engine);
  state.ReviewDecision = AgentPermissionReviewDecision::Ask;
  state.ChangeModeEngine = &engine;
  state.ChangeModeTo = AgentPermissionMode::Yolo;

  Tool sensitive = SensitiveTool();
  ToolArgs args = Args(&arena, SVL("{\"command\":\"switch\"}"));
  AgentPermissionGate gate = Gate(&engine, &arena);
  AgentPermissionDecision decision = {};
  AgentPermission_Decide(&arena, &gate, &sensitive, &args, &decision);
  CHECK_ALL(!decision.Block, state.Reviews == 1,
            AgentPermissionEngine_GetMode(&engine) ==
                AgentPermissionMode::Yolo);

  Tool inspect = {};
  inspect.Name = SVL("read");
  inspect.PermissionEffect = ToolPermissionEffect::Inspection;
  AgentPermissionEngine_SetMode(&engine, AgentPermissionMode::Ask);
  state.PromptAvailable = false;
  AgentPermission_Decide(&arena, &gate, &inspect, &args, &decision);
  CHECK_ALL(!decision.Block, state.Prompts == 0);
  AgentPermission_Decide(&arena, &gate, &sensitive, &args, &decision);
  CHECK_ALL(decision.Block,
            StrView_Contains(decision.Reason, "No user is available"));

  AgentPermissionEngine_SetMode(&engine, AgentPermissionMode::Auto);
  state.ChangeModeTo = AgentPermissionMode::Ask;
  state.ChangeModeTwice = false;
  state.ChangeModeOnlyOnce = true;
  state.ModeChanged = false;
  state.PromptAvailable = true;
  state.PromptChoice = 0;
  state.ReviewDecision = AgentPermissionReviewDecision::Allow;
  u32 reviewsBeforeAskSwitch = state.Reviews;
  u32 promptsBeforeAskSwitch = state.Prompts;
  ToolArgs askSwitchArgs =
      Args(&arena, SVL("{\"command\":\"switch-to-ask\"}"));
  AgentPermission_Decide(&arena, &gate, &sensitive, &askSwitchArgs,
                         &decision);
  CHECK(!decision.Block);
  CHECK(state.Reviews == reviewsBeforeAskSwitch + 1);
  CHECK(state.Prompts == promptsBeforeAskSwitch + 1);
  CHECK(AgentPermissionEngine_GetMode(&engine) == AgentPermissionMode::Ask);

  AgentPermissionEngine_SetMode(&engine, AgentPermissionMode::Auto);
  state.ChangeModeTo = AgentPermissionMode::Ask;
  state.ChangeModeAfter = AgentPermissionMode::Auto;
  state.ChangeModeTwice = true;
  state.ChangeModeOnlyOnce = true;
  state.ModeChanged = false;
  u32 reviewsBeforeAutoSwitch = state.Reviews;
  ToolArgs autoSwitchArgs =
      Args(&arena, SVL("{\"command\":\"switch-to-auto\"}"));
  AgentPermission_Decide(&arena, &gate, &sensitive, &autoSwitchArgs,
                         &decision);
  CHECK(!decision.Block);
  CHECK(state.Reviews == reviewsBeforeAutoSwitch + 2);
  CHECK(AgentPermissionEngine_GetMode(&engine) == AgentPermissionMode::Auto);

  StrView prompt = AgentPermission_AppendSystemPrompt(
      &arena, SVL("base"), AgentPermissionMode::Auto);
  CHECK_ALL(StrView_Contains(prompt, "<permission_mode>"),
            StrView_Contains(prompt, "permission_request_id"),
            StrView_Contains(prompt, "External PreToolUse"));

  AgentPermissionEngine_EndRun(&engine);
  AgentPermissionEngine_Close(&engine);
  ArenaRelease(&arena);
}

void TestGrantCapAndUnbindableKeys() {
  Arena arena = ArenaCreate();
  PermissionTestState state = {};
  state.PromptAvailable = true;
  state.PromptChoice = 1;
  AgentAskUser ask = {&state, Ask};
  AgentPermissionEngine engine = {};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Ask,
                             SVL("C:\\work"), &ask, {});
  AgentPermissionEngine_BeginRun(&engine);
  Tool tool = SensitiveTool();
  AgentPermissionGate gate = Gate(&engine, &arena);
  AgentPermissionDecision decision = {};

  for (u32 i = 0; i < 64; i++) {
    ToolArgs args = IndexedArgs(&arena, i);
    AgentPermission_Decide(&arena, &gate, &tool, &args, &decision);
    CHECK(!decision.Block);
  }
  CHECK(AgentPermissionEngine_GrantCount(&engine) == 64);

  ToolArgs exhaustedArgs = IndexedArgs(&arena, 64);
  AgentPermission_Decide(&arena, &gate, &tool, &exhaustedArgs, &decision);
  CHECK(decision.Block);
  CHECK(StrView_Contains(
      decision.Reason, "The session permission grant limit was reached."));
  CHECK(AgentPermissionEngine_GrantCount(&engine) == 64);
  AgentPermissionEngine_Close(&engine);

  state = {};
  state.PromptAvailable = true;
  state.PromptChoice = 1;
  state.ReviewDecision = AgentPermissionReviewDecision::Allow;
  state.Rationale = SVL("reviewed");
  AgentPermissionReviewer reviewer = {&state, Review};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Auto,
                             SVL("C:\\work"), &ask, reviewer);
  AgentPermissionEngine_BeginRun(&engine);
  tool.DescribePermission = OversizedGrantKey;

  ToolArgs first = Args(&arena, SVL("{\"command\":\"oversized-one\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &first, &decision);
  CHECK(!decision.Block);
  CHECK(state.Reviews == 1);

  AgentPermission_Decide(&arena, &gate, &tool, &first, &decision);
  CHECK(!decision.Block);
  CHECK(state.Reviews == 1);

  ToolArgs different =
      Args(&arena, SVL("{\"command\":\"oversized-different\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &different, &decision);
  CHECK(!decision.Block);
  CHECK(state.Reviews == 2);
  CHECK(AgentPermissionEngine_GrantCount(&engine) == 0);

  AgentPermissionEngine_SetMode(&engine, AgentPermissionMode::Ask);
  state.PromptChoice = 1;
  ToolArgs askOversized =
      Args(&arena, SVL("{\"command\":\"oversized-ask\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &askOversized, &decision);
  CHECK(decision.Block);
  CHECK(StrView_Contains(
      decision.Reason,
      "This action is too large to bind safely to a session grant."));
  CHECK(AgentPermissionEngine_GrantCount(&engine) == 0);

  AgentPermissionEngine_EndRun(&engine);
  AgentPermissionEngine_Close(&engine);
  ArenaRelease(&arena);
}

void TestHeadlessFailClosed() {
  Arena arena = ArenaCreate();
  PermissionTestState state = {};
  state.PromptAvailable = false;
  AgentAskUser ask = {&state, Ask};
  AgentPermissionEngine engine = {};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Ask,
                             SVL("C:\\work"), &ask, {});
  AgentPermissionEngine_BeginRun(&engine);

  Tool tool = SensitiveTool();
  AgentPermissionGate gate = Gate(&engine, &arena);
  AgentPermissionDecision decision = {};
  ToolArgs askArgs = Args(&arena, SVL("{\"command\":\"headless-ask\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &askArgs, &decision);
  CHECK(decision.Block);
  CHECK(StrView_Contains(decision.Reason, "permission_denied"));
  CHECK(StrView_Contains(
      decision.Reason, "No user is available to approve this action."));
  AgentPermissionEngine_Close(&engine);

  state = {};
  state.PromptAvailable = false;
  state.ReviewUnavailable = true;
  state.Rationale = {};
  AgentPermissionReviewer reviewer = {&state, Review};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Auto,
                             SVL("C:\\work"), &ask, reviewer);
  AgentPermissionEngine_BeginRun(&engine);
  ToolArgs autoArgs = Args(&arena, SVL("{\"command\":\"headless-auto\"}"));
  AgentPermission_Decide(&arena, &gate, &tool, &autoArgs, &decision);
  CHECK(decision.Block);
  CHECK(RequestId(&arena, decision.Reason).Length == DURABLE_ID_CHAR_COUNT);
  CHECK(StrView_Contains(decision.Reason, "Call ask_user"));
  CHECK(StrView_Contains(
      decision.Reason,
      "Automatic permission review was unavailable or invalid."));

  AgentPermissionEngine_EndRun(&engine);
  AgentPermissionEngine_Close(&engine);
  ArenaRelease(&arena);
}

void TestReviewerAbort() {
  Arena arena = ArenaCreate();
  AbortReviewState reviewState = {};
  AgentPermissionReviewer reviewer = {&reviewState, AbortReview};
  AgentPermissionEngine engine = {};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Auto,
                             SVL("C:\\work"), nullptr, reviewer);
  AgentPermissionEngine_BeginRun(&engine);

  volatile i32 abort = 0;
  Tool tool = SensitiveTool();
  DecisionThread decisionThread = {};
  decisionThread.Engine = &engine;
  decisionThread.Tool = &tool;
  decisionThread.ArgsJson = SVL("{\"command\":\"abort-review\"}");
  decisionThread.Abort = &abort;
  Thread thread = {};
  bool started = Thread_Start(&thread, DecideThread, &decisionThread);
  CHECK(started);
  if (started) {
    u32 waited = 0;
    while (Atomic_LoadI32(&reviewState.Entered) == 0 && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(Atomic_LoadI32(&reviewState.Entered) != 0);
    Atomic_StoreI32(&abort, 1);
    Thread_Join(&thread);
    CHECK(decisionThread.Decision.Block);
    CHECK(RequestId(&decisionThread.Arena, decisionThread.Decision.Reason)
              .Length == DURABLE_ID_CHAR_COUNT);
    CHECK(StrView_Contains(decisionThread.Decision.Reason, "Call ask_user"));
    ArenaRelease(&decisionThread.Arena);
  }

  AgentPermissionEngine_EndRun(&engine);
  AgentPermissionEngine_Close(&engine);
  ArenaRelease(&arena);
}

void TestConcurrentDecisions() {
  constexpr u32 WORKERS = 12;
  constexpr u32 UNIQUE_CALLS = 4;
  Arena arena = ArenaCreate();
  AgentAskUser ask = {nullptr, ConcurrentAlwaysAsk};
  AgentPermissionEngine engine = {};
  AgentPermissionEngine_Init(&engine, AgentPermissionMode::Ask,
                             SVL("C:\\work"), &ask, {});
  AgentPermissionEngine_BeginRun(&engine);

  Tool tool = SensitiveTool();
  DecisionThread decisions[WORKERS] = {};
  Thread threads[WORKERS] = {};
  u32 started = 0;
  for (u32 i = 0; i < WORKERS; i++) {
    StrBuf json = StrBuf_Make(&arena, 24);
    StrBuf_AppendCStr(&json, "{\"command\":\"parallel-");
    StrBuf_AppendU64(&json, i % UNIQUE_CALLS);
    StrBuf_AppendCStr(&json, "\"}");
    decisions[i].Engine = &engine;
    decisions[i].Tool = &tool;
    decisions[i].ArgsJson = StrBuf_View(&json);
    bool ok = Thread_Start(&threads[i], DecideThread, &decisions[i]);
    if (ok) started++;
    CHECK(ok);
  }
  for (u32 i = 0; i < started; i++) Thread_Join(&threads[i]);

  CHECK(started == WORKERS);
  CHECK(AgentPermissionEngine_GrantCount(&engine) == UNIQUE_CALLS);
  for (u32 i = 0; i < started; i++) {
    CHECK(!decisions[i].Decision.Block);
    ArenaRelease(&decisions[i].Arena);
  }

  AgentPermissionEngine_EndRun(&engine);
  AgentPermissionEngine_Close(&engine);
  ArenaRelease(&arena);
}

void TestPermissionPromptModes() {
  Arena arena = ArenaCreate();
  StrView ask = AgentPermission_AppendSystemPrompt(
      &arena, SVL("base"), AgentPermissionMode::Ask);
  CHECK(StrView_Contains(ask, "Permission mode: ask."));
  CHECK(StrView_Contains(
      ask, "Inspection tools run directly; unresolved mutating actions require user approval."));
  CHECK(!StrView_Contains(ask, "permission_request_id"));
  CHECK(!StrView_Contains(ask, "built-in permission checks are disabled"));

  StrView autoPrompt = AgentPermission_AppendSystemPrompt(
      &arena, SVL("base"), AgentPermissionMode::Auto);
  CHECK(StrView_Contains(autoPrompt, "Permission mode: auto."));
  CHECK(StrView_Contains(autoPrompt, "permission_request_id"));
  CHECK(StrView_Contains(autoPrompt, "External PreToolUse"));
  CHECK(!StrView_Contains(autoPrompt, "Permission mode: ask."));
  CHECK(!StrView_Contains(autoPrompt, "built-in permission checks are disabled"));

  StrView yolo = AgentPermission_AppendSystemPrompt(
      &arena, SVL("base"), AgentPermissionMode::Yolo);
  CHECK(StrView_Contains(yolo, "Permission mode: yolo."));
  CHECK(StrView_Contains(yolo, "built-in permission checks are disabled"));
  CHECK(!StrView_Contains(yolo, "permission_request_id"));
  CHECK(!StrView_Contains(yolo, "Permission mode: ask."));
  CHECK(!StrView_Contains(yolo, "Permission mode: auto."));
  ArenaRelease(&arena);
}

bool CountExec(void* user, Arena* arena, StrView, const ToolArgs*,
               volatile i32*, const ToolUpdateSink*, ToolResult* out,
               StrBuf*) {
  (*(u32*)user)++;
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("ran")));
  return true;
}

void AdministrativeBlock(void*, Arena*, StrView, StrView, const ToolArgs*,
                         AgentToolGateDecision* out) {
  out->Block = true;
  out->Reason = SVL("administrative policy blocked this action");
}

void TestHarnessReviewerIsolation() {
  Arena arena = ArenaCreate();
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("permission-tests");
  create.WorkingDirectory = SVL("C:\\work");
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      repository.Create(repository.Ctx, &arena, &create, &log, &info)));

  ScriptToolCall actionCall[] = {
      {SVL("sensitive"), SVL("action-1"), SVL("{\"value\":1}")}};
  ScriptToolCall reviewCall[] = {
      {SVL("permission_decision"), SVL("review-1"),
       SVL("{\"risk\":\"low\",\"authorization\":\"high\","
           "\"decision\":\"allow\",\"rationale\":\"requested work\"}")}};
  ScriptStep steps[3] = {};
  steps[0].Tools = actionCall;
  steps[0].ToolCount = 1;
  steps[1].Tools = reviewCall;
  steps[1].ToolCount = 1;
  steps[2].Text = SVL("done");
  steps[2].Stop = StopReason::Stop;
  ScriptProvider script = {steps, 3, 0};

  u32 executions = 0;
  Tool tool = {};
  tool.Name = SVL("sensitive");
  tool.Description = SVL("sensitive action");
  tool.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
  tool.PermissionEffect = ToolPermissionEffect::Sensitive;
  tool.Ctx = &executions;
  tool.Execute = CountExec;

  AgentHarnessConfig config = {};
  config.SystemPrompt = SVL("system");
  config.ModelInfo.Id = SVL("chat");
  config.ModelInfo.Provider = SVL("script");
  config.Tools = &tool;
  config.ToolCount = 1;
  config.ProviderImpl = ScriptProvider_Make(&script);
  config.PermissionMode = AgentPermissionMode::Auto;
  config.PermissionReviewerModel.Id = SVL("reviewer");
  config.PermissionReviewerModel.Provider = SVL("script");
  config.WorkingDirectory = SVL("C:\\work");

  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
  AgentHarnessSnapshot before = {};
  AgentHarnessWatch watch = {};
  CHECK(SessionResult_IsOk(
      AgentHarness_Watch(&arena, &harness, 64, &before, &watch)));
  CHECK(SessionResult_IsOk(
      AgentHarness_Run(&harness, SVL("trusted requested work"))));
  CHECK_ALL(executions == 1, script.Index == 3);
  u32 reviewStarts = 0;
  u32 reviewEnds = 0;
  bool sawAgentEnd = false;
  while (!sawAgentEnd) {
    AgentHarnessEvent event = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_WatchNext(&arena, &watch, &event)));
    if (event.Kind ==
        AgentHarnessEventKind::TransientPermissionReviewStart)
      reviewStarts++;
    if (event.Kind == AgentHarnessEventKind::TransientPermissionReviewEnd) {
      reviewEnds++;
      CHECK(event.PermissionReview.Outcome ==
            AgentPermissionReviewOutcome::Allow);
    }
    if (event.Kind == AgentHarnessEventKind::TransientAgentEnd)
      sawAgentEnd = true;
  }
  CHECK_ALL(reviewStarts == 1, reviewEnds == 1);
  CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));

  AgentHarnessSnapshot snapshot = {};
  CHECK(SessionResult_IsOk(
      AgentHarness_SnapshotFull(&arena, &harness, &snapshot)));
  CHECK(snapshot.Transcript.Count == 4);
  bool reviewerPersisted = false;
  for (u32 i = 0; i < snapshot.Transcript.Count; i++) {
    const Message* message = &snapshot.Transcript.Data[i];
    for (u32 j = 0; j < message->Content.Count; j++)
      if (StrView_Eq(message->Content.Data[j].ToolName,
                     SVL("permission_decision")))
        reviewerPersisted = true;
  }
  CHECK(!reviewerPersisted);

  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));

  SessionLog yoloLog = {};
  CHECK(SessionResult_IsOk(
      repository.Create(repository.Ctx, &arena, &create, &yoloLog, &info)));
  ScriptStep yoloSteps[2] = {};
  yoloSteps[0].Tools = actionCall;
  yoloSteps[0].ToolCount = 1;
  yoloSteps[1].Text = SVL("handled block");
  yoloSteps[1].Stop = StopReason::Stop;
  ScriptProvider yoloScript = {yoloSteps, 2, 0};
  executions = 0;
  config.ProviderImpl = ScriptProvider_Make(&yoloScript);
  config.PermissionMode = AgentPermissionMode::Yolo;
  config.ToolHooks.BeforeTool = AdministrativeBlock;
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, yoloLog, &config)));
  CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("try it"))));
  CHECK_ALL(executions == 0, yoloScript.Index == 2);
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));

  SessionLog malformedLog = {};
  CHECK(SessionResult_IsOk(repository.Create(repository.Ctx, &arena, &create,
                                              &malformedLog, &info)));
  ScriptToolCall malformedReview[] = {
      {SVL("permission_decision"), SVL("review-bad"),
       SVL("{\"decision\":\"allow\"}")}};
  ScriptStep malformedSteps[3] = {};
  malformedSteps[0].Tools = actionCall;
  malformedSteps[0].ToolCount = 1;
  malformedSteps[1].Tools = malformedReview;
  malformedSteps[1].ToolCount = 1;
  malformedSteps[2].Text = SVL("replanned");
  malformedSteps[2].Stop = StopReason::Stop;
  ScriptProvider malformedScript = {malformedSteps, 3, 0};
  executions = 0;
  config.ProviderImpl = ScriptProvider_Make(&malformedScript);
  config.PermissionMode = AgentPermissionMode::Auto;
  config.ToolHooks = {};
  CHECK(
      SessionResult_IsOk(AgentHarness_Init(&harness, malformedLog, &config)));
  CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("try it safely"))));
  CHECK_ALL(executions == 0, malformedScript.Index == 3);
  AgentHarnessSnapshot malformedSnapshot = {};
  CHECK(SessionResult_IsOk(
      AgentHarness_SnapshotFull(&arena, &harness, &malformedSnapshot)));
  bool recoverableDenial = false;
  for (u32 i = 0; i < malformedSnapshot.Transcript.Count; i++) {
    const Message* message = &malformedSnapshot.Transcript.Data[i];
    if (message->Kind != Role::ToolResult) continue;
    for (u32 j = 0; j < message->Content.Count; j++)
      if (StrView_Contains(message->Content.Data[j].Text,
                           "permission_request_id"))
        recoverableDenial = true;
  }
  CHECK(recoverableDenial);
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));

  repository.Close(repository.Ctx);
  ArenaRelease(&arena);
}

}  // namespace

void TestPermissions() {
  TestModeHelpers();
  TestModesAndRecovery();
  TestLiveModeChangeAndFastPaths();
  TestGrantCapAndUnbindableKeys();
  TestHeadlessFailClosed();
  TestReviewerAbort();
  TestConcurrentDecisions();
  TestPermissionPromptModes();
  TestHarnessReviewerIsolation();
}
