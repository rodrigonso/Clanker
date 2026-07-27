#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "app/command.h"
#include "app/plain_select.h"
#include "app/provider_registry.h"
#include "app/settings.h"
#include "auth/oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/sysinfo.h"
#include "llm/faux.h"
#include "net/browser.h"
#include "net/http.h"
#include "term/term.h"
#include "term/tui.h"
#include "term/sanitize.h"
#include "tools/tools.h"
#include "tools/filelock.h"

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

struct InteractiveRuntime {
  Arena* Mem;
  StrView AuthPath;
  StrView SettingsPath;
  const ProviderSpec* Spec;
  Model CurrentModel;
  Agent* Ag;
  bool Faux;
  bool AutoOpen;
  Tui* TuiView;
};

static bool AppResolveAuth(void* user, Arena* arena, StrView provider, volatile i32* abort,
                           StrView* out) {
  AppAuth* aa = (AppAuth*)user;
  const ProviderSpec* spec = ProviderRegistry_Find(provider);
  StrView t =
      Auth_ResolveToken(arena, aa->Path, provider, spec ? spec->ApiKeyEnvVar : nullptr, abort);
  if (t.Length) {
    *out = t;
    return true;
  }
  return false;
}

static bool ResolvePromptAuth(Agent* ag) {
  if (ag->Stream.Abort && *ag->Stream.Abort) return false;
  if (!ag->ResolveAuth) return ag->Stream.ApiKey.Length > 0;
  ag->Stream.ApiKey = {};
  StrView token = {};
  if (!ag->ResolveAuth(ag->AuthUser, ag->Mem, ag->Model.Provider, ag->Stream.Abort, &token))
    return false;
  if (ag->Stream.Abort && *ag->Stream.Abort) return false;
  ag->Stream.ApiKey = token;
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
  StrBuf_AppendCStr(&b, " before sending a prompt (or use --faux for an offline demo).");
  return StrBuf_View(&b);
}

struct RuntimeSelection {
  const ProviderSpec* Spec;
  StrView ModelId;
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

