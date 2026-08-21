#include "agent/permissions.h"

#include "base/thread.h"
#include "json/json.h"

namespace {

constexpr u32 AGENT_PERMISSION_MAX_GRANTS = 64;
constexpr u32 AGENT_PERMISSION_MAX_CACHE = 128;
constexpr u32 AGENT_PERMISSION_MAX_REQUESTS = 64;
constexpr u32 AGENT_PERMISSION_MAX_KEY_BYTES = 8192;
constexpr u32 AGENT_PERMISSION_MAX_ACTION_BYTES = 2048;
constexpr u32 AGENT_PERMISSION_MAX_RATIONALE_BYTES = 512;
constexpr u32 AGENT_PERMISSION_MAX_TRUSTED_TEXT_BYTES = 8192;

StrView Bounded(StrView value, u32 limit) {
  if (value.Length > limit) value.Length = limit;
  return value;
}

StrView BuildExactKey(Arena* arena, StrView toolName, StrView argumentsJson) {
  if ((u64)toolName.Length + argumentsJson.Length + 1 >
      AGENT_PERMISSION_MAX_KEY_BYTES)
    return {};
  StrBuf key =
      StrBuf_Make(arena, toolName.Length + argumentsJson.Length + 2);
  StrBuf_Append(&key, toolName);
  StrBuf_AppendChar(&key, '\n');
  StrBuf_Append(&key, argumentsJson);
  return StrBuf_View(&key);
}

StrView BuildFallbackAction(Arena* arena, StrView toolName) {
  StrBuf action =
      StrBuf_Make(arena, toolName.Length + 64);
  StrBuf_AppendCStr(&action, "Run tool ");
  StrBuf_Append(&action, toolName);
  StrBuf_AppendCStr(
      &action, " (arguments omitted because the tool supplied no safe action projection)");
  return StrBuf_View(&action);
}

StrView SanitizeAction(Arena* arena, StrView action) {
  action = Bounded(action, AGENT_PERMISSION_MAX_ACTION_BYTES);
  StrBuf safe = StrBuf_Make(arena, action.Length + 1);
  for (u32 i = 0; i < action.Length; i++) {
    u8 c = (u8)action.Data[i];
    StrBuf_AppendChar(&safe, c < 0x20 || c == 0x7f ? ' ' : (char)c);
  }
  return StrBuf_View(&safe);
}

StrView TrustedUserText(Arena* arena, const AgentPermissionGate* gate) {
  StrBuf text = StrBuf_Make(arena, 512);
  if (!gate) return StrBuf_View(&text);
  for (u32 i = 0; i < gate->NewMessageCount; i++) {
    const Message* message = &gate->NewMessages[i];
    if (message->Kind != Role::User) continue;
    for (u32 j = 0; j < message->Content.Count; j++) {
      const Content* content = &message->Content.Data[j];
      if (content->Kind != ContentKind::Text || !content->Text.Length) continue;
      if (text.Length) StrBuf_AppendChar(&text, '\n');
      u32 room = AGENT_PERMISSION_MAX_TRUSTED_TEXT_BYTES - text.Length;
      StrBuf_Append(&text, Bounded(content->Text, room));
      if (text.Length >= AGENT_PERMISSION_MAX_TRUSTED_TEXT_BYTES)
        return StrBuf_View(&text);
    }
  }
  return StrBuf_View(&text);
}

bool HasGrantLocked(const AgentPermissionEngine* engine, StrView key) {
  if (!key.Length) return false;
  for (u32 i = 0; i < engine->Grants.Count; i++)
    if (StrView_Eq(engine->Grants.Data[i].Key, key)) return true;
  return false;
}

bool AddGrantLocked(AgentPermissionEngine* engine, StrView key) {
  if (!key.Length || key.Length > AGENT_PERMISSION_MAX_KEY_BYTES) return false;
  if (HasGrantLocked(engine, key)) return true;
  if (engine->Grants.Count >= AGENT_PERMISSION_MAX_GRANTS) return false;
  AgentPermissionGrant grant = {};
  grant.Key = StrView_Dup(&engine->GrantMem,
                          Bounded(key, AGENT_PERMISSION_MAX_KEY_BYTES));
  Array_Push(&engine->Grants, grant);
  return true;
}

i32 FindCacheLocked(const AgentPermissionEngine* engine, StrView key) {
  if (!key.Length) return -1;
  for (u32 i = 0; i < engine->Cache.Count; i++)
    if (StrView_Eq(engine->Cache.Data[i].Key, key)) return (i32)i;
  return -1;
}

i32 FindRequestLocked(const AgentPermissionEngine* engine, StrView id) {
  DurableId parsed = {};
  if (!DurableId_Parse(id, &parsed)) return -1;
  for (u32 i = 0; i < engine->Requests.Count; i++)
    if (DurableId_Eq(engine->Requests.Data[i].Id, parsed)) return (i32)i;
  return -1;
}

i32 FindApprovedRequestLocked(const AgentPermissionEngine* engine,
                              StrView toolName, StrView argumentsJson) {
  for (u32 i = 0; i < engine->Requests.Count; i++) {
    const AgentPermissionRequest* request = &engine->Requests.Data[i];
    if (request->Approval != AgentPermissionApproval::Once) continue;
    if (StrView_Eq(request->ToolName, toolName) &&
        StrView_Eq(request->ArgumentsJson, argumentsJson))
      return (i32)i;
  }
  return -1;
}

StrView DenialJson(Arena* arena, const AgentPermissionRequest* request) {
  StrBuf json = StrBuf_Make(arena, 512);
  StrBuf_AppendCStr(&json, "{\"permission_denied\":true,"
                           "\"permission_request_id\":");
  Json_WriteString(&json, DurableId_View(&request->Id));
  StrBuf_AppendCStr(&json, ",\"reason\":");
  Json_WriteString(&json, request->Rationale.Length
                              ? request->Rationale
                              : SVL("This action requires approval."));
  StrBuf_AppendCStr(
      &json,
      ",\"instructions\":\"Call ask_user with permission_request_id set to "
      "this exact ID to open the real permission prompt. Do not retry the "
      "unchanged action until it is approved.\"}");
  return StrBuf_View(&json);
}

StrView PlainDenial(Arena* arena, StrView reason) {
  StrBuf json = StrBuf_Make(arena, reason.Length + 64);
  StrBuf_AppendCStr(&json, "{\"permission_denied\":true,\"reason\":");
  Json_WriteString(&json, reason);
  StrBuf_AppendChar(&json, '}');
  return StrBuf_View(&json);
}

AgentAskResponse Prompt(AgentPermissionEngine* engine, Arena* arena,
                        StrView action) {
  AgentAskRequest request = {};
  request.Question = action.Length ? action : SVL("Allow this action?");
  request.Category = SVL("Permission");
  StrView choices[] = {SVL("Allow once"),
                       SVL("Always for this session"), SVL("Deny")};
  request.Choices = choices;
  request.ChoiceCount = 3;
  return AgentAskUser_Invoke(engine->AskUser, arena, &request);
}

void HandleAsk(AgentPermissionEngine* engine, Arena* arena, StrView action,
               StrView grantKey, AgentPermissionDecision* out) {
  AgentAskResponse response = Prompt(engine, arena, action);
  if (response.Status != AgentAskStatus::Answered) {
    out->Block = true;
    out->Reason =
        PlainDenial(arena, response.Status == AgentAskStatus::Declined
                               ? SVL("The user denied or dismissed this action.")
                               : SVL("No user is available to approve this action."));
    return;
  }
  if (response.ChoiceIndex == 0) return;
  if (response.ChoiceIndex == 1) {
    ScopedLock lock(&engine->Lock);
    if (AddGrantLocked(engine, grantKey)) return;
    out->Block = true;
    out->Reason = PlainDenial(
        arena, grantKey.Length
                   ? SVL("The session permission grant limit was reached.")
                   : SVL("This action is too large to bind safely to a session grant."));
    return;
  }
  out->Block = true;
  out->Reason = PlainDenial(arena, SVL("The user denied this action."));
}

AgentPermissionRequest* AddRequestLocked(
    AgentPermissionEngine* engine, StrView toolName, StrView argumentsJson,
    StrView grantKey, StrView action, StrView rationale) {
  if (engine->Requests.Count >= AGENT_PERMISSION_MAX_REQUESTS) return nullptr;
  AgentPermissionRequest request = {};
  request.Id = DurableId_Generate();
  request.ToolName = StrView_Dup(&engine->Run, toolName);
  request.ArgumentsJson =
      StrView_Dup(&engine->Run,
                  Bounded(argumentsJson, AGENT_PERMISSION_MAX_KEY_BYTES));
  request.GrantKey =
      StrView_Dup(&engine->Run,
                  Bounded(grantKey, AGENT_PERMISSION_MAX_KEY_BYTES));
  request.Action =
      StrView_Dup(&engine->Run,
                  Bounded(action, AGENT_PERMISSION_MAX_ACTION_BYTES));
  request.Rationale =
      StrView_Dup(&engine->Run,
                  Bounded(rationale, AGENT_PERMISSION_MAX_RATIONALE_BYTES));
  request.Approval = AgentPermissionApproval::Pending;
  return Array_Push(&engine->Requests, request);
}

void CacheLocked(AgentPermissionEngine* engine, StrView key, bool allow,
                 u32 requestIndex) {
  if (!key.Length || key.Length > AGENT_PERMISSION_MAX_KEY_BYTES ||
      engine->Cache.Count >= AGENT_PERMISSION_MAX_CACHE)
    return;
  AgentPermissionCacheEntry cached = {};
  cached.Key =
      StrView_Dup(&engine->Run, Bounded(key, AGENT_PERMISSION_MAX_KEY_BYTES));
  cached.Allow = allow;
  cached.RequestIndex = requestIndex;
  Array_Push(&engine->Cache, cached);
}

}  // namespace

