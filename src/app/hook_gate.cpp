#include "app/hook_gate.h"

#include "base/thread.h"
#include "json/json.h"

namespace {

// A blocked call's reason becomes model-visible text on every retry, so a
// chatty hook is capped rather than allowed to fill the context window.
constexpr u32 HOOK_GATE_REASON_MAX_BYTES = 4096;

// A verdict is a handful of fields; more than this is a hook logging to the
// wrong stream, and output that cannot be read blocks.
constexpr u32 HOOK_GATE_JSON_MAX_BYTES = 1u << 16;

// One byte of headroom, so output clipped at the cap stays distinguishable from
// output that merely fit it.
constexpr u32 HOOK_GATE_CAPTURE_MAX_BYTES = HOOK_GATE_JSON_MAX_BYTES + 1u;

StrView BoundedTrim(StrView value) {
  StrView trimmed = StrView_Trim(value);
  if (trimmed.Length > HOOK_GATE_REASON_MAX_BYTES)
    trimmed.Length = HOOK_GATE_REASON_MAX_BYTES;
  return trimmed;
}

StrView BoundedDup(Arena* arena, StrView value) {
  return StrView_Dup(arena, BoundedTrim(value));
}

void BlockWith(Arena* arena, AgentToolGateDecision* out, StrView summary, StrView detail) {
  StrView trimmed = BoundedTrim(detail);
  StrBuf reason = StrBuf_Make(arena, summary.Length + trimmed.Length + 8);
  StrBuf_Append(&reason, summary);
  if (trimmed.Length) {
    StrBuf_AppendCStr(&reason, ": ");
    StrBuf_Append(&reason, trimmed);
  }
  out->Block = true;
  out->Reason = StrBuf_View(&reason);
  out->Terminate = false;
}

}  // namespace

struct HookGate {
  const HookConfig* Config;
  volatile i32* Abort;
  Mutex Lock;
  HookSessionStorage Session;
};

HookGate* HookGate_Create(Arena* arena, const HookConfig* config,
                          const HookSessionInfo* session, volatile i32* abort) {
  if (!arena || !config || !config->CommandCount) return nullptr;
  // An empty subject cannot be narrowed by a matcher, so this asks only whether
  // any PreToolUse command exists at all. None means no policy, and no policy
  // must leave a tool call costing exactly what it did before.
  const HookCommand* probe = nullptr;
  if (!HookConfig_Select(config, HookEvent_Name(HookEvent::PreToolUse), StrView{}, &probe, 1))
    return nullptr;

  HookGate* gate = ArenaPushStruct(arena, HookGate);
  memset(gate, 0, sizeof(*gate));
  gate->Config = config;
  gate->Abort = abort;
  HookSession_Store(&gate->Session, session);
  Mutex_Init(&gate->Lock);
  return gate;
}

void HookGate_SetSession(HookGate* gate, const HookSessionInfo* session) {
  if (!gate) return;
  Mutex_Lock(&gate->Lock);
  HookSession_Store(&gate->Session, session);
  Mutex_Unlock(&gate->Lock);
}

void HookGate_Decide(Arena* arena, const ExecResult* result, AgentToolGateDecision* out) {
  *out = {};
  if (!result) {
    BlockWith(arena, out, SVL("PreToolUse hook produced no verdict"), StrView{});
    return;
  }
  if (!result->Spawned) {
    BlockWith(arena, out, SVL("PreToolUse hook failed to run"), result->Output);
    return;
  }
  if (result->TimedOut) {
    BlockWith(arena, out, SVL("PreToolUse hook timed out"), StrView{});
    return;
  }
  if (result->Output.Length > HOOK_GATE_JSON_MAX_BYTES) {
    BlockWith(arena, out, SVL("PreToolUse hook produced too much output to read a verdict from"),
              StrView{});
    return;
  }

  // Only an explicit boolean `block` is a verdict, and it outranks the exit
  // code in both directions. An object without one is another tool's permission
  // schema or a hook logging JSON; reading it as consent would let a denial
  // spelled any other way through.
  if (result->Output.Length) {
    JsonValue* root = Json_Parse(arena, result->Output);
    JsonValue* blocked = Json_IsObject(root) ? Json_Get(root, "block") : nullptr;
    if (blocked && blocked->Kind == JsonKind::Bool) {
      if (!blocked->Bool) return;
      out->Block = true;
      out->Reason = BoundedDup(arena, Json_GetStr(root, "reason"));
      // pi reads terminate only alongside a block.
      out->Terminate = Json_GetBool(root, "terminate", false);
      return;
    }
  }

  if (result->ExitCode != 0) {
    out->Block = true;
    out->Reason = BoundedDup(arena, result->Output);
  }
}

void HookGate_BeforeTool(void* user, Arena* arena, StrView toolName, StrView toolCallId,
                         const ToolArgs* args, AgentToolGateDecision* out) {
  *out = {};
  HookGate* gate = (HookGate*)user;
  if (!gate) return;

  // Default reservation rather than a small one: the capture is capped, but
  // StrBuf grows by doubling into fresh blocks, so the ceiling is loose. The
  // reserve is virtual; an ordinary verdict commits a single page.
  Arena scratch = ArenaCreate();
  u32 capacity = gate->Config->CommandCount;
  const HookCommand** commands = ArenaPushArray(&scratch, const HookCommand*, capacity);
  u32 count = HookConfig_Select(gate->Config, HookEvent_Name(HookEvent::PreToolUse), toolName,
                                commands, capacity);
  if (!count) {
    ArenaRelease(&scratch);
    return;
  }

  HookSessionStorage snapshot;
  Mutex_Lock(&gate->Lock);
  snapshot = gate->Session;
  Mutex_Unlock(&gate->Lock);
  HookSessionInfo session = HookSession_View(&snapshot);

  HookToolInfo tool = {};
  tool.Name = toolName;
  tool.CallId = toolCallId;
  tool.InputJson = args ? args->Json : StrView{};

  StrBuf payload = StrBuf_Make(&scratch, 1024);
  HookRunner_BuildPayload(&payload, HookEvent::PreToolUse, &session, &tool);
  if (payload.Length >= EXEC_INPUT_PIPE_BYTES) {
    // Dropped whole rather than truncated: half a JSON object is not a JSON
    // object.
    tool.InputJson = StrView{};
    tool.InputOmitted = true;
    StrBuf_Clear(&payload);
    HookRunner_BuildPayload(&payload, HookEvent::PreToolUse, &session, &tool);
  }
  StrView payloadView = StrBuf_View(&payload);

  StrBuf captured = StrBuf_Make(&scratch, 1024);
  for (u32 i = 0; i < count; i++) {
    if (gate->Abort && Atomic_LoadI32(gate->Abort)) {
      BlockWith(arena, out, SVL("PreToolUse hook was cancelled"), StrView{});
      break;
    }
    StrBuf_Clear(&captured);
    ExecResult result = Exec_RunShellInput(&scratch, commands[i]->Command, payloadView,
                                           commands[i]->TimeoutMs, gate->Abort, &captured,
                                           HOOK_GATE_CAPTURE_MAX_BYTES);
    HookGate_Decide(arena, &result, out);
    // First block wins, matching pi's extension runner.
    if (out->Block) break;
  }
  ArenaRelease(&scratch);
}
