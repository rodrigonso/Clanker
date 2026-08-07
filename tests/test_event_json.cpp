#include <string.h>

#include "agent/driver.h"
#include "agent/event_json.h"
#include "agent/harness.h"
#include "base/arena.h"
#include "check.h"
#include "json/json.h"
#include "script_provider.h"
#include "session/memory_repository.h"

namespace {

StrView WriteEvent(Arena* arena, const AgentHarnessEvent* event, bool* wrote) {
  StrBuf line = StrBuf_Make(arena, 128);
  *wrote = AgentEventJson_Write(&line, event);
  return StrBuf_View(&line);
}

// Every emitted line must be a standalone JSON object with a string "type".
bool IsValidEventLine(Arena* arena, StrView line, const char* expectedType) {
  JsonValue* root = Json_Parse(arena, line);
  if (!Json_IsObject(root)) return false;
  StrView type = Json_GetStr(root, "type");
  return StrView_EqCStr(type, expectedType);
}

void TestTransientShapes(Arena* arena) {
  bool wrote = false;

  AgentHarnessEvent agentStart = {};
  agentStart.Kind = AgentHarnessEventKind::TransientAgentStart;
  agentStart.Turn.Kind = AgentTurnKind::Run;
  StrView line = WriteEvent(arena, &agentStart, &wrote);
  CHECK_ALL(wrote, StrView_EqCStr(line, "{\"type\":\"agent_start\",\"turn\":\"run\"}"));

  AgentHarnessEvent compactionStart = agentStart;
  compactionStart.Turn.Kind = AgentTurnKind::Compaction;
  line = WriteEvent(arena, &compactionStart, &wrote);
  CHECK_ALL(wrote,
            StrView_EqCStr(line, "{\"type\":\"agent_start\",\"turn\":\"compaction\"}"));

  AgentHarnessEvent agentEnd = {};
  agentEnd.Kind = AgentHarnessEventKind::TransientAgentEnd;
  agentEnd.Turn.Kind = AgentTurnKind::Run;
  agentEnd.Turn.Outcome = AgentTurnOutcome::Failed;
  agentEnd.Turn.Error = SVL("boom");
  line = WriteEvent(arena, &agentEnd, &wrote);
  CHECK_ALL(wrote,
            StrView_EqCStr(
                line,
                "{\"type\":\"agent_end\",\"turn\":\"run\",\"outcome\":\"failed\",\"error\":\"boom\"}"));

  // A succeeded turn carries no error key at all.
  AgentHarnessEvent turnEnd = {};
  turnEnd.Kind = AgentHarnessEventKind::TransientTurnEnd;
  turnEnd.Turn.Kind = AgentTurnKind::Run;
  turnEnd.Turn.Outcome = AgentTurnOutcome::Succeeded;
  line = WriteEvent(arena, &turnEnd, &wrote);
  CHECK_ALL(wrote,
            StrView_EqCStr(line,
                           "{\"type\":\"turn_end\",\"turn\":\"run\",\"outcome\":\"succeeded\"}"));

  // Text deltas carry payload; structural stream markers are not representable.
  AgentHarnessEvent delta = {};
  delta.Kind = AgentHarnessEventKind::TransientStream;
  delta.Stream.Kind = StreamEventKind::TextDelta;
  delta.Stream.ContentIndex = 2;
  delta.Stream.Delta = SVL("hi \"there\"");
  line = WriteEvent(arena, &delta, &wrote);
  CHECK_ALL(wrote,
            StrView_EqCStr(
                line, "{\"type\":\"text_delta\",\"index\":2,\"text\":\"hi \\\"there\\\"\"}"));

  AgentHarnessEvent structural = delta;
  structural.Stream.Kind = StreamEventKind::TextStart;
  StrBuf untouched = StrBuf_Make(arena, 32);
  StrBuf_AppendCStr(&untouched, "keep");
  CHECK(!AgentEventJson_Write(&untouched, &structural));
  // A rejected event must leave the buffer exactly as it was found.
  CHECK(StrView_EqCStr(StrBuf_View(&untouched), "keep"));

  AgentHarnessEvent toolStart = {};
  toolStart.Kind = AgentHarnessEventKind::TransientToolStart;
  toolStart.Tool.CallId = SVL("call-1");
  toolStart.Tool.Name = SVL("shell");
  toolStart.Tool.ArgumentsJson = SVL("{\"command\":\"ls\"}");
  line = WriteEvent(arena, &toolStart, &wrote);
  CHECK_ALL(wrote,
            StrView_EqCStr(line,
                           "{\"type\":\"tool_start\",\"id\":\"call-1\",\"name\":\"shell\","
                           "\"arguments\":{\"command\":\"ls\"}}"));

  // Absent arguments still produce a valid object rather than a bare key.
  AgentHarnessEvent bareTool = toolStart;
  bareTool.Tool.ArgumentsJson = StrView{};
  line = WriteEvent(arena, &bareTool, &wrote);
  CHECK_ALL(wrote, IsValidEventLine(arena, line, "tool_start"),
            StrView_Contains(line, "\"arguments\":{}"));

  ToolResult result = {};
  result.Content = Array_Make<Content>(arena, 2);
  Array_Push(&result.Content, Content_Text(SVL("out")));
  Array_Push(&result.Content, Content_Text(SVL("put")));
  AgentHarnessEvent toolEnd = {};
  toolEnd.Kind = AgentHarnessEventKind::TransientToolEnd;
  toolEnd.Tool.CallId = SVL("call-1");
  toolEnd.Tool.Name = SVL("shell");
  toolEnd.Tool.IsError = true;
  toolEnd.Tool.Result = result;
  line = WriteEvent(arena, &toolEnd, &wrote);
  CHECK_ALL(wrote,
            StrView_EqCStr(line,
                           "{\"type\":\"tool_end\",\"id\":\"call-1\",\"name\":\"shell\","
                           "\"isError\":true,\"content\":\"output\"}"));

  AgentHarnessEvent retry = {};
  retry.Kind = AgentHarnessEventKind::TransientRetryStart;
  retry.Retry.Layer = RetryLayer::AssistantTurn;
  retry.Retry.Attempt = 1;
  retry.Retry.MaxAttempts = 3;
  retry.Retry.DelayMs = 250;
  retry.Retry.Error = SVL("overloaded");
  line = WriteEvent(arena, &retry, &wrote);
  CHECK_ALL(wrote, IsValidEventLine(arena, line, "retry_start"),
            StrView_Contains(line, "\"layer\":\"assistant_turn\""),
            StrView_Contains(line, "\"delayMs\":250"));

  AgentHarnessEvent queue = {};
  queue.Kind = AgentHarnessEventKind::TransientQueueUpdate;
  queue.Queue.SteerCount = 1;
  queue.Queue.FollowUpCount = 2;
  queue.Queue.NextTurnCount = 3;
  line = WriteEvent(arena, &queue, &wrote);
  CHECK_ALL(wrote,
            StrView_EqCStr(
                line,
                "{\"type\":\"queue_update\",\"steer\":1,\"followUp\":2,\"nextTurn\":3}"));

  AgentHarnessEvent cancel = {};
  cancel.Kind = AgentHarnessEventKind::TransientCancelRequested;
  line = WriteEvent(arena, &cancel, &wrote);
  CHECK_ALL(wrote, StrView_EqCStr(line, "{\"type\":\"cancel_requested\"}"));

  CHECK(!AgentEventJson_Write(nullptr, &cancel));
  StrBuf sink = StrBuf_Make(arena, 8);
  CHECK(!AgentEventJson_Write(&sink, nullptr));
}

bool EchoToolExec(void* context, Arena* arena, StrView id, const ToolArgs* args,
                  volatile i32* abort, const ToolUpdateSink* updates, ToolResult* out,
                  StrBuf* error) {
  (void)context;
  (void)id;
  (void)args;
  (void)abort;
  (void)updates;
  (void)error;
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("tool output")));
  return true;
}