StrView AgentPermissionMode_Name(AgentPermissionMode mode) {
  switch (mode) {
    case AgentPermissionMode::Ask: return SVL("ask");
    case AgentPermissionMode::Auto: return SVL("auto");
    case AgentPermissionMode::Yolo: return SVL("yolo");
  }
  return {};
}

bool AgentPermissionMode_Parse(StrView value, AgentPermissionMode* out) {
  if (!out) return false;
  for (u8 i = (u8)AgentPermissionMode::Ask;
       i <= (u8)AgentPermissionMode::Yolo; i++) {
    AgentPermissionMode mode = (AgentPermissionMode)i;
    if (!StrView_Eq(value, AgentPermissionMode_Name(mode))) continue;
    *out = mode;
    return true;
  }
  return false;
}

AgentPermissionMode AgentPermissionMode_Next(AgentPermissionMode mode) {
  switch (mode) {
    case AgentPermissionMode::Ask: return AgentPermissionMode::Auto;
    case AgentPermissionMode::Auto: return AgentPermissionMode::Yolo;
    case AgentPermissionMode::Yolo: return AgentPermissionMode::Ask;
  }
  return AgentPermissionMode::Ask;
}

void AgentPermissionEngine_Init(AgentPermissionEngine* engine,
                                AgentPermissionMode mode,
                                StrView workingDirectory,
                                const AgentAskUser* askUser,
                                AgentPermissionReviewer reviewer) {
  *engine = AgentPermissionEngine{};
  engine->Session = ArenaCreate((usize)1 << 20);
  engine->GrantMem = ArenaCreate((usize)1 << 20);
  engine->Run = ArenaCreate((usize)4 << 20);
  Mutex_Init(&engine->Lock);
  engine->AskUser = askUser;
  engine->Reviewer = reviewer;
  engine->WorkingDirectory =
      StrView_Dup(&engine->Session, workingDirectory);
  engine->Grants =
      Array_Make<AgentPermissionGrant>(&engine->GrantMem,
                                       AGENT_PERMISSION_MAX_GRANTS);
  Atomic_StoreI32(&engine->Mode, (i32)mode);
  Atomic_StoreI32(&engine->Revision, 1);
  engine->Initialized = true;
}

