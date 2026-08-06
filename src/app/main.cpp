#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/event.h"
#include "agent/event_json.h"
#include "agent/harness.h"
#include "app/command.h"
#include "app/plain_select.h"
#include "app/project_trust.h"
#include "app/prompt_context.h"
#include "app/provider_registry.h"
#include "app/settings.h"
#include "auth/oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/sysinfo.h"

#include "net/browser.h"
#include "net/http.h"
#include "term/term.h"
#include "tui/tui.h"
#include "term/sanitize.h"
#include "tools/tools.h"
#include "tools/fs.h"
#include "tools/filelock.h"
#include "session/jsonl_repository.h"
#include "session/memory_repository.h"

#if defined(_WIN32)
#include <windows.h>
static void EnableTerminal() {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD m = 0;
  if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  SetConsoleOutputCP(CP_UTF8);
}
#else
static void EnableTerminal() {}
#endif

static volatile i32 g_abort = 0;
static AgentHarness* g_activeHarness = nullptr;
static void OnSigint(int) {
  g_abort = 1;
  if (g_activeHarness) g_activeHarness->AbortFlag = 1;
}

static SessionResult RunHarnessPrompt(AgentHarness* harness, StrView prompt) {
  g_activeHarness = harness;
  SessionResult result = AgentHarness_Run(harness, prompt);
  g_activeHarness = nullptr;
  return result;
}

// ---- auth wiring ---------------------------------------------------------

struct AuthWarmup;
struct HarnessEventPump;

struct AppAuth {
  Arena* Mem;
  StrView Path;
  AuthTokenCache Cache;
  AuthWarmup* Warmup;
};

struct AuthWarmup {
  Thread Worker;
  Arena ResultMem;
  ResolvedAuthToken Result;
  StrView Path;
  StrView Provider;
  const char* EnvVar;
  volatile i32* Abort;
  bool Resolved;
  bool Started;
  bool Joined;
};

struct InteractiveRuntime {
  Arena* Mem;
  StrView AuthPath;
  StrView SettingsPath;
  const ProviderSpec* Spec;
  Model CurrentModel;
  ModelRequestProfile PreferredProfile;
  AgentHarness* Harness;
  SessionRepository* Sessions;
  StrView ProjectKey;
  StrView WorkingDirectory;
  StrView AgentDirectory;
  StrView TrustPath;
  bool NoSession;
  bool ProjectTrusted;
  bool HasProjectResources;
  bool TrustCoversProjectResources;
  ProjectTrustOverride TrustOverride;
  DefaultProjectTrust DefaultTrust;
  PromptContext* Prompt;
  PromptContextOptions PromptOptions;
  HarnessEventPump* EventPump;
  AgentSink EventSink;
 bool AutoOpen;
  Tui* TuiView;
  AppAuth* Auth;
  AuthWarmup* AuthWarmupState;
};

struct PromptContextLifetime {
  PromptContext* Context;
  ~PromptContextLifetime() { PromptContext_Release(Context); }
};

static void EmitPromptDiagnostics(const PromptContext* context,
                                  const AppCommandUx* ux);

static void AppAuth_Invalidate(AppAuth* auth) {
  if (auth) AuthTokenCache_Invalidate(&auth->Cache);
}

static bool AppAuth_CacheToken(AppAuth* auth, StrView provider,
                               const ResolvedAuthToken& token) {
  return auth && AuthTokenCache_Store(auth->Mem, &auth->Cache, provider,
                                      &token);
}

static bool AppAuth_CacheCredential(AppAuth* auth, StrView provider,
                                    const Credential& credential) {
  ResolvedAuthToken token = {};
  token.Kind = credential.Kind;
  token.AccountId = credential.AccountId;
  token.IsFedRamp = credential.IsFedRamp;
  if (credential.Kind == CredKind::ApiKey) {
    token.Value = credential.ApiKey;
  } else if (credential.Kind == CredKind::OAuth) {
    token.Value = credential.Access;
    token.ExpiresMs = credential.ExpiresMs;
  }
  return AppAuth_CacheToken(auth, provider, token);
}

static bool AppAuth_HasUsableCache(const AppAuth* auth, StrView provider) {
  ResolvedAuthToken ignored = {};
  return auth &&
         AuthTokenCache_Get(&auth->Cache, provider, NowMillis(), &ignored);
}

static ProviderAuth ToProviderAuth(const ResolvedAuthToken& token) {
  ProviderAuth result = {};
  result.Token = token.Value;
  result.AccountId = token.AccountId;
  result.IsFedRamp = token.IsFedRamp;
  if (token.Kind == CredKind::ApiKey)
    result.Kind = ProviderAuthKind::ApiKey;
  else if (token.Kind == CredKind::OAuth)
    result.Kind = ProviderAuthKind::OAuth;
  return result;
}

static void RunAuthWarmup(void* user) {
  AuthWarmup* warmup = (AuthWarmup*)user;
  warmup->Resolved = Auth_ResolveTokenInfo(
      &warmup->ResultMem, warmup->Path, warmup->Provider, warmup->EnvVar,
      &warmup->Result, warmup->Abort);
}

static bool StartAuthWarmup(AuthWarmup* warmup, AppAuth* auth,
                            const ProviderSpec* spec, volatile i32* abort) {
  if (!warmup || !auth || !spec) return false;
  *warmup = AuthWarmup{};
  warmup->Path = auth->Path;
  warmup->Provider = spec->Id;
  warmup->EnvVar = spec->ApiKeyEnvVar;
  warmup->Abort = abort;
  warmup->ResultMem = ArenaCreate((usize)16 << 20);
  auth->Warmup = warmup;
  if (!Thread_Start(&warmup->Worker, RunAuthWarmup, warmup)) {
    ArenaRelease(&warmup->ResultMem);
    auth->Warmup = nullptr;
    return false;
  }
  warmup->Started = true;
  return true;
}

static void JoinAuthWarmup(AuthWarmup* warmup, AppAuth* auth) {
  if (!warmup || !warmup->Started || warmup->Joined) return;
  Thread_Join(&warmup->Worker);
  warmup->Joined = true;
  if (auth && warmup->Resolved)
    AppAuth_CacheToken(auth, warmup->Provider, warmup->Result);
  ArenaRelease(&warmup->ResultMem);
  if (auth && auth->Warmup == warmup) auth->Warmup = nullptr;
}

static void CancelAuthWarmup(AuthWarmup* warmup, AppAuth* auth) {
  if (!warmup) return;
  if (warmup->Abort) *warmup->Abort = 1;
  JoinAuthWarmup(warmup, nullptr);
  if (auth && auth->Warmup == warmup) {
    auth->Warmup = nullptr;
  }
}

static void FinishRuntimeAuthWarmup(InteractiveRuntime* rt) {
  if (rt && rt->AuthWarmupState)
    JoinAuthWarmup(rt->AuthWarmupState, rt->Auth);
}

static void CancelRuntimeAuthWarmup(InteractiveRuntime* rt) {
  if (!rt || !rt->AuthWarmupState) return;
  CancelAuthWarmup(rt->AuthWarmupState, rt->Auth);
  rt->AuthWarmupState = nullptr;
}

static bool AppResolveAuth(void* user, Arena* arena, StrView provider, volatile i32* abort,
                           ProviderAuth* out) {
  if (out) *out = {};
  if (!out) return false;
  AppAuth* aa = (AppAuth*)user;
  AuthWarmup* warmup = aa->Warmup;
  if (warmup && StrView_Eq(warmup->Provider, provider))
    JoinAuthWarmup(warmup, aa);
  if (AppAuth_HasUsableCache(aa, provider)) {
    ResolvedAuthToken cached = {};
    AuthTokenCache_Get(&aa->Cache, provider, NowMillis(), &cached);
    *out = ToProviderAuth(cached);
    return true;
  }
  AppAuth_Invalidate(aa);
  const ProviderSpec* spec = ProviderRegistry_Find(provider);
  ResolvedAuthToken resolved = {};
  if (Auth_ResolveTokenInfo(arena, aa->Path, provider,
                            spec ? spec->ApiKeyEnvVar : nullptr, &resolved,
                            abort)) {
    AppAuth_CacheToken(aa, provider, resolved);
    if (AppAuth_HasUsableCache(aa, provider)) {
      ResolvedAuthToken cached = {};
      AuthTokenCache_Get(&aa->Cache, provider, NowMillis(), &cached);
      *out = ToProviderAuth(cached);
    } else {
      *out = ToProviderAuth(resolved);
    }
    return true;
  }
  return false;
}

static bool ResolvePromptAuth(AgentHarness* harness, volatile i32* abort) {
  if (!harness || (abort && *abort)) return false;
  if (!harness->Config.ResolveAuth)
    return harness->Config.Stream.Auth.Token.Length > 0;
  harness->Config.Stream.Auth = {};
  ProviderAuth auth = {};
  if (!harness->Config.ResolveAuth(harness->Config.AuthUser, &harness->Work,
                                   harness->Config.ModelInfo.Provider, abort,
                                   &auth))
    return false;
  if (abort && *abort) return false;
  harness->Config.Stream.Auth = auth;
  return true;
}

static bool ProviderCanLogin(const ProviderSpec* spec) {
  return spec && OAuth_GetConfig(spec->Id);
}

static bool ProviderIsSelectable(const ProviderSpec* spec, bool oauthOnly) {
  return spec && spec->CreateProvider && spec->CreateModel &&
         (!oauthOnly || ProviderCanLogin(spec));
}

static StrView AuthGuidance(Arena* arena, const ProviderSpec* spec, bool interactive) {
  if (!spec) return SVL("No provider is selected.");
  bool canLogin = ProviderCanLogin(spec);
  StrBuf b = StrBuf_Make(arena, 192);
  StrBuf_AppendCStr(&b, "No credentials are configured for ");
  StrBuf_Append(&b, spec->Name);
  StrBuf_AppendCStr(&b, " ('");
  StrBuf_Append(&b, spec->Id);
  StrBuf_AppendCStr(&b, "'). ");
  if (canLogin) {
    if (interactive) {
      StrBuf_AppendCStr(&b, "Run `/login` and choose ");
      StrBuf_Append(&b, spec->Name);
    } else {
      StrBuf_AppendCStr(&b, "Run `clank --login ");
      StrBuf_Append(&b, spec->Id);
      StrBuf_AppendCStr(&b, "`");
    }
    if (spec->ApiKeyEnvVar) StrBuf_AppendCStr(&b, " or ");
  }
  if (spec->ApiKeyEnvVar) {
    StrBuf_AppendCStr(&b, "set ");
    StrBuf_AppendCStr(&b, spec->ApiKeyEnvVar);
  }
  if (!canLogin && !spec->ApiKeyEnvVar)
    StrBuf_AppendCStr(&b, "Configure credentials for this provider");
  StrBuf_AppendCStr(&b, " before sending a prompt.");
  return StrBuf_View(&b);
}

struct RuntimeSelection {
  const ProviderSpec* Spec;
  StrView ModelId;
  ModelRequestProfile RequestedProfile;
  DefaultProjectTrust DefaultTrust;
  AgentCompactionPolicy Compaction;
};

static void WarnUsingDefaults(StrView path, const ProviderSpec* defaults, const char* reason) {
  fprintf(stderr,
          "warning: %s %.*s; using built-in defaults provider '%.*s', model '%.*s'\n",
          reason, (int)path.Length, path.Data, (int)defaults->Id.Length, defaults->Id.Data,
          (int)defaults->DefaultModel.Length, defaults->DefaultModel.Data);
}

static RuntimeSelection LoadRuntimeSelection(Arena* arena, StrView settingsPath,
                                             const char* modelOverride) {
  RuntimeSelection selected = {};
  const ProviderSpec* defaults = ProviderRegistry_At(0);
  if (!defaults) return selected;

  selected.Spec = defaults;
  selected.ModelId = defaults->DefaultModel;
  selected.DefaultTrust = DefaultProjectTrust::Ask;
  selected.Compaction = AgentCompactionPolicy_Default();

  AppSettings settings = {};
  AppSettingsLoadResult load = AppSettings_Load(arena, settingsPath, &settings);
  if (load == AppSettingsLoadResult::Success) {
    selected.DefaultTrust = settings.DefaultTrust;
    selected.Compaction = settings.Compaction;
    const ProviderSpec* configured = ProviderRegistry_Find(settings.Provider);
    if (configured) {
      selected.Spec = configured;
      selected.ModelId = settings.Model;
      selected.RequestedProfile = settings.Profile;
    } else {
      fprintf(stderr,
              "warning: settings %.*s names unknown provider '%.*s'; using built-in defaults "
              "provider '%.*s', model '%.*s'\n",
              (int)settingsPath.Length, settingsPath.Data, (int)settings.Provider.Length,
              settings.Provider.Data, (int)defaults->Id.Length, defaults->Id.Data,
              (int)defaults->DefaultModel.Length, defaults->DefaultModel.Data);
    }
  } else if (load == AppSettingsLoadResult::Malformed) {
    WarnUsingDefaults(settingsPath, defaults, "settings file is malformed:");
  } else if (load == AppSettingsLoadResult::ReadFailure) {
    WarnUsingDefaults(settingsPath, defaults, "could not read settings file:");
  }

  if (modelOverride) selected.ModelId = SV(modelOverride);
  return selected;
}

// ---- REPL renderer -------------------------------------------------------

static void PrintPreview(StrView t) {
  // Reused scratch (REPL events are emitted on a single thread).
  static Arena sa = ArenaCreate((usize)16 << 20);
  static StrBuf sb = StrBuf_Make(&sa, 256);
  StrBuf_Clear(&sb);
  Sanitize_Display(&sb, t);  // strip \r / ANSI / control bytes from tool output
  StrView s = StrBuf_View(&sb);
  u32 n = s.Length > 200 ? 200 : s.Length;
  fputs("\x1b[90m", stdout);
  for (u32 i = 0; i < n; i++) {
    char c = s.Data[i];
    putchar(c == '\n' ? ' ' : c);
  }
  if (s.Length > 200) fputs("\xe2\x80\xa6", stdout);
  fputs("\x1b[0m\n", stdout);
}

static void ReplSink(void* user, const AgentEvent& ev) {
  (void)user;
  switch (ev.Kind) {
    case AgentEventKind::MessageUpdate:
      if (ev.Stream.Kind == StreamEventKind::TextDelta) {
        fwrite(ev.Stream.Delta.Data, 1, ev.Stream.Delta.Length, stdout);
        fflush(stdout);
      }
      break;
    case AgentEventKind::ToolStart:
      printf("\n\x1b[36m\xe2\x9a\x99 %.*s\x1b[0m \x1b[90m%.*s\x1b[0m\n", (int)ev.ToolName.Length,
             ev.ToolName.Data, (int)ev.ToolArgsJson.Length, ev.ToolArgsJson.Data);
      break;
    case AgentEventKind::ToolEnd:
      if (ev.Result && ev.Result->Content.Count > 0) PrintPreview(ev.Result->Content.Data[0].Text);
      break;
    case AgentEventKind::MessageEnd:
      if (ev.Msg && ev.Msg->Kind == Role::Assistant) putchar('\n');
      break;
    case AgentEventKind::RetryStart:
      fprintf(stderr, "\nretrying %s in %u ms (retry %u of %u): %.*s\n",
              ev.Layer == RetryLayer::ProviderRequest
                  ? "provider request"
                  : "assistant turn",
              ev.RetryDelayMs, ev.RetryAttempt, ev.RetryMaxAttempts,
              (int)ev.RetryError.Length, ev.RetryError.Data);
      break;
    default:
      break;
  }
}

static void ReportHarnessResult(SessionResult result) {
  if (SessionResult_IsOk(result)) return;
  fprintf(stderr, "\nerror: %.*s\n", (int)result.Message.Length,
          result.Message.Data);
}

struct HarnessEventPump {
  AgentHarnessWatch Watch;
  AgentHarness* Harness;
  Tui* TuiView;
  AgentSink Sink;
  Arena CompletionMem;
  Array<StrView> LiveToolEnds;
  Thread Worker;
  Mutex WatchGuard;
  bool WatchGuardInitialized;
  bool StopRequested;
  volatile i32 Exited;
  bool Started;
  // `--mode json`: write raw watch events as NDJSON instead of translating them
  // into the human-facing AgentSink.
  bool JsonOutput;
};

