#include "app/auth_flow.h"

#include <stdio.h>
#include <string.h>

#include "app/settings.h"
#include "auth/store.h"
#include "base/thread.h"

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
  if (token.Length) {
    AppAuth_CacheToken(aa, provider, token);
    out->Token = token;
    return true;
  }
  // Local OpenAI-compatible servers authenticate nobody. Succeeding with an
  // empty token lets the request go out without an Authorization header
  // instead of failing the prompt on a credential the endpoint would ignore.
  return spec && spec->Endpoint && spec->Endpoint->AllowAnonymous;
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
  // Interactive `/auth` can take a key directly; the headless path can only
  // point at the environment.
  if (interactive) {
    StrBuf_AppendCStr(&b, "Run `/auth` and choose ");
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

  AppSettings settings = {};
  AppSettingsLoadResult load = AppSettings_Load(arena, settingsPath, &settings);
  if (load == AppSettingsLoadResult::Success) {
    selected.DefaultTrust = settings.DefaultTrust;
    selected.Compaction = settings.Compaction;
    selected.Retry = settings.Retry;
    selected.TurnLimit = settings.TurnLimit;
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

static AppCommandHandlerResult RunApiKeyEntry(InteractiveRuntime* rt,
                                              const ProviderSpec* spec,
                                              const AppCommandUx* ux) {
  if (!spec)
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("No provider is selected.")};

  Arena scratch = ArenaCreate((usize)16 << 20);
  StrBuf title = StrBuf_Make(&scratch, spec->Name.Length + 24);
  StrBuf_AppendCStr(&title, "API key for ");
  StrBuf_Append(&title, spec->Name);

  StrView entered = {};
  for (;;) {
    RuntimeInputResult entry =
        RuntimeReadText(rt, &scratch, StrBuf_View(&title), SVL("API key"),
                        StrView{}, &entered);
    if (entry != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     SVL("Authentication cancelled.")};
    }
    entered = TrimAscii(entered);
    if (entered.Length) break;
    EmitCommandError(ux, SVL("An API key is required."));
  }

  Credential credential = {};
  credential.ApiKey = entered;
  if (!AuthStore_Write(&scratch, rt->AuthPath, spec->Id, &credential)) {
    StrBuf message = StrBuf_Make(&scratch, rt->AuthPath.Length + 48);
    StrBuf_AppendCStr(&message, "Failed to save credentials to ");
    StrBuf_Append(&message, rt->AuthPath);
    StrBuf_AppendChar(&message, '.');
    AppCommandHandlerResult result = {
        AppCommandHandlerStatus::Failed,
        StrView_Dup(&rt->CommandMem, StrBuf_View(&message))};
    ArenaRelease(&scratch);
    return result;
  }
  AppAuth_CacheToken(rt->Auth, spec->Id, credential.ApiKey);

  // The key itself is never echoed back into the transcript.
  StrBuf message =
      StrBuf_Make(&scratch, spec->Name.Length + rt->AuthPath.Length + 48);
  StrBuf_AppendCStr(&message, "Saved an API key for ");
  StrBuf_Append(&message, spec->Name);
  StrBuf_AppendCStr(&message, " to ");
  StrBuf_Append(&message, rt->AuthPath);
  StrBuf_AppendChar(&message, '.');
  AppCommandHandlerResult result = {
      AppCommandHandlerStatus::Success,
      StrView_Dup(&rt->CommandMem, StrBuf_View(&message))};
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
      AppCommandHandlerResult keyEntry = RunApiKeyEntry(rt, provider.Spec, ux);
      if (keyEntry.Status != AppCommandHandlerStatus::Success) return keyEntry;
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
            SVL("The API key was saved, but the provider could not be activated.")};

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
          SVL("The API key was saved, but the selected model could not be activated.")};
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
