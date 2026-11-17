#include "app/auth_flow.h"

#include <stdio.h>
#include <string.h>

#include "app/browser.h"
#include "app/model_cache.h"
#include "app/settings.h"
#include "auth/openai_codex_oauth.h"
#include "auth/openrouter_oauth.h"
#include "auth/store.h"
#include "base/process_file_lock.h"
#include "base/thread.h"
#include "net/loopback.h"

void AppAuth_Invalidate(AppAuth* auth) {
  if (auth) AuthTokenCache_Invalidate(&auth->Cache);
}

static bool AppAuth_CacheToken(AppAuth* auth, StrView provider, StrView token,
                               StrView accountId = {}, u64 expiresAtMs = 0) {
  return auth && AuthTokenCache_Store(auth->Mem, &auth->Cache, provider, token,
                                      accountId, expiresAtMs);
}

// Returned views are copied into the caller's arena: a refresh replaces the
// cached token and wipes the old one, which must not corrupt a request that is
// still holding it.
static bool ReturnAuth(Arena* arena, StrView token, StrView accountId,
                       ProviderAuth* out) {
  out->Token = StrView_Dup(arena, token);
  out->AccountId = accountId.Length ? StrView_Dup(arena, accountId) : StrView{};
  return true;
}

static bool UseChatGptCredential(AppAuth* aa, Arena* arena, Arena* scratch,
                                 StrView provider, const Credential* cred,
                                 ProviderAuth* out) {
  StrView accountId = cred->AccountId.Length
                          ? cred->AccountId
                          : OpenAICodexOAuth_AccountId(scratch, cred->AccessToken);
  if (!accountId.Length) return false;
  AppAuth_CacheToken(aa, provider, cred->AccessToken, accountId,
                     cred->ExpiresAtMs);
  return ReturnAuth(arena, cred->AccessToken, accountId, out);
}

// Covers the token request's own timeout, so a waiter outlasts a holder that
// is still refreshing rather than giving up while it works.
static const u64 CHATGPT_REFRESH_LOCK_WAIT_MS = 65 * 1000;

// Refresh tokens are single-use: two processes refreshing the same one would
// race, and the loser's refusal can sign the account out. A refresh therefore
// holds the store's lock and re-reads the store first, so a process that
// waited reuses whatever the holder wrote.
static bool RefreshChatGptCredential(AppAuth* aa, Arena* arena, Arena* scratch,
                                     StrView provider, const Credential* stale,
                                     volatile i32* abort, ProviderAuth* out) {
  StrBuf lockPath = StrBuf_Make(scratch, aa->Path.Length + 8);
  StrBuf_Append(&lockPath, aa->Path);
  StrBuf_AppendCStr(&lockPath, ".lock");
  ProcessFileLock lock = {};
  ProcessFileLockStatus status = ProcessFileLockStatus::Busy;
  u64 deadline = MonotonicMillis() + CHATGPT_REFRESH_LOCK_WAIT_MS;
  for (;;) {
    status = ProcessFileLock_TryAcquire(scratch, StrBuf_View(&lockPath), &lock);
    if (status != ProcessFileLockStatus::Busy ||
        (abort && Atomic_LoadI32(abort)) || MonotonicMillis() >= deadline)
      break;
    Sleep_Ms(50);
  }
  if (status != ProcessFileLockStatus::Acquired) {
    // Inside the refresh margin the old token still works for a while.
    return NowMillis() < stale->ExpiresAtMs &&
           UseChatGptCredential(aa, arena, scratch, provider, stale, out);
  }

  bool ok = false;
  Credential current = {};
  if (AuthStore_Read(scratch, aa->Path, provider, &current) &&
      current.Kind == CredentialKind::OAuth) {
    if (!Credential_NeedsRefresh(current.ExpiresAtMs, NowMillis())) {
      ok = UseChatGptCredential(aa, arena, scratch, provider, &current, out);
    } else {
      OpenAICodexTokens tokens =
          OpenAICodexOAuth_Refresh(scratch, current.RefreshToken, abort);
      if (tokens.Ok) {
        Credential next = {};
        next.Kind = CredentialKind::OAuth;
        next.AccessToken = tokens.Access;
        next.RefreshToken = tokens.Refresh;
        next.AccountId = tokens.AccountId;
        next.ExpiresAtMs = tokens.ExpiresAtMs;
        // A failed save still leaves this process a working token; the next
        // process will find the old refresh token refused and ask for /auth.
        AuthStore_Write(scratch, aa->Path, provider, &next);
        ok = UseChatGptCredential(aa, arena, scratch, provider, &next, out);
      } else if (NowMillis() < current.ExpiresAtMs) {
        ok = UseChatGptCredential(aa, arena, scratch, provider, &current, out);
      }
    }
  }
  ProcessFileLock_Release(&lock, StrBuf_View(&lockPath));
  return ok;
}