// One event, one line, flushed immediately so a consumer reading the pipe sees
// progress as it happens. Events with no JSON representation emit nothing.
static void EmitHarnessEventJson(Arena* arena, const AgentHarnessEvent* event) {
  StrBuf line = StrBuf_Make(arena, 256);
  if (!AgentEventJson_Write(&line, event)) return;
  fwrite(line.Data, 1, line.Length, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static void ClearLiveToolEnds(HarnessEventPump* pump) {
  ArenaReset(&pump->CompletionMem);
  pump->LiveToolEnds = Array_Make<StrView>(&pump->CompletionMem, 16);
}

static void RememberLiveToolEnd(HarnessEventPump* pump, StrView toolCallId) {
  Array_Push(&pump->LiveToolEnds,
             StrView_Dup(&pump->CompletionMem, toolCallId));
}

static bool ConsumeLiveToolEnd(HarnessEventPump* pump, StrView toolCallId) {
  for (u32 i = 0; i < pump->LiveToolEnds.Count; i++) {
    if (!StrView_Eq(pump->LiveToolEnds.Data[i], toolCallId)) continue;
    for (u32 j = i + 1; j < pump->LiveToolEnds.Count; j++)
      pump->LiveToolEnds.Data[j - 1] = pump->LiveToolEnds.Data[j];
    pump->LiveToolEnds.Count--;
    if (pump->LiveToolEnds.Count == 0) ClearLiveToolEnds(pump);
    return true;
  }
  return false;
}

static void PumpHarnessEvent(HarnessEventPump* pump,
                             const AgentHarnessEvent* event) {
  AgentEvent translated = {};
  switch (event->Kind) {
    case AgentHarnessEventKind::TransientAgentStart:
      translated.Kind = AgentEventKind::AgentStart;
      AgentSink_Emit(pump->Sink, translated);
      break;
    case AgentHarnessEventKind::TransientAgentEnd:
      translated.Kind = AgentEventKind::AgentEnd;
      AgentSink_Emit(pump->Sink, translated);
      ClearLiveToolEnds(pump);
      break;
    case AgentHarnessEventKind::TransientTurnStart:
      translated.Kind = AgentEventKind::TurnStart;
      AgentSink_Emit(pump->Sink, translated);
      break;
    case AgentHarnessEventKind::TransientTurnEnd:
      translated.Kind = AgentEventKind::TurnEnd;
      AgentSink_Emit(pump->Sink, translated);
      break;
    case AgentHarnessEventKind::TransientStream:
      translated.Kind = AgentEventKind::MessageUpdate;
      translated.Stream = event->Stream;
      AgentSink_Emit(pump->Sink, translated);
      break;
    case AgentHarnessEventKind::TransientToolStart:
      translated.Kind = AgentEventKind::ToolStart;
      translated.ToolCallId = event->ToolCallId;
      translated.ToolName = event->ToolName;
      translated.ToolArgsJson = event->ToolArgsJson;
      AgentSink_Emit(pump->Sink, translated);
      break;
    case AgentHarnessEventKind::TransientToolUpdate:
    case AgentHarnessEventKind::TransientToolEnd:
      translated.Kind =
          event->Kind == AgentHarnessEventKind::TransientToolUpdate
              ? AgentEventKind::ToolUpdate
              : AgentEventKind::ToolEnd;
      translated.ToolCallId = event->ToolCallId;
      translated.ToolName = event->ToolName;
      translated.Result = &event->ToolResultValue;
      translated.IsError = event->IsError;
      AgentSink_Emit(pump->Sink, translated);
      if (event->Kind == AgentHarnessEventKind::TransientToolEnd)
        RememberLiveToolEnd(pump, event->ToolCallId);
      break;
    case AgentHarnessEventKind::TransientRetryStart:
    case AgentHarnessEventKind::TransientRetryEnd:
      translated.Kind =
          event->Kind == AgentHarnessEventKind::TransientRetryStart
              ? AgentEventKind::RetryStart
              : AgentEventKind::RetryEnd;
      translated.Layer = event->Layer;
      translated.RetryAttempt = event->RetryAttempt;
      translated.RetryMaxAttempts = event->RetryMaxAttempts;
      translated.RetryDelayMs = event->RetryDelayMs;
      translated.RetryError = event->RetryError;
      AgentSink_Emit(pump->Sink, translated);
      break;
    case AgentHarnessEventKind::TransientQueueUpdate:
      translated.Kind = AgentEventKind::QueueUpdate;
      translated.SteerCount = event->SteerCount;
      translated.FollowUpCount = event->FollowUpCount;
      translated.NextTurnCount = event->NextTurnCount;
      AgentSink_Emit(pump->Sink, translated);
      break;
    case AgentHarnessEventKind::DurableRecord: {
      const SessionRecord* record = &event->Record;
      if (record->Kind != SessionRecordKind::Message) break;
      if (record->MessageEntry.Value.Kind == Role::Assistant) {
        translated.Kind = AgentEventKind::MessageEnd;
        translated.Msg = &record->MessageEntry.Value;
        AgentSink_Emit(pump->Sink, translated);
      } else if (record->MessageEntry.Value.Kind == Role::ToolResult) {
        if (ConsumeLiveToolEnd(
                pump, record->MessageEntry.Value.ToolCallId))
          break;
        ToolResult toolResult = {};
        toolResult.Content = record->MessageEntry.Value.Content;
        translated.Kind = AgentEventKind::ToolEnd;
        translated.ToolCallId = record->MessageEntry.Value.ToolCallId;
        translated.Result = &toolResult;
        translated.IsError = record->MessageEntry.Value.IsError;
        AgentSink_Emit(pump->Sink, translated);
      }
      break;
    }
    case AgentHarnessEventKind::TransientCancelRequested:
    default:
      break;
  }
}

static void RunHarnessEventPump(void* user) {
  HarnessEventPump* pump = (HarnessEventPump*)user;
  Arena scratch = ArenaCreate((usize)16 << 20);
  for (;;) {
    ArenaReset(&scratch);
    AgentHarnessEvent event = {};
    SessionResult next =
        AgentHarness_WatchNext(&scratch, &pump->Watch, &event);
    if (next.Status == SessionStatus::WatcherLagged) {
      Mutex_Lock(&pump->WatchGuard);
      if (pump->StopRequested) {
        Mutex_Unlock(&pump->WatchGuard);
        break;
      }
      AgentHarness_WatchClose(&pump->Watch);
      AgentHarnessSnapshot snapshot = {};
      SessionResult watched =
          AgentHarness_Watch(&scratch, pump->Harness,
                             AGENT_HARNESS_MAX_WATCH_EVENTS, &snapshot,
                             &pump->Watch);
      ClearLiveToolEnds(pump);
      if (SessionResult_IsOk(watched) && pump->TuiView)
        Tui_ReplaceTranscript(pump->TuiView, snapshot.Transcript.Data,
                              snapshot.Transcript.Count);
      Mutex_Unlock(&pump->WatchGuard);
      if (!SessionResult_IsOk(watched)) break;
      continue;
    }
    if (!SessionResult_IsOk(next)) break;
    if (pump->JsonOutput)
      EmitHarnessEventJson(&scratch, &event);
    else
      PumpHarnessEvent(pump, &event);
  }
  ArenaRelease(&scratch);
  pump->Exited = 1;
}

static SessionResult StartHarnessEventPump(Arena* arena, AgentHarness* harness,
                                           AgentSink sink,
                                           Tui* tuiView,
                                           HarnessEventPump* pump,
                                           AgentHarnessSnapshot* snapshot,
                                           bool jsonOutput = false) {
  memset(pump, 0, sizeof(*pump));
  pump->Harness = harness;
  pump->TuiView = tuiView;
  pump->Sink = sink;
  pump->JsonOutput = jsonOutput;
  pump->CompletionMem = ArenaCreate((usize)4 << 20);
  ClearLiveToolEnds(pump);
  Mutex_Init(&pump->WatchGuard);
  pump->WatchGuardInitialized = true;
  SessionResult watched = AgentHarness_Watch(
      arena, harness, AGENT_HARNESS_MAX_WATCH_EVENTS, snapshot,
      &pump->Watch);
  if (SessionResult_IsOk(watched)) {
    if (!Thread_Start(&pump->Worker, RunHarnessEventPump, pump)) {
      AgentHarness_WatchClose(&pump->Watch);
      Mutex_Destroy(&pump->WatchGuard);
      ArenaRelease(&pump->CompletionMem);
      pump->WatchGuardInitialized = false;
      watched = SessionResult_Error(SessionStatus::IoError,
                                    SVL("failed to start session event pump"));
    } else {
      pump->Started = true;
    }
  }
  if (!SessionResult_IsOk(watched) && pump->WatchGuardInitialized) {
    Mutex_Destroy(&pump->WatchGuard);
    ArenaRelease(&pump->CompletionMem);
    pump->WatchGuardInitialized = false;
  }
  return watched;
}

static void StopHarnessEventPump(HarnessEventPump* pump) {
  if (!pump || !pump->Started) return;
  Mutex_Lock(&pump->WatchGuard);
  pump->StopRequested = true;
  AgentHarness_WatchRequestClose(&pump->Watch);
  Mutex_Unlock(&pump->WatchGuard);
  Thread_Join(&pump->Worker);
  Mutex_Lock(&pump->WatchGuard);
  if (pump->Watch.Initialized) AgentHarness_WatchClose(&pump->Watch);
  Mutex_Unlock(&pump->WatchGuard);
  Mutex_Destroy(&pump->WatchGuard);
  ArenaRelease(&pump->CompletionMem);
  pump->WatchGuardInitialized = false;
  pump->Started = false;
}

static void MaybeReportMem() {
  if (getenv("CLANK_PRINT_MEM")) {
    u64 b = PeakRssBytes();
    fprintf(stderr, "[peak RSS: %llu KB]\n", (unsigned long long)(b / 1024));
  }
}

// ---- command UX ----------------------------------------------------------

static void PlainCommandNotice(void*, StrView message) {
  printf("%.*s\n", (int)message.Length, message.Data);
}

static void PlainCommandError(void*, StrView message) {
  fprintf(stderr, "error: %.*s\n", (int)message.Length, message.Data);
}

static void TuiCommandNotice(void* user, StrView message) {
  Tui_CommandNotice((Tui*)user, message);
}

static void TuiCommandError(void* user, StrView message) {
  Tui_CommandError((Tui*)user, message);
}

static void EmitCommandNotice(const AppCommandUx* ux, StrView message) {
  if (message.Length && ux && ux->Notice) ux->Notice(ux->User, message);
}

static void EmitCommandError(const AppCommandUx* ux, StrView message) {
  if (message.Length && ux && ux->Error) ux->Error(ux->User, message);
}

static bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static StrView TrimAscii(StrView value) {
  u32 first = 0;
  while (first < value.Length && IsAsciiSpace(value.Data[first])) first++;
  u32 last = value.Length;
  while (last > first && IsAsciiSpace(value.Data[last - 1])) last--;
  return StrView{value.Data ? value.Data + first : nullptr, last - first};
}

enum class RuntimeInputResult : u8 { Confirmed, Back, Cancelled };

static RuntimeInputResult RuntimeSelect(InteractiveRuntime* rt, StrView title,
                                        const StrView* options, u32 optionCount,
                                        u32* selected,
                                        u32 initialSelection = 0) {
  if (rt->TuiView) {
    TuiModalResult result =
        Tui_Select(rt->TuiView, title, options, optionCount, selected,
                   initialSelection);
    if (result == TuiModalResult::Confirmed)
      return RuntimeInputResult::Confirmed;
    if (result == TuiModalResult::Back) return RuntimeInputResult::Back;
    return RuntimeInputResult::Cancelled;
  }
  return Plain_Select(title, options, optionCount, selected)
             ? RuntimeInputResult::Confirmed
             : RuntimeInputResult::Cancelled;
}

static RuntimeInputResult RuntimeReadText(InteractiveRuntime* rt, Arena* arena,
                                          StrView title, StrView prompt,
                                          StrView initial, StrView* out) {
  if (rt->TuiView) {
    StrView value = {};
    TuiModalResult result =
        Tui_ReadText(rt->TuiView, title, prompt, initial, &value);
    if (result == TuiModalResult::Back) return RuntimeInputResult::Back;
    if (result != TuiModalResult::Confirmed)
      return RuntimeInputResult::Cancelled;
    *out = StrView_Dup(arena, value);
    return RuntimeInputResult::Confirmed;
  }

  char buffer[1024];
  StrView value = {};
  if (!Plain_ReadText(prompt, buffer, (u32)sizeof(buffer), &value))
    return RuntimeInputResult::Cancelled;
  *out = StrView_Dup(arena, value);
  return RuntimeInputResult::Confirmed;
}

static volatile i32* RuntimeBeginOperation(InteractiveRuntime* rt, const AppCommandUx* ux,
                                           StrView label) {
  g_abort = 0;
  if (rt->TuiView) {
    Tui_BeginOperation(rt->TuiView, label);
    return Tui_OperationAbort(rt->TuiView);
  }
  EmitCommandNotice(ux, label);
  return &g_abort;
}

static void RuntimeSetOperationStatus(InteractiveRuntime* rt, const AppCommandUx* ux,
                                      StrView status) {
  if (rt->TuiView)
    Tui_SetOperationStatus(rt->TuiView, status);
  else
    EmitCommandNotice(ux, status);
}

static bool RuntimeEndOperation(InteractiveRuntime* rt) {
  if (rt->TuiView) return Tui_EndOperation(rt->TuiView);
  return g_abort != 0;
}

enum class RuntimeChoiceStatus : u8 { Selected, Back, Cancelled, Failed };

struct RuntimeProviderChoice {
  RuntimeChoiceStatus Status;
  const ProviderSpec* Spec;
  StrView Message;
};

static RuntimeProviderChoice ChooseProvider(InteractiveRuntime* rt, bool oauthOnly) {
  Arena scratch = ArenaCreate((usize)16 << 20);
  u32 count = 0;
  for (u32 i = 0; i < ProviderRegistry_Count(); i++) {
    const ProviderSpec* spec = ProviderRegistry_At(i);
    if (ProviderIsSelectable(spec, oauthOnly)) count++;
  }

  if (count == 0) {
    ArenaRelease(&scratch);
    return RuntimeProviderChoice{
        RuntimeChoiceStatus::Failed, nullptr,
        oauthOnly ? SVL("No registered providers support OAuth login.")
                  : SVL("No usable model providers are registered.")};
  }

  const ProviderSpec** specs = ArenaPushArray(&scratch, const ProviderSpec*, count);
  StrView* options = ArenaPushArray(&scratch, StrView, count);
  u32 initial = 0;
  u32 at = 0;
  for (u32 i = 0; i < ProviderRegistry_Count(); i++) {
    const ProviderSpec* spec = ProviderRegistry_At(i);
    if (!ProviderIsSelectable(spec, oauthOnly)) continue;
    specs[at] = spec;
    StrBuf label = StrBuf_Make(&scratch, spec->Name.Length + spec->Id.Length + 8);
    StrBuf_Append(&label, spec->Name);
    StrBuf_AppendCStr(&label, " (");
    StrBuf_Append(&label, spec->Id);
    StrBuf_AppendChar(&label, ')');
    options[at] = StrBuf_View(&label);
    if (rt->Spec && StrView_Eq(rt->Spec->Id, spec->Id)) initial = at;
    at++;
  }

  u32 selected = 0;
  StrView title = oauthOnly ? SVL("Choose a provider to log in")
                            : SVL("Choose a model provider");
  RuntimeInputResult result =
      RuntimeSelect(rt, title, options, count, &selected, initial);
  const ProviderSpec* spec =
      result == RuntimeInputResult::Confirmed ? specs[selected] : nullptr;
  ArenaRelease(&scratch);
  return RuntimeProviderChoice{result == RuntimeInputResult::Confirmed
                                   ? RuntimeChoiceStatus::Selected
                               : result == RuntimeInputResult::Back
                                   ? RuntimeChoiceStatus::Back
                                   : RuntimeChoiceStatus::Cancelled,
                               spec, StrView{}};
}

struct LoginReadyContext {
  InteractiveRuntime* Runtime;
  const AppCommandUx* Ux;
  Arena* Scratch;
};

static void OnLoginReady(void* user, StrView url) {
  LoginReadyContext* ready = (LoginReadyContext*)user;
  RuntimeSetOperationStatus(ready->Runtime, ready->Ux,
                            SVL("Waiting for browser approval; Ctrl+C cancels."));

  StrBuf message = StrBuf_Make(ready->Scratch, url.Length + 64);
  StrBuf_AppendCStr(&message, "Open this URL in your browser to approve access:\n");
  StrBuf_Append(&message, url);
  EmitCommandNotice(ready->Ux, StrBuf_View(&message));

  if (ready->Runtime->AutoOpen) {
    OpenBrowser(url);
    EmitCommandNotice(ready->Ux, SVL("Opened the authorization URL in your default browser."));
  }
}

static AppCommandHandlerResult RunOAuthLogin(InteractiveRuntime* rt, const ProviderSpec* spec,
                                             const AppCommandUx* ux) {
  CancelRuntimeAuthWarmup(rt);
  const OAuthConfig* cfg = spec ? OAuth_GetConfig(spec->Id) : nullptr;
  if (!cfg)
    return AppCommandHandlerResult{AppCommandHandlerStatus::Unsupported,
                                   SVL("OAuth login is unavailable for this provider.")};

  Arena scratch = ArenaCreate((usize)64 << 20);
  StrBuf label = StrBuf_Make(&scratch, spec->Name.Length + 24);
  StrBuf_AppendCStr(&label, "Logging in to ");
  StrBuf_Append(&label, spec->Name);
  volatile i32* abort = RuntimeBeginOperation(rt, ux, StrBuf_View(&label));
  RuntimeSetOperationStatus(rt, ux, SVL("Starting the local callback server."));

  OAuthBegin begin = OAuth_BeginAuthCode(&scratch, cfg);
  OAuthCode code = {};
  StrBuf error = StrBuf_Make(&scratch, 128);
  LoginReadyContext ready = {rt, ux, &scratch};
  if (!OAuth_WaitForCallback(&scratch, cfg, begin.AuthUrl, begin.State, &code,
                             abort, &error,
                             OnLoginReady, &ready)) {
    bool cancelled = (abort && *abort) || StrView_Contains(StrBuf_View(&error), "cancelled");
    AppCommandHandlerResult result = {};
    if (cancelled) {
      result.Status = AppCommandHandlerStatus::Cancelled;
      result.Message = SVL("Login cancelled.");
    } else {
      StrBuf message = StrBuf_Make(&scratch, error.Length + 24);
      StrBuf_AppendCStr(&message, "Login failed: ");
      StrBuf_Append(&message, StrBuf_View(&error));
      result.Status = AppCommandHandlerStatus::Failed;
      result.Message = StrView_Dup(rt->Mem, StrBuf_View(&message));
    }
    if (RuntimeEndOperation(rt))
      result = AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                       SVL("Login cancelled.")};
    ArenaRelease(&scratch);
    return result;
  }

  if (abort && *abort) {
    RuntimeEndOperation(rt);
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled, SVL("Login cancelled.")};
  }

  RuntimeSetOperationStatus(rt, ux, SVL("Exchanging the authorization code."));
  StrView state = code.State.Length ? code.State : begin.State;
  Credential credential = {};
  StrBuf_Clear(&error);
  if (!OAuth_ExchangeCode(&scratch, cfg, code.Code, state, begin.Verifier, &credential, &error,
                          abort)) {
    if (abort && *abort) {
      RuntimeEndOperation(rt);
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled, SVL("Login cancelled.")};
    }
    StrBuf message = StrBuf_Make(&scratch, error.Length + 24);
    StrBuf_AppendCStr(&message, "Login failed: ");
    StrBuf_Append(&message, StrBuf_View(&error));
    AppCommandHandlerResult result = {AppCommandHandlerStatus::Failed,
                                      StrView_Dup(rt->Mem, StrBuf_View(&message))};
    if (RuntimeEndOperation(rt))
      result = AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                       SVL("Login cancelled.")};
    ArenaRelease(&scratch);
    return result;
  }

  RuntimeSetOperationStatus(rt, ux, SVL("Saving credentials."));
  if (!AuthStore_Write(&scratch, rt->AuthPath, cfg->Id, &credential)) {
    StrBuf message = StrBuf_Make(&scratch, rt->AuthPath.Length + 48);
    StrBuf_AppendCStr(&message, "Failed to save credentials to ");
    StrBuf_Append(&message, rt->AuthPath);
    StrBuf_AppendChar(&message, '.');
    AppCommandHandlerResult result = {AppCommandHandlerStatus::Failed,
                                      StrView_Dup(rt->Mem, StrBuf_View(&message))};
    if (RuntimeEndOperation(rt))
      result = AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                       SVL("Login cancelled.")};
    ArenaRelease(&scratch);
    return result;
  }
  AppAuth_CacheCredential(rt->Auth, cfg->Id, credential);

  StrBuf message = StrBuf_Make(&scratch, spec->Name.Length + rt->AuthPath.Length + 48);
  StrBuf_AppendCStr(&message, "Logged in to ");
  StrBuf_Append(&message, spec->Name);
  StrBuf_AppendCStr(&message, ". Credentials saved to ");
  StrBuf_Append(&message, rt->AuthPath);
  StrBuf_AppendChar(&message, '.');
  AppCommandHandlerResult result = {AppCommandHandlerStatus::Success,
                                    StrView_Dup(rt->Mem, StrBuf_View(&message))};
  if (RuntimeEndOperation(rt))
    result = AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     SVL("Login cancelled.")};
  ArenaRelease(&scratch);
  return result;
}

