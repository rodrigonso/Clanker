#include "app/auth_flow.h"

#include <stdio.h>
#include <string.h>

#include "app/browser.h"
#include "app/settings.h"
#include "auth/openrouter_oauth.h"
#include "auth/store.h"
#include "base/thread.h"
#include "net/loopback.h"

void AppAuth_Invalidate(AppAuth* auth) {
  if (auth) AuthTokenCache_Invalidate(&auth->Cache);
}

static bool AppAuth_CacheToken(AppAuth* auth, StrView provider, StrView token) {
  return auth &&
         AuthTokenCache_Store(auth->Mem, &auth->Cache, provider, token);
}

bool AppResolveAuth(void* user, Arena* arena, StrView provider, volatile i32* abort,
                           ProviderAuth* out) {
  if (out) *out = {};
  if (!out) return false;
  AppAuth* aa = (AppAuth*)user;
  if (abort && Atomic_LoadI32(abort)) return false;
  StrView cached = {};
  if (AuthTokenCache_Get(&aa->Cache, provider, &cached)) {
    out->Token = cached;
    return true;
  }
  const ProviderSpec* spec = ProviderRegistry_Find(provider);
  StrView token = Auth_ResolveToken(arena, aa->Path, provider,
                                    spec ? spec->ApiKeyEnvVar : nullptr);
  if (!token.Length) return false;
  AppAuth_CacheToken(aa, provider, token);
  out->Token = token;
  return true;
}

bool ResolvePromptAuth(AgentHarness* harness, volatile i32* abort) {
  if (!harness || (abort && Atomic_LoadI32(abort))) return false;
  if (!harness->Config.ResolveAuth)
    return harness->Config.Stream.Auth.Token.Length > 0;
  harness->Config.Stream.Auth = {};
  ProviderAuth auth = {};
  if (!harness->Config.ResolveAuth(harness->Config.AuthUser, &harness->Work,
                                   harness->Config.ModelInfo.Provider, abort,
                                   &auth))
    return false;
  if (abort && Atomic_LoadI32(abort)) return false;
  harness->Config.Stream.Auth = auth;
  return true;
}

StrView AuthGuidance(Arena* arena, const ProviderSpec* spec, bool interactive) {
  if (!spec) return SVL("No provider is selected.");
  StrBuf b = StrBuf_Make(arena, 192);
  StrBuf_AppendCStr(&b, "No credentials are configured for ");
  StrBuf_Append(&b, spec->Name);
  StrBuf_AppendCStr(&b, " ('");
  StrBuf_Append(&b, spec->Id);
  StrBuf_AppendCStr(&b, "'). ");
  // Interactive `/auth` can sign in through the browser; the headless path can
  // only point at the environment.
  if (interactive) {
    StrBuf_AppendCStr(&b, "Run `/auth` to sign in to ");
    StrBuf_Append(&b, spec->Name);
    if (spec->ApiKeyEnvVar) StrBuf_AppendCStr(&b, " or ");
  }
  if (spec->ApiKeyEnvVar) {
    StrBuf_AppendCStr(&b, "set ");
    StrBuf_AppendCStr(&b, spec->ApiKeyEnvVar);
  } else if (!interactive) {
    StrBuf_AppendCStr(&b, "Configure credentials for this provider");
  }
  StrBuf_AppendCStr(&b, " before sending a prompt.");
  return StrBuf_View(&b);
}

static void WarnUsingDefaults(StrView path, const ProviderSpec* defaults, const char* reason) {
  fprintf(stderr,
          "warning: %s %.*s; using built-in defaults provider '%.*s', model '%.*s'\n",
          reason, (int)path.Length, path.Data, (int)defaults->Id.Length, defaults->Id.Data,
          (int)defaults->DefaultModel.Length, defaults->DefaultModel.Data);
}