void AgentPermissionEngine_Close(AgentPermissionEngine* engine) {
  if (!engine || !engine->Initialized) return;
  Mutex_Destroy(&engine->Lock);
  ArenaRelease(&engine->Run);
  ArenaRelease(&engine->GrantMem);
  ArenaRelease(&engine->Session);
  *engine = AgentPermissionEngine{};
}

void AgentPermissionEngine_BeginRun(AgentPermissionEngine* engine) {
  if (!engine || !engine->Initialized) return;
  ScopedLock lock(&engine->Lock);
  ArenaReset(&engine->Run);
  engine->Requests =
      Array_Make<AgentPermissionRequest>(&engine->Run,
                                         AGENT_PERMISSION_MAX_REQUESTS);
  engine->Cache =
      Array_Make<AgentPermissionCacheEntry>(&engine->Run,
                                            AGENT_PERMISSION_MAX_CACHE);
  engine->RunActive = true;
}

void AgentPermissionEngine_EndRun(AgentPermissionEngine* engine) {
  if (!engine || !engine->Initialized) return;
  ScopedLock lock(&engine->Lock);
  engine->Requests = {};
  engine->Cache = {};
  engine->RunActive = false;
  ArenaReset(&engine->Run);
}

AgentPermissionMode AgentPermissionEngine_GetMode(
    const AgentPermissionEngine* engine) {
  if (!engine || !engine->Initialized) return AgentPermissionMode::Auto;
  i32 value = Atomic_LoadI32(&engine->Mode);
  if (value < (i32)AgentPermissionMode::Ask ||
      value > (i32)AgentPermissionMode::Yolo)
    return AgentPermissionMode::Auto;
  return (AgentPermissionMode)value;
}