struct RuntimeModelChoice {
  RuntimeChoiceStatus Status;
  Model Selected;
  StrView Message;
  ModelRequestProfile Profile;
};

static RuntimeModelChoice ChooseModelForProvider(InteractiveRuntime* rt,
                                                 const ProviderSpec* spec,
                                                 const AppCommandUx* ux) {
  if (!spec || !spec->CreateModel)
    return RuntimeModelChoice{RuntimeChoiceStatus::Failed, Model{},
                              SVL("The selected provider cannot create models.")};

  Arena scratch = ArenaCreate((usize)128 << 20);
  StrBuf label = StrBuf_Make(&scratch, spec->Name.Length + 32);
  StrBuf_AppendCStr(&label, "Discovering ");
  StrBuf_Append(&label, spec->Name);
  StrBuf_AppendCStr(&label, " models");
  volatile i32* abort = RuntimeBeginOperation(rt, ux, StrBuf_View(&label));

  RuntimeSetOperationStatus(rt, ux, SVL("Resolving provider credentials."));
  FinishRuntimeAuthWarmup(rt);
  ProviderAuth auth = {};
  AppResolveAuth(rt->Auth, &scratch, spec->Id, abort, &auth);
  ProviderModelList live = {};
  if (!(abort && *abort)) {
    RuntimeSetOperationStatus(rt, ux, SVL("Fetching the provider model catalog."));
    if (spec->ListModels) {
      live = spec->ListModels(&scratch, auth, abort);
    } else {
      live.Status = ProviderModelListStatus::Unavailable;
      live.Error = SVL("this provider does not support live model discovery");
    }
  }
  bool cancelled = RuntimeEndOperation(rt);

  if (cancelled || (abort && *abort)) {
    ArenaRelease(&scratch);
    return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{}, StrView{}};
  }

  ProviderModelList models = {};
  if (live.Status == ProviderModelListStatus::Success) {
    models = ProviderModelList_Merge(&scratch, live, spec->FallbackModels,
                                     spec->FallbackModelCount);
  } else {
    StrBuf message = StrBuf_Make(&scratch, spec->Name.Length + live.Error.Length + 144);
    StrBuf_AppendCStr(&message, "Live model discovery ");
    StrBuf_AppendCStr(&message, live.Status == ProviderModelListStatus::Failed
                                   ? "failed for "
                                   : "is unavailable for ");
    StrBuf_Append(&message, spec->Name);
    if (live.Error.Length) {
      StrBuf_AppendCStr(&message, ": ");
      StrBuf_Append(&message, live.Error);
    }
    if (spec->FallbackModelCount)
      StrBuf_AppendCStr(&message,
                       ". Using provider fallback models; manual model-ID entry is also available.");
    else
      StrBuf_AppendCStr(&message, ". Manual model-ID entry remains available.");
    if (live.Status == ProviderModelListStatus::Failed)
      EmitCommandError(ux, StrBuf_View(&message));
    else
      EmitCommandNotice(ux, StrBuf_View(&message));

    ProviderModelList empty = {};
    empty.Status = ProviderModelListStatus::Success;
    models = ProviderModelList_Merge(&scratch, empty, spec->FallbackModels,
                                     spec->FallbackModelCount);
  }

  u32 optionCount = models.Count + 1;
  StrView* options = ArenaPushArray(&scratch, StrView, optionCount);
  u32 initial = optionCount - 1;
  for (u32 i = 0; i < models.Count; i++) {
    const Model* model = &models.Models[i];
    StrBuf option = StrBuf_Make(&scratch, model->Name.Length + model->Id.Length + 8);
    if (model->Name.Length && !StrView_Eq(model->Name, model->Id)) {
      StrBuf_Append(&option, model->Name);
      StrBuf_AppendCStr(&option, " (");
      StrBuf_Append(&option, model->Id);
      StrBuf_AppendChar(&option, ')');
    } else {
      StrBuf_Append(&option, model->Id);
    }
    options[i] = StrBuf_View(&option);
    if (rt->Spec && StrView_Eq(rt->Spec->Id, spec->Id) &&
        StrView_Eq(rt->CurrentModel.Id, model->Id))
      initial = i;
    else if ((!rt->Spec || !StrView_Eq(rt->Spec->Id, spec->Id)) &&
             StrView_Eq(spec->DefaultModel, model->Id))
      initial = i;
  }
  options[models.Count] = SVL("Enter a model ID manually...");

  StrBuf title = StrBuf_Make(&scratch, spec->Name.Length + 24);
  StrBuf_AppendCStr(&title, "Choose ");
  StrBuf_Append(&title, spec->Name);
  StrBuf_AppendCStr(&title, " model");

  Model chosen = {};
  ModelRequestProfile chosenProfile = {};
  for (;;) {
    u32 selected = 0;
    RuntimeInputResult selection =
        RuntimeSelect(rt, StrBuf_View(&title), options, optionCount, &selected,
                      initial);
    if (selection == RuntimeInputResult::Back) {
      ArenaRelease(&scratch);
      return RuntimeModelChoice{RuntimeChoiceStatus::Back, Model{}, StrView{}};
    }
    if (selection != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{},
                                StrView{}};
    }
    initial = selected;

    if (selected != models.Count) {
      chosen = Model_Dup(rt->Mem, models.Models[selected]);
    } else {
      StrView initialId =
          rt->Spec && StrView_Eq(rt->Spec->Id, spec->Id)
              ? rt->CurrentModel.Id
              : spec->DefaultModel;
      bool returnToModels = false;
      for (;;) {
        StrView entered = {};
        RuntimeInputResult entry =
            RuntimeReadText(rt, &scratch, StrBuf_View(&title), SVL("Model ID"),
                            initialId, &entered);
        if (entry == RuntimeInputResult::Back) {
          returnToModels = true;
          break;
        }
        if (entry != RuntimeInputResult::Confirmed) {
          ArenaRelease(&scratch);
          return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{},
                                    StrView{}};
        }
        entered = TrimAscii(entered);
        if (entered.Length) {
          chosen = spec->CreateModel(rt->Mem, entered);
          break;
        }
        EmitCommandError(ux, SVL("A model ID is required."));
      }
      if (returnToModels) continue;
    }

    chosen.Provider = StrView_Dup(rt->Mem, spec->Id);
    if (!chosen.Name.Length) chosen.Name = chosen.Id;
    if (!Model_MetadataIsValid(&chosen)) {
      ArenaRelease(&scratch);
      return RuntimeModelChoice{
          RuntimeChoiceStatus::Failed, Model{},
          SVL("The selected model has invalid capability metadata.")};
    }
    bool sameModel =
        rt->Spec && StrView_Eq(rt->Spec->Id, spec->Id) &&
        StrView_Eq(rt->CurrentModel.Id, chosen.Id);
    ModelRequestProfile activeProfile =
        sameModel ? rt->Harness->Config.Stream.Profile
                  : Model_DefaultRequestProfile(&chosen);
    ModelRequestProfile preference = {};

    if (Model_EffortOptionCount(&chosen) > 1) {
      StrView effortOptions[MODEL_EFFORT_OPTION_CAPACITY] = {};
      ModelEffort effortValues[MODEL_EFFORT_OPTION_CAPACITY] = {};
      u32 effortCount = 0;
      u32 effortInitial = 0;
      for (u8 i = (u8)ModelEffort::Low; i <= (u8)ModelEffort::Ultra; i++) {
        ModelEffort effort = (ModelEffort)i;
        if (!Model_SupportsEffort(&chosen, effort)) continue;
        effortOptions[effortCount] = ModelEffort_Name(effort);
        effortValues[effortCount] = effort;
        if (activeProfile.Effort == effort)
          effortInitial = effortCount;
        effortCount++;
      }
      u32 effortSelected = 0;
      RuntimeInputResult effortResult =
          RuntimeSelect(rt, SVL("Choose model effort"), effortOptions,
                        effortCount, &effortSelected, effortInitial);
      if (effortResult == RuntimeInputResult::Back) continue;
      if (effortResult != RuntimeInputResult::Confirmed) {
        ArenaRelease(&scratch);
        return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{},
                                  StrView{}};
      }
      preference.Effort = effortValues[effortSelected];
    }

    if (chosen.ContextWindowOptionCount > 1) {
      char contextStorage[MODEL_CONTEXT_WINDOW_OPTION_CAPACITY]
                         [MODEL_CONTEXT_WINDOW_LABEL_CAPACITY] = {};
      StrView contextOptions[MODEL_CONTEXT_WINDOW_OPTION_CAPACITY] = {};
      u32 contextInitial = 0;
      for (u32 i = 0; i < chosen.ContextWindowOptionCount; i++) {
        u32 length = ModelContextWindow_Format(
            contextStorage[i], MODEL_CONTEXT_WINDOW_LABEL_CAPACITY,
            chosen.ContextWindowOptions[i]);
        contextOptions[i] = StrView{contextStorage[i], length};
        if (activeProfile.ContextWindow ==
            chosen.ContextWindowOptions[i])
          contextInitial = i;
      }
      u32 contextSelected = 0;
      RuntimeInputResult contextResult =
          RuntimeSelect(rt, SVL("Choose model context window"),
                        contextOptions, chosen.ContextWindowOptionCount,
                        &contextSelected, contextInitial);
      if (contextResult == RuntimeInputResult::Back) continue;
      if (contextResult != RuntimeInputResult::Confirmed) {
        ArenaRelease(&scratch);
        return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{},
                                  StrView{}};
      }
      preference.ContextWindow =
          chosen.ContextWindowOptions[contextSelected];
    }

    chosenProfile = preference;
    break;
  }

  ArenaRelease(&scratch);
  RuntimeModelChoice result = {
      RuntimeChoiceStatus::Selected, chosen, StrView{}};
  result.Profile = chosenProfile;
  return result;
}

static bool RuntimeCheckAuth(InteractiveRuntime* rt, const AppCommandUx* ux, bool asError) {
 FinishRuntimeAuthWarmup(rt);
  volatile i32* abort =
      rt->TuiView ? Tui_OperationAbort(rt->TuiView) : &g_abort;
  if (abort && *abort) return false;
  if (rt->Harness && ResolvePromptAuth(rt->Harness, abort)) return true;
  if (abort && *abort) return false;

  Arena scratch = ArenaCreate((usize)4 << 20);
  bool interactive = rt->TuiView || Term_IsInteractive();
  StrView guidance = AuthGuidance(&scratch, rt->Spec, interactive);
  if (asError)
    EmitCommandError(ux, guidance);
  else
    EmitCommandNotice(ux, guidance);
  ArenaRelease(&scratch);
  return false;
}

static bool PersistRuntimeSelection(InteractiveRuntime* rt, const AppCommandUx* ux) {
  Arena scratch = ArenaCreate((usize)8 << 20);
  AppSettings settings = {};
  settings.Provider = rt->Spec->Id;
  settings.Model = rt->CurrentModel.Id;
  settings.DefaultTrust = rt->DefaultTrust;
  settings.Profile = rt->PreferredProfile;
  settings.Compaction = rt->Harness->Config.Compaction;
  if (!AppSettings_Save(&scratch, rt->SettingsPath, &settings)) {
    StrBuf message = StrBuf_Make(&scratch, rt->SettingsPath.Length + 96);
    StrBuf_AppendCStr(
        &message,
        "The model configuration is active, but settings could not be saved to ");
    StrBuf_Append(&message, rt->SettingsPath);
    StrBuf_AppendChar(&message, '.');
    EmitCommandError(ux, StrBuf_View(&message));
    ArenaRelease(&scratch);
    return false;
  }
  ArenaRelease(&scratch);
  return true;
}

static bool ActivateRuntimeSelection(InteractiveRuntime* rt, const ProviderSpec* spec,
                                     const Model& model,
                                     ModelRequestProfile preference,
                                     const AppCommandUx* ux,
                                     bool* persisted = nullptr) {
  if (!rt->Harness || !spec || !spec->CreateProvider || !model.Id.Length ||
      !Model_MetadataIsValid(&model))
    return false;

  preference = Model_NormalizeRequestPreference(&model, preference);
  ModelRequestProfile activeProfile =
      Model_ResolveRequestProfile(&model, preference);
  Provider provider = spec->CreateProvider(rt->Mem);
  SessionResult changed =
      AgentHarness_SetModel(rt->Harness, model, provider, activeProfile);
  if (!SessionResult_IsOk(changed)) {
    EmitCommandError(ux, changed.Message);
    return false;
  }
  bool providerChanged = !rt->Spec || !StrView_Eq(rt->Spec->Id, spec->Id);
  rt->Spec = spec;
  rt->CurrentModel = model;
  rt->PreferredProfile = preference;
  if (providerChanged &&
      (!rt->Auth ||
       !StrView_Eq(rt->Auth->Cache.Provider, spec->Id)))
    AppAuth_Invalidate(rt->Auth);
  if (rt->TuiView)
    Tui_SetModelProfile(rt->TuiView, model.Id, activeProfile);
  bool saved = PersistRuntimeSelection(rt, ux);
  if (persisted) *persisted = saved;
  RuntimeCheckAuth(rt, ux, false);
  return true;
}

static StrView SelectionMessage(InteractiveRuntime* rt, const char* prefix) {
  ModelRequestProfile profile = rt->Harness->Config.Stream.Profile;
  StrBuf message =
      StrBuf_Make(rt->Mem, rt->Spec->Name.Length +
                               rt->CurrentModel.Id.Length + 96);
  StrBuf_AppendCStr(&message, prefix);
  StrBuf_Append(&message, rt->Spec->Name);
  StrBuf_AppendCStr(&message, " model '");
  StrBuf_Append(&message, rt->CurrentModel.Id);
  StrBuf_AppendChar(&message, '\'');
  if (profile.Effort != ModelEffort::None) {
    StrBuf_AppendCStr(&message, " at ");
    StrBuf_Append(&message, ModelEffort_Name(profile.Effort));
    StrBuf_AppendCStr(&message, " effort");
  }
  if (profile.ContextWindow) {
    char context[MODEL_CONTEXT_WINDOW_LABEL_CAPACITY];
    u32 length = ModelContextWindow_Format(
        context, sizeof(context), profile.ContextWindow);
    StrBuf_AppendCStr(&message, " with ");
    StrBuf_Append(&message, StrView{context, length});
    StrBuf_AppendCStr(&message, " context");
  }
  StrBuf_AppendChar(&message, '.');
  return StrBuf_View(&message);
}

static AppCommandHandlerResult ReturnToCommandPalette(InteractiveRuntime* rt) {
  if (rt->TuiView) Tui_OpenCommandPalette(rt->TuiView);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success, StrView{}};
}

