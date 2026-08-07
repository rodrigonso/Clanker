#include "agent/event_json.h"

#include "json/json.h"

namespace {

StrView TurnKindName(AgentTurnKind kind) {
  return kind == AgentTurnKind::Compaction ? SVL("compaction") : SVL("run");
}

StrView OutcomeName(AgentTurnOutcome outcome) {
  switch (outcome) {
    case AgentTurnOutcome::Succeeded: return SVL("succeeded");
    case AgentTurnOutcome::Failed: return SVL("failed");
    case AgentTurnOutcome::Aborted: return SVL("aborted");
  }
  return SVL("failed");
}

StrView RetryLayerName(RetryLayer layer) {
  return layer == RetryLayer::AssistantTurn ? SVL("assistant_turn")
                                            : SVL("provider_request");
}

// Tool results are content arrays; a JSON consumer wants the text. Image blocks
// are skipped rather than base64-inlined into an event stream.
void WriteToolContent(JsonWriter* w, const ToolResult* result) {
  StrBuf text = StrBuf_Make(w->Out->Backing, 128);
  if (result) {
    for (u32 i = 0; i < result->Content.Count; i++) {
      const Content* content = &result->Content.Data[i];
      if (content->Kind == ContentKind::Text) StrBuf_Append(&text, content->Text);
    }
  }
  JsonWrite_String(w, "content", StrBuf_View(&text));
}

void WriteToolIdentity(JsonWriter* w, const AgentHarnessEvent* event) {
  JsonWrite_String(w, "id", event->Tool.CallId);
  JsonWrite_String(w, "name", event->Tool.Name);
}

void WriteOutcome(JsonWriter* w, const AgentHarnessEvent* event) {
  JsonWrite_String(w, "turn", TurnKindName(event->Turn.Kind));
  JsonWrite_String(w, "outcome", OutcomeName(event->Turn.Outcome));
  if (event->Turn.Error.Length) JsonWrite_String(w, "error", event->Turn.Error);
}

void WriteRetry(JsonWriter* w, const AgentHarnessEvent* event) {
  JsonWrite_String(w, "layer", RetryLayerName(event->Retry.Layer));
  JsonWrite_U64(w, "attempt", event->Retry.Attempt);
  JsonWrite_U64(w, "maxAttempts", event->Retry.MaxAttempts);
  JsonWrite_U64(w, "delayMs", event->Retry.DelayMs);
  if (event->Retry.Error.Length) JsonWrite_String(w, "error", event->Retry.Error);
}

// Only the deltas carry text a consumer can render; the structural start/end
// markers are better recovered from the durable record.
const char* StreamDeltaType(StreamEventKind kind) {
  switch (kind) {
    case StreamEventKind::TextDelta: return "text_delta";
    case StreamEventKind::ThinkingDelta: return "thinking_delta";
    case StreamEventKind::ToolCallDelta: return "tool_call_delta";
    default: return nullptr;
  }
}

}  // namespace

bool AgentEventJson_Write(StrBuf* out, const AgentHarnessEvent* event) {
  if (!out || !event) return false;

  // A rejected event must leave `out` byte-for-byte as it was found, so callers
  // can append unconditionally and flush only on success.
  u32 mark = out->Length;
  auto reject = [out, mark]() {
    out->Length = mark;
    out->Data[mark] = '\0';
    return false;
  };

  JsonWriter w = {};
  JsonWrite_Begin(&w, out);

  switch (event->Kind) {
    case AgentHarnessEventKind::DurableRecord:
      JsonWrite_String(&w, "type", SVL("record"));
      JsonWrite_String(&w, "kind", SessionRecordKind_Name(event->Record->Kind));
      JsonWrite_Key(&w, "record");
      if (!SessionResult_IsOk(SessionRecord_Encode(out, event->Record)))
        return reject();
      break;
    case AgentHarnessEventKind::TransientAgentStart:
      JsonWrite_String(&w, "type", SVL("agent_start"));
      JsonWrite_String(&w, "turn", TurnKindName(event->Turn.Kind));
      break;
    case AgentHarnessEventKind::TransientAgentEnd:
      JsonWrite_String(&w, "type", SVL("agent_end"));
      WriteOutcome(&w, event);
      break;
    case AgentHarnessEventKind::TransientTurnStart:
      JsonWrite_String(&w, "type", SVL("turn_start"));
      JsonWrite_String(&w, "turn", TurnKindName(event->Turn.Kind));
      break;
    case AgentHarnessEventKind::TransientTurnEnd:
      JsonWrite_String(&w, "type", SVL("turn_end"));
      WriteOutcome(&w, event);
      break;
    case AgentHarnessEventKind::TransientStream: {
      const char* type = StreamDeltaType(event->Stream.Kind);
      if (!type) return reject();
      JsonWrite_String(&w, "type", SV(type));
      JsonWrite_U64(&w, "index", event->Stream.ContentIndex);
      JsonWrite_String(&w, "text", event->Stream.Delta);
      break;
    }
    case AgentHarnessEventKind::TransientToolStart:
      JsonWrite_String(&w, "type", SVL("tool_start"));
      WriteToolIdentity(&w, event);
      // Arguments are already validated JSON, so embed rather than re-quote.
      JsonWrite_Key(&w, "arguments");
      StrBuf_Append(out, event->Tool.ArgumentsJson.Length ? event->Tool.ArgumentsJson
                                                         : SVL("{}"));
      break;
    case AgentHarnessEventKind::TransientToolUpdate:
      JsonWrite_String(&w, "type", SVL("tool_update"));
      WriteToolIdentity(&w, event);
      WriteToolContent(&w, &event->Tool.Result);
      break;
    case AgentHarnessEventKind::TransientToolEnd:
      JsonWrite_String(&w, "type", SVL("tool_end"));
      WriteToolIdentity(&w, event);
      JsonWrite_Bool(&w, "isError", event->Tool.IsError);
      WriteToolContent(&w, &event->Tool.Result);
      break;
    case AgentHarnessEventKind::TransientRetryStart:
      JsonWrite_String(&w, "type", SVL("retry_start"));
      WriteRetry(&w, event);
      break;
    case AgentHarnessEventKind::TransientRetryEnd:
      JsonWrite_String(&w, "type", SVL("retry_end"));
      WriteRetry(&w, event);
      break;
    case AgentHarnessEventKind::TransientQueueUpdate:
      JsonWrite_String(&w, "type", SVL("queue_update"));
      JsonWrite_U64(&w, "steer", event->Queue.SteerCount);
      JsonWrite_U64(&w, "followUp", event->Queue.FollowUpCount);
      JsonWrite_U64(&w, "nextTurn", event->Queue.NextTurnCount);
      break;
    case AgentHarnessEventKind::TransientCancelRequested:
      JsonWrite_String(&w, "type", SVL("cancel_requested"));
      break;
    default:
      return reject();
  }

  JsonWrite_End(&w);
  return true;
}