AgentPermissionMode AgentPermissionEngine_SetMode(AgentPermissionEngine* engine,
                                                  AgentPermissionMode mode) {
  if (!engine || !engine->Initialized) return mode;
  ScopedLock lock(&engine->Lock);
  if (Atomic_LoadI32(&engine->Mode) != (i32)mode) {
    Atomic_StoreI32(&engine->Mode, (i32)mode);
    Atomic_StoreI32(&engine->Revision,
                    Atomic_LoadI32(&engine->Revision) + 1);
  }
  return mode;
}

AgentPermissionMode AgentPermissionEngine_CycleMode(
    AgentPermissionEngine* engine) {
  if (!engine || !engine->Initialized) return AgentPermissionMode::Auto;
  ScopedLock lock(&engine->Lock);
  AgentPermissionMode next =
      AgentPermissionMode_Next(AgentPermissionEngine_GetMode(engine));
  Atomic_StoreI32(&engine->Mode, (i32)next);
  Atomic_StoreI32(&engine->Revision,
                  Atomic_LoadI32(&engine->Revision) + 1);
  return next;
}

AgentPermissionMode AgentPermissionEngine_Reset(AgentPermissionEngine* engine) {
  if (!engine || !engine->Initialized) return AgentPermissionMode::Ask;
  ScopedLock lock(&engine->Lock);
  ArenaReset(&engine->GrantMem);
  engine->Grants =
      Array_Make<AgentPermissionGrant>(&engine->GrantMem,
                                       AGENT_PERMISSION_MAX_GRANTS);
  Atomic_StoreI32(&engine->Mode, (i32)AgentPermissionMode::Ask);
  Atomic_StoreI32(&engine->Revision,
                  Atomic_LoadI32(&engine->Revision) + 1);
  return AgentPermissionMode::Ask;
}

u32 AgentPermissionEngine_GrantCount(AgentPermissionEngine* engine) {
  if (!engine || !engine->Initialized) return 0;
  ScopedLock lock(&engine->Lock);
  return engine->Grants.Count;
}