static AppCommandHandlerResult HandleModelCommand(void* user, const AppCommandUx* ux) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  for (;;) {
    RuntimeProviderChoice provider = ChooseProvider(rt, false);
    if (provider.Status == RuntimeChoiceStatus::Back)
      return ReturnToCommandPalette(rt);
    if (provider.Status == RuntimeChoiceStatus::Cancelled)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     StrView{}};
    if (provider.Status == RuntimeChoiceStatus::Failed)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     provider.Message};

    RuntimeModelChoice model = ChooseModelForProvider(rt, provider.Spec, ux);
    if (model.Status == RuntimeChoiceStatus::Back) continue;
    if (model.Status == RuntimeChoiceStatus::Cancelled)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     StrView{}};
    if (model.Status == RuntimeChoiceStatus::Failed)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     model.Message};
    if (!ActivateRuntimeSelection(rt, provider.Spec, model.Selected,
                                  model.Profile, ux))
      return AppCommandHandlerResult{
          AppCommandHandlerStatus::Failed,
          SVL("The selected model could not be activated.")};

    return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                   SelectionMessage(rt, "Using ")};
  }
}

static AppCommandHandlerResult HandleLoginCommand(void* user, const AppCommandUx* ux) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  const ProviderSpec* authenticated = nullptr;
  for (;;) {
    RuntimeProviderChoice provider = ChooseProvider(rt, true);
    if (provider.Status == RuntimeChoiceStatus::Back)
      return ReturnToCommandPalette(rt);
    if (provider.Status == RuntimeChoiceStatus::Cancelled)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     StrView{}};
    if (provider.Status == RuntimeChoiceStatus::Failed)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     provider.Message};

    if (authenticated != provider.Spec) {
      AppCommandHandlerResult login = RunOAuthLogin(rt, provider.Spec, ux);
      if (login.Status != AppCommandHandlerStatus::Success) return login;
      authenticated = provider.Spec;
    }

    RuntimeModelChoice model = ChooseModelForProvider(rt, provider.Spec, ux);
    if (model.Status == RuntimeChoiceStatus::Back) continue;
    if (model.Status != RuntimeChoiceStatus::Selected) {
      if (model.Status == RuntimeChoiceStatus::Failed && model.Message.Length)
        EmitCommandError(ux, model.Message);
      Model fallback =
          provider.Spec->CreateModel(rt->Mem, provider.Spec->DefaultModel);
      bool persisted = false;
      if (!ActivateRuntimeSelection(rt, provider.Spec, fallback,
                                    ModelRequestProfile{}, ux, &persisted))
        return AppCommandHandlerResult{
            AppCommandHandlerStatus::Failed,
            SVL("Login succeeded, but the provider could not be activated.")};

      StrBuf notice =
          StrBuf_Make(rt->Mem, provider.Spec->Name.Length +
                                  provider.Spec->DefaultModel.Length + 96);
      StrBuf_AppendCStr(&notice,
                       model.Status == RuntimeChoiceStatus::Cancelled
                           ? "Model selection was cancelled; switched to "
                           : "Model selection failed; switched to ");
      StrBuf_Append(&notice, provider.Spec->Name);
      StrBuf_AppendCStr(&notice, " default model '");
      StrBuf_Append(&notice, provider.Spec->DefaultModel);
      StrBuf_AppendCStr(
          &notice, persisted ? "' and saved the provider selection."
                             : "' in memory; settings were not saved.");
      EmitCommandNotice(ux, StrBuf_View(&notice));
    } else if (!ActivateRuntimeSelection(rt, provider.Spec, model.Selected,
                                         model.Profile, ux)) {
      return AppCommandHandlerResult{
          AppCommandHandlerStatus::Failed,
          SVL("Login succeeded, but the selected model could not be activated.")};
    }

    return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                   SelectionMessage(rt, "Logged in; using ")};
  }
}

static AppCommandHandlerResult CommandResult(SessionResult result,
                                             StrView success) {
  return AppCommandHandlerResult{
      SessionResult_IsOk(result) ? AppCommandHandlerStatus::Success
                                 : AppCommandHandlerStatus::Failed,
      SessionResult_IsOk(result) ? success : result.Message};
}

static bool RuntimeRestartWatch(InteractiveRuntime* rt,
                                const AppCommandUx* ux) {
  if (!rt->EventPump) return true;
  AgentHarnessSnapshot snapshot = {};
  SessionResult watched =
      StartHarnessEventPump(rt->Mem, rt->Harness, rt->EventSink,
                            rt->TuiView, rt->EventPump, &snapshot);
  if (!SessionResult_IsOk(watched)) {
    EmitCommandError(ux, watched.Message);
    return false;
  }
  if (rt->TuiView)
    Tui_ReplaceTranscript(rt->TuiView, snapshot.Transcript.Data,
                          snapshot.Transcript.Count);
  return true;
}

// One project-trust menu, shared by startup, `/trust`, and cross-project
// resume. The session-only choices apply to the running process instead of
// being written to the trust store.
enum class TrustChoice : u8 {
  TrustCurrent,
  TrustParent,
  TrustSession,
  UntrustCurrent,
  UntrustSession,
};

constexpr u32 TRUST_CHOICE_CAPACITY = 5;

// Writes the menu into `outOptions`/`outChoices` (each at least
// TRUST_CHOICE_CAPACITY long) and returns the number of entries. `/trust`
// exists to persist a decision, so it omits the session-only choices.
static u32 BuildTrustChoices(Arena* arena, StrView parent, bool includeSessionOnly,
                             StrView* outOptions, TrustChoice* outChoices) {
  u32 count = 0;
  outOptions[count] = SVL("Trust this folder");
  outChoices[count++] = TrustChoice::TrustCurrent;
  if (parent.Length) {
    StrBuf parentOption = StrBuf_Make(arena, parent.Length + 32);
    StrBuf_AppendCStr(&parentOption, "Trust parent folder: ");
    StrBuf_Append(&parentOption, parent);
    outOptions[count] = StrBuf_View(&parentOption);
    outChoices[count++] = TrustChoice::TrustParent;
  }
  if (includeSessionOnly) {
    outOptions[count] = SVL("Trust for this session only");
    outChoices[count++] = TrustChoice::TrustSession;
  }
  outOptions[count] = SVL("Do not trust this folder");
  outChoices[count++] = TrustChoice::UntrustCurrent;
  if (includeSessionOnly) {
    outOptions[count] = SVL("Do not trust for this session only");
    outChoices[count++] = TrustChoice::UntrustSession;
  }
  return count;
}

struct TrustDecision {
  bool Trusted;
  bool Persist;  // false for the session-only choices
  StrView DecisionPath;
  StrView RemovePath;  // a parent-scoped decision supersedes any child entry
};

static TrustDecision ResolveTrustChoice(TrustChoice choice, StrView cwd, StrView parent) {
  TrustDecision decision = {};
  decision.Trusted = choice == TrustChoice::TrustCurrent ||
                     choice == TrustChoice::TrustParent ||
                     choice == TrustChoice::TrustSession;
  decision.Persist =
      choice != TrustChoice::TrustSession && choice != TrustChoice::UntrustSession;
  bool parentScoped = choice == TrustChoice::TrustParent;
  decision.DecisionPath = parentScoped ? parent : cwd;
  decision.RemovePath = parentScoped ? cwd : StrView{};
  return decision;
}

static SessionResult ResolveRuntimeProjectTrust(
    InteractiveRuntime* rt, Arena* scratch, StrView workingDirectory,
    bool* trusted, bool* hasResources) {
  *hasResources =
      ProjectTrust_HasGatedResources(scratch, workingDirectory);
  if (!*hasResources) {
    *trusted = true;
    return SessionResult_Ok();
  }
  if (rt->TrustOverride != ProjectTrustOverride::Unspecified) {
    *trusted = rt->TrustOverride == ProjectTrustOverride::Trusted;
    return SessionResult_Ok();
  }

  ProjectTrustStore store = {};
  ProjectTrustStoreLoadResult loaded =
      ProjectTrustStore_Load(scratch, rt->TrustPath, &store);
  if (loaded == ProjectTrustStoreLoadResult::Malformed)
    return SessionResult_Error(SessionStatus::Corrupt,
                               SVL("the project trust store is malformed"));
  if (loaded == ProjectTrustStoreLoadResult::ReadFailure)
    return SessionResult_Error(SessionStatus::IoError,
                               SVL("the project trust store could not be read"));

  StrView cwd = ProjectTrust_CanonicalPath(scratch, workingDirectory);
  bool interactive = rt->TuiView || Term_IsInteractive();
  ProjectTrustResolution resolution = ProjectTrust_Resolve(
      true, rt->TrustOverride, &store, cwd, rt->DefaultTrust, interactive);
  if (!resolution.NeedsPrompt) {
    *trusted = resolution.Trusted;
    return SessionResult_Ok();
  }

  StrView parent = ProjectTrust_ParentPath(scratch, cwd);
  StrView options[TRUST_CHOICE_CAPACITY] = {};
  TrustChoice actions[TRUST_CHOICE_CAPACITY] = {};
  u32 count = BuildTrustChoices(scratch, parent, true, options, actions);

  StrBuf title = StrBuf_Make(scratch, cwd.Length + 96);
  StrBuf_AppendCStr(
      &title,
      "The selected project contains prompt files that can replace system instructions:\n");
  StrBuf_Append(&title, cwd);
  u32 selected = 0;
  RuntimeInputResult input =
      RuntimeSelect(rt, StrBuf_View(&title), options, count, &selected);
  if (input != RuntimeInputResult::Confirmed)
    return SessionResult_Error(SessionStatus::Cancelled,
                               SVL("project trust selection was cancelled"));

  TrustDecision decision = ResolveTrustChoice(actions[selected], cwd, parent);
  *trusted = decision.Trusted;
  if (!decision.Persist) return SessionResult_Ok();

  StrBuf error = StrBuf_Make(scratch, 128);
  if (!ProjectTrustStore_Update(scratch, rt->TrustPath, decision.DecisionPath,
                                decision.Trusted, decision.RemovePath, &error))
    return SessionResult_Error(SessionStatus::IoError,
                               StrView_Dup(rt->Mem, StrBuf_View(&error)));
  return SessionResult_Ok();
}

static bool RuntimeRestorePreviousSession(
    InteractiveRuntime* rt, DurableId previousSessionId,
    const AgentHarnessConfig* config, const AppCommandUx* ux) {
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionLog log = {};
  SessionInfo info = {};
  SessionOpenOptions open = {};
  open.SessionId = previousSessionId;
  open.ProjectKey = rt->ProjectKey;
  open.Writable = true;
  SessionResult restored =
      rt->Sessions->Open(rt->Sessions->Ctx, &scratch, &open, &log, &info);
  if (SessionResult_IsOk(restored)) {
    restored = AgentHarness_Init(rt->Harness, log, config);
  }
  bool ok = SessionResult_IsOk(restored) && RuntimeRestartWatch(rt, ux);
  ArenaRelease(&scratch);
  return ok;
}

static bool RuntimeRollbackSessionSwitch(
    InteractiveRuntime* rt, StrView previousWorkingDirectory,
    DurableId previousSessionId, const AgentHarnessConfig* previousConfig,
    const AppCommandUx* ux) {
  if (!Sys_SetCurrentDir(previousWorkingDirectory))
    return false;
  bool restored = RuntimeRestorePreviousSession(
      rt, previousSessionId, previousConfig, ux);
  if (restored && rt->TuiView)
    Tui_LoadStartupContext(rt->TuiView);
  return restored;
}