RuntimeSelection LoadRuntimeSelection(Arena* arena, StrView settingsPath,
                                             const char* modelOverride) {
  RuntimeSelection selected = {};
  const ProviderSpec* defaults = ProviderRegistry_At(0);
  if (!defaults) return selected;

  selected.Spec = defaults;
  selected.ModelId = defaults->DefaultModel;
  selected.DefaultTrust = DefaultProjectTrust::Ask;
  selected.Compaction = AgentCompactionPolicy_Default();
  selected.Retry = AgentRetryPolicy_Default();
  selected.TurnLimit = AgentTurnLimitPolicy_Default();
  selected.PermissionMode = AgentPermissionMode::Auto;
  selected.PermissionReviewerModel = AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL;
  selected.YoloAcknowledged = false;

  AppSettings settings = {};
  AppSettingsLoadResult load = AppSettings_Load(arena, settingsPath, &settings);
  if (load == AppSettingsLoadResult::Success) {
    selected.DefaultTrust = settings.DefaultTrust;
    selected.Compaction = settings.Compaction;
    selected.Retry = settings.Retry;
    selected.TurnLimit = settings.TurnLimit;
    selected.PermissionMode = settings.PermissionMode;
    selected.PermissionReviewerModel = settings.PermissionReviewerModel;
    selected.YoloAcknowledged = settings.YoloAcknowledged;
    selected.Rounded = settings.Rounded;
    selected.Skills = settings.Skills;
    selected.PromptTemplates = settings.PromptTemplates;
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

struct RuntimeProviderChoice {
  RuntimeChoiceStatus Status;
  const ProviderSpec* Spec;
};

static RuntimeProviderChoice ChooseProvider(InteractiveRuntime* rt) {
  Arena scratch = ArenaCreate((usize)16 << 20);
  u32 count = ProviderRegistry_Count();

  const ProviderSpec** specs = ArenaPushArray(&scratch, const ProviderSpec*, count);
  StrView* options = ArenaPushArray(&scratch, StrView, count);
  u32 initial = 0;
  for (u32 i = 0; i < count; i++) {
    const ProviderSpec* spec = ProviderRegistry_At(i);
    specs[i] = spec;
    options[i] = ProviderRegistry_Label(&scratch, spec);
    if (rt->Spec && StrView_Eq(rt->Spec->Id, spec->Id)) initial = i;
  }

  u32 selected = 0;
  RuntimeInputResult result = RuntimeSelect(rt, SVL("Choose a model provider"),
                                            options, count, &selected, initial);
  const ProviderSpec* spec =
      result == RuntimeInputResult::Confirmed ? specs[selected] : nullptr;
  ArenaRelease(&scratch);
  return RuntimeProviderChoice{result == RuntimeInputResult::Confirmed
                                   ? RuntimeChoiceStatus::Selected
                               : result == RuntimeInputResult::Back
                                   ? RuntimeChoiceStatus::Back
                                   : RuntimeChoiceStatus::Cancelled,
                               spec};
}

enum class OAuthCodeStatus : u8 { Received, Unavailable, Cancelled, Failed };

struct OAuthCodeResult {
  OAuthCodeStatus Status;
  StrView Code;
  StrView Message;
};

// OpenRouter's code expires after ten minutes, so waiting longer is pointless.
static const u32 OAUTH_CALLBACK_WAIT_MS = 10 * 60 * 1000;

// Browser + loopback redirect. Unavailable means fall back to pasting a code.
static OAuthCodeResult ReceiveOAuthCallback(InteractiveRuntime* rt, Arena* scratch,
                                            const OAuthPkce* pkce,
                                            const AppCommandUx* ux) {
  LoopbackServer server;
  if (!LoopbackServer_Open(&server))
    return OAuthCodeResult{OAuthCodeStatus::Unavailable, StrView{}, StrView{}};

  StrBuf callback = StrBuf_Make(scratch, 48);
  StrBuf_AppendCStr(&callback, "http://localhost:");
  StrBuf_AppendU64(&callback, server.Port);
  StrBuf_AppendCStr(&callback, OPENROUTER_OAUTH_CALLBACK_PATH);
  StrView url = OpenRouterOAuth_AuthorizeUrl(scratch, pkce, StrBuf_View(&callback));
  if (!Browser_Open(url)) {
    LoopbackServer_Close(&server);
    return OAuthCodeResult{OAuthCodeStatus::Unavailable, StrView{}, StrView{}};
  }

  StrBuf notice = StrBuf_Make(scratch, url.Length + 96);
  StrBuf_AppendCStr(&notice,
                    "Opened your browser to sign in to OpenRouter. If it did not "
                    "open, visit:\n");
  StrBuf_Append(&notice, url);
  EmitCommandNotice(ux, StrBuf_View(&notice));

  volatile i32* abort =
      RuntimeBeginOperation(rt, ux, SVL("Waiting for OpenRouter sign-in"));
  RuntimeSetOperationStatus(rt, ux, SVL("Approve Clanker in your browser."));
  OAuthCodeResult result = {OAuthCodeStatus::Failed, StrView{}, StrView{}};
  u64 deadline = MonotonicMillis() + OAUTH_CALLBACK_WAIT_MS;
  for (;;) {
    // Rejected requests roll the arena back so stray connections cannot grow it.
    usize mark = scratch->Used;
    u64 now = MonotonicMillis();
    LoopbackRequest request;
    LoopbackWait wait = LoopbackServer_Accept(
        &server, scratch, abort, now < deadline ? (u32)(deadline - now) : 0, &request);
    if (wait == LoopbackWait::Aborted) {
      result.Status = OAuthCodeStatus::Cancelled;
      break;
    }
    if (wait == LoopbackWait::TimedOut) {
      result.Message = SVL("Timed out waiting for OpenRouter sign-in.");
      break;
    }
    if (wait != LoopbackWait::Request) {
      result.Message = SVL("The sign-in callback listener failed.");
      break;
    }

    OAuthCallback callbackResult = OAuthCallback_Parse(scratch, request.Target, pkce->State);
    if (callbackResult.Status == OAuthCallbackStatus::WrongPath) {
      LoopbackRequest_Respond(&request, 404, SVL("Not found."));
      ArenaRestore(scratch, mark);
      continue;
    }
    // A stale tab from an earlier attempt must not end this one.
    if (callbackResult.Status == OAuthCallbackStatus::StateMismatch ||
        callbackResult.Status == OAuthCallbackStatus::MissingCode) {
      LoopbackRequest_Respond(
          &request, 400,
          SVL("<!doctype html><meta charset=utf-8><title>Clanker</title>"
              "<p>This sign-in link is stale. Return to Clanker and run /auth again.</p>"));
      ArenaRestore(scratch, mark);
      continue;
    }
    if (callbackResult.Status == OAuthCallbackStatus::Denied) {
      LoopbackRequest_Respond(
          &request, 200,
          SVL("<!doctype html><meta charset=utf-8><title>Clanker</title>"
              "<p>Sign-in was cancelled. You can close this tab.</p>"));
      StrBuf message = StrBuf_Make(scratch, callbackResult.Error.Length + 48);
      StrBuf_AppendCStr(&message, "OpenRouter sign-in was denied: ");
      StrBuf_Append(&message, callbackResult.Error);
      result.Message = StrBuf_View(&message);
      break;
    }
    LoopbackRequest_Respond(
        &request, 200,
        SVL("<!doctype html><meta charset=utf-8><title>Clanker</title>"
            "<p>Clanker is signed in to OpenRouter. You can close this tab.</p>"));
    result.Status = OAuthCodeStatus::Received;
    result.Code = callbackResult.Code;
    break;
  }
  bool cancelled = RuntimeEndOperation(rt);
  LoopbackServer_Close(&server);
  if (cancelled && result.Status != OAuthCodeStatus::Received)
    result.Status = OAuthCodeStatus::Cancelled;
  return result;
}

// Headless mode: OpenRouter shows the code and the user pastes it here.
static OAuthCodeResult ReadPastedOAuthCode(InteractiveRuntime* rt, Arena* scratch,
                                           const OAuthPkce* pkce,
                                           const AppCommandUx* ux) {
  StrView url = OpenRouterOAuth_AuthorizeUrl(scratch, pkce, StrView{});
  StrBuf notice = StrBuf_Make(scratch, url.Length + 128);
  StrBuf_AppendCStr(&notice,
                    "Open this URL in a browser, approve Clanker, then paste the "
                    "code OpenRouter shows:\n");
  StrBuf_Append(&notice, url);
  EmitCommandNotice(ux, StrBuf_View(&notice));

  for (;;) {
    StrView entered = {};
    RuntimeInputResult entry =
        RuntimeReadText(rt, scratch, SVL("Sign in to OpenRouter"),
                        SVL("Authorization code"), StrView{}, &entered);
    if (entry != RuntimeInputResult::Confirmed)
      return OAuthCodeResult{OAuthCodeStatus::Cancelled, StrView{}, StrView{}};
    entered = StrView_Trim(entered);
    if (entered.Length)
      return OAuthCodeResult{OAuthCodeStatus::Received, entered, StrView{}};
    EmitCommandError(ux, SVL("An authorization code is required."));
  }
}

static AppCommandHandlerResult RunOAuthLogin(InteractiveRuntime* rt,
                                             const ProviderSpec* spec,
                                             const AppCommandUx* ux) {
  if (!spec || !spec->Endpoint)
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("No provider is selected.")};

  Arena scratch = ArenaCreate((usize)16 << 20);
  OAuthPkce pkce = OAuthPkce_Create(&scratch);
  OAuthCodeResult code = {OAuthCodeStatus::Unavailable, StrView{}, StrView{}};
  if (!Browser_IsRemoteSession())
    code = ReceiveOAuthCallback(rt, &scratch, &pkce, ux);
  if (code.Status == OAuthCodeStatus::Unavailable)
    code = ReadPastedOAuthCode(rt, &scratch, &pkce, ux);

  AppCommandHandlerResult result = {AppCommandHandlerStatus::Success, StrView{}};
  if (code.Status == OAuthCodeStatus::Cancelled) {
    result = {AppCommandHandlerStatus::Cancelled, SVL("Authentication cancelled.")};
  } else if (code.Status != OAuthCodeStatus::Received) {
    result = RuntimeOwnedFailure(rt, code.Message);
  } else {
    volatile i32* abort =
        RuntimeBeginOperation(rt, ux, SVL("Creating an OpenRouter API key"));
    StrView keysUrl = Responses_Url(
        &scratch, ResponsesEndpoint_ResolveBaseUrl(&scratch, spec->Endpoint),
        "auth/keys");
    OAuthKeyResult key =
        OpenRouterOAuth_Exchange(&scratch, keysUrl, code.Code, pkce.Verifier, abort);
    bool cancelled = RuntimeEndOperation(rt);

    Credential credential = {};
    credential.ApiKey = key.Key;
    if (cancelled) {
      result = {AppCommandHandlerStatus::Cancelled, SVL("Authentication cancelled.")};
    } else if (!key.Ok) {
      StrBuf message = StrBuf_Make(&scratch, key.Error.Length + 48);
      StrBuf_AppendCStr(&message, "OpenRouter sign-in failed: ");
      StrBuf_Append(&message, key.Error);
      result = RuntimeOwnedFailure(rt, StrBuf_View(&message));
    } else if (!AuthStore_Write(&scratch, rt->AuthPath, spec->Id, &credential)) {
      StrBuf message = StrBuf_Make(&scratch, rt->AuthPath.Length + 48);
      StrBuf_AppendCStr(&message, "Failed to save credentials to ");
      StrBuf_Append(&message, rt->AuthPath);
      StrBuf_AppendChar(&message, '.');
      result = RuntimeOwnedFailure(rt, StrBuf_View(&message));
    } else {
      AppAuth_CacheToken(rt->Auth, spec->Id, credential.ApiKey);
    }
  }
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
  ProviderAuth auth = {};
  AppResolveAuth(rt->Auth, &scratch, spec->Id, abort, &auth);
  ProviderModelList live = {};
  if (!(abort && Atomic_LoadI32(abort))) {
    RuntimeSetOperationStatus(rt, ux, SVL("Fetching the provider model catalog."));
    if (spec->ListModels) {
      live = spec->ListModels(&scratch, spec, auth, abort);
    } else {
      live.Status = ProviderModelListStatus::Unavailable;
      live.Error = SVL("this provider does not support live model discovery");
    }
  }
  bool cancelled = RuntimeEndOperation(rt);

  if (cancelled || (abort && Atomic_LoadI32(abort))) {
    ArenaRelease(&scratch);
    return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{}, StrView{}};
  }

  ProviderModelList models = {};
  if (live.Status == ProviderModelListStatus::Success) {
    models = live;
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
    StrBuf_AppendCStr(&message, ". Manual model-ID entry remains available.");
    if (live.Status == ProviderModelListStatus::Failed)
      EmitCommandError(ux, StrBuf_View(&message));
    else
      EmitCommandNotice(ux, StrBuf_View(&message));

    models.Status = ProviderModelListStatus::Success;
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
        entered = StrView_Trim(entered);
        if (entered.Length) {
          chosen = spec->CreateModel(rt->Mem, spec, entered);
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

bool RuntimeCheckAuth(InteractiveRuntime* rt, const AppCommandUx* ux, bool asError) {
  volatile i32* abort =
      rt->TuiView ? Tui_OperationAbort(rt->TuiView) : &g_abort;
  if (abort && Atomic_LoadI32(abort)) return false;
  if (rt->Harness && ResolvePromptAuth(rt->Harness, abort)) return true;
  if (abort && Atomic_LoadI32(abort)) return false;

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
  if (RuntimePersistSettings(rt)) return true;
  Arena scratch = ArenaCreate((usize)8 << 20);
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

bool ActivateRuntimeSelection(InteractiveRuntime* rt, const ProviderSpec* spec,
                              const Model& model, ModelRequestProfile preference,
                              const AppCommandUx* ux, bool* persisted) {
  if (!rt->Harness || !spec || !spec->CreateProvider || !model.Id.Length ||
      !Model_MetadataIsValid(&model))
    return false;

  preference = Model_NormalizeRequestPreference(&model, preference);
  ModelRequestProfile activeProfile =
      Model_ResolveRequestProfile(&model, preference);
  Provider provider = spec->CreateProvider(rt->Mem, spec);
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
  if (!RuntimeRefreshPermissionReviewer(rt)) {
    EmitCommandError(ux, SVL("The permission reviewer could not be configured."));
    return false;
  }
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

StrView SelectionMessage(InteractiveRuntime* rt, const char* prefix) {
  ModelRequestProfile profile = rt->Harness->Config.Stream.Profile;
  StrBuf message =
      StrBuf_Make(&rt->CommandMem, rt->Spec->Name.Length +
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

AppCommandHandlerResult HandleModelCommand(void* user, const AppCommandUx* ux) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  for (;;) {
    RuntimeProviderChoice provider = ChooseProvider(rt);
    if (provider.Status == RuntimeChoiceStatus::Back)
      return ReturnToCommandPalette(rt);
    if (provider.Status == RuntimeChoiceStatus::Cancelled)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     StrView{}};

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

AppCommandHandlerResult HandleAuthCommand(void* user, const AppCommandUx* ux) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  const ProviderSpec* authenticated = nullptr;
  for (;;) {
    RuntimeProviderChoice provider = ChooseProvider(rt);
    if (provider.Status == RuntimeChoiceStatus::Back)
      return ReturnToCommandPalette(rt);
    if (provider.Status == RuntimeChoiceStatus::Cancelled)
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     StrView{}};

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
      Model fallback = provider.Spec->CreateModel(
          rt->Mem, provider.Spec, provider.Spec->DefaultModel);
      bool persisted = false;
      if (!ActivateRuntimeSelection(rt, provider.Spec, fallback,
                                    ModelRequestProfile{}, ux, &persisted))
        return AppCommandHandlerResult{
            AppCommandHandlerStatus::Failed,
            SVL("Signed in, but the provider could not be activated.")};

      StrBuf notice =
          StrBuf_Make(&rt->CommandMem, provider.Spec->Name.Length +
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
          SVL("Signed in, but the selected model could not be activated.")};
    }

    return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                   SelectionMessage(rt, "Authenticated; using ")};
  }
}

int RunCommandLineLogout(Arena* arena, StrView authPath, const char* providerId) {
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