  AppSettings settings = {};
  AppSettingsLoadResult load = AppSettings_Load(arena, settingsPath, &settings);
  if (load == AppSettingsLoadResult::Success) {
    const ProviderSpec* configured = ProviderRegistry_Find(settings.Provider);
    if (configured) {
      selected.Spec = configured;
      selected.ModelId = settings.Model;
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
  if (!OAuth_WaitForCallback(&scratch, cfg, begin.AuthUrl, begin.Verifier, &code, abort, &error,
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
  StrView state = code.State.Length ? code.State : begin.Verifier;
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
  StrView token =
      Auth_ResolveToken(&scratch, rt->AuthPath, spec->Id, spec->ApiKeyEnvVar, abort);
  ProviderModelList live = {};
  if (!(abort && *abort)) {
    RuntimeSetOperationStatus(rt, ux, SVL("Fetching the provider model catalog."));
    if (spec->ListModels) {
      live = spec->ListModels(&scratch, token, abort);
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
      break;
    }

    StrView initialId =
        rt->Spec && StrView_Eq(rt->Spec->Id, spec->Id) ? rt->CurrentModel.Id
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
    break;
  }

  chosen.Provider = StrView_Dup(rt->Mem, spec->Id);
  if (!chosen.Name.Length) chosen.Name = chosen.Id;
  ArenaRelease(&scratch);
  return RuntimeModelChoice{RuntimeChoiceStatus::Selected, chosen, StrView{}};
}

static bool RuntimeCheckAuth(InteractiveRuntime* rt, const AppCommandUx* ux, bool asError) {
  if (rt->Faux) return true;
  volatile i32* abort = rt->Ag ? rt->Ag->Stream.Abort : nullptr;
  if (abort && *abort) return false;
  if (rt->Ag && ResolvePromptAuth(rt->Ag)) return true;
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
  AppSettings settings = {rt->Spec->Id, rt->CurrentModel.Id};
  if (!AppSettings_Save(&scratch, rt->SettingsPath, &settings)) {
    StrBuf message = StrBuf_Make(&scratch, rt->SettingsPath.Length + 96);
    StrBuf_AppendCStr(
        &message,
        "The provider/model selection is active, but settings could not be saved to ");
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
                                     const Model& model, const AppCommandUx* ux,
                                     bool* persisted = nullptr) {
  if (!rt->Ag || !spec || !spec->CreateProvider || !model.Id.Length) return false;

  Provider provider = spec->CreateProvider(rt->Mem);
  rt->Spec = spec;
  rt->CurrentModel = model;
  rt->Ag->Model = model;
  rt->Ag->Prov = provider;
  rt->Ag->Stream.ApiKey = {};
  if (rt->TuiView) Tui_SetModelName(rt->TuiView, model.Id);
  bool saved = PersistRuntimeSelection(rt, ux);
  if (persisted) *persisted = saved;
  RuntimeCheckAuth(rt, ux, false);
  return true;
}

static StrView SelectionMessage(InteractiveRuntime* rt, const char* prefix) {
  StrBuf message =
      StrBuf_Make(rt->Mem, rt->Spec->Name.Length + rt->CurrentModel.Id.Length + 48);
  StrBuf_AppendCStr(&message, prefix);
  StrBuf_Append(&message, rt->Spec->Name);
  StrBuf_AppendCStr(&message, " model '");
  StrBuf_Append(&message, rt->CurrentModel.Id);
  StrBuf_AppendCStr(&message, "'.");
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
    if (!ActivateRuntimeSelection(rt, provider.Spec, model.Selected, ux))
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
      if (!ActivateRuntimeSelection(rt, provider.Spec, fallback, ux, &persisted))
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
    } else if (!ActivateRuntimeSelection(rt, provider.Spec, model.Selected, ux)) {
      return AppCommandHandlerResult{
          AppCommandHandlerStatus::Failed,
          SVL("Login succeeded, but the selected model could not be activated.")};
    }

    return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                   SelectionMessage(rt, "Logged in; using ")};
  }
}

static bool RuntimeResetSession(InteractiveRuntime* rt, const AppCommandUx* ux) {
  if (!rt->Ag || !Agent_ResetSession(rt->Ag)) {
    EmitCommandError(ux, SVL("Unable to clear the session while the agent is active."));
    return false;
  }
  if (rt->TuiView && !Tui_ClearSession(rt->TuiView)) {
    EmitCommandError(
        ux, SVL("The agent session was reset, but the TUI session could not be cleared."));
    return false;
  }
  EmitCommandNotice(ux, SVL("session cleared"));
  return true;
}

static AppCommandHandlers RuntimeCommandHandlers(InteractiveRuntime* rt) {
  AppCommandHandlers handlers = {};
  bool selectorsAvailable = rt->TuiView || Term_IsInteractive();
  if (!rt->Faux && selectorsAvailable) {
    handlers.User = rt;
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
      "  --login [prov]    OAuth login in your browser (default provider: anthropic)\n"
      "  --logout [prov]   Remove stored credentials (default: anthropic)\n"
      "  --no-open         Do not auto-open a browser for --login or interactive /login\n"
      "  --faux            Scripted provider (no key/network; for demos)\n"
      "  --help            Show this help\n\n"
      "Auth: run `--login anthropic` for a Claude Pro/Max subscription (a local\n"
      "callback server captures the redirect automatically), or set ANTHROPIC_API_KEY.\n"
      "Interactive commands: /login, /model, /clear, /exit (alias: /quit).\n");
}

// Plain line REPL (non-TTY / fallback): the original fgets loop.
static void RunPlainRepl(InteractiveRuntime* rt, Faux* fauxState) {
  rt->TuiView = nullptr;
  if (rt->Faux)
    printf("Clank (faux demo). Ctrl+C interrupts a turn, /exit quits.\n");
  else
    printf("Clank (%.*s / %.*s). Ctrl+C interrupts a turn, /exit quits.\n",
           (int)rt->Spec->Name.Length, rt->Spec->Name.Data, (int)rt->CurrentModel.Id.Length,
           rt->CurrentModel.Id.Data);

  char line[8192];
  AppCommandUx commandUx = {nullptr, PlainCommandNotice, PlainCommandError};
  if (!rt->Faux) RuntimeCheckAuth(rt, &commandUx, false);
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
    if (rt->Faux) fauxState->Index = 0;
    Agent_Prompt(rt->Ag, SV(line));
    ReportOutcome(rt->Ag);
  }
}

// Full-screen TUI REPL. Falls back to the plain REPL if not a TTY.
static void RunTuiRepl(InteractiveRuntime* rt, Faux* fauxState) {
  TuiOpts opts;
  opts.ModelName = rt->Faux ? SVL("faux") : rt->CurrentModel.Id;
  opts.Color = (getenv("NO_COLOR") == nullptr);
  u32 catalogCount = AppCommandSpec_Count();
  TuiCommandSpec* commands = ArenaPushArray(rt->Mem, TuiCommandSpec, catalogCount);
  for (u32 i = 0; i < catalogCount; i++) {
    const AppCommandSpec* command = AppCommandSpec_At(i);
    if (rt->Faux &&
        (command->Kind == AppCommandKind::Login || command->Kind == AppCommandKind::Model))
      continue;
    commands[opts.CommandCount++] =
        TuiCommandSpec{command->Name, command->Alias, command->Description};
  }
  opts.Commands = commands;

  Tui tui;
  if (!Tui_Start(&tui, rt->Mem, opts)) {
    RunPlainRepl(rt, fauxState);
    return;
  }
  rt->TuiView = &tui;
  rt->Ag->Sink.User = &tui;
  rt->Ag->Sink.OnEvent = Tui_AgentEvent;
  rt->Ag->User = &tui;
  tui.Ag = rt->Ag;
  rt->Ag->Stream.Abort = Tui_Abort(&tui);

  AppCommandUx commandUx = {&tui, TuiCommandNotice, TuiCommandError};
  if (!rt->Faux) RuntimeCheckAuth(rt, &commandUx, false);
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
    volatile i32* preflightAbort = Tui_OperationAbort(&tui);
    rt->Ag->Stream.Abort = preflightAbort;
    bool authenticated = RuntimeCheckAuth(rt, &commandUx, true);
    rt->Ag->Stream.Abort = Tui_Abort(&tui);
    bool cancelled = Tui_EndOperation(&tui);
    if (!authenticated || cancelled) continue;
    *Tui_Abort(&tui) = 0;
    if (!Tui_AcceptPrompt(&tui, submission.Text)) continue;
    if (rt->Faux) fauxState->Index = 0;
    Agent_Prompt(rt->Ag, submission.Text);
  }
  Tui_Stop(&tui);
  rt->Ag->Sink.User = nullptr;
  rt->Ag->Sink.OnEvent = ReplSink;
  rt->Ag->User = nullptr;
  rt->Ag->Stream.Abort = &g_abort;
  rt->TuiView = nullptr;
}