static SessionResult RuntimeSwitchToLog(InteractiveRuntime* rt,
                                        SessionLog target,
                                        const SessionInfo* targetInfo,
                                        const SessionReduction* targetState,
                                        const AppCommandUx* ux) {
  if (!targetInfo || !targetInfo->WorkingDirectory.Length ||
      !targetInfo->ProjectKey.Length) {
    SessionLog_Close(&target);
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("target session metadata is incomplete"));
  }
  if (!Fs_IsDir(targetInfo->WorkingDirectory)) {
    SessionLog_Close(&target);
    return SessionResult_Error(
        SessionStatus::NotFound,
        SVL("the session working directory no longer exists"));
  }

  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionReduction previousState = {};
  SessionResult previous =
      AgentHarness_Snapshot(&scratch, rt->Harness, &previousState);
  if (!SessionResult_IsOk(previous) || previousState.RecordCount == 0) {
    SessionLog_Close(&target);
    ArenaRelease(&scratch);
    return SessionResult_IsOk(previous)
               ? SessionResult_Error(SessionStatus::InvalidState,
                                     SVL("current session has no durable root"))
               : previous;
  }
  DurableId previousSessionId = previousState.Records[0].SessionId;
  AgentHarnessConfig previousConfig = rt->Harness->Config;
  AgentHarnessConfig config = previousConfig;
  StrView previousWorkingDirectory = rt->WorkingDirectory;
  bool switchingProject =
      !StrView_Eq(targetInfo->ProjectKey, rt->ProjectKey) ||
      !StrView_Eq(targetInfo->WorkingDirectory, rt->WorkingDirectory);
  PromptContext nextPrompt = {};
  PromptContextOptions nextPromptOptions = rt->PromptOptions;
  bool nextTrusted = rt->ProjectTrusted;
  bool nextHasResources = rt->HasProjectResources;
  if (switchingProject) {
    SessionResult trust = ResolveRuntimeProjectTrust(
        rt, &scratch, targetInfo->WorkingDirectory, &nextTrusted,
        &nextHasResources);
    if (!SessionResult_IsOk(trust)) {
      SessionLog_Close(&target);
      ArenaRelease(&scratch);
      return trust;
    }
    nextPromptOptions.WorkingDirectory = targetInfo->WorkingDirectory;
    nextPromptOptions.ProjectTrusted = nextTrusted;
    StrBuf error = StrBuf_Make(&scratch, 160);
    if (!PromptContext_Build(&nextPromptOptions, &nextPrompt, &error)) {
      SessionLog_Close(&target);
      StrView message = StrView_Dup(rt->Mem, StrBuf_View(&error));
      ArenaRelease(&scratch);
      return SessionResult_Error(SessionStatus::IoError, message);
    }
    config.SystemPrompt = nextPrompt.SystemPrompt;
  }
  const ProviderSpec* targetSpec = rt->Spec;
  Model targetModel = rt->CurrentModel;
  ModelRequestProfile targetProfile = config.Stream.Profile;
  if (targetState && targetState->Provider.Length &&
      targetState->Model.Length) {
    {
      targetSpec = ProviderRegistry_Find(targetState->Provider);
      if (!targetSpec || !targetSpec->CreateModel ||
          !targetSpec->CreateProvider) {
        SessionLog_Close(&target);
        PromptContext_Release(&nextPrompt);
        ArenaRelease(&scratch);
        return SessionResult_Error(
            SessionStatus::InvalidState,
            SVL("the target session uses an unavailable provider"));
      }
      targetModel = targetSpec->CreateModel(rt->Mem, targetState->Model);
      targetProfile = Model_ResolveRequestPreference(
          &targetModel, rt->PreferredProfile);
      config.ModelInfo = targetModel;
      config.ProviderImpl = targetSpec->CreateProvider(rt->Mem);
      config.Stream.Auth = {};
      config.Stream.Profile = targetProfile;
    }
  }

  if (rt->EventPump) StopHarnessEventPump(rt->EventPump);
  SessionResult closed = AgentHarness_Close(rt->Harness);
  if (!SessionResult_IsOk(closed)) {
    SessionLog_Close(&target);
    PromptContext_Release(&nextPrompt);
    RuntimeRestartWatch(rt, ux);
    ArenaRelease(&scratch);
    return closed;
  }

  if (!Sys_SetCurrentDir(targetInfo->WorkingDirectory)) {
    SessionLog_Close(&target);
    bool restored = RuntimeRollbackSessionSwitch(
        rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
    PromptContext_Release(&nextPrompt);
    ArenaRelease(&scratch);
    return restored
               ? SessionResult_Error(
                     SessionStatus::IoError,
                     SVL("failed to enter the session working directory"))
               : SessionResult_Error(
                     SessionStatus::Faulted,
                     SVL("failed to enter the session directory and restore the previous session"));
  }

  SessionResult initialized = AgentHarness_Init(rt->Harness, target, &config);
  if (!SessionResult_IsOk(initialized)) {
    bool restored = RuntimeRollbackSessionSwitch(
        rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
    PromptContext_Release(&nextPrompt);
    ArenaRelease(&scratch);
    return restored
               ? initialized
               : SessionResult_Error(
                     SessionStatus::Faulted,
                     SVL("session switch failed and the previous session could not be restored"));
  }
  if (rt->TuiView) {
    if (!Tui_ClearSession(rt->TuiView)) {
      AgentHarness_Close(rt->Harness);
      bool restored = RuntimeRollbackSessionSwitch(
          rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
      PromptContext_Release(&nextPrompt);
      ArenaRelease(&scratch);
      return restored
                 ? SessionResult_Error(
                       SessionStatus::Busy,
                       SVL("the TUI could not switch sessions"))
                 : SessionResult_Error(
                       SessionStatus::Faulted,
                       SVL("the TUI switch failed and the previous session could not be restored"));
    }
  }
  if (!RuntimeRestartWatch(rt, ux)) {
    AgentHarness_Close(rt->Harness);
    bool restored = RuntimeRollbackSessionSwitch(
        rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
    PromptContext_Release(&nextPrompt);
    ArenaRelease(&scratch);
    return restored
               ? SessionResult_Error(
                     SessionStatus::IoError,
                     SVL("failed to watch the selected session"))
               : SessionResult_Error(
                     SessionStatus::Faulted,
                     SVL("failed to watch the selected session and restore the previous session"));
  }

  rt->Spec = targetSpec;
  rt->CurrentModel = targetModel;
  if (switchingProject) {
    PromptContext previousPrompt = *rt->Prompt;
    *rt->Prompt = nextPrompt;
    nextPrompt = {};
    rt->ProjectKey = StrView_Dup(rt->Mem, targetInfo->ProjectKey);
    rt->WorkingDirectory =
        StrView_Dup(rt->Mem, targetInfo->WorkingDirectory);
    rt->PromptOptions = nextPromptOptions;
    rt->PromptOptions.WorkingDirectory = rt->WorkingDirectory;
    rt->ProjectTrusted = nextTrusted;
    rt->HasProjectResources = nextHasResources;
    rt->TrustCoversProjectResources =
        nextHasResources ||
        rt->TrustOverride != ProjectTrustOverride::Unspecified;
    PromptContext_Release(&previousPrompt);
    EmitPromptDiagnostics(rt->Prompt, ux);
  }
  if (rt->TuiView) {
    Tui_SetModelProfile(rt->TuiView, targetModel.Id, targetProfile);
    Tui_LoadStartupContext(rt->TuiView);
  }
  ArenaRelease(&scratch);
  return initialized;
}

static StrView SessionTitle(const SessionInfo* info) {
  return info->DisplayTitle.Length ? info->DisplayTitle
                                   : SVL("(untitled session)");
}

static StrView FormatDuration(Arena* arena, u64 milliseconds) {
  u64 seconds = milliseconds / 1000;
  u64 days = seconds / 86400;
  u64 hours = (seconds / 3600) % 24;
  u64 minutes = (seconds / 60) % 60;
  seconds %= 60;
  StrBuf text = StrBuf_Make(arena, 32);
  if (days) {
    StrBuf_AppendU64(&text, days);
    StrBuf_AppendCStr(&text, "d ");
    StrBuf_AppendU64(&text, hours);
    StrBuf_AppendChar(&text, 'h');
  } else if (hours) {
    StrBuf_AppendU64(&text, hours);
    StrBuf_AppendCStr(&text, "h ");
    StrBuf_AppendU64(&text, minutes);
    StrBuf_AppendChar(&text, 'm');
  } else if (minutes) {
    StrBuf_AppendU64(&text, minutes);
    StrBuf_AppendCStr(&text, "m ");
    StrBuf_AppendU64(&text, seconds);
    StrBuf_AppendChar(&text, 's');
  } else {
    StrBuf_AppendU64(&text, seconds);
    StrBuf_AppendChar(&text, 's');
  }
  return StrBuf_View(&text);
}

static StrView FormatRelativeAge(Arena* arena, u64 timestamp, u64 now) {
  if (!timestamp || now < timestamp) return SVL("now");
  u64 seconds = (now - timestamp) / 1000;
  StrBuf text = StrBuf_Make(arena, 32);
  if (seconds < 60) {
    StrBuf_AppendU64(&text, seconds);
    StrBuf_AppendCStr(&text, "s ago");
  } else if (seconds < 3600) {
    StrBuf_AppendU64(&text, seconds / 60);
    StrBuf_AppendCStr(&text, "m ago");
  } else if (seconds < 86400) {
    StrBuf_AppendU64(&text, seconds / 3600);
    StrBuf_AppendCStr(&text, "h ago");
  } else {
    StrBuf_AppendU64(&text, seconds / 86400);
    StrBuf_AppendCStr(&text, "d ago");
  }
  return StrBuf_View(&text);
}

static StrView FormatTimestamp(Arena* arena, u64 timestamp) {
  if (!timestamp) return SVL("(unknown)");
  struct tm local = {};
  if (!Sys_LocalTime(timestamp, &local)) return SVL("(unknown)");
  char value[32];
  int length =
      snprintf(value, sizeof(value), "%04d-%02d-%02d %02d:%02d:%02d",
               local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
               local.tm_hour, local.tm_min, local.tm_sec);
  return length > 0 ? StrView_Dup(arena, StrView{value, (u32)length})
                    : SVL("(unknown)");
}

static void AddDetail(Array<StrView>* lines, Arena* arena, const char* label,
                      StrView value) {
  StrBuf line = StrBuf_Make(arena, value.Length + 24);
  StrBuf_AppendCStr(&line, label);
  StrBuf_Append(&line, value.Length ? value : SVL("(none)"));
  Array_Push(lines, StrBuf_View(&line));
}

static Array<StrView> SessionDetailLines(Arena* arena,
                                         const SessionInfo* info,
                                         StrView storage, StrView status) {
  Array<StrView> lines = Array_Make<StrView>(arena, 20);
  AddDetail(&lines, arena, "Title      ", SessionTitle(info));
  AddDetail(&lines, arena, "Name       ", info->ExplicitName);
  AddDetail(&lines, arena, "ID         ", DurableId_View(&info->SessionId));
  AddDetail(&lines, arena, "Project    ", info->ProjectKey);
  AddDetail(&lines, arena, "CWD        ", info->WorkingDirectory);
  AddDetail(&lines, arena, "Storage    ", storage);
  AddDetail(&lines, arena, "Status     ", status);
  AddDetail(&lines, arena, "Created    ",
            FormatTimestamp(arena, info->CreatedAt));
  AddDetail(&lines, arena, "Activity   ",
            FormatTimestamp(arena, info->LastActivityAt));
  AddDetail(&lines, arena, "Updated    ",
            FormatTimestamp(arena, info->StorageUpdatedAt));
  AddDetail(&lines, arena, "Wall span  ",
            FormatDuration(arena, info->WallSpanMs));

  StrBuf model =
      StrBuf_Make(arena, info->CurrentProvider.Length +
                             info->CurrentModel.Length + 2);
  if (info->CurrentProvider.Length) {
    StrBuf_Append(&model, info->CurrentProvider);
    StrBuf_AppendChar(&model, '/');
  }
  StrBuf_Append(&model, info->CurrentModel);
  AddDetail(&lines, arena, "Model      ", StrBuf_View(&model));

  StrBuf messages = StrBuf_Make(arena, 96);
  StrBuf_AppendU64(&messages, info->MessageCount);
  StrBuf_AppendCStr(&messages, " total (");
  StrBuf_AppendU64(&messages, info->UserMessageCount);
  StrBuf_AppendCStr(&messages, " user, ");
  StrBuf_AppendU64(&messages, info->AssistantMessageCount);
  StrBuf_AppendCStr(&messages, " assistant, ");
  StrBuf_AppendU64(&messages, info->ToolResultMessageCount);
  StrBuf_AppendCStr(&messages, " tool)");
  AddDetail(&lines, arena, "Messages   ", StrBuf_View(&messages));

  StrBuf counts = StrBuf_Make(arena, 96);
  StrBuf_AppendU64(&counts, info->ToolCallCount);
  StrBuf_AppendCStr(&counts, " tool calls, ");
  StrBuf_AppendU64(&counts, info->CompactionCount);
  StrBuf_AppendCStr(&counts, " compactions");
  AddDetail(&lines, arena, "Counts     ", StrBuf_View(&counts));

  StrBuf tokens = StrBuf_Make(arena, 128);
  StrBuf_AppendU64(&tokens, info->TotalTokens);
  StrBuf_AppendCStr(&tokens, " total (in ");
  StrBuf_AppendU64(&tokens, info->InputTokens);
  StrBuf_AppendCStr(&tokens, ", out ");
  StrBuf_AppendU64(&tokens, info->OutputTokens);
  StrBuf_AppendCStr(&tokens, ", cache read ");
  StrBuf_AppendU64(&tokens, info->CacheReadTokens);
  StrBuf_AppendCStr(&tokens, ", write ");
  StrBuf_AppendU64(&tokens, info->CacheWriteTokens);
  StrBuf_AppendCStr(&tokens, ", reasoning ");
  StrBuf_AppendU64(&tokens, info->ReasoningTokens);
  StrBuf_AppendChar(&tokens, ')');
  AddDetail(&lines, arena, "Tokens     ", StrBuf_View(&tokens));

  if (DurableId_IsSet(info->ForkedFromSessionId)) {
    StrBuf ancestry = StrBuf_Make(arena, 64);
    StrBuf_Append(&ancestry, DurableId_View(&info->ForkedFromSessionId));
    StrBuf_AppendCStr(&ancestry, " @ ");
    StrBuf_Append(&ancestry, DurableId_View(&info->ForkedFromEntryId));
    AddDetail(&lines, arena, "Forked     ", StrBuf_View(&ancestry));
  }
  if (info->Label.Length)
    AddDetail(&lines, arena, "Branch     ", info->Label);
  return lines;
}

static StrView SessionStatusText(const SessionInfo* info) {
  if (info->Busy) return SVL("busy");
  return SVL("available");
}

static StrView FormatSessionOption(Arena* arena, const SessionInfo* info,
                                   DurableId currentSessionId,
                                   bool allProjects, bool includeFullId,
                                   u64 nowMs, StrView home) {
  StrBuf option =
      StrBuf_Make(arena, SessionTitle(info).Length +
                             info->WorkingDirectory.Length + 160);
  StrBuf_Append(&option, SessionTitle(info));
  StrBuf_AppendCStr(&option, "  |  ");
  StrBuf_AppendU64(&option, info->MessageCount);
  StrBuf_AppendCStr(&option, " msg  |  ");
  StrBuf_AppendU64(&option, info->TotalTokens);
  StrBuf_AppendCStr(&option, " tok  |  ");
  StrBuf_Append(&option,
                FormatRelativeAge(arena, info->LastActivityAt, nowMs));
  StrBuf_AppendCStr(&option, "  |  ");
  StrBuf_Append(&option, FormatDuration(arena, info->WallSpanMs));
  StrBuf_AppendCStr(&option, " span");
  if (allProjects) {
    StrView cwd =
        Sys_AbbreviatePath(arena, info->WorkingDirectory, home);
    StrBuf_AppendCStr(&option, "  |  ");
    StrBuf_Append(&option, cwd);
  }
  if (includeFullId) {
    StrBuf_AppendCStr(&option, "  |  ");
    StrBuf_Append(&option, DurableId_View(&info->SessionId));
  }
  if (DurableId_Eq(info->SessionId, currentSessionId))
    StrBuf_AppendCStr(&option, "  [current]");
  else if (info->Busy)
    StrBuf_AppendCStr(&option, "  [busy]");
  return StrBuf_View(&option);
}

struct SessionOptionList {
  SessionInfo** Sessions;
  StrView* Options;
  u32 Count;
};

static SessionOptionList BuildSessionOptions(
    Arena* arena, Array<SessionInfo>* items, DurableId currentSessionId,
    bool allProjects, bool includeFullId, bool availableOnly, u64 nowMs,
    StrView home) {
  SessionOptionList result = {};
  u32 capacity = items->Count + 1;
  result.Sessions = ArenaPushArray(arena, SessionInfo*, capacity);
  result.Options = ArenaPushArray(arena, StrView, capacity);
  for (u32 i = 0; i < items->Count; i++) {
    SessionInfo* info = &items->Data[i];
    if (availableOnly &&
        (DurableId_Eq(info->SessionId, currentSessionId) || info->Busy))
      continue;
    result.Sessions[result.Count] = info;
    result.Options[result.Count] =
        FormatSessionOption(arena, info, currentSessionId, allProjects,
                            includeFullId, nowMs, home);
    result.Count++;
  }
  return result;
}

static SessionResult CurrentSessionId(Arena* arena, InteractiveRuntime* rt,
                                      DurableId* out) {
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(arena, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) return snapshot;
  if (!state.RecordCount)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("current session has no durable root"));
  *out = state.Records[0].SessionId;
  return snapshot;
}

static AppCommandHandlerResult HandleSessionCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(&scratch, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) {
    ArenaRelease(&scratch);
    return CommandResult(snapshot, {});
  }
  SessionMetadataBuildOptions build = {};
  build.Busy = rt->Harness->Running;
  build.NowMs = NowMillis();
  SessionInfo info = {};
  SessionResult metadata =
      SessionMetadata_Build(&scratch, state.Records, state.RecordCount,
                            &build, &info);
  if (!SessionResult_IsOk(metadata)) {
    ArenaRelease(&scratch);
    return CommandResult(metadata, {});
  }
  StrView status = rt->Harness->Faulted
                       ? SVL("faulted")
                   : rt->Harness->Running
                       ? SVL("running")
                       : SVL("idle");
  Array<StrView> lines = SessionDetailLines(
      &scratch, &info, rt->NoSession ? SVL("memory") : SVL("jsonl"),
      status);
  if (rt->TuiView) {
    u32 selected = 0;
    RuntimeInputResult result =
        RuntimeSelect(rt, SVL("Current session"), lines.Data, lines.Count,
                      &selected);
    ArenaRelease(&scratch);
    if (result == RuntimeInputResult::Back)
      return ReturnToCommandPalette(rt);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
  }
  StrBuf text = StrBuf_Make(rt->Mem, 768);
  for (u32 i = 0; i < lines.Count; i++) {
    StrBuf_Append(&text, lines.Data[i]);
    if (i + 1 < lines.Count) StrBuf_AppendChar(&text, '\n');
  }
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                 StrBuf_View(&text)};
}

static AppCommandHandlerResult HandleNameCommand(
    void* user, const AppCommandUx*, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)16 << 20);
  StrView name = arguments;
  if (!arguments.Length) {
    if (!rt->TuiView) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{
          AppCommandHandlerStatus::Failed,
          AppCommand_Usage(AppCommandKind::Name)};
    }
    SessionReduction state = {};
    SessionResult snapshot =
        AgentHarness_Snapshot(&scratch, rt->Harness, &state);
    if (!SessionResult_IsOk(snapshot)) {
      ArenaRelease(&scratch);
      return CommandResult(snapshot, {});
    }
    SessionMetadataBuildOptions build = {};
    SessionInfo info = {};
    SessionResult metadata =
        SessionMetadata_Build(&scratch, state.Records, state.RecordCount,
                              &build, &info);
    if (!SessionResult_IsOk(metadata)) {
      ArenaRelease(&scratch);
      return CommandResult(metadata, {});
    }
    StrView entered = {};
    RuntimeInputResult result =
        RuntimeReadText(rt, &scratch, SVL("Session name"),
                        SVL("Name (empty clears)"), info.ExplicitName,
                        &entered);
    if (result != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      if (result == RuntimeInputResult::Back)
        return ReturnToCommandPalette(rt);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     SVL("Session name unchanged.")};
    }
    name = TrimAscii(entered);
  } else if (StrView_Eq(arguments, SVL("--clear"))) {
    name = {};
  }
  SessionResult named = AgentHarness_SetName(rt->Harness, name);
  ArenaRelease(&scratch);
  return CommandResult(named, name.Length ? SVL("session name updated")
                                          : SVL("session name cleared"));
}

static SessionResult ListSessions(InteractiveRuntime* rt, Arena* arena,
                                  SessionListScope scope,
                                  Array<SessionInfo>* out,
                                  u32* skippedCount = nullptr) {
  SessionListOptions options = {};
  options.Scope = scope;
  options.ProjectKey = rt->ProjectKey;
  options.SkippedCount = skippedCount;
  return rt->Sessions->List(rt->Sessions->Ctx, arena, &options, out);
}

static void ReportSkippedSessions(const AppCommandUx* ux, Arena* arena,
                                  u32 skippedCount) {
  if (!skippedCount) return;
  StrBuf warning = StrBuf_Make(arena, 96);
  StrBuf_AppendCStr(&warning, "Skipped ");
  StrBuf_AppendU64(&warning, skippedCount);
  StrBuf_AppendCStr(&warning, skippedCount == 1
                                 ? " unreadable session file."
                                 : " unreadable session files.");
  EmitCommandNotice(ux, StrBuf_View(&warning));
}

static AppCommandHandlerResult HandleSessionsCommand(
    void* user, const AppCommandUx* ux, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionListScope scope = SessionListScope::CurrentProject;
  if (arguments.Length) {
    if (!StrView_Eq(arguments, SVL("all"))) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     AppCommand_Usage(
                                         AppCommandKind::Sessions)};
    }
    scope = SessionListScope::AllProjects;
  }
list_scope:
  Array<SessionInfo> items = {};
  u32 skippedCount = 0;
  SessionResult listed =
      ListSessions(rt, &scratch, scope, &items, &skippedCount);
  if (!SessionResult_IsOk(listed)) {
    ArenaRelease(&scratch);
    return CommandResult(listed, {});
  }
  ReportSkippedSessions(ux, &scratch, skippedCount);
  DurableId currentSessionId = {};
  SessionResult current =
      CurrentSessionId(&scratch, rt, &currentSessionId);
  if (!SessionResult_IsOk(current)) {
    ArenaRelease(&scratch);
    return CommandResult(current, {});
  }
  bool allProjects = scope == SessionListScope::AllProjects;
  u64 nowMs = NowMillis();
  StrView home = allProjects ? Sys_HomeDir(&scratch) : StrView{};
  SessionOptionList rows = BuildSessionOptions(
      &scratch, &items, currentSessionId, allProjects, !rt->TuiView, false,
      nowMs, home);
  u32 optionCount = rows.Count;
  if (rt->TuiView && !allProjects)
    rows.Options[optionCount++] = SVL("All projects...");
  else if (!optionCount)
    rows.Options[optionCount++] = SVL("No sessions found");

  if (rt->TuiView) {
    u32 selected = 0;
    RuntimeInputResult result =
        RuntimeSelect(rt, allProjects ? SVL("All sessions")
                                      : SVL("Project sessions"),
                      rows.Options, optionCount,
                      &selected);
    if (result == RuntimeInputResult::Back) {
      ArenaRelease(&scratch);
      return ReturnToCommandPalette(rt);
    }
    if (result != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
    }
    if (!allProjects && selected == rows.Count) {
      scope = SessionListScope::AllProjects;
      goto list_scope;
    }
    if (selected < rows.Count) {
      Array<StrView> details =
          SessionDetailLines(&scratch, rows.Sessions[selected],
                             rt->NoSession ? SVL("memory") : SVL("jsonl"),
                             SessionStatusText(rows.Sessions[selected]));
      u32 ignored = 0;
      RuntimeSelect(rt, SVL("Session details"), details.Data, details.Count,
                    &ignored);
    }
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
  }

  StrBuf text = StrBuf_Make(rt->Mem, optionCount * 96 + 32);
  for (u32 i = 0; i < optionCount; i++) {
    StrBuf_Append(&text, rows.Options[i]);
    if (i + 1 < optionCount) StrBuf_AppendChar(&text, '\n');
  }
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                 StrBuf_View(&text)};
}