void AgentPermission_Decide(Arena* arena, const AgentPermissionGate* gate,
                            const Tool* tool, const ToolArgs* args,
                            AgentPermissionDecision* out) {
  *out = {};
  if (!arena || !gate || !gate->Engine || !tool || !args) return;
  AgentPermissionEngine* engine = gate->Engine;
  if (!engine->Initialized) {
    out->Block = true;
    out->Reason =
        PlainDenial(arena, SVL("The permission engine is unavailable."));
    return;
  }

  if (tool->PermissionEffect == ToolPermissionEffect::Inspection ||
      tool->PermissionEffect == ToolPermissionEffect::UserInteraction)
    return;

  // Yolo must bypass classification failures as well as classified actions.
  if (AgentPermissionEngine_GetMode(engine) == AgentPermissionMode::Yolo) return;

  ToolPermissionInfo info = {};
  if (tool->DescribePermission) {
    StrBuf error = StrBuf_Make(arena, 128);
    if (!tool->DescribePermission(tool->Ctx, arena, args,
                                  engine->WorkingDirectory, &info, &error)) {
      out->Block = true;
      out->Reason = PlainDenial(
          arena, error.Length ? StrBuf_View(&error)
                              : SVL("The action could not be classified safely."));
      return;
    }
  }
  if (!info.Action.Length)
    info.Action = BuildFallbackAction(arena, tool->Name);
  info.Action = SanitizeAction(arena, info.Action);
  if (!info.GrantKey.Length)
    info.GrantKey = BuildExactKey(arena, tool->Name, args->Json);
  if (info.GrantKey.Length > AGENT_PERMISSION_MAX_KEY_BYTES)
    info.GrantKey = {};
  StrView exactKey = BuildExactKey(arena, tool->Name, args->Json);

  for (u32 restart = 0; restart < 8; restart++) {
    i32 revision = 0;
    AgentPermissionMode mode = AgentPermissionMode::Auto;
    {
      ScopedLock lock(&engine->Lock);
      revision = Atomic_LoadI32(&engine->Revision);
      mode = AgentPermissionEngine_GetMode(engine);
      if (mode == AgentPermissionMode::Yolo) return;
      if (HasGrantLocked(engine, info.GrantKey)) return;
      i32 approved =
          FindApprovedRequestLocked(engine, tool->Name, args->Json);
      if (approved >= 0) {
        AgentPermissionRequest* request = &engine->Requests.Data[approved];
        if (request->Approval == AgentPermissionApproval::Once)
          request->Approval = AgentPermissionApproval::Consumed;
        return;
      }
    }

    if (mode == AgentPermissionMode::Ask) {
      HandleAsk(engine, arena, info.Action, info.GrantKey, out);
      return;
    }
    if (info.AutoAllow) {
      ScopedLock lock(&engine->Lock);
      if (Atomic_LoadI32(&engine->Revision) != revision) continue;
      return;
    }

    {
      ScopedLock lock(&engine->Lock);
      if (Atomic_LoadI32(&engine->Revision) != revision) continue;
      i32 cached = FindCacheLocked(engine, exactKey);
      if (cached >= 0) {
        AgentPermissionCacheEntry* entry =
            &engine->Cache.Data[cached];
        if (entry->Allow) return;
        if (entry->RequestIndex < engine->Requests.Count) {
          AgentPermissionRequest* pending =
              &engine->Requests.Data[entry->RequestIndex];
          if (pending->Approval == AgentPermissionApproval::Pending) {
            out->Block = true;
            out->Reason = DenialJson(arena, pending);
            return;
          }
          if (pending->Approval == AgentPermissionApproval::Denied) {
            out->Block = true;
            out->Reason =
                PlainDenial(arena, SVL("The user denied this action."));
            return;
          }
          if (pending->Approval == AgentPermissionApproval::Consumed)
            entry->Key = {};
        }
      }
    }

    AgentPermissionReviewResult reviewed = {};
    AgentPermissionReviewRequest request = {};
    request.ToolName = tool->Name;
    request.Action = info.Action;
    // Only the bounded action projection reaches the reviewer; exact arguments
    // remain in the run-scoped revalidation record.
    request.TrustedUserText = TrustedUserText(arena, gate);
    bool reviewOk =
        engine->Reviewer.Review &&
        engine->Reviewer.Review(engine->Reviewer.User, arena, &request,
                                gate->Abort, &reviewed);
    if (!reviewOk) {
      reviewed.Rationale = {};
    }

    ScopedLock lock(&engine->Lock);
    if (Atomic_LoadI32(&engine->Revision) != revision) continue;
    if (!engine->RunActive) {
      out->Block = true;
      out->Reason =
          PlainDenial(arena, SVL("Permission review state is unavailable."));
      return;
    }
    if (reviewOk &&
        reviewed.Decision == AgentPermissionReviewDecision::Allow) {
      CacheLocked(engine, exactKey, true, 0);
      return;
    }

    StrView rationale =
        reviewed.Rationale.Length
            ? reviewed.Rationale
            : !reviewOk
                  ? SVL("Automatic permission review was unavailable or invalid.")
                  : SVL("Automatic review requires human approval.");
    if (!exactKey.Length) {
      out->Block = true;
      out->Reason = PlainDenial(
          arena,
          SVL("This tool call is too large to bind safely to an approval request."));
      return;
    }
    AgentPermissionRequest* pending =
        AddRequestLocked(engine, tool->Name, args->Json, info.GrantKey,
                         info.Action, rationale);
    if (!pending) {
      out->Block = true;
      out->Reason =
          PlainDenial(arena, SVL("The permission request limit was reached."));
      return;
    }
    u32 pendingIndex = (u32)(pending - engine->Requests.Data);
    CacheLocked(engine, exactKey, false, pendingIndex);
    out->Block = true;
    out->Reason = DenialJson(arena, pending);
    return;
  }

  out->Block = true;
  out->Reason = PlainDenial(
      arena, SVL("Permission mode changed too often to decide this action."));
}