// End-to-end: drive a real run and serialize every event the watch produces,
// proving the whole chain emits well-formed NDJSON in the documented order.
void TestRunStream(Arena* arena) {
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions options = {};
  options.ProjectKey = SVL("event-json-tests");
  options.WorkingDirectory = SVL("C:\\work\\clank");
  options.Label = SVL("events");
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      repository.Create(repository.Ctx, arena, &options, &log, &info)));

  Tool echo[1] = {};
  echo[0].Name = SVL("echo");
  echo[0].Description = SVL("echo tool");
  echo[0].ParamsJsonSchema = SVL("{\"type\":\"object\"}");
  echo[0].Execute = EchoToolExec;

  ScriptToolCall calls[1] = {ScriptToolCall{SVL("echo"), SVL("call-1"), SVL("{}")}};
  ScriptStep steps[2] = {};
  steps[0].Text = SVL("working");
  steps[0].Tools = calls;
  steps[0].ToolCount = 1;
  steps[1].Text = SVL("done");
  steps[1].Stop = StopReason::Stop;
  ScriptProvider script = {steps, 2, 0};

  AgentHarnessConfig config = {};
  config.SystemPrompt = SVL("sys");
  config.ModelInfo.Id = SVL("script-model");
  config.ModelInfo.Provider = SVL("script");
  config.Tools = echo;
  config.ToolCount = 1;
  config.ProviderImpl = ScriptProvider_Make(&script);
  config.ToolExec = ToolExecution::Parallel;

  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

  AgentHarnessSnapshot snapshot = {};
  AgentHarnessWatch* watchPtr = ArenaPushStruct(arena, AgentHarnessWatch);
  *watchPtr = AgentHarnessWatch{};
  AgentHarnessWatch& watch = *watchPtr;
  CHECK(SessionResult_IsOk(
      AgentHarness_Watch(arena, &harness, 128, &snapshot, &watch)));

  CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));

  u32 lines = 0, agentStarts = 0, agentEnds = 0, records = 0, toolStarts = 0;
  u32 toolEnds = 0, deltas = 0, invalid = 0;
  bool sawWorkingDelta = false, sawToolOutput = false;
  bool finished = false;
  StrView firstLine = {};
  StrView lastLine = {};
  while (!finished) {
    AgentHarnessEvent event = {};
    CHECK(SessionResult_IsOk(AgentHarness_WatchNext(arena, &watch, &event)));
    if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) finished = true;

    StrBuf line = StrBuf_Make(arena, 256);
    if (!AgentEventJson_Write(&line, &event)) continue;
    StrView text = StrBuf_View(&line);
    lines++;
    if (!lines) continue;
    if (lines == 1) firstLine = text;
    lastLine = text;

    JsonValue* root = Json_Parse(arena, text);
    if (!Json_IsObject(root)) {
      invalid++;
      continue;
    }
    StrView type = Json_GetStr(root, "type");
    if (StrView_EqCStr(type, "agent_start")) agentStarts++;
    else if (StrView_EqCStr(type, "agent_end")) agentEnds++;
    else if (StrView_EqCStr(type, "record")) records++;
    else if (StrView_EqCStr(type, "tool_start")) toolStarts++;
    else if (StrView_EqCStr(type, "tool_end")) toolEnds++;
    else if (StrView_EqCStr(type, "text_delta")) {
      deltas++;
      if (StrView_EqCStr(Json_GetStr(root, "text"), "working")) sawWorkingDelta = true;
    }
    if (StrView_EqCStr(type, "tool_end") &&
        StrView_EqCStr(Json_GetStr(root, "content"), "tool output"))
      sawToolOutput = true;
  }

  // Every line parsed, the run is bracketed by agent_start/agent_end, and the
  // durable records plus tool lifecycle all made it through.
  CHECK_ALL(invalid == 0, agentStarts == 1, agentEnds == 1, toolStarts == 1,
            toolEnds == 1, deltas >= 1, sawWorkingDelta, sawToolOutput);
  // user + assistant + tool result + assistant
  CHECK(records >= 4);
  CHECK_ALL(StrView_Contains(firstLine, "\"type\":\"agent_start\""),
            StrView_Contains(lastLine, "\"type\":\"agent_end\""),
            StrView_Contains(lastLine, "\"outcome\":\"succeeded\""));

  CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
  repository.Close(repository.Ctx);
}

}  // namespace

void TestEventJson() {
  Arena arena = ArenaCreate((usize)32 << 20);
  TestTransientShapes(&arena);
  TestRunStream(&arena);
  ArenaRelease(&arena);
}