static bool ResolveChatGptAuth(AppAuth* aa, Arena* arena, StrView provider,
                               volatile i32* abort, ProviderAuth* out) {
  Arena scratch = ArenaCreate((usize)4 << 20);
  bool ok = false;
  Credential cred = {};
  if (AuthStore_Read(&scratch, aa->Path, provider, &cred) &&
      cred.Kind == CredentialKind::OAuth) {
    ok = Credential_NeedsRefresh(cred.ExpiresAtMs, NowMillis())
             ? RefreshChatGptCredential(aa, arena, &scratch, provider, &cred,
                                        abort, out)
             : UseChatGptCredential(aa, arena, &scratch, provider, &cred, out);
  }
  ArenaReleaseSecure(&scratch);
  return ok;
}

bool AppResolveAuth(void* user, Arena* arena, StrView provider, volatile i32* abort,
                           ProviderAuth* out) {
  if (out) *out = {};
  if (!out) return false;
  AppAuth* aa = (AppAuth*)user;
  if (abort && Atomic_LoadI32(abort)) return false;
  StrView cached = {};
  if (AuthTokenCache_Get(&aa->Cache, provider, &cached) &&
      (!aa->Cache.ExpiresAtMs ||
       !Credential_NeedsRefresh(aa->Cache.ExpiresAtMs, NowMillis())))
    return ReturnAuth(arena, cached, aa->Cache.AccountId, out);
  const ProviderSpec* spec = ProviderRegistry_Find(provider);
  if (spec && spec->SignIn == ProviderSignIn::ChatGpt)
    return ResolveChatGptAuth(aa, arena, provider, abort, out);
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
  if (spec->SignIn == ProviderSignIn::ChatGpt) {
    // An expired sign-in whose refresh failed lands here too, so this does not
    // claim nothing was ever configured.
    StrBuf_AppendCStr(&b, "No usable ");
    StrBuf_Append(&b, spec->Name);
    StrBuf_AppendCStr(&b, " sign-in is available for '");
    StrBuf_Append(&b, spec->Id);
    StrBuf_AppendCStr(&b, interactive
                              ? "'. Run `/auth` to sign in with your ChatGPT "
                                "account before sending a prompt."
                              : "'. Sign in with `/auth` in an interactive "
                                "session before sending a prompt.");
    return StrBuf_View(&b);
  }
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

  AppSettings* settings = &selected.Settings;
  AppSettingsLoadResult load = AppSettings_Load(arena, settingsPath, settings);
  if (load == AppSettingsLoadResult::Success) {
    const ProviderSpec* configured = ProviderRegistry_Find(settings->Provider);
    if (configured) {
      selected.Spec = configured;
      selected.ModelId = settings->Model;
    } else {
      settings->Profile = {};
      fprintf(stderr,
              "warning: settings %.*s names unknown provider '%.*s'; using built-in defaults "
              "provider '%.*s', model '%.*s'\n",
              (int)settingsPath.Length, settingsPath.Data, (int)settings->Provider.Length,
              settings->Provider.Data, (int)defaults->Id.Length, defaults->Id.Data,
              (int)defaults->DefaultModel.Length, defaults->DefaultModel.Data);
    }
  } else if (load == AppSettingsLoadResult::Malformed) {
    if (!settings->PermissionReviewerInvalid && !settings->PermissionReviewerThresholdInvalid)
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

// Authorization codes expire after about ten minutes, so waiting longer is
// pointless.
static const u32 OAUTH_CALLBACK_WAIT_MS = 10 * 60 * 1000;

// What a browser sign-in shows and expects. The caller owns the listener.
struct OAuthBrowserFlow {
  StrView ProviderName;
  StrView Url;
  StrView CallbackPath;
  StrView State;
  StrView BrowserHint;  // operation status while waiting
};

// Browser + loopback redirect. Unavailable means fall back to pasting a code.
static OAuthCodeResult AwaitOAuthCallback(InteractiveRuntime* rt, Arena* scratch,
                                          LoopbackServer* server,
                                          const OAuthBrowserFlow* flow,
                                          const AppCommandUx* ux) {
  if (!Browser_Open(flow->Url))
    return OAuthCodeResult{OAuthCodeStatus::Unavailable, StrView{}, StrView{}};

  StrBuf notice = StrBuf_Make(scratch, flow->Url.Length + 96);
  StrBuf_AppendCStr(&notice, "Opened your browser to sign in to ");
  StrBuf_Append(&notice, flow->ProviderName);
  StrBuf_AppendCStr(&notice, ". If it did not open, visit:\n");
  StrBuf_Append(&notice, flow->Url);
  EmitCommandNotice(ux, StrBuf_View(&notice));

  StrBuf waiting = StrBuf_Make(scratch, flow->ProviderName.Length + 24);
  StrBuf_AppendCStr(&waiting, "Waiting for ");
  StrBuf_Append(&waiting, flow->ProviderName);
  StrBuf_AppendCStr(&waiting, " sign-in");
  volatile i32* abort = RuntimeBeginOperation(rt, ux, StrBuf_View(&waiting));
  RuntimeSetOperationStatus(rt, ux, flow->BrowserHint);
  OAuthCodeResult result = {OAuthCodeStatus::Failed, StrView{}, StrView{}};
  u64 deadline = MonotonicMillis() + OAUTH_CALLBACK_WAIT_MS;
  for (;;) {
    // Rejected requests roll the arena back so stray connections cannot grow it.
    usize mark = scratch->Used;
    u64 now = MonotonicMillis();
    LoopbackRequest request;
    LoopbackWait wait = LoopbackServer_Accept(
        server, scratch, abort, now < deadline ? (u32)(deadline - now) : 0, &request);
    if (wait == LoopbackWait::Aborted) {
      result.Status = OAuthCodeStatus::Cancelled;
      break;
    }
    if (wait == LoopbackWait::TimedOut) {
      StrBuf message = StrBuf_Make(scratch, flow->ProviderName.Length + 40);
      StrBuf_AppendCStr(&message, "Timed out waiting for ");
      StrBuf_Append(&message, flow->ProviderName);
      StrBuf_AppendCStr(&message, " sign-in.");
      result.Message = StrBuf_View(&message);
      break;
    }
    if (wait != LoopbackWait::Request) {
      result.Message = SVL("The sign-in callback listener failed.");
      break;
    }

    OAuthCallback callbackResult = OAuthCallback_Parse(
        scratch, request.Target, flow->CallbackPath, flow->State);
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
      StrBuf message = StrBuf_Make(
          scratch, flow->ProviderName.Length + callbackResult.Error.Length + 48);
      StrBuf_Append(&message, flow->ProviderName);
      StrBuf_AppendCStr(&message, " sign-in was denied: ");
      StrBuf_Append(&message, callbackResult.Error);
      result.Message = StrBuf_View(&message);
      break;
    }
    StrBuf page = StrBuf_Make(scratch, flow->ProviderName.Length + 128);
    StrBuf_AppendCStr(&page,
                      "<!doctype html><meta charset=utf-8><title>Clanker</title>"
                      "<p>Clanker is signed in to ");
    StrBuf_Append(&page, flow->ProviderName);
    StrBuf_AppendCStr(&page, ". You can close this tab.</p>");
    LoopbackRequest_Respond(&request, 200, StrBuf_View(&page));
    result.Status = OAuthCodeStatus::Received;
    result.Code = callbackResult.Code;
    break;
  }
  bool cancelled = RuntimeEndOperation(rt);
  if (cancelled && result.Status != OAuthCodeStatus::Received)
    result.Status = OAuthCodeStatus::Cancelled;
  return result;
}

static OAuthCodeResult ReceiveOpenRouterCallback(InteractiveRuntime* rt,
                                                 Arena* scratch,
                                                 const OAuthPkce* pkce,
                                                 const AppCommandUx* ux) {
  LoopbackServer server;
  if (!LoopbackServer_Open(&server))
    return OAuthCodeResult{OAuthCodeStatus::Unavailable, StrView{}, StrView{}};

  StrBuf callback = StrBuf_Make(scratch, 48);
  StrBuf_AppendCStr(&callback, "http://localhost:");
  StrBuf_AppendU64(&callback, server.Port);
  StrBuf_AppendCStr(&callback, OPENROUTER_OAUTH_CALLBACK_PATH);
  OAuthBrowserFlow flow = {};
  flow.ProviderName = SVL("OpenRouter");
  flow.Url = OpenRouterOAuth_AuthorizeUrl(scratch, pkce, StrBuf_View(&callback));
  flow.CallbackPath = SVL(OPENROUTER_OAUTH_CALLBACK_PATH);
  flow.State = pkce->State;
  flow.BrowserHint = SVL("Approve Clanker in your browser.");
  OAuthCodeResult result = AwaitOAuthCallback(rt, scratch, &server, &flow, ux);
  LoopbackServer_Close(&server);
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

// ChatGPT's redirect URI is registered on a fixed localhost port. When the
// browser cannot reach Clanker there (a remote session, or another sign-in
// holding the port), its address bar still carries the code once it lands.
static OAuthCodeResult ReadPastedChatGptCode(InteractiveRuntime* rt, Arena* scratch,
                                             const OAuthPkce* pkce, StrView url,
                                             bool portBusy, const AppCommandUx* ux) {
  StrBuf notice = StrBuf_Make(scratch, url.Length + 384);
  if (portBusy) {
    StrBuf_AppendCStr(&notice, "Port ");
    StrBuf_AppendU64(&notice, OPENAI_CODEX_OAUTH_CALLBACK_PORT);
    StrBuf_AppendCStr(&notice,
                      " is in use, perhaps by another ChatGPT sign-in, so "
                      "Clanker cannot receive the browser redirect directly.\n");
  }
  StrBuf_AppendCStr(&notice,
                    "Open this URL in a browser and sign in with your ChatGPT "
                    "account. The browser then lands on a localhost page that "
                    "may fail to load; paste that page's full address from the "
                    "address bar:\n");
  StrBuf_Append(&notice, url);
  EmitCommandNotice(ux, StrBuf_View(&notice));

  for (;;) {
    StrView entered = {};
    RuntimeInputResult entry =
        RuntimeReadText(rt, scratch, SVL("Sign in to ChatGPT"),
                        SVL("Redirect URL or authorization code"), StrView{},
                        &entered);
    if (entry != RuntimeInputResult::Confirmed)
      return OAuthCodeResult{OAuthCodeStatus::Cancelled, StrView{}, StrView{}};
    StrView code = {};
    StrView error = {};
    if (OpenAICodexOAuth_ParsePastedInput(scratch, entered, pkce->State, &code,
                                          &error))
      return OAuthCodeResult{OAuthCodeStatus::Received, code, StrView{}};
    StrBuf message = StrBuf_Make(scratch, error.Length + 40);
    StrBuf_AppendCStr(&message, "That did not work: ");
    StrBuf_Append(&message, error);
    StrBuf_AppendChar(&message, '.');
    EmitCommandError(ux, StrBuf_View(&message));
  }
}

static AppCommandHandlerResult RunChatGptLogin(InteractiveRuntime* rt,
                                               const ProviderSpec* spec,
                                               const AppCommandUx* ux) {
  Arena scratch = ArenaCreate((usize)16 << 20);
  OAuthPkce pkce = OAuthPkce_Create(&scratch);
  StrView url = OpenAICodexOAuth_AuthorizeUrl(&scratch, &pkce);
  OAuthCodeResult code = {OAuthCodeStatus::Unavailable, StrView{}, StrView{}};
  bool portBusy = false;
  if (!Browser_IsRemoteSession()) {
    LoopbackServer server;
    if (LoopbackServer_OpenPort(&server, OPENAI_CODEX_OAUTH_CALLBACK_PORT)) {
      OAuthBrowserFlow flow = {};
      flow.ProviderName = spec->Name;
      flow.Url = url;
      flow.CallbackPath = SVL(OPENAI_CODEX_OAUTH_CALLBACK_PATH);
      flow.State = pkce.State;
      flow.BrowserHint = SVL("Sign in with your ChatGPT account in your browser.");
      code = AwaitOAuthCallback(rt, &scratch, &server, &flow, ux);
      LoopbackServer_Close(&server);
    } else {
      portBusy = true;
    }
  }
  if (code.Status == OAuthCodeStatus::Unavailable)
    code = ReadPastedChatGptCode(rt, &scratch, &pkce, url, portBusy, ux);

  AppCommandHandlerResult result = {AppCommandHandlerStatus::Success, StrView{}};
  if (code.Status == OAuthCodeStatus::Cancelled) {
    result = {AppCommandHandlerStatus::Cancelled, SVL("Authentication cancelled.")};
  } else if (code.Status != OAuthCodeStatus::Received) {
    result = RuntimeOwnedFailure(rt, code.Message);
  } else {
    volatile i32* abort =
        RuntimeBeginOperation(rt, ux, SVL("Completing ChatGPT sign-in"));
    OpenAICodexTokens tokens =
        OpenAICodexOAuth_Exchange(&scratch, code.Code, pkce.Verifier, abort);
    bool cancelled = RuntimeEndOperation(rt);

    Credential credential = {};
    credential.Kind = CredentialKind::OAuth;
    credential.AccessToken = tokens.Access;
    credential.RefreshToken = tokens.Refresh;
    credential.AccountId = tokens.AccountId;
    credential.ExpiresAtMs = tokens.ExpiresAtMs;
    if (cancelled) {
      result = {AppCommandHandlerStatus::Cancelled, SVL("Authentication cancelled.")};
    } else if (!tokens.Ok) {
      StrBuf message = StrBuf_Make(&scratch, tokens.Error.Length + 48);
      StrBuf_AppendCStr(&message, "ChatGPT sign-in failed: ");
      StrBuf_Append(&message, tokens.Error);
      result = RuntimeOwnedFailure(rt, StrBuf_View(&message));
    } else if (!AuthStore_Write(&scratch, rt->AuthPath, spec->Id, &credential)) {
      StrBuf message = StrBuf_Make(&scratch, rt->AuthPath.Length + 48);
      StrBuf_AppendCStr(&message, "Failed to save credentials to ");
      StrBuf_Append(&message, rt->AuthPath);
      StrBuf_AppendChar(&message, '.');
      result = RuntimeOwnedFailure(rt, StrBuf_View(&message));
    } else {
      AppAuth_CacheToken(rt->Auth, spec->Id, tokens.Access, tokens.AccountId,
                         tokens.ExpiresAtMs);
    }
  }
  ArenaReleaseSecure(&scratch);
  return result;
}

static AppCommandHandlerResult RunOAuthLogin(InteractiveRuntime* rt,
                                             const ProviderSpec* spec,
                                             const AppCommandUx* ux) {
  if (!spec || !spec->Endpoint)
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("No provider is selected.")};
  if (spec->SignIn == ProviderSignIn::ChatGpt) return RunChatGptLogin(rt, spec, ux);

  Arena scratch = ArenaCreate((usize)16 << 20);
  OAuthPkce pkce = OAuthPkce_Create(&scratch);
  OAuthCodeResult code = {OAuthCodeStatus::Unavailable, StrView{}, StrView{}};
  if (!Browser_IsRemoteSession())
    code = ReceiveOpenRouterCallback(rt, &scratch, &pkce, ux);
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
  ArenaReleaseSecure(&scratch);
  return result;
}

AppCommandHandlerResult RuntimeEnterApiKey(
    InteractiveRuntime* rt, StrView credentialId, StrView title,
    StrView category, const AppCommandUx* ux) {
  Arena scratch = ArenaCreate((usize)8 << 20);
  StrView entered = {};
  for (;;) {
    RuntimeInputResult entry =
        RuntimeReadText(rt, &scratch, title, SVL("API key"),
                        StrView{}, &entered, category, TextInputMode::Secret);
    if (entry != RuntimeInputResult::Confirmed) {
      ArenaReleaseSecure(&scratch);
      return entry == RuntimeInputResult::Failed
                 ? AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                     SVL("Secure API key input is unavailable; credentials were not changed.")}
                 : AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                     SVL("API key unchanged.")};
    }
    entered = StrView_Trim(entered);
    bool valid = entered.Length > 0 && entered.Length <= 8192;
    for (u32 i = 0; valid && i < entered.Length; i++)
      valid = (u8)entered.Data[i] > 0x20 && (u8)entered.Data[i] < 0x7f;
    if (valid) break;
    ArenaResetSecure(&scratch);
    entered = {};
    EmitCommandError(ux, SVL("Enter a non-empty API key of at most 8192 characters, without spaces or control characters."));
  }

  Credential credential = {};
  credential.ApiKey = entered;
  bool saved = AuthStore_Write(&scratch, rt->AuthPath, credentialId, &credential);
  ArenaReleaseSecure(&scratch);
  return saved
      ? AppCommandHandlerResult{AppCommandHandlerStatus::Success, SVL("API key saved.")}
      : AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
          SVL("The API key was not changed; the authentication store could not be saved. Check its format and file permissions.")};
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
    ArenaReleaseSecure(&scratch);
    return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{}, StrView{}};
  }

  ProviderModelList models = {};
  if (live.Status == ProviderModelListStatus::Success) {
    models = live;
    ModelCache_Save(rt->AgentDirectory, spec->Id, live.Models, live.Count);
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
      ArenaReleaseSecure(&scratch);
      return RuntimeModelChoice{RuntimeChoiceStatus::Back, Model{}, StrView{}};
    }
    if (selection != RuntimeInputResult::Confirmed) {
      ArenaReleaseSecure(&scratch);
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
          ArenaReleaseSecure(&scratch);
          return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{},
                                    StrView{}};
        }
        entered = StrView_Trim(entered);
        if (entered.Length) {
          chosen = ModelCache_CreateModel(rt->Mem, rt->AgentDirectory, spec,
                                          entered);
          break;
        }
        EmitCommandError(ux, SVL("A model ID is required."));
      }
      if (returnToModels) continue;
    }

    chosen.Provider = StrView_Dup(rt->Mem, spec->Id);
    if (!chosen.Name.Length) chosen.Name = chosen.Id;
    if (!Model_MetadataIsValid(&chosen)) {
      ArenaReleaseSecure(&scratch);
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
        ArenaReleaseSecure(&scratch);
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
        ArenaReleaseSecure(&scratch);
        return RuntimeModelChoice{RuntimeChoiceStatus::Cancelled, Model{},
                                  StrView{}};
      }
      preference.ContextWindow =
          chosen.ContextWindowOptions[contextSelected];
    }

    chosenProfile = preference;
    break;
  }

  ArenaReleaseSecure(&scratch);
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
      AppCommandHandlerResult login = RunOAuthLogin(rt, provider.Spec, ux);
      if (login.Status != AppCommandHandlerStatus::Success) return login;
      authenticated = provider.Spec;
    }

    RuntimeModelChoice model = ChooseModelForProvider(rt, provider.Spec, ux);
    if (model.Status == RuntimeChoiceStatus::Back) continue;
    if (model.Status != RuntimeChoiceStatus::Selected) {
      if (model.Status == RuntimeChoiceStatus::Failed && model.Message.Length)
        EmitCommandError(ux, model.Message);
      Model fallback = ModelCache_CreateModel(
          rt->Mem, rt->AgentDirectory, provider.Spec,
          provider.Spec->DefaultModel);
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
  Arena scratch = ArenaCreate((usize)8 << 20);
  bool removed = AuthStore_Delete(&scratch, authPath, SV(providerId));
  ArenaReleaseSecure(&scratch);
  if (!removed) {
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