bool AgentPermission_ResolveRequest(AgentPermissionEngine* engine, Arena* arena,
                                    StrView requestId, volatile i32* abort,
                                    StrView* outMessage, StrBuf* error) {
  if (!engine || !engine->Initialized || !arena || !outMessage || !error)
    return false;
  StrView action = {};
  {
    ScopedLock lock(&engine->Lock);
    i32 index = FindRequestLocked(engine, requestId);
    if (index < 0 || !engine->RunActive) {
      StrBuf_AppendCStr(error,
                        "permission_request_id is invalid or no longer active");
      return false;
    }
    AgentPermissionRequest* request = &engine->Requests.Data[index];
    if (request->Approval != AgentPermissionApproval::Pending) {
      StrBuf_AppendCStr(error,
                        "permission_request_id was already resolved or consumed");
      return false;
    }
    action = StrView_Dup(arena, request->Action);
  }

  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(error, "cancelled before permission could be requested");
    return false;
  }
  AgentAskResponse response = Prompt(engine, arena, action);
  if (response.Status != AgentAskStatus::Answered) {
    StrBuf_AppendCStr(
        error, response.Status == AgentAskStatus::Declined
                   ? "the user dismissed the permission request"
                   : "no user is available to approve this permission request");
    return false;
  }

  ScopedLock lock(&engine->Lock);
  i32 index = FindRequestLocked(engine, requestId);
  if (index < 0 || !engine->RunActive) {
    StrBuf_AppendCStr(error, "permission request expired while awaiting input");
    return false;
  }
  AgentPermissionRequest* request = &engine->Requests.Data[index];
  if (request->Approval != AgentPermissionApproval::Pending) {
    StrBuf_AppendCStr(error, "permission request changed while awaiting input");
    return false;
  }
  if (response.ChoiceIndex == 0) {
    request->Approval = AgentPermissionApproval::Once;
    *outMessage =
        SVL("Permission granted once. Retry the exact original tool call.");
    return true;
  }
  if (response.ChoiceIndex == 1) {
    if (!AddGrantLocked(engine, request->GrantKey)) {
      StrBuf_AppendCStr(
          error, request->GrantKey.Length
                     ? "the session permission grant limit was reached"
                     : "this action is too large to bind safely to a session grant");
      return false;
    }
    request->Approval = AgentPermissionApproval::Consumed;
    *outMessage = SVL(
        "Permission granted for this session. Retry the exact original tool call.");
    return true;
  }
  request->Approval = AgentPermissionApproval::Denied;
  *outMessage = SVL("Permission denied. Do not retry the unchanged tool call.");
  return true;
}

StrView AgentPermission_AppendSystemPrompt(Arena* arena, StrView base,
                                           AgentPermissionMode mode) {
  StrView suffix = {};
  switch (mode) {
    case AgentPermissionMode::Ask:
      suffix = SVL(
          "Permission mode: ask. Inspection tools run directly; unresolved "
          "mutating actions require user approval. External PreToolUse policy "
          "may still block any tool.");
      break;
    case AgentPermissionMode::Auto:
      suffix = SVL(
          "Permission mode: auto. Clanker reviews unresolved sensitive tool "
          "calls once. A non-allow is a failed tool result for replanning. If "
          "it contains permission_request_id, call ask_user with that exact "
          "ID to open the real permission prompt; do not invent an ID or retry "
          "unchanged before approval. External PreToolUse policy may still "
          "block any tool.");
      break;
    case AgentPermissionMode::Yolo:
      suffix = SVL(
          "Permission mode: yolo. Clanker's built-in permission checks are "
          "disabled, but external PreToolUse policy may still block tools.");
      break;
  }
  StrBuf prompt = StrBuf_Make(arena, base.Length + suffix.Length + 48);
  StrBuf_Append(&prompt, base);
  if (base.Length) StrBuf_AppendCStr(&prompt, "\n\n");
  StrBuf_AppendCStr(&prompt, "<permission_mode>\n");
  StrBuf_Append(&prompt, suffix);
  StrBuf_AppendCStr(&prompt, "\n</permission_mode>");
  return StrBuf_View(&prompt);
}
