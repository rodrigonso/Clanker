#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "auth/oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/sysinfo.h"
#include "llm/anthropic.h"
#include "llm/faux.h"
#include "net/browser.h"
#include "net/http.h"
#include "term/term.h"
#include "term/tui.h"
#include "tools/tools.h"

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
static void OnSigint(int) { g_abort = 1; }

// ---- auth wiring ---------------------------------------------------------

struct AppAuth {
  StrView Path;
};

// Maps a provider to its API-key env var (only Anthropic is wired for now).
static const char* EnvVarFor(StrView provider) {
  if (StrView_EqCStr(provider, "anthropic")) return "ANTHROPIC_API_KEY";
  return nullptr;
}

static bool AppResolveAuth(void* user, Arena* arena, StrView provider, StrView* out) {
  AppAuth* aa = (AppAuth*)user;
  StrView t = Auth_ResolveToken(arena, aa->Path, provider, EnvVarFor(provider));
  if (t.Length) {
    *out = t;
    return true;
  }
  return false;
}

// ---- REPL renderer -------------------------------------------------------

static void PrintPreview(StrView t) {
  u32 n = t.Length > 200 ? 200 : t.Length;
  fputs("\x1b[90m", stdout);
  for (u32 i = 0; i < n; i++) {
    char c = t.Data[i];
    putchar(c == '\n' ? ' ' : c);
  }
  if (t.Length > 200) fputs("\xe2\x80\xa6", stdout);
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
    default:
      break;
  }
}

static void ReportOutcome(Agent* ag) {
  Message* last = Array_Last(&ag->Messages);
  if (!last || last->Kind != Role::Assistant) return;
  if (last->Stop == StopReason::Error)
    printf("\n\x1b[31merror: %.*s\x1b[0m\n", (int)last->ErrorMessage.Length, last->ErrorMessage.Data);
  else if (last->Stop == StopReason::Aborted)
    printf("\n\x1b[33m[aborted]\x1b[0m\n");
}

static void MaybeReportMem() {
  if (getenv("MA_PRINT_MEM")) {
    u64 b = PeakRssBytes();
    fprintf(stderr, "[peak RSS: %llu KB]\n", (unsigned long long)(b / 1024));
  }
}

// ---- login flow (loopback callback server) -------------------------------

struct LoginUx {
  bool AutoOpen;
};

// Invoked once the local callback server is listening, just before we block.
static void OnLoginReady(void* user, StrView url) {
  LoginUx* ux = (LoginUx*)user;
  printf("Open this URL in your browser to approve access:\n\n%.*s\n\n", (int)url.Length, url.Data);
  if (ux->AutoOpen) {
    OpenBrowser(url);
    printf("(opened in your default browser)\n");
  }
  printf("Waiting for you to approve access\xe2\x80\xa6 press Ctrl+C to cancel.\n");
  fflush(stdout);
}

static int RunLogin(Arena* arena, StrView authPath, const char* provider, bool autoOpen) {
  const OAuthConfig* cfg = OAuth_GetConfig(SV(provider));
  if (!cfg) {
    fprintf(stderr, "no OAuth config for provider '%s'\n", provider);
    return 1;
  }
  OAuthBegin beg = OAuth_BeginAuthCode(arena, cfg);

  g_abort = 0;
  signal(SIGINT, OnSigint);

  LoginUx ux = {autoOpen};
  OAuthCode code = {};
  StrBuf err = StrBuf_Make(arena, 128);
  if (!OAuth_WaitForCallback(arena, cfg, beg.AuthUrl, beg.Verifier, &code, &g_abort, &err, OnLoginReady,
                             &ux)) {
    fprintf(stderr, "\nlogin failed: %.*s\n", (int)err.Length, err.Data);
    return 1;
  }

  StrView state = code.State.Length ? code.State : beg.Verifier;
  Credential cred;
  StrBuf_Clear(&err);
  if (!OAuth_ExchangeCode(arena, cfg, code.Code, state, beg.Verifier, &cred, &err)) {
    fprintf(stderr, "login failed: %.*s\n", (int)err.Length, err.Data);
    return 1;
  }
  if (!AuthStore_Write(arena, authPath, cfg->Id, &cred)) {
    fprintf(stderr, "failed to save credentials to %.*s\n", (int)authPath.Length, authPath.Data);
    return 1;
  }
  printf("\nLogged in to %.*s. Saved to %.*s\n", (int)cfg->Name.Length, cfg->Name.Data,
         (int)authPath.Length, authPath.Data);
  return 0;
}

static void PrintHelp() {
  printf(
      "mini-agent - a tiny memory-efficient coding agent\n\n"
      "Usage: mini-agent [options]\n"
      "  -m <model>        Anthropic model id (default: claude-sonnet-4-6)\n"
      "  -p <prompt>       Run a single prompt and exit (print mode)\n"
      "  --login [prov]    OAuth login in your browser (default provider: anthropic)\n"
      "  --logout [prov]   Remove stored credentials (default: anthropic)\n"
      "  --no-open         With --login, print the URL instead of opening a browser\n"
      "  --faux            Scripted provider (no key/network; for demos)\n"
      "  --help            Show this help\n\n"
      "Auth: run `--login anthropic` for a Claude Pro/Max subscription (a local\n"
      "callback server captures the redirect automatically), or set ANTHROPIC_API_KEY.\n"
      "Interactive mode: Ctrl+C interrupts, /exit quits.\n");
}