int main(int argc, char** argv) {
  const char* modelOverride = nullptr;
  const char* onePrompt = nullptr;
  const char* loginProvider = nullptr;
  const char* logoutProvider = nullptr;
  bool faux = false;
  bool noOpen = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
      modelOverride = argv[++i];
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
  StrView settingsPath = AppSettings_DefaultPath(&arena);
  signal(SIGINT, OnSigint);

  if (loginProvider) {
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

  AppAuth appAuth = {authPath};
  RuntimeSelection selected = {};
  if (!faux) {
    selected = LoadRuntimeSelection(&arena, settingsPath, modelOverride);
    if (!selected.Spec || !selected.Spec->CreateProvider || !selected.Spec->CreateModel) {
      fprintf(stderr, "error: no usable built-in provider is registered\n");
      Http_GlobalShutdown();
      ArenaRelease(&arena);
      return 1;
    }
  }

  Model model = faux ? Model{} : selected.Spec->CreateModel(&arena, selected.ModelId);
  Agent ag = Agent_Make(&arena, model,
                        SV("You are Clank, a concise coding assistant. You have these tools: "
                           "read, write, edit, ls, find, grep, and bash. Use them when they help "
                           "answer the user."));
  Agent_InitQueues(&ag);
  FileLock_Init();
  Agent_AddTool(&ag, ReadTool());
  Agent_AddTool(&ag, WriteTool());
  Agent_AddTool(&ag, EditTool());
  Agent_AddTool(&ag, LsTool());
  Agent_AddTool(&ag, FindTool());
  Agent_AddTool(&ag, GrepTool());
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
    ag.Prov = selected.Spec->CreateProvider(&arena);
    ag.AuthUser = &appAuth;
    ag.ResolveAuth = AppResolveAuth;
  }

  ag.Sink.User = nullptr;
  ag.Sink.OnEvent = ReplSink;

  InteractiveRuntime runtime = {};
  runtime.Mem = &arena;
  runtime.AuthPath = authPath;
  runtime.SettingsPath = settingsPath;
  runtime.Spec = selected.Spec;
  runtime.CurrentModel = model;
  runtime.Ag = &ag;
  runtime.Faux = faux;
  runtime.AutoOpen = !noOpen;

  if (onePrompt) {
    g_abort = 0;
    if (!faux && !ResolvePromptAuth(&ag)) {
      if (g_abort) {
        Agent_Release(&ag);
        Http_GlobalShutdown();
        ArenaRelease(&arena);
        return 130;
      }
      StrView authGuidance = AuthGuidance(&arena, runtime.Spec, false);
      fprintf(stderr, "error: %.*s\n", (int)authGuidance.Length, authGuidance.Data);
      Agent_Release(&ag);
      Http_GlobalShutdown();
      ArenaRelease(&arena);
      return 1;
    }
    g_abort = 0;
    Agent_Prompt(&ag, SV(onePrompt));
    ReportOutcome(&ag);
    MaybeReportMem();
    Agent_Release(&ag);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 0;
  }

  if (Term_IsInteractive())
    RunTuiRepl(&runtime, &fauxState);
  else
    RunPlainRepl(&runtime, &fauxState);

  MaybeReportMem();
  Agent_Release(&ag);
  Http_GlobalShutdown();
  ArenaRelease(&arena);
  return 0;
}
