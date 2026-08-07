#include "app/auth_flow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/settings.h"
#include "auth/oauth.h"
#include "auth/store.h"
#include "net/browser.h"

void AppAuth_Invalidate(AppAuth* auth) {
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

bool StartAuthWarmup(AuthWarmup* warmup, AppAuth* auth,
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

void FinishRuntimeAuthWarmup(InteractiveRuntime* rt) {
  if (rt && rt->AuthWarmupState)
    JoinAuthWarmup(rt->AuthWarmupState, rt->Auth);
}

void CancelRuntimeAuthWarmup(InteractiveRuntime* rt) {
  if (!rt || !rt->AuthWarmupState) return;
  CancelAuthWarmup(rt->AuthWarmupState, rt->Auth);
  rt->AuthWarmupState = nullptr;
}

bool AppResolveAuth(void* user, Arena* arena, StrView provider, volatile i32* abort,
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

bool ResolvePromptAuth(AgentHarness* harness, volatile i32* abort) {
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

StrView AuthGuidance(Arena* arena, const ProviderSpec* spec, bool interactive) {
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

bool RuntimeCheckAuth(InteractiveRuntime* rt, const AppCommandUx* ux, bool asError) {
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

bool ActivateRuntimeSelection(InteractiveRuntime* rt, const ProviderSpec* spec,
                              const Model& model, ModelRequestProfile preference,
                              const AppCommandUx* ux, bool* persisted) {
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

StrView SelectionMessage(InteractiveRuntime* rt, const char* prefix) {
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

AppCommandHandlerResult HandleModelCommand(void* user, const AppCommandUx* ux) {
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

AppCommandHandlerResult HandleLoginCommand(void* user, const AppCommandUx* ux) {
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

static void ReportStandaloneCommandResult(const AppCommandUx* ux,
                                          AppCommandHandlerResult result) {
  if (result.Status == AppCommandHandlerStatus::Success ||
      result.Status == AppCommandHandlerStatus::Cancelled)
    EmitCommandNotice(ux, result.Message);
  else
    EmitCommandError(ux, result.Message);
}

int RunCommandLineLogin(Arena* arena, StrView authPath, const char* providerId,
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