// Plain line REPL (non-TTY / fallback): the original fgets loop.
static void RunPlainRepl(Agent* ag, bool faux, Faux* fauxState, const char* modelId) {
  printf("mini-agent (%s%s). Ctrl+C interrupts a turn, /exit quits.\n", faux ? "faux demo" : "model: ",
         faux ? "" : modelId);
  char line[8192];
  for (;;) {
    fputs("\n\x1b[1m\xe2\x80\xba \x1b[0m", stdout);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) break;
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
    if (L == 0) continue;
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) break;
    g_abort = 0;
    if (faux) fauxState->Index = 0;
    Agent_Prompt(ag, SV(line));
    ReportOutcome(ag);
  }
}

// Full-screen TUI REPL. Falls back to the plain REPL if not a TTY.
static void RunTuiRepl(Arena* arena, Agent* ag, bool faux, Faux* fauxState, const char* modelId) {
  TuiOpts opts;
  opts.ModelName = faux ? SVL("faux") : SV(modelId);
  opts.Color = (getenv("NO_COLOR") == nullptr);

  Tui tui;
  if (!Tui_Start(&tui, arena, opts)) {
    RunPlainRepl(ag, faux, fauxState, modelId);
    return;
  }
  ag->Sink.User = &tui;
  ag->Sink.OnEvent = Tui_AgentEvent;
  ag->User = &tui;
  tui.Ag = ag;
  ag->Stream.Abort = Tui_Abort(&tui);

  for (;;) {
    StrView line;
    if (!Tui_WaitForSubmit(&tui, &line)) break;
    *Tui_Abort(&tui) = 0;
    if (faux) fauxState->Index = 0;
    Agent_Prompt(ag, line);
  }
  Tui_Stop(&tui);
}

int main(int argc, char** argv) {
  const char* modelId = "claude-sonnet-4-6";
  const char* onePrompt = nullptr;
  const char* loginProvider = nullptr;
  const char* logoutProvider = nullptr;
  bool faux = false;
  bool noOpen = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
      modelId = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      onePrompt = argv[++i];
    } else if (strcmp(argv[i], "--faux") == 0) {
      faux = true;
    } else if (strcmp(argv[i], "--no-open") == 0) {
      noOpen = true;
    } else if (strcmp(argv[i], "--login") == 0) {
      loginProvider = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "anthropic";
    } else if (strcmp(argv[i], "--logout") == 0) {
      logoutProvider = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "anthropic";
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      PrintHelp();
      return 0;
    }
  }

  EnableTerminal();
  Http_GlobalInit();
  Arena arena = ArenaCreate((usize)2 << 30);
  StrView authPath = AuthStore_DefaultPath(&arena);

  if (loginProvider) {
    int rc = RunLogin(&arena, authPath, loginProvider, !noOpen);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return rc;
  }
  if (logoutProvider) {
    AuthStore_Delete(&arena, authPath, SV(logoutProvider));
    printf("Logged out of %s\n", logoutProvider);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 0;
  }

  AppAuth appAuth = {authPath};

  if (!faux) {
    StrView probe = Auth_ResolveToken(&arena, authPath, SV("anthropic"), "ANTHROPIC_API_KEY");
    if (probe.Length == 0) {
      fprintf(stderr,
              "error: no credentials. Run `mini-agent --login anthropic` or set "
              "ANTHROPIC_API_KEY (or use --faux for an offline demo)\n");
      Http_GlobalShutdown();
      ArenaRelease(&arena);
      return 1;
    }
  }

  Model model = Anthropic_Model(SV(modelId));
  Agent ag = Agent_Make(&arena, model,
                        SV("You are mini-agent, a concise coding assistant. Use the read and bash "
                           "tools when they help answer the user."));
  Agent_InitQueues(&ag);
  Agent_AddTool(&ag, ReadTool());
  Agent_AddTool(&ag, BashTool());
  ag.Stream.Abort = &g_abort;

  FauxStep steps[2] = {};
  Faux fauxState = {};
  if (faux) {
    steps[0].Text = SVL("Let me run a quick command.\n");
    steps[0].ToolName = SVL("bash");
    steps[0].ToolId = SVL("t1");
    steps[0].ToolArgsJson = SVL("{\"command\":\"echo hello-from-bash\"}");
    steps[1].Text = SVL("The command printed its greeting. All done.");
    steps[1].Stop = StopReason::Stop;
    fauxState.Steps = steps;
    fauxState.Count = 2;
    ag.Prov = Faux_Provider(&fauxState);
  } else {
    ag.Prov = Anthropic_Provider();
    ag.AuthUser = &appAuth;
    ag.ResolveAuth = AppResolveAuth;
  }

  ag.Sink.User = nullptr;
  ag.Sink.OnEvent = ReplSink;
  signal(SIGINT, OnSigint);

  if (onePrompt) {
    g_abort = 0;
    Agent_Prompt(&ag, SV(onePrompt));
    ReportOutcome(&ag);
    MaybeReportMem();
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 0;
  }

  if (Term_IsInteractive())
    RunTuiRepl(&arena, &ag, faux, &fauxState, modelId);
  else
    RunPlainRepl(&ag, faux, &fauxState, modelId);

  MaybeReportMem();
  Http_GlobalShutdown();
  ArenaRelease(&arena);
  return 0;
}