static AppCommandHandlerResult HandleResumeCommand(
    void* user, const AppCommandUx* ux, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  if (rt->Harness->Running)
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("cannot switch sessions while an operation is active")};
  Arena scratch = ArenaCreate((usize)64 << 20);
  DurableId sessionId = {};
  SessionInfo selectedInfo = {};
  DurableId currentSessionId = {};
  SessionResult current =
      CurrentSessionId(&scratch, rt, &currentSessionId);
  if (!SessionResult_IsOk(current)) {
    ArenaRelease(&scratch);
    return CommandResult(current, {});
  }
  if (arguments.Length) {
    if (!DurableId_Parse(arguments, &sessionId)) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     SVL("invalid session ID")};
    }
    SessionResult resolved = rt->Sessions->Resolve(
        rt->Sessions->Ctx, &scratch, sessionId, &selectedInfo);
    if (!SessionResult_IsOk(resolved)) {
      ArenaRelease(&scratch);
      return CommandResult(resolved, {});
    }
  } else {
    if (!rt->TuiView) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     AppCommand_Usage(
                                         AppCommandKind::Resume)};
    }
    SessionListScope scope = SessionListScope::CurrentProject;
resume_scope:
    Array<SessionInfo> items = {};
    u32 skippedCount = 0;
    SessionResult listed =
        ListSessions(rt, &scratch, scope, &items, &skippedCount);
    if (!SessionResult_IsOk(listed)) {
      ArenaRelease(&scratch);
      return CommandResult(listed, {});
    }
    ReportSkippedSessions(ux, &scratch, skippedCount);
    bool allProjects = scope == SessionListScope::AllProjects;
    u64 nowMs = NowMillis();
    StrView home = allProjects ? Sys_HomeDir(&scratch) : StrView{};
    SessionOptionList rows = BuildSessionOptions(
        &scratch, &items, currentSessionId, allProjects, false, true, nowMs,
        home);
    u32 optionCount = rows.Count;
    if (!allProjects)
      rows.Options[optionCount++] = SVL("All projects...");
    else if (!optionCount)
      rows.Options[optionCount++] = SVL("No other sessions are available");
    u32 selected = 0;
    RuntimeInputResult result =
        RuntimeSelect(rt, allProjects ? SVL("Resume from all projects")
                                      : SVL("Resume session"),
                      rows.Options, optionCount,
                      &selected);
    if (result != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      if (result == RuntimeInputResult::Back)
        return ReturnToCommandPalette(rt);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
    }
    if (!allProjects && selected == rows.Count) {
      scope = SessionListScope::AllProjects;
      goto resume_scope;
    }
    if (selected >= rows.Count) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
    }
    selectedInfo = *rows.Sessions[selected];
    sessionId = selectedInfo.SessionId;
  }
  if (DurableId_Eq(sessionId, currentSessionId)) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("that session is already current")};
  }
  if (selectedInfo.Busy) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("session already has a writer")};
  }
  SessionOpenOptions open = {};
  open.SessionId = sessionId;
  open.ProjectKey = selectedInfo.ProjectKey;
  open.Writable = true;
  SessionLog log = {};
  SessionInfo info = {};
  SessionResult opened =
      rt->Sessions->Open(rt->Sessions->Ctx, &scratch, &open, &log, &info);
  if (!SessionResult_IsOk(opened)) {
    ArenaRelease(&scratch);
    return CommandResult(opened, {});
  }
  Array<SessionRecord> records = {};
  SessionReduction state = {};
  SessionResult read = SessionLog_ReadAll(&log, &scratch, &records);
  if (SessionResult_IsOk(read))
    read = SessionReducer_Reduce(&scratch, records.Data, records.Count, &state);
  if (!SessionResult_IsOk(read)) {
    SessionLog_Close(&log);
    ArenaRelease(&scratch);
    return CommandResult(read, {});
  }
  SessionResult switched =
      RuntimeSwitchToLog(rt, log, &info, &state, ux);
  ArenaRelease(&scratch);
  return CommandResult(switched, SVL("session resumed"));
}

static AppCommandHandlerResult HandleTreeCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(&scratch, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) {
    ArenaRelease(&scratch);
    return CommandResult(snapshot, {});
  }
  StrBuf text = StrBuf_Make(rt->Mem, state.RecordCount * 72 + 32);
  u32 treeCount = 0;
  for (u32 i = 0; i < state.RecordCount; i++) {
    const SessionRecord* record = &state.Records[i];
    if (!SessionRecord_IsTreeNode(record->Kind)) continue;
    if (treeCount++) StrBuf_AppendChar(&text, '\n');
    StrBuf_AppendCStr(&text,
                     DurableId_Eq(record->EntryId, state.HeadId) ? "* " : "  ");
    StrBuf_Append(&text, DurableId_View(&record->EntryId));
    if (record->Kind == SessionRecordKind::Message) {
      if (record->MessageEntry.Value.Kind == Role::User)
        StrBuf_AppendCStr(&text, " user");
      else if (record->MessageEntry.Value.Kind == Role::Assistant)
        StrBuf_AppendCStr(&text, " assistant");
      else
        StrBuf_AppendCStr(&text, " tool");
    } else if (record->Kind == SessionRecordKind::Compaction) {
      StrBuf_AppendCStr(&text, " compaction");
    } else if (record->Kind == SessionRecordKind::Model) {
      StrBuf_AppendCStr(&text, " model");
    } else if (record->Kind == SessionRecordKind::BranchLabel) {
      StrBuf_AppendCStr(&text, " label");
    }
    DurableId parentId = SessionRecord_ParentId(record);
    if (DurableId_IsSet(parentId)) {
      StrBuf_AppendCStr(&text, " <- ");
      StrBuf_Append(&text, DurableId_View(&parentId));
    }
  }
  if (!treeCount) StrBuf_AppendCStr(&text, "(empty tree)");
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                 StrBuf_View(&text)};
}

static AppCommandHandlerResult HandleCheckoutCommand(
    void* user, const AppCommandUx*, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  DurableId entryId = {};
  if (!DurableId_Parse(arguments, &entryId))
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("invalid entry ID")};
  return CommandResult(AgentHarness_Navigate(rt->Harness, entryId),
                       SVL("branch checked out"));
}

static AppCommandHandlerResult HandleForkCommand(
    void* user, const AppCommandUx* ux, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)64 << 20);
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(&scratch, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) {
    ArenaRelease(&scratch);
    return CommandResult(snapshot, {});
  }
  DurableId head = state.HeadId;
  if (arguments.Length && !DurableId_Parse(arguments, &head)) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("invalid entry ID")};
  }
  SessionLog forkLog = {};
  SessionInfo forkInfo = {};
  SessionResult forked =
      AgentHarness_Fork(&scratch, rt->Harness, rt->Sessions, head, {}, {},
                        &forkLog, &forkInfo);
  if (SessionResult_IsOk(forked)) {
    Array<SessionRecord> records = {};
    SessionReduction forkState = {};
    forked = SessionLog_ReadAll(&forkLog, &scratch, &records);
    if (SessionResult_IsOk(forked))
      forked = SessionReducer_Reduce(&scratch, records.Data, records.Count,
                                     &forkState);
    if (SessionResult_IsOk(forked))
      forked =
          RuntimeSwitchToLog(rt, forkLog, &forkInfo, &forkState, ux);
    else
      SessionLog_Close(&forkLog);
  }
  ArenaRelease(&scratch);
  return CommandResult(forked, SVL("session forked"));
}

static AppCommandHandlerResult HandleCompactCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  return CommandResult(AgentHarness_Compact(rt->Harness),
                       SVL("session compacted"));
}

static StrView PromptContextReport(Arena* arena, const PromptContext* context,
                                   bool hasProjectResources,
                                   bool projectTrusted, StrView heading) {
  StrBuf message = StrBuf_Make(arena, 256);
  StrBuf_Append(&message, heading);
  StrBuf_AppendCStr(&message, "\nSystem prompt: ");
  StrBuf_Append(&message, context->SystemPromptSource.Length
                             ? context->SystemPromptSource
                             : SVL("built-in"));
  StrBuf_AppendCStr(&message, "\nAppend system prompt: ");
  StrBuf_Append(&message, context->AppendPromptSource.Length
                             ? context->AppendPromptSource
                             : SVL("none"));
  StrBuf_AppendCStr(&message, "\nProject prompt resources: ");
  StrBuf_AppendCStr(&message, !hasProjectResources
                                 ? "not present"
                             : projectTrusted ? "enabled"
                                              : "disabled (project untrusted)");
  if (context->ContextFiles.Count) {
    StrBuf_AppendCStr(&message, "\nProject instructions:");
    for (u32 i = 0; i < context->ContextFiles.Count; i++) {
      StrBuf_AppendCStr(&message, "\n- ");
      StrBuf_Append(&message, context->ContextFiles.Data[i].Path);
    }
  } else {
    StrBuf_AppendCStr(&message, "\nProject instructions: none");
  }
  return StrBuf_View(&message);
}

static void EmitPromptDiagnostics(const PromptContext* context,
                                  const AppCommandUx* ux) {
  if (!context || !ux) return;
  Arena scratch = ArenaCreate((usize)4 << 20);
  for (u32 i = 0; i < context->Diagnostics.Count; i++) {
    const PromptDiagnostic& diagnostic = context->Diagnostics.Data[i];
    StrBuf message =
        StrBuf_Make(&scratch, diagnostic.Path.Length + diagnostic.Message.Length + 24);
    if (diagnostic.Severity == PromptDiagnosticSeverity::Warning)
      StrBuf_AppendCStr(&message, "warning: ");
    if (diagnostic.Path.Length) {
      StrBuf_Append(&message, diagnostic.Path);
      StrBuf_AppendCStr(&message, ": ");
    }
    StrBuf_Append(&message, diagnostic.Message);
    if (diagnostic.Severity == PromptDiagnosticSeverity::Error)
      EmitCommandError(ux, StrBuf_View(&message));
    else
      EmitCommandNotice(ux, StrBuf_View(&message));
    ArenaReset(&scratch);
  }
  ArenaRelease(&scratch);
}

static void PrintPromptDiagnostics(const PromptContext* context) {
  if (!context) return;
  for (u32 i = 0; i < context->Diagnostics.Count; i++) {
    const PromptDiagnostic& diagnostic = context->Diagnostics.Data[i];
    fprintf(stderr, "%s%.*s%s%.*s\n",
            diagnostic.Severity == PromptDiagnosticSeverity::Warning
                ? "warning: "
                : "error: ",
            (int)diagnostic.Path.Length, diagnostic.Path.Data,
            diagnostic.Path.Length ? ": " : "", (int)diagnostic.Message.Length,
            diagnostic.Message.Data);
  }
}

static AppCommandHandlerResult RuntimeOwnedFailure(InteractiveRuntime* rt,
                                                   StrView message) {
  return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                 StrView_Dup(rt->Mem, message)};
}

static AppCommandHandlerResult HandleTrustCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)16 << 20);
  ProjectTrustStore store = {};
  ProjectTrustStoreLoadResult loaded =
      ProjectTrustStore_Load(&scratch, rt->TrustPath, &store);
  if (loaded == ProjectTrustStoreLoadResult::Malformed ||
      loaded == ProjectTrustStoreLoadResult::ReadFailure) {
    StrView message =
        loaded == ProjectTrustStoreLoadResult::Malformed
            ? SVL("The project trust store is malformed.")
            : SVL("The project trust store could not be read.");
    AppCommandHandlerResult result = RuntimeOwnedFailure(rt, message);
    ArenaRelease(&scratch);
    return result;
  }

  StrView cwd = ProjectTrust_CanonicalPath(&scratch, rt->WorkingDirectory);
  StrView parent = ProjectTrust_ParentPath(&scratch, cwd);
  ProjectTrustMatch match = ProjectTrustStore_Find(&store, cwd);
  StrBuf title = StrBuf_Make(&scratch, cwd.Length + match.Path.Length + 160);
  StrBuf_AppendCStr(&title, "Project trust for ");
  StrBuf_Append(&title, cwd);
  StrBuf_AppendCStr(&title, "\nCurrent session: ");
  StrBuf_AppendCStr(&title, rt->ProjectTrusted ? "trusted" : "untrusted");
  StrBuf_AppendCStr(&title, "\nSaved decision: ");
  if (match.Found) {
    StrBuf_AppendCStr(&title, match.Trusted ? "trust " : "do not trust ");
    StrBuf_Append(&title, match.Path);
  } else {
    StrBuf_AppendCStr(&title, "none");
  }

  StrView options[TRUST_CHOICE_CAPACITY] = {};
  TrustChoice actions[TRUST_CHOICE_CAPACITY] = {};
  u32 count = BuildTrustChoices(&scratch, parent, false, options, actions);

  u32 selected = 0;
  RuntimeInputResult input =
      RuntimeSelect(rt, StrBuf_View(&title), options, count, &selected);
  if (input != RuntimeInputResult::Confirmed) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                   SVL("Trust unchanged.")};
  }

  TrustDecision decision = ResolveTrustChoice(actions[selected], cwd, parent);
  StrBuf error = StrBuf_Make(&scratch, 128);
  if (!ProjectTrustStore_Update(&scratch, rt->TrustPath, decision.DecisionPath,
                                decision.Trusted, decision.RemovePath, &error)) {
    AppCommandHandlerResult result =
        RuntimeOwnedFailure(rt, StrBuf_View(&error));
    ArenaRelease(&scratch);
    return result;
  }
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{
      AppCommandHandlerStatus::Success,
      SVL("Trust decision saved. Restart Clank for it to take effect.")};
}

static AppCommandHandlerResult HandleReloadCommand(
    void* user, const AppCommandUx* ux, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  if (!rt->Harness || !rt->Prompt || rt->Harness->Running) {
    return AppCommandHandlerResult{
        AppCommandHandlerStatus::Failed,
        SVL("Prompt context can only be reloaded while the agent is idle.")};
  }

  Arena scratch = ArenaCreate((usize)16 << 20);
  DefaultProjectTrust defaultTrust = DefaultProjectTrust::Ask;
  AgentCompactionPolicy compaction = AgentCompactionPolicy_Default();
  AppSettings settings = {};
  AppSettingsLoadResult settingsResult =
      AppSettings_Load(&scratch, rt->SettingsPath, &settings);
  if (settingsResult == AppSettingsLoadResult::Success) {
    defaultTrust = settings.DefaultTrust;
    compaction = settings.Compaction;
  } else if (settingsResult == AppSettingsLoadResult::Malformed ||
             settingsResult == AppSettingsLoadResult::ReadFailure) {
    StrView message =
        settingsResult == AppSettingsLoadResult::Malformed
            ? SVL("Settings are malformed; prompt context was not reloaded.")
            : SVL("Settings could not be read; prompt context was not reloaded.");
    AppCommandHandlerResult result = RuntimeOwnedFailure(rt, message);
    ArenaRelease(&scratch);
    return result;
  }

  PromptContext next = {};
  bool hasProjectResources =
      ProjectTrust_HasGatedResources(&scratch, rt->WorkingDirectory);
  ProjectTrustReloadResolution trust = ProjectTrust_ResolveReload(
      hasProjectResources, rt->TrustCoversProjectResources,
      rt->TrustOverride, rt->ProjectTrusted);
  if (!trust.CanReload) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{
        AppCommandHandlerStatus::Failed,
        SVL("New project prompt files were detected. Restart Clank to resolve trust.")};
  }
  PromptContextOptions options = rt->PromptOptions;
  options.ProjectTrusted = trust.Trusted;
  StrBuf error = StrBuf_Make(&scratch, 160);
  if (!PromptContext_Build(&options, &next, &error)) {
    AppCommandHandlerResult result =
        RuntimeOwnedFailure(rt, StrBuf_View(&error));
    ArenaRelease(&scratch);
    return result;
  }

  PromptContext previous = *rt->Prompt;
  *rt->Prompt = next;
  rt->Harness->Config.SystemPrompt = rt->Prompt->SystemPrompt;
  rt->Harness->Config.Compaction = compaction;
  rt->DefaultTrust = defaultTrust;
  rt->ProjectTrusted = trust.Trusted;
  rt->HasProjectResources = hasProjectResources;
  AppAuth_Invalidate(rt->Auth);
  PromptContext_Release(&previous);

  EmitPromptDiagnostics(rt->Prompt, ux);
  StrView report =
      PromptContextReport(rt->Mem, rt->Prompt, rt->HasProjectResources,
                          rt->ProjectTrusted, SVL("Prompt context reloaded."));
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success, report};
}

static bool RuntimeResetSession(InteractiveRuntime* rt, const AppCommandUx* ux) {
  if (!rt->Harness || !rt->Sessions || rt->Harness->Running) {
    EmitCommandError(ux, SVL("Unable to clear the session while the agent is active."));
    return false;
  }

  SessionCreateOptions options = {};
  options.ProjectKey = rt->ProjectKey;
  options.WorkingDirectory = rt->WorkingDirectory;
  SessionLog log = {};
  SessionInfo info = {};
  SessionResult created =
      rt->Sessions->Create(rt->Sessions->Ctx, rt->Mem, &options, &log, &info);
  if (!SessionResult_IsOk(created)) {
    EmitCommandError(ux, created.Message);
    return false;
  }

  SessionResult switched =
      RuntimeSwitchToLog(rt, log, &info, nullptr, ux);
  if (!SessionResult_IsOk(switched)) {
    EmitCommandError(ux, switched.Message);
    return false;
  }
  SessionResult selected =
      AgentHarness_SetModel(rt->Harness, rt->CurrentModel,
                            rt->Harness->Config.ProviderImpl,
                            rt->Harness->Config.Stream.Profile);
  if (!SessionResult_IsOk(selected)) {
    EmitCommandError(ux, selected.Message);
    return false;
  }

  EmitCommandNotice(ux, SVL("session cleared"));
  return true;
}

static AppCommandHandlers RuntimeCommandHandlers(InteractiveRuntime* rt) {
  AppCommandHandlers handlers = {};
  handlers.User = rt;
  handlers.Session = HandleSessionCommand;
  handlers.Name = HandleNameCommand;
  handlers.Sessions = HandleSessionsCommand;
  handlers.Resume = HandleResumeCommand;
  handlers.Tree = HandleTreeCommand;
  handlers.Checkout = HandleCheckoutCommand;
  handlers.Fork = HandleForkCommand;
  handlers.Compact = HandleCompactCommand;
  handlers.Reload = HandleReloadCommand;
  bool selectorsAvailable = rt->TuiView || Term_IsInteractive();
  if (selectorsAvailable) handlers.Trust = HandleTrustCommand;
  if (selectorsAvailable) {
    handlers.Login = HandleLoginCommand;
    handlers.Model = HandleModelCommand;
  }
  return handlers;
}

static void ReportStandaloneCommandResult(const AppCommandUx* ux,
                                          AppCommandHandlerResult result) {
  if (result.Status == AppCommandHandlerStatus::Success ||
      result.Status == AppCommandHandlerStatus::Cancelled)
    EmitCommandNotice(ux, result.Message);
  else
    EmitCommandError(ux, result.Message);
}

static int RunCommandLineLogin(Arena* arena, StrView authPath, const char* providerId,
                               bool autoOpen) {
  AppCommandUx ux = {nullptr, PlainCommandNotice, PlainCommandError};
  const ProviderSpec* spec = ProviderRegistry_Find(SV(providerId));
  if (!ProviderCanLogin(spec)) {
    StrBuf message = StrBuf_Make(arena, (u32)strlen(providerId) + 48);
    StrBuf_AppendCStr(&message, "No OAuth login is configured for provider '");
    StrBuf_AppendCStr(&message, providerId);
    StrBuf_AppendCStr(&message, "'.");
    EmitCommandError(&ux, StrBuf_View(&message));
    return 1;
  }

  InteractiveRuntime runtime = {};
  runtime.Mem = arena;
  runtime.AuthPath = authPath;
  runtime.Spec = spec;
  runtime.AutoOpen = autoOpen;
  AppCommandHandlerResult result = RunOAuthLogin(&runtime, spec, &ux);
  ReportStandaloneCommandResult(&ux, result);
  return result.Status == AppCommandHandlerStatus::Success ? 0 : 1;
}

static int RunCommandLineLogout(Arena* arena, StrView authPath, const char* providerId) {
  AppCommandUx ux = {nullptr, PlainCommandNotice, PlainCommandError};
  if (!AuthStore_Delete(arena, authPath, SV(providerId))) {
    StrBuf message = StrBuf_Make(arena, (u32)strlen(providerId) + authPath.Length + 64);
    StrBuf_AppendCStr(&message, "Failed to remove credentials for '");
    StrBuf_AppendCStr(&message, providerId);
    StrBuf_AppendCStr(&message, "' from ");
    StrBuf_Append(&message, authPath);
    StrBuf_AppendChar(&message, '.');
    EmitCommandError(&ux, StrBuf_View(&message));
    return 1;
  }

  StrBuf message = StrBuf_Make(arena, (u32)strlen(providerId) + 24);
  StrBuf_AppendCStr(&message, "Logged out of ");
  StrBuf_AppendCStr(&message, providerId);
  EmitCommandNotice(&ux, StrBuf_View(&message));
  return 0;
}

static void PrintHelp() {
  printf(
      "Clank - a tiny memory-efficient coding agent\n\n"
      "Usage: clank [options]\n"
      "  -m <model>        Override the configured model for this run only\n"
      "  -p <prompt>       Run a single prompt and exit (print mode)\n"
      "  --mode <text|json>\n"
      "                    Print-mode output: human text (default) or an\n"
      "                    NDJSON event stream on stdout. Requires -p\n"
      "  -c, --continue    Continue the most recent session for this project\n"
      "  -r, --resume ID   Resume an exact session ID from any project\n"
      "  --no-session      Keep the session in memory; write no session files\n"
      "  --system-prompt VALUE\n"
      "                    Replace the built-in prompt with text or file content\n"
      "  --append-system-prompt VALUE\n"
      "                    Append text or file content (repeatable)\n"
      "  -nc, --no-context-files\n"
      "                    Do not load AGENTS.md or CLAUDE.md files\n"
      "  -a, --approve     Trust project prompt resources for this run\n"
      "  -na, --no-approve Do not trust project prompt resources for this run\n"
      "  --login [prov]    OAuth login in your browser (anthropic or openai)\n"
      "  --logout [prov]   Remove stored credentials (default: anthropic)\n"
      "  --no-open         Do not auto-open a browser for --login or interactive /login\n"
     "  --help            Show this help\n\n"
      "Auth: run `--login anthropic` for Claude Pro/Max or `--login openai` for\n"
      "a ChatGPT subscription; alternatively set ANTHROPIC_API_KEY or OPENAI_API_KEY.\n"
      "Interactive commands: /session, /name, /sessions, /resume, /tree,\n"
      "/checkout, /fork, /compact, /trust, /reload, /login, /model,\n"
      "/clear, /exit.\n");
}

static bool ResolveStartupProjectTrust(
    Arena* arena, StrView trustPath, StrView workingDirectory,
    DefaultProjectTrust defaultTrust, ProjectTrustOverride overrideValue,
    bool interactive, bool* trusted, bool* hasResources) {
  *hasResources =
      ProjectTrust_HasGatedResources(arena, workingDirectory);
  if (!*hasResources) {
    *trusted = true;
    return true;
  }
  if (overrideValue != ProjectTrustOverride::Unspecified) {
    *trusted = overrideValue == ProjectTrustOverride::Trusted;
    return true;
  }

  ProjectTrustStore store = {};
  ProjectTrustStoreLoadResult loaded =
      ProjectTrustStore_Load(arena, trustPath, &store);
  if (loaded == ProjectTrustStoreLoadResult::Malformed ||
      loaded == ProjectTrustStoreLoadResult::ReadFailure) {
    fprintf(stderr, "error: project trust store %.*s %s\n",
            (int)trustPath.Length, trustPath.Data,
            loaded == ProjectTrustStoreLoadResult::Malformed
                ? "is malformed"
                : "could not be read");
    return false;
  }

  StrView cwd = ProjectTrust_CanonicalPath(arena, workingDirectory);
  ProjectTrustResolution resolution =
      ProjectTrust_Resolve(true, overrideValue, &store, cwd, defaultTrust,
                           interactive);
  if (!resolution.NeedsPrompt) {
    *trusted = resolution.Trusted;
    return true;
  }

  StrView parent = ProjectTrust_ParentPath(arena, cwd);
  StrView options[TRUST_CHOICE_CAPACITY] = {};
  TrustChoice actions[TRUST_CHOICE_CAPACITY] = {};
  u32 count = BuildTrustChoices(arena, parent, true, options, actions);

  StrBuf title = StrBuf_Make(arena, cwd.Length + 96);
  StrBuf_AppendCStr(
      &title,
      "This project contains .clank prompt files that can replace system instructions:\n");
  StrBuf_Append(&title, cwd);
  u32 selected = 0;
  if (!Plain_Select(StrBuf_View(&title), options, count, &selected)) {
    *trusted = false;
    return true;
  }

  TrustDecision decision = ResolveTrustChoice(actions[selected], cwd, parent);
  *trusted = decision.Trusted;
  if (!decision.Persist) return true;

  StrBuf error = StrBuf_Make(arena, 128);
  if (!ProjectTrustStore_Update(arena, trustPath, decision.DecisionPath,
                                decision.Trusted, decision.RemovePath, &error)) {
    fprintf(stderr, "error: %.*s\n", (int)error.Length, error.Data);
    return false;
  }
  return true;
}

static StrView SessionDataRoot(Arena* arena) {
  StrView home = Sys_HomeDir(arena);
  StrBuf path = StrBuf_Make(arena, home.Length + 16);
  StrBuf_Append(&path, home);
  StrBuf_AppendCStr(&path, "/.clank");
  return StrBuf_View(&path);
}

static StrView SessionProjectKey(Arena* arena, StrView workingDirectory) {
  u64 hash = 1469598103934665603ull;
  for (u32 i = 0; i < workingDirectory.Length; i++) {
    char c = workingDirectory.Data[i];
#if defined(_WIN32)
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
    hash ^= (u8)c;
    hash *= 1099511628211ull;
  }

  u32 baseStart = workingDirectory.Length;
  while (baseStart > 0 && workingDirectory.Data[baseStart - 1] != '/' &&
         workingDirectory.Data[baseStart - 1] != '\\')
    baseStart--;
  StrView base = {workingDirectory.Data + baseStart,
                  workingDirectory.Length - baseStart};
  StrBuf key = StrBuf_Make(arena, base.Length + 18);
  for (u32 i = 0; i < base.Length; i++) {
    char c = base.Data[i];
    bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                 c == '.';
    StrBuf_AppendChar(&key, valid ? c : '-');
  }
  if (key.Length == 0) StrBuf_AppendCStr(&key, "project");
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)hash);
  StrBuf_AppendCStr(&key, suffix);
  return StrBuf_View(&key);
}

// Plain line REPL (non-TTY / fallback): the original fgets loop.
static void RunPlainRepl(InteractiveRuntime* rt) {
  EnableTerminal();
  rt->TuiView = nullptr;
  HarnessEventPump pump = {};
  AgentHarnessSnapshot snapshot = {};
  AgentSink sink = {nullptr, ReplSink};
  SessionResult watching =
      StartHarnessEventPump(rt->Mem, rt->Harness, sink, nullptr, &pump,
                            &snapshot);
  if (!SessionResult_IsOk(watching)) {
    ReportHarnessResult(watching);
    return;
  }
  rt->EventPump = &pump;
  rt->EventSink = sink;
  printf("Clank (%.*s / %.*s). Ctrl+C interrupts a turn, /exit quits.\n",
           (int)rt->Spec->Name.Length, rt->Spec->Name.Data, (int)rt->CurrentModel.Id.Length,
           rt->CurrentModel.Id.Data);

  char line[8192];
  AppCommandUx commandUx = {nullptr, PlainCommandNotice, PlainCommandError};
  EmitCommandNotice(
      &commandUx,
      PromptContextReport(rt->Mem, rt->Prompt, rt->HasProjectResources,
                          rt->ProjectTrusted, SVL("Prompt context loaded.")));
  EmitPromptDiagnostics(rt->Prompt, &commandUx);
  RuntimeCheckAuth(rt, &commandUx, false);
  AppCommandHandlers handlers = RuntimeCommandHandlers(rt);
  for (;;) {
    fputs("\n\x1b[1m\xe2\x80\xba \x1b[0m", stdout);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) break;
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
    if (L == 0) continue;
    AppCommandDispatchResult command = AppCommand_Dispatch(SV(line), &handlers, &commandUx);
    if (command.Handled) {
      if (command.Action == AppLoopAction::Exit) break;
      if (command.Action == AppLoopAction::ResetSession) RuntimeResetSession(rt, &commandUx);
      continue;
    }
    g_abort = 0;
    if (!RuntimeCheckAuth(rt, &commandUx, true)) continue;
    if (g_abort) continue;
   SessionResult run = RunHarnessPrompt(rt->Harness, SV(line));
    ReportHarnessResult(run);
  }
  StopHarnessEventPump(&pump);
  rt->EventPump = nullptr;
}

// Full-screen TUI REPL. Falls back to the plain REPL if not a TTY.
static void RunTuiRepl(InteractiveRuntime* rt) {
  TuiOpts opts;
  opts.ModelName = rt->CurrentModel.Id;
  opts.Profile = rt->Harness->Config.Stream.Profile;
  opts.Color = (getenv("NO_COLOR") == nullptr);
  u32 catalogCount = AppCommandSpec_Count();
  TuiCommandSpec* commands = ArenaPushArray(rt->Mem, TuiCommandSpec, catalogCount);
  for (u32 i = 0; i < catalogCount; i++) {
    const AppCommandSpec* command = AppCommandSpec_At(i);
    commands[opts.CommandCount++] =
        TuiCommandSpec{command->Name, command->Alias, command->Description};
  }
  opts.Commands = commands;

  Tui tui;
  if (!Tui_Start(&tui, rt->Mem, opts)) {
    Http_GlobalInit();
    RunPlainRepl(rt);
    return;
  }
  rt->TuiView = &tui;
  tui.Harness = rt->Harness;

  HarnessEventPump pump = {};
  AgentHarnessSnapshot snapshot = {};
  AgentSink sink = {&tui, Tui_AgentEvent};
  SessionResult watching =
      StartHarnessEventPump(rt->Mem, rt->Harness, sink, &tui, &pump,
                            &snapshot);
  if (!SessionResult_IsOk(watching)) {
    ReportHarnessResult(watching);
    Tui_Stop(&tui);
    rt->TuiView = nullptr;
    return;
  }
  rt->EventPump = &pump;
  rt->EventSink = sink;
  Tui_HydrateTranscript(&tui, snapshot.Transcript.Data,
                        snapshot.Transcript.Count);

  AuthWarmup warmup = {};
  Http_GlobalInit();
  rt->AuthWarmupState = &warmup;
  StartAuthWarmup(&warmup, rt->Auth, rt->Spec, Tui_OperationAbort(&tui));
  Tui_LoadStartupContext(&tui);

  AppCommandUx commandUx = {&tui, TuiCommandNotice, TuiCommandError};
  EmitCommandNotice(
      &commandUx,
      PromptContextReport(rt->Mem, rt->Prompt, rt->HasProjectResources,
                          rt->ProjectTrusted, SVL("Prompt context loaded.")));
  EmitPromptDiagnostics(rt->Prompt, &commandUx);
  AppCommandHandlers handlers = RuntimeCommandHandlers(rt);
  for (;;) {
    TuiSubmission submission = {};
    if (!Tui_WaitForSubmit(&tui, &submission)) break;
    if (submission.Kind == TuiSubmissionKind::Command) {
      AppCommandDispatchResult command =
          AppCommand_Dispatch(submission.Text, &handlers, &commandUx);
      if (command.Action == AppLoopAction::Exit) break;
      if (command.Action == AppLoopAction::ResetSession) RuntimeResetSession(rt, &commandUx);
      continue;
    }
    Tui_BeginOperation(&tui, SVL("Resolving credentials"));
    bool authenticated = RuntimeCheckAuth(rt, &commandUx, true);
    bool cancelled = Tui_EndOperation(&tui);
    if (!authenticated || cancelled) continue;
    *Tui_Abort(&tui) = 0;
    if (!Tui_AcceptPrompt(&tui, submission.Text)) continue;
   SessionResult run = RunHarnessPrompt(rt->Harness, submission.Text);
    if (!SessionResult_IsOk(run)) Tui_CommandError(&tui, run.Message);
  }
  CancelRuntimeAuthWarmup(rt);
  StopHarnessEventPump(&pump);
  rt->EventPump = nullptr;
  Tui_Stop(&tui);
  rt->TuiView = nullptr;
}

int main(int argc, char** argv) {
  Arena arena = ArenaCreate((usize)2 << 30);
  const char* modelOverride = nullptr;
  const char* onePrompt = nullptr;
  bool jsonMode = false;
  const char* loginProvider = nullptr;
  const char* logoutProvider = nullptr;
  const char* resumeSession = nullptr;
  bool hasExplicitSystemPrompt = false;
  StrView explicitSystemPrompt = {};
  Array<StrView> explicitAppendPrompts = Array_Make<StrView>(&arena, 4);
  ProjectTrustOverride trustOverride = ProjectTrustOverride::Unspecified;
  bool noOpen = false;
  bool noSession = false;
  bool noContextFiles = false;
  bool continueSession = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-m") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: -m requires a model\n");
        ArenaRelease(&arena);
        return 1;
      }
      modelOverride = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: -p requires a prompt\n");
        ArenaRelease(&arena);
        return 1;
      }
      onePrompt = argv[++i];
    } else if (strcmp(argv[i], "--mode") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --mode requires text or json\n");
        ArenaRelease(&arena);
        return 1;
      }
      const char* value = argv[++i];
      if (strcmp(value, "json") == 0) {
        jsonMode = true;
      } else if (strcmp(value, "text") != 0) {
        fprintf(stderr, "error: unknown mode '%s' (expected text or json)\n", value);
        ArenaRelease(&arena);
        return 1;
      }
    } else if (strcmp(argv[i], "--system-prompt") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --system-prompt requires a value\n");
        ArenaRelease(&arena);
        return 1;
      }
      hasExplicitSystemPrompt = true;
      explicitSystemPrompt = SV(argv[++i]);
    } else if (strcmp(argv[i], "--append-system-prompt") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --append-system-prompt requires a value\n");
        ArenaRelease(&arena);
        return 1;
      }
      Array_Push(&explicitAppendPrompts, SV(argv[++i]));
    } else if (strcmp(argv[i], "-nc") == 0 ||
               strcmp(argv[i], "--no-context-files") == 0) {
      noContextFiles = true;
    } else if (strcmp(argv[i], "-a") == 0 ||
               strcmp(argv[i], "--approve") == 0) {
      if (trustOverride == ProjectTrustOverride::Untrusted) {
        fprintf(stderr,
                "error: choose only one of --approve or --no-approve\n");
        ArenaRelease(&arena);
        return 1;
      }
      trustOverride = ProjectTrustOverride::Trusted;
    } else if (strcmp(argv[i], "-na") == 0 ||
               strcmp(argv[i], "--no-approve") == 0) {
      if (trustOverride == ProjectTrustOverride::Trusted) {
        fprintf(stderr,
                "error: choose only one of --approve or --no-approve\n");
        ArenaRelease(&arena);
        return 1;
      }
      trustOverride = ProjectTrustOverride::Untrusted;
    } else if (strcmp(argv[i], "--no-session") == 0) {
      noSession = true;
    } else if (strcmp(argv[i], "-c") == 0 ||
               strcmp(argv[i], "--continue") == 0) {
      continueSession = true;
    } else if ((strcmp(argv[i], "-r") == 0 ||
                strcmp(argv[i], "--resume") == 0)) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --resume requires a session ID\n");
        ArenaRelease(&arena);
        return 1;
      }
      resumeSession = argv[++i];
    } else if (strcmp(argv[i], "--no-open") == 0) {
      noOpen = true;
    } else if (strcmp(argv[i], "--login") == 0) {
      loginProvider = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "anthropic";
    } else if (strcmp(argv[i], "--logout") == 0) {
      logoutProvider = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "anthropic";
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      PrintHelp();
      ArenaRelease(&arena);
      return 0;
    } else {
      fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
      ArenaRelease(&arena);
      return 1;
    }
  }

  if ((continueSession && resumeSession) ||
      (noSession && (continueSession || resumeSession))) {
    fprintf(stderr,
            "error: choose only one of --no-session, --continue, or --resume\n");
    ArenaRelease(&arena);
    return 1;
  }

  // JSON is a machine surface for a single non-interactive run; there is no
  // meaningful interactive form of it without a bidirectional protocol.
  if (jsonMode && !onePrompt) {
    fprintf(stderr, "error: --mode json requires -p <prompt>\n");
    ArenaRelease(&arena);
    return 1;
  }

  StrView authPath = AuthStore_DefaultPath(&arena);
  StrView settingsPath = AppSettings_DefaultPath(&arena);
  signal(SIGINT, OnSigint);

  if (loginProvider) {
    Http_GlobalInit();
    int rc = RunCommandLineLogin(&arena, authPath, loginProvider, !noOpen);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return rc;
  }
  if (logoutProvider) {
    int rc = RunCommandLineLogout(&arena, authPath, logoutProvider);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return rc;
  }

  AppAuth appAuth = {};
  appAuth.Mem = &arena;
  appAuth.Path = authPath;
  RuntimeSelection selected =
      LoadRuntimeSelection(&arena, settingsPath, modelOverride);
  if (!selected.Spec || !selected.Spec->CreateProvider || !selected.Spec->CreateModel) {
    fprintf(stderr, "error: no usable built-in provider is registered\n");
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 1;
  }

  StrView workingDirectory = Sys_CurrentDir(&arena);
  if (!workingDirectory.Length) {
    fprintf(stderr, "error: could not determine the current working directory\n");
    ArenaRelease(&arena);
    return 1;
  }
  StrView projectKey = SessionProjectKey(&arena, workingDirectory);
  MemoryRepository memoryRepository = {};
  JsonlRepository jsonlRepository = {};
  SessionRepository sessions = {};
  if (noSession) {
    MemoryRepository_Init(&memoryRepository);
    sessions = MemoryRepository_AsRepository(&memoryRepository);
  } else {
    StrView dataRoot = SessionDataRoot(&arena);
    JsonlRepository_Init(&jsonlRepository, dataRoot);
    sessions = JsonlRepository_AsRepository(&jsonlRepository);
  }

  SessionInfo resolvedResume = {};
  DurableId resolvedResumeId = {};
  if (resumeSession) {
    if (!DurableId_Parse(SV(resumeSession), &resolvedResumeId)) {
      fprintf(stderr, "error: invalid session ID\n");
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    SessionResult resolved = sessions.Resolve(
        sessions.Ctx, &arena, resolvedResumeId, &resolvedResume);
    if (!SessionResult_IsOk(resolved)) {
      fprintf(stderr, "error: session startup failed: %.*s\n",
              (int)resolved.Message.Length, resolved.Message.Data);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    if (!Fs_IsDir(resolvedResume.WorkingDirectory)) {
      fprintf(stderr,
              "error: session working directory no longer exists: %.*s\n",
              (int)resolvedResume.WorkingDirectory.Length,
              resolvedResume.WorkingDirectory.Data);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    if (!Sys_SetCurrentDir(resolvedResume.WorkingDirectory)) {
      fprintf(stderr, "error: could not enter session working directory: %.*s\n",
              (int)resolvedResume.WorkingDirectory.Length,
              resolvedResume.WorkingDirectory.Data);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    workingDirectory =
        StrView_Dup(&arena, resolvedResume.WorkingDirectory);
    projectKey = StrView_Dup(&arena, resolvedResume.ProjectKey);
  }
  StrView agentDirectory = Fs_Parent(settingsPath);
  StrView trustPath = ProjectTrust_DefaultPath(&arena);
  bool projectTrusted = false;
  bool hasProjectResources = false;
  bool trustInteractive = !onePrompt && Term_IsInteractive();
  if (!ResolveStartupProjectTrust(
          &arena, trustPath, workingDirectory, selected.DefaultTrust,
          trustOverride, trustInteractive, &projectTrusted,
          &hasProjectResources)) {
    sessions.Close(sessions.Ctx);
    ArenaRelease(&arena);
    return 1;
  }

  Model model = selected.Spec->CreateModel(&arena, selected.ModelId);
  bool unsupportedProfilePreference = false;
  ModelRequestProfile preference = Model_NormalizeRequestPreference(
      &model, selected.RequestedProfile, &unsupportedProfilePreference);
  ModelRequestProfile profile = Model_ResolveRequestProfile(&model, preference);
  if (unsupportedProfilePreference) {
    fprintf(stderr,
            "warning: saved effort/context preferences are unsupported by model "
            "'%.*s'; using model defaults where needed\n",
            (int)model.Id.Length, model.Id.Data);
  }
  FileLock_Init();
  Tool tools[] = {ReadTool(), WriteTool(), EditTool(), LsTool(),
                  FindTool(), GrepTool(), ShellTool()};

  PromptContextOptions promptOptions = {};
  promptOptions.WorkingDirectory = workingDirectory;
  promptOptions.AgentDirectory = agentDirectory;
  promptOptions.Tools = tools;
  promptOptions.ToolCount = sizeof(tools) / sizeof(tools[0]);
  promptOptions.NoContextFiles = noContextFiles;
  promptOptions.ProjectTrusted = projectTrusted;
  promptOptions.HasExplicitSystemPrompt = hasExplicitSystemPrompt;
  promptOptions.ExplicitSystemPrompt = explicitSystemPrompt;
  promptOptions.ExplicitAppendPrompts = explicitAppendPrompts.Data;
  promptOptions.ExplicitAppendPromptCount = explicitAppendPrompts.Count;
  PromptContext promptContext = {};
  StrBuf promptError = StrBuf_Make(&arena, 160);
  if (!PromptContext_Build(&promptOptions, &promptContext, &promptError)) {
    fprintf(stderr, "error: %.*s\n", (int)promptError.Length,
            promptError.Data);
    sessions.Close(sessions.Ctx);
    ArenaRelease(&arena);
    return 1;
  }
  PromptContextLifetime promptLifetime = {&promptContext};
  if (onePrompt) {
    if (hasProjectResources && !projectTrusted)
      fprintf(stderr,
              "warning: project .clank prompt files are disabled because the project is untrusted\n");
    PrintPromptDiagnostics(&promptContext);
  }

  AgentHarnessConfig harnessConfig = {};
  harnessConfig.SystemPrompt = promptContext.SystemPrompt;
  harnessConfig.ModelInfo = model;
  harnessConfig.Tools = tools;
  harnessConfig.ToolCount = sizeof(tools) / sizeof(tools[0]);
  harnessConfig.ProviderImpl = selected.Spec->CreateProvider(&arena);
  harnessConfig.Stream.Profile = profile;
  harnessConfig.ToolExec = ToolExecution::Parallel;
  harnessConfig.Compaction = selected.Compaction;
  harnessConfig.AuthUser = &appAuth;
  harnessConfig.ResolveAuth = AppResolveAuth;

  SessionLog sessionLog = {};
  SessionInfo sessionInfo = {};
  SessionResult sessionResult = {};
  bool openedExisting = continueSession || resumeSession;
  if (continueSession) {
    Array<SessionInfo> items = {};
    SessionListOptions list = {};
    list.Scope = SessionListScope::CurrentProject;
    list.ProjectKey = projectKey;
    u32 skippedCount = 0;
    list.SkippedCount = &skippedCount;
    sessionResult =
        sessions.List(sessions.Ctx, &arena, &list, &items);
    if (SessionResult_IsOk(sessionResult) && skippedCount)
      fprintf(stderr, "warning: skipped %u unreadable session file%s\n",
              skippedCount, skippedCount == 1 ? "" : "s");
    if (SessionResult_IsOk(sessionResult) && items.Count == 0)
      sessionResult = SessionResult_Error(
          SessionStatus::NotFound,
          SVL("no previous session exists for this project"));
    if (SessionResult_IsOk(sessionResult)) {
      SessionOpenOptions open = {};
      open.SessionId = items.Data[0].SessionId;
      open.ProjectKey = projectKey;
      open.Writable = true;
      sessionResult =
          sessions.Open(sessions.Ctx, &arena, &open, &sessionLog, &sessionInfo);
    }
  } else if (resumeSession) {
    SessionOpenOptions open = {};
    open.SessionId = resolvedResumeId;
    open.ProjectKey = resolvedResume.ProjectKey;
    open.Writable = true;
    sessionResult =
        sessions.Open(sessions.Ctx, &arena, &open, &sessionLog, &sessionInfo);
  } else {
    SessionCreateOptions create = {};
    create.ProjectKey = projectKey;
    create.WorkingDirectory = workingDirectory;
    sessionResult =
        sessions.Create(sessions.Ctx, &arena, &create, &sessionLog, &sessionInfo);
  }
  if (!SessionResult_IsOk(sessionResult)) {
    fprintf(stderr, "error: session startup failed: %.*s\n",
            (int)sessionResult.Message.Length, sessionResult.Message.Data);
    sessions.Close(sessions.Ctx);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 1;
  }

  AgentHarness harness = {};
  SessionResult initialized =
      AgentHarness_Init(&harness, sessionLog, &harnessConfig);
  if (!SessionResult_IsOk(initialized)) {
    fprintf(stderr, "error: session restore failed: %.*s\n",
            (int)initialized.Message.Length, initialized.Message.Data);
    sessions.Close(sessions.Ctx);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 1;
  }

  if (openedExisting) {
    Arena scratch = ArenaCreate((usize)16 << 20);
    SessionReduction state = {};
    SessionResult snapshot =
        AgentHarness_Snapshot(&scratch, &harness, &state);
    if (SessionResult_IsOk(snapshot) && state.Provider.Length &&
        state.Model.Length) {
      {
        const ProviderSpec* restored = ProviderRegistry_Find(state.Provider);
        if (!restored || !restored->CreateProvider ||
            !restored->CreateModel) {
          fprintf(stderr,
                  "error: session uses unavailable provider '%.*s'\n",
                  (int)state.Provider.Length, state.Provider.Data);
          ArenaRelease(&scratch);
          AgentHarness_Close(&harness);
          sessions.Close(sessions.Ctx);
          ArenaRelease(&arena);
          return 1;
        }
        selected.Spec = restored;
        model = restored->CreateModel(
            &arena, modelOverride ? SV(modelOverride) : state.Model);
        profile = Model_ResolveRequestPreference(&model, preference);
        harness.Config.ModelInfo = model;
        harness.Config.ProviderImpl = restored->CreateProvider(&arena);
        harness.Config.Stream.Profile = profile;
        if (modelOverride) {
          SessionResult overridden = AgentHarness_SetModel(
              &harness, model, harness.Config.ProviderImpl, profile);
          if (!SessionResult_IsOk(overridden)) {
            fprintf(stderr, "error: failed to apply model override: %.*s\n",
                    (int)overridden.Message.Length, overridden.Message.Data);
            ArenaRelease(&scratch);
            AgentHarness_Close(&harness);
            sessions.Close(sessions.Ctx);
            ArenaRelease(&arena);
            return 1;
          }
        }
      }
    } else if (SessionResult_IsOk(snapshot)) {
      SessionResult selectedModel = AgentHarness_SetModel(
          &harness, model, harness.Config.ProviderImpl, profile);
      if (!SessionResult_IsOk(selectedModel)) {
        fprintf(stderr, "error: failed to initialize session model: %.*s\n",
                (int)selectedModel.Message.Length,
                selectedModel.Message.Data);
        ArenaRelease(&scratch);
        AgentHarness_Close(&harness);
        sessions.Close(sessions.Ctx);
        ArenaRelease(&arena);
        return 1;
      }
    }
    ArenaRelease(&scratch);
  } else {
    SessionResult selectedModel = AgentHarness_SetModel(
        &harness, model, harnessConfig.ProviderImpl, profile);
    if (!SessionResult_IsOk(selectedModel)) {
      fprintf(stderr, "error: failed to persist initial model: %.*s\n",
              (int)selectedModel.Message.Length, selectedModel.Message.Data);
      AgentHarness_Close(&harness);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
  }

  InteractiveRuntime runtime = {};
  runtime.Mem = &arena;
  runtime.AuthPath = authPath;
  runtime.SettingsPath = settingsPath;
  runtime.Spec = selected.Spec;
  runtime.CurrentModel = model;
  runtime.PreferredProfile = preference;
  runtime.Harness = &harness;
  runtime.Sessions = &sessions;
  runtime.ProjectKey = projectKey;
  runtime.WorkingDirectory = workingDirectory;
  runtime.AgentDirectory = agentDirectory;
  runtime.TrustPath = trustPath;
  runtime.NoSession = noSession;
  runtime.ProjectTrusted = projectTrusted;
  runtime.HasProjectResources = hasProjectResources;
  runtime.TrustCoversProjectResources =
      hasProjectResources ||
      trustOverride != ProjectTrustOverride::Unspecified;
  runtime.TrustOverride = trustOverride;
  runtime.DefaultTrust = selected.DefaultTrust;
  runtime.Prompt = &promptContext;
  runtime.PromptOptions = promptOptions;
  runtime.AutoOpen = !noOpen;
  runtime.Auth = &appAuth;

  if (onePrompt) {
    EnableTerminal();
    Http_GlobalInit();
    HarnessEventPump pump = {};
    AgentHarnessSnapshot snapshot = {};
    AgentSink sink = {nullptr, ReplSink};
    SessionResult watching =
        StartHarnessEventPump(&arena, &harness, sink, nullptr, &pump,
                              &snapshot, jsonMode);
    if (!SessionResult_IsOk(watching)) {
      ReportHarnessResult(watching);
      AgentHarness_Close(&harness);
      sessions.Close(sessions.Ctx);
      Http_GlobalShutdown();
      ArenaRelease(&arena);
      return 1;
    }
    g_abort = 0;
    if (!ResolvePromptAuth(&harness, &g_abort)) {
      if (g_abort) {
        StopHarnessEventPump(&pump);
        AgentHarness_Close(&harness);
        sessions.Close(sessions.Ctx);
        Http_GlobalShutdown();
        ArenaRelease(&arena);
        return 130;
      }
      StrView authGuidance = AuthGuidance(&arena, runtime.Spec, false);
      fprintf(stderr, "error: %.*s\n", (int)authGuidance.Length, authGuidance.Data);
      StopHarnessEventPump(&pump);
      AgentHarness_Close(&harness);
      sessions.Close(sessions.Ctx);
      Http_GlobalShutdown();
      ArenaRelease(&arena);
      return 1;
    }
    g_abort = 0;
    SessionResult run = RunHarnessPrompt(&harness, SV(onePrompt));
    StopHarnessEventPump(&pump);
    if (jsonMode) {
      // Terminator: the pump has drained, so this is always the last line and
      // carries the same verdict as the exit code.
      StrBuf line = StrBuf_Make(&arena, 128);
      JsonWriter w = {};
      JsonWrite_Begin(&w, &line);
      JsonWrite_String(&w, "type", SVL("result"));
      JsonWrite_Bool(&w, "ok", SessionResult_IsOk(run));
      if (!SessionResult_IsOk(run)) JsonWrite_String(&w, "error", run.Message);
      JsonWrite_End(&w);
      fwrite(line.Data, 1, line.Length, stdout);
      fputc('\n', stdout);
      fflush(stdout);
    }
    ReportHarnessResult(run);
    MaybeReportMem();
    AgentHarness_Close(&harness);
    sessions.Close(sessions.Ctx);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return SessionResult_IsOk(run) ? 0 : 1;
  }

  if (Term_IsInteractive())
    RunTuiRepl(&runtime);
  else {
    Http_GlobalInit();
    RunPlainRepl(&runtime);
  }

  MaybeReportMem();
  AgentHarness_Close(&harness);
  sessions.Close(sessions.Ctx);
  Http_GlobalShutdown();
  ArenaRelease(&arena);
  return 0;
}
