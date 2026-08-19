#include <stdio.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app/hook_config.h"
#include "app/hook_gate.h"
#include "app/hook_runner.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "base/str.h"
#include "base/thread.h"
#include "check.h"
#include "json/json.h"
#include "tests.h"
#include "tools/exec.h"
#include "tools/fs.h"

namespace {

StrView SelectOne(const HookConfig* config, const char* event, const char* subject) {
  const HookCommand* commands[4];
  u32 count = HookConfig_Select(config, SV(event), SV(subject), commands, 4);
  return count ? commands[0]->Command : StrView{"", 0};
}

u32 SelectCount(const HookConfig* config, const char* event, const char* subject) {
  const HookCommand* commands[8];
  return HookConfig_Select(config, SV(event), SV(subject), commands, 8);
}

void TestMatcher() {
  // An absent matcher is the common case and must accept everything.
  CHECK_ALL(HookMatcher_Accepts(StrView{"", 0}, SVL("read")),
            HookMatcher_Accepts(StrView{"", 0}, StrView{"", 0}));

  CHECK_ALL(HookMatcher_Accepts(SVL("read"), SVL("read")),
            !HookMatcher_Accepts(SVL("read"), SVL("write")),
            // Exact alternation only: a matcher never matches a prefix.
            !HookMatcher_Accepts(SVL("read"), SVL("read_file")));

  CHECK_ALL(HookMatcher_Accepts(SVL("read|write|edit"), SVL("read")),
            HookMatcher_Accepts(SVL("read|write|edit"), SVL("write")),
            HookMatcher_Accepts(SVL("read|write|edit"), SVL("edit")),
            !HookMatcher_Accepts(SVL("read|write|edit"), SVL("grep")));

  // "*" is the ecosystem's spelling of "every tool"; treating it literally
  // would register commands that silently never fire.
  CHECK_ALL(HookMatcher_Accepts(SVL("*"), SVL("read")),
            HookMatcher_Accepts(SVL("*"), SVL("anything")),
            HookMatcher_Accepts(SVL("read|*"), SVL("grep")));

  // Non-tool events carry no subject, so a matcher has nothing to narrow and
  // must not reject: a stray matcher on SessionStart would otherwise leave a
  // hook registered but dead.
  CHECK_ALL(HookMatcher_Accepts(SVL("read"), StrView{"", 0}),
            HookMatcher_Accepts(SVL("a||b"), StrView{"", 0}));
}

void TestParse() {
  Arena a = ArenaCreate();

  HookConfig config = {};
  CHECK(HookConfig_Parse(&a, SVL("{bad"), &config) == HookConfigLoadResult::Malformed);
  CHECK(HookConfig_Parse(&a, SVL("[]"), &config) == HookConfigLoadResult::Malformed);

  // An empty table parses cleanly and simply registers nothing.
  CHECK(HookConfig_Parse(&a, SVL("{\"hooks\":{}}"), &config) ==
        HookConfigLoadResult::Success);
  CHECK_ALL(config.CommandCount == 0, config.Events.Count == 0);

  StrView document = SVL(
      "{\"hooks\":{"
      "\"SessionStart\":[{\"hooks\":[{\"type\":\"command\","
      "\"command\":\"start.exe\",\"timeout\":10}]}],"
      "\"PreToolUse\":[{\"matcher\":\"read|write\",\"hooks\":[{\"type\":\"command\","
      "\"command\":\"tool.exe\",\"timeout\":3}]}],"
      "\"SessionEnd\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"end.exe\"}]}]"
      "}}");
  CHECK(HookConfig_Parse(&a, document, &config) == HookConfigLoadResult::Success);
  CHECK_ALL(config.Events.Count == 3, config.CommandCount == 3);

  CHECK(StrView_Eq(SelectOne(&config, "SessionStart", ""), SVL("start.exe")));
  CHECK(StrView_Eq(SelectOne(&config, "SessionEnd", ""), SVL("end.exe")));

  // The matcher gates a tool event on the tool name.
  CHECK(StrView_Eq(SelectOne(&config, "PreToolUse", "read"), SVL("tool.exe")));
  CHECK(StrView_Eq(SelectOne(&config, "PreToolUse", "write"), SVL("tool.exe")));
  CHECK(SelectCount(&config, "PreToolUse", "grep") == 0);

  // Unregistered events select nothing at all.
  CHECK(SelectCount(&config, "Stop", "") == 0);

  // Seconds become milliseconds; an absent timeout falls back to the default.
  const HookCommand* selected[4];
  CHECK(HookConfig_Select(&config, SVL("SessionStart"), StrView{"", 0}, selected, 4) == 1);
  CHECK(selected[0]->TimeoutMs == 10000);
  CHECK(HookConfig_Select(&config, SVL("SessionEnd"), StrView{"", 0}, selected, 4) == 1);
  CHECK(selected[0]->TimeoutMs == HOOK_DEFAULT_TIMEOUT_MS);

  // A sub-millisecond timeout must not truncate to zero, which the exec layer
  // reads as "no timeout at all".
  CHECK(HookConfig_Parse(
            &a,
            SVL("{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\","
                "\"command\":\"x\",\"timeout\":0.0005}]}]}}"),
            &config) == HookConfigLoadResult::Success);
  CHECK(HookConfig_Select(&config, SVL("Stop"), StrView{"", 0}, selected, 4) == 1);
  CHECK(selected[0]->TimeoutMs >= 1);

  ArenaRelease(&a);
}

void TestParseTolerance() {
  Arena a = ArenaCreate();
  HookConfig config = {};

  // A bare table without the "hooks" wrapper is accepted so a hand-written
  // file need not nest.
  CHECK(HookConfig_Parse(
            &a, SVL("{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"s.exe\"}]}]}"),
            &config) == HookConfigLoadResult::Success);
  CHECK(StrView_Eq(SelectOne(&config, "Stop", ""), SVL("s.exe")));

  // Entries of an unknown type, or with no command, are skipped without
  // failing the whole file -- one bad entry must not disable every hook.
  CHECK(HookConfig_Parse(
            &a,
            SVL("{\"hooks\":{\"Stop\":[{\"hooks\":["
                "{\"type\":\"webhook\",\"url\":\"http://x\"},"
                "{\"type\":\"command\"},"
                "{\"type\":\"command\",\"command\":\"good.exe\"}]}]}}"),
            &config) == HookConfigLoadResult::Success);
  CHECK_ALL(config.CommandCount == 1,
            StrView_Eq(SelectOne(&config, "Stop", ""), SVL("good.exe")));

  // Several groups and several commands accumulate in configuration order.
  CHECK(HookConfig_Parse(
            &a,
            SVL("{\"hooks\":{\"Stop\":["
                "{\"hooks\":[{\"type\":\"command\",\"command\":\"a.exe\"},"
                "{\"type\":\"command\",\"command\":\"b.exe\"}]},"
                "{\"hooks\":[{\"type\":\"command\",\"command\":\"c.exe\"}]}]}}"),
            &config) == HookConfigLoadResult::Success);
  CHECK(SelectCount(&config, "Stop", "") == 3);

  ArenaRelease(&a);
}

void TestLoad() {
  Arena a = ArenaCreate();
  const char* dir = "clanker_hooks_test";
  const char* path = "clanker_hooks_test/hooks.json";
  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif

  HookConfig config = {};
  CHECK(HookConfig_Load(&a, SV(path), &config) == HookConfigLoadResult::Missing);
  // A missing file must still leave a usable, empty config behind.
  CHECK(config.CommandCount == 0);

  CHECK(ConfigFile_WriteAtomic(&a, SV(path), SVL("{oops")));
  CHECK(HookConfig_Load(&a, SV(path), &config) == HookConfigLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\","
          "\"command\":\"echo hi\"}]}]}}")));
  CHECK(HookConfig_Load(&a, SV(path), &config) == HookConfigLoadResult::Success);
  CHECK(StrView_Eq(SelectOne(&config, "SessionStart", ""), SVL("echo hi")));

  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  ArenaRelease(&a);
}

void TestPayload() {
  Arena a = ArenaCreate();

  HookSessionInfo session = {};
  session.SessionId = SVL("0123456789abcdefghijkl");
  session.WorkingDirectory = SVL("C:\\work\\repo");
  session.TranscriptPath = SVL("C:\\home\\.clanker\\sessions\\v2\\repo-1\\s.jsonl");
  session.Interactive = true;

  HookToolInfo tool = {};
  tool.Name = SVL("read");
  tool.CallId = SVL("call_42");
  tool.InputJson = SVL("{\"path\":\"a.txt\"}");

  StrBuf payload = StrBuf_Make(&a, 256);
  HookRunner_BuildPayload(&payload, HookEvent::PreToolUse, &session, &tool);
  StrView text = StrBuf_View(&payload);
  CHECK_ALL(StrView_Contains(text, "\"hook_event_name\":\"PreToolUse\""),
            StrView_Contains(text, "\"session_id\":\"0123456789abcdefghijkl\""),
            StrView_Contains(text, "\"tool_name\":\"read\""),
            StrView_Contains(text, "\"tool_call_id\":\"call_42\""),
            // Arguments arrive as a nested object, not an escaped string.
            StrView_Contains(text, "\"tool_input\":{\"path\":\"a.txt\"}"),
            StrView_Contains(text, "\"interactive\":true"),
            !StrView_Contains(text, "tool_input_omitted"),
            // Backslashes must survive as JSON escapes, not raw bytes.
            StrView_Contains(text, "C:\\\\work\\\\repo"),
            StrView_Contains(text, "s.jsonl"));

  // Oversized arguments are dropped whole and flagged, never half-written.
  HookToolInfo omitted = {};
  omitted.Name = SVL("write");
  omitted.InputOmitted = true;
  StrBuf dropped = StrBuf_Make(&a, 256);
  HookRunner_BuildPayload(&dropped, HookEvent::PreToolUse, &session, &omitted);
  CHECK_ALL(StrView_Contains(StrBuf_View(&dropped), "\"tool_input_omitted\":true"),
            !StrView_Contains(StrBuf_View(&dropped), "\"tool_input\":"));

  // A notification has nobody to prompt, so it carries no interactive flag.
  StrBuf post = StrBuf_Make(&a, 256);
  HookRunner_BuildPayload(&post, HookEvent::PostToolUse, &session, &tool);
  CHECK(!StrView_Contains(StrBuf_View(&post), "interactive"));

  // Events with no tool subject omit the key rather than sending an empty one.
  StrBuf sessionStart = StrBuf_Make(&a, 256);
  HookRunner_BuildPayload(&sessionStart, HookEvent::SessionStart, &session, nullptr);
  CHECK_ALL(StrView_Contains(StrBuf_View(&sessionStart),
                             "\"hook_event_name\":\"SessionStart\""),
            !StrView_Contains(StrBuf_View(&sessionStart), "tool_name"));

  // A session with nothing to report still produces a well-formed object.
  HookSessionInfo empty = {};
  StrBuf bare = StrBuf_Make(&a, 128);
  HookRunner_BuildPayload(&bare, HookEvent::SessionEnd, &empty, nullptr);
  CHECK(StrView_Eq(StrBuf_View(&bare),
                   SVL("{\"hook_event_name\":\"SessionEnd\"}")));

  ArenaRelease(&a);
}

void TestRunnerLifecycle() {
  Arena a = ArenaCreate();

  // No configured commands means no runner and therefore no worker thread.
  HookConfig empty = {};
  HookConfig_Parse(&a, SVL("{\"hooks\":{}}"), &empty);
  HookSessionInfo session = {};
  CHECK(HookRunner_Create(&a, &empty, &session) == nullptr);

  // Dispatching through a null runner is a no-op, which is what every call
  // site relies on when hooks are disabled.
  HookRunner_Dispatch(nullptr, HookEvent::Stop, StrView{"", 0});
  HookRunner_Shutdown(nullptr, HookEvent::SessionEnd);

  CHECK(StrView_Eq(HookEvent_Name(HookEvent::UserPromptSubmit), SVL("UserPromptSubmit")));
  CHECK(StrView_Eq(HookEvent_Name(HookEvent::PostToolUse), SVL("PostToolUse")));
  // Stop and StopFailure must stay distinct names: a consumer maps them to
  // different terminal states.
  CHECK_ALL(StrView_Eq(HookEvent_Name(HookEvent::Stop), SVL("Stop")),
            StrView_Eq(HookEvent_Name(HookEvent::StopFailure), SVL("StopFailure")));

  ArenaRelease(&a);
}

// Quotes a path for a shell command line so a test executable living under a
// path with spaces still runs.
StrView SelfCommand(Arena* a, const char* subcommand, StrView argument) {
  StrView exe = Fs_RealPath(a, SV(g_test_executable));
  StrBuf out = StrBuf_Make(a, exe.Length + argument.Length + 32);
  StrBuf_AppendChar(&out, '"');
  StrBuf_Append(&out, exe);
  StrBuf_AppendCStr(&out, "\" ");
  StrBuf_AppendCStr(&out, subcommand);
  StrBuf_AppendCStr(&out, " \"");
  StrBuf_Append(&out, argument);
  StrBuf_AppendChar(&out, '"');
  return StrBuf_View(&out);
}

StrView Slurp(Arena* a, const char* path) {
  StrView content = {};
  return ConfigFile_ReadAll(a, SV(path), &content) == ConfigFileReadResult::Success
             ? content
             : StrView{"", 0};
}

// Each --sink-stdin child appends one payload terminated by a newline, so this
// counts how many hook commands actually ran.
u32 CountLines(StrView text) {
  u32 count = 0;
  for (u32 i = 0; i < text.Length; i++)
    if (text.Data[i] == '\n') count++;
  return count;
}

// The threaded half of the runner: without this, the queue, the worker, the
// stdin pipe and the shutdown guarantee have no execution coverage at all.
void TestRunnerDelivery() {
  Arena a = ArenaCreate();
  const char* sinkPath = "clanker_hooks_live.txt";
  remove(sinkPath);

  StrBuf document = StrBuf_Make(&a, 512);
  StrBuf_AppendCStr(&document, "{\"hooks\":{\"PreToolUse\":[{\"hooks\":[{\"type\":\"command\",\"command\":");
  Json_WriteString(&document, SelfCommand(&a, "--sink-stdin", SV(sinkPath)));
  StrBuf_AppendCStr(&document, "}]}],\"SessionEnd\":[{\"hooks\":[{\"type\":\"command\",\"command\":");
  Json_WriteString(&document, SelfCommand(&a, "--sink-stdin", SV(sinkPath)));
  StrBuf_AppendCStr(&document, "}]}]}}");

  HookConfig config = {};
  CHECK(HookConfig_Parse(&a, StrBuf_View(&document), &config) ==
        HookConfigLoadResult::Success);

  HookSessionInfo session = {};
  session.SessionId = SVL("live-session-id");
  session.WorkingDirectory = SVL("live-cwd");
  HookRunner* runner = HookRunner_Create(&a, &config, &session);
  CHECK(runner != nullptr);

  HookRunner_Dispatch(runner, HookEvent::PreToolUse, SVL("read"));
  // Shutdown must both flush what is running and deliver the terminal event.
  HookRunner_Shutdown(runner, HookEvent::SessionEnd);

  StrView written = Slurp(&a, sinkPath);
  CHECK_ALL(StrView_Contains(written, "\"hook_event_name\":\"PreToolUse\""),
            StrView_Contains(written, "\"tool_name\":\"read\""),
            StrView_Contains(written, "\"session_id\":\"live-session-id\""),
            // The terminal event is the one delivery a consumer cannot do
            // without, so it must be present even though it was never queued.
            StrView_Contains(written, "\"hook_event_name\":\"SessionEnd\""));

  remove(sinkPath);
  ArenaRelease(&a);
}

// A backlog of slow hooks must not hold the process open for the sum of their
// timeouts, and must not cost the terminal event its delivery.
void TestRunnerShutdownIsBounded() {
  Arena a = ArenaCreate();
  const char* sinkPath = "clanker_hooks_bounded.txt";
  remove(sinkPath);

  StrBuf document = StrBuf_Make(&a, 512);
  StrBuf_AppendCStr(&document, "{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"command\":");
  Json_WriteString(&document, SelfCommand(&a, "--hang", SVL("30000")));
  StrBuf_AppendCStr(&document, ",\"timeout\":600}]}],\"SessionEnd\":[{\"hooks\":[{\"type\":\"command\",\"command\":");
  Json_WriteString(&document, SelfCommand(&a, "--sink-stdin", SV(sinkPath)));
  StrBuf_AppendCStr(&document, "}]}]}}");

  HookConfig config = {};
  CHECK(HookConfig_Parse(&a, StrBuf_View(&document), &config) ==
        HookConfigLoadResult::Success);

  HookSessionInfo session = {};
  session.SessionId = SVL("bounded");
  HookRunner* runner = HookRunner_Create(&a, &config, &session);
  CHECK(runner != nullptr);

  // Far more than the queue holds, each nominally a 600 s hook: draining this
  // honestly would outlive any plausible user's patience.
  for (u32 i = 0; i < 200; i++)
    HookRunner_Dispatch(runner, HookEvent::Stop, StrView{"", 0});

  u64 start = MonotonicMillis();
  HookRunner_Shutdown(runner, HookEvent::SessionEnd);
  u64 elapsed = MonotonicMillis() - start;

  CHECK(elapsed < 20000);
  CHECK(StrView_Contains(Slurp(&a, sinkPath), "\"hook_event_name\":\"SessionEnd\""));

  remove(sinkPath);
  ArenaRelease(&a);
}

// The timeout is the only thing standing between a wedged hook and a wedged
// agent, so verify the exec layer actually enforces it.
void TestExecInputTimeout() {
  Arena a = ArenaCreate();

  u64 start = MonotonicMillis();
  // A command beginning with a quoted path is exactly what `cmd /c` mangles
  // unless the whole thing is re-wrapped, so this doubles as that regression.
  StrView hangCommand = SelfCommand(&a, "--hang", SVL("30000"));
  ExecResult result =
      Exec_RunShellInput(&a, hangCommand, SVL("{\"ignored\":true}"), 500, nullptr);
  u64 elapsed = MonotonicMillis() - start;
  CHECK_ALL(result.Spawned, result.TimedOut, elapsed < 10000);

  // An oversized payload is refused rather than blocking on a pipe write that
  // no timeout can interrupt.
  StrBuf huge = StrBuf_Make(&a, (1u << 16) + 16);
  for (u32 i = 0; i < (1u << 16); i++) StrBuf_AppendChar(&huge, 'x');
  ExecResult refused = Exec_RunShellInput(&a, SVL("cmd /c exit 0"), StrBuf_View(&huge),
                                          1000, nullptr);
  CHECK(!refused.Spawned);

  ArenaRelease(&a);
}

// Capturing a hook's stdout means the parent now writes the payload and reads
// the reply over two pipes at once. A child that floods stdout without ever
// reading stdin is the case that wedges a naive implementation, and it is not
// reachable from the gate tests because their children all stay small.
void TestExecInputCapture() {
  Arena a = ArenaCreate();

  const int kLines = 4000;  // ~260 KB, several times any default pipe buffer
  StrView floodCommand = SelfCommand(&a, "--flood-stdout", SVL("4000"));
  StrBuf captured = StrBuf_Make(&a, 1024);
  u64 start = MonotonicMillis();
  ExecResult result = Exec_RunShellInput(&a, floodCommand, SVL("{\"unread\":true}"), 30000,
                                         nullptr, &captured);
  u64 elapsed = MonotonicMillis() - start;
  CHECK_ALL(result.Spawned, !result.TimedOut, result.ExitCode == 0, elapsed < 30000);

  // Counting newlines rather than bytes keeps this exact across the CRLF
  // translation Windows applies to the child's text-mode stdout.
  u32 newlines = 0;
  for (u32 i = 0; i < result.Output.Length; i++)
    if (result.Output.Data[i] == '\n') newlines++;
  CHECK(newlines == (u32)kLines);

  // With a limit the capture stops growing but the child still runs to
  // completion, so a hook that dumps a log file bounds memory instead of
  // walking the scratch arena into its fastfail.
  StrBuf clipped = StrBuf_Make(&a, 1024);
  ExecResult limited = Exec_RunShellInput(&a, floodCommand, SVL("{\"unread\":true}"), 30000,
                                          nullptr, &clipped, 8192);
  CHECK_ALL(limited.Spawned, !limited.TimedOut, limited.ExitCode == 0,
            limited.Output.Length == 8192);

  ArenaRelease(&a);
}
void TestRunnerSessionSwitch() {
  Arena a = ArenaCreate();
  const char* sinkPath = "clanker_hooks_switch.txt";
  remove(sinkPath);

  StrBuf document = StrBuf_Make(&a, 512);
  StrBuf_AppendCStr(&document,
                    "{\"hooks\":{\"SessionEnd\":[{\"hooks\":[{\"type\":\"command\",\"command\":");
  Json_WriteString(&document, SelfCommand(&a, "--sink-stdin", SV(sinkPath)));
  StrBuf_AppendCStr(&document, "}]}]}}");

  HookConfig config = {};
  CHECK(HookConfig_Parse(&a, StrBuf_View(&document), &config) ==
        HookConfigLoadResult::Success);

  HookSessionInfo first = {};
  first.SessionId = SVL("first-session");
  first.WorkingDirectory = SVL("C:\\Users\\dev\\projects\\clanker");
  first.TranscriptPath = SVL("C:\\Users\\dev\\.clanker\\sessions\\v2\\clanker-c838f21a\\first.jsonl");
  HookRunner* runner = HookRunner_Create(&a, &config, &first);
  CHECK(runner != nullptr);

  // Far more switches than the old per-switch duplication could have survived,
  // at the identity sizes a real session actually reports.
  HookSessionInfo second = {};
  second.SessionId = SVL("second-session");
  second.WorkingDirectory = SVL("C:\\Users\\dev\\projects\\clanker");
  second.TranscriptPath =
      SVL("C:\\Users\\dev\\.clanker\\sessions\\v2\\clanker-c838f21a\\second.jsonl");
  for (u32 i = 0; i < 20000; i++) HookRunner_SetSession(runner, &second);

  // A value too large to store is reported as absent rather than truncated:
  // half a path is worse than no path, because a consumer would act on it.
  StrBuf overlong = StrBuf_Make(&a, 4096);
  for (u32 i = 0; i < 3000; i++) StrBuf_AppendChar(&overlong, 'p');
  HookSessionInfo third = second;
  third.TranscriptPath = StrBuf_View(&overlong);
  HookRunner_SetSession(runner, &third);

  HookRunner_Shutdown(runner, HookEvent::SessionEnd);

  StrView written = Slurp(&a, sinkPath);
  CHECK_ALL(StrView_Contains(written, "\"session_id\":\"second-session\""),
            !StrView_Contains(written, "first-session"),
            !StrView_Contains(written, "transcript_path"));

  remove(sinkPath);
  ArenaRelease(&a);
}

// More commands on one event than any fixed selection buffer would hold. Every
// one must actually run: a cap here means a config that looks correct silently
// stops firing partway down the list.
void TestSelectionIsNotCapped() {
  Arena a = ArenaCreate();
  constexpr u32 kCommands = 12;
  const char* sinkPath = "clanker_hooks_fanout.txt";
  remove(sinkPath);

  StrView command = SelfCommand(&a, "--sink-stdin", SV(sinkPath));
  StrBuf document = StrBuf_Make(&a, 4096);
  StrBuf_AppendCStr(&document, "{\"hooks\":{\"SessionEnd\":[{\"hooks\":[");
  for (u32 i = 0; i < kCommands; i++) {
    if (i) StrBuf_AppendChar(&document, ',');
    StrBuf_AppendCStr(&document, "{\"type\":\"command\",\"command\":");
    Json_WriteString(&document, command);
    StrBuf_AppendChar(&document, '}');
  }
  StrBuf_AppendCStr(&document, "]}]}}");

  HookConfig config = {};
  CHECK(HookConfig_Parse(&a, StrBuf_View(&document), &config) ==
        HookConfigLoadResult::Success);
  CHECK(config.CommandCount == kCommands);

  // CommandCount is what the runner sizes its selection by, so it has to bound
  // any single event's fan-out.
  const HookCommand* selected[kCommands];
  CHECK(HookConfig_Select(&config, SVL("SessionEnd"), StrView{"", 0}, selected, kCommands) ==
        kCommands);

  HookSessionInfo session = {};
  session.SessionId = SVL("fanout");
  HookRunner* runner = HookRunner_Create(&a, &config, &session);
  CHECK(runner != nullptr);
  HookRunner_Shutdown(runner, HookEvent::SessionEnd);

  // One appended line per command that ran.
  CHECK(CountLines(Slurp(&a, sinkPath)) == kCommands);

  remove(sinkPath);
  ArenaRelease(&a);
}

// Builds a hooks document registering `commands` for PreToolUse, optionally
// narrowed by `matcher`.
StrView GateDocument(Arena* a, const StrView* commands, u32 count, const char* matcher) {
  StrBuf out = StrBuf_Make(a, 1024);
  StrBuf_AppendCStr(&out, "{\"hooks\":{\"PreToolUse\":[{");
  if (matcher) {
    StrBuf_AppendCStr(&out, "\"matcher\":");
    Json_WriteString(&out, SV(matcher));
    StrBuf_AppendChar(&out, ',');
  }
  StrBuf_AppendCStr(&out, "\"hooks\":[");
  for (u32 i = 0; i < count; i++) {
    if (i) StrBuf_AppendChar(&out, ',');
    StrBuf_AppendCStr(&out, "{\"type\":\"command\",\"command\":");
    Json_WriteString(&out, commands[i]);
    StrBuf_AppendChar(&out, '}');
  }
  StrBuf_AppendCStr(&out, "]}]}}");
  return StrBuf_View(&out);
}

HookGate* MakeGate(Arena* a, HookConfig* config, StrView document) {
  CHECK(HookConfig_Parse(a, document, config) == HookConfigLoadResult::Success);
  HookSessionInfo session = {};
  session.SessionId = SVL("gate-session");
  session.WorkingDirectory = SVL("gate-cwd");
  session.Interactive = true;
  return HookGate_Create(a, config, &session, nullptr);
}

// Keeps each case below a single line now that captured stdout lives on the
// ExecResult rather than being passed alongside it.
AgentToolGateDecision Decide(Arena* a, ExecResult result, StrView output) {
  result.Output = output;
  AgentToolGateDecision decision = {};
  HookGate_Decide(a, &result, &decision);
  return decision;
}

// The decision protocol on its own, with no process in the way. Every branch
// here is a security boundary: a wrong answer either runs a tool the user
// forbade or blocks one they allowed.
void TestGateDecision() {
  Arena a = ArenaCreate();
  AgentToolGateDecision decision = {};

  ExecResult ok = {};
  ok.Spawned = true;
  ExecResult failed = ok;
  failed.ExitCode = 3;

  // Exit 0 with nothing to say is the ordinary allow.
  decision = Decide(&a, ok, StrView{"", 0});
  CHECK(!decision.Block);

  // A non-zero exit blocks, and whatever it printed becomes the reason.
  decision = Decide(&a, failed, SVL("  denied by policy\n"));
  CHECK_ALL(decision.Block, StrView_EqCStr(decision.Reason, "denied by policy"),
            !decision.Terminate);

  // A structured verdict outranks the exit code in both directions.
  decision = Decide(&a, failed, SVL("{\"block\":false}"));
  CHECK(!decision.Block);
  decision = Decide(&a, ok, SVL("{\"block\":true,\"reason\":\"no\",\"terminate\":true}"));
  CHECK_ALL(decision.Block, StrView_EqCStr(decision.Reason, "no"), decision.Terminate);

  // An empty reason stays empty so the agent layer owns pi's wording.
  decision = Decide(&a, ok, SVL("{\"block\":true}"));
  CHECK_ALL(decision.Block, decision.Reason.Length == 0);

  // terminate without block is meaningless and must not leak through.
  decision = Decide(&a, ok, SVL("{\"block\":false,\"terminate\":true}"));
  CHECK_ALL(!decision.Block, !decision.Terminate);

  // Output that is not an object falls through to the exit code rather than
  // being taken as consent.
  decision = Decide(&a, ok, SVL("[1,2,3]"));
  CHECK(!decision.Block);
  decision = Decide(&a, failed, SVL("{not json"));
  CHECK(decision.Block);

  // An object is a verdict only when it carries a boolean `block`. Anything
  // else is another tool's schema or a hook logging JSON, and must not suppress
  // the exit code that would otherwise have blocked.
  decision = Decide(&a, failed, SVL("{\"decision\":\"block\",\"reason\":\"nope\"}"));
  CHECK(decision.Block);
  decision = Decide(&a, failed, SVL("{\"block\":\"true\"}"));
  CHECK(decision.Block);
  decision = Decide(&a, failed, SVL("{\"hookSpecificOutput\":{\"permissionDecision\":\"deny\"}}"));
  CHECK(decision.Block);
  // Still an allow when the hook agrees on both channels.
  decision = Decide(&a, ok, SVL("{\"unrelated\":1}"));
  CHECK(!decision.Block);

  // An answer too large to read is not an answer, whatever the exit code says.
  StrBuf flood = StrBuf_Make(&a, (1u << 16) + 64);
  StrBuf_AppendCStr(&flood, "{\"block\":true,\"reason\":\"");
  while (flood.Length < (1u << 16) + 8) StrBuf_AppendChar(&flood, 'x');
  StrBuf_AppendCStr(&flood, "\"}");
  decision = Decide(&a, ok, StrBuf_View(&flood));
  CHECK_ALL(decision.Block, StrView_Contains(decision.Reason, "too much output"));

  // A long but readable reason is kept, and capped rather than dropped.
  StrBuf chatty = StrBuf_Make(&a, 16384);
  StrBuf_AppendCStr(&chatty, "{\"block\":true,\"reason\":\"");
  for (u32 i = 0; i < 9000; i++) StrBuf_AppendChar(&chatty, 'y');
  StrBuf_AppendCStr(&chatty, "\"}");
  decision = Decide(&a, ok, StrBuf_View(&chatty));
  CHECK_ALL(decision.Block, decision.Reason.Length == 4096);

  // Fail closed: nothing that leaves the verdict unknown may allow.
  ExecResult unspawned = {};
  unspawned.ExitCode = -1;
  decision = Decide(&a, unspawned, SVL("failed to start process"));
  CHECK_ALL(decision.Block, StrView_Contains(decision.Reason, "failed to start process"));

  ExecResult timedOut = ok;
  timedOut.TimedOut = true;
  // A timeout that exited 0 is the dangerous case: without an explicit check it
  // would read as an allow.
  decision = Decide(&a, timedOut, SVL("{\"block\":false}"));
  CHECK_ALL(decision.Block, StrView_Contains(decision.Reason, "timed out"));

  HookGate_Decide(&a, nullptr, &decision);
  CHECK(decision.Block);

  ArenaRelease(&a);
}

// A spurious timeout in the gate is a spurious *block*, so the wait loop's
// elapsed-time arithmetic has to be right. The POSIX loop used to subtract
// timespec nanosecond fields into an unsigned value, which underflowed whenever
// the nanosecond component wrapped and reported centuries of elapsed time; the
// odds of tripping it are roughly the child's runtime as a fraction of one
// second, so this repeats a clearly-under-timeout child rather than trusting a
// single pass. Keep both the count and the sleep: shortening either trades away
// most of the detection.
void TestExecInputTimeoutIsNotSpurious() {
  Arena a = ArenaCreate();

  StrView briefCommand = SelfCommand(&a, "--hang", SVL("200"));
  bool everTimedOut = false;
  bool everFailed = false;
  for (u32 i = 0; i < 10; i++) {
    StrBuf captured = StrBuf_Make(&a, 64);
    ExecResult result =
        Exec_RunShellInput(&a, briefCommand, SVL("{}"), 60000, nullptr, &captured);
    if (result.TimedOut) everTimedOut = true;
    if (!result.Spawned || result.ExitCode != 0) everFailed = true;
  }
  CHECK_ALL(!everTimedOut, !everFailed);

  ArenaRelease(&a);
}
void TestGateDelivery() {
  Arena a = ArenaCreate();
  const char* sinkPath = "clanker_gate_live.txt";
  ToolArgs args = {};
  args.Json = SVL("{\"path\":\"a.txt\"}");
  AgentToolGateDecision decision = {};

  // A non-zero exit blocks, and the payload the child saw carries everything a
  // policy needs.
  {
    remove(sinkPath);
    StrView commands[1] = {SelfCommand(&a, "--deny-stdin", SV(sinkPath))};
    HookConfig config = {};
    HookGate* gate = MakeGate(&a, &config, GateDocument(&a, commands, 1, nullptr));
    CHECK(gate != nullptr);
    HookGate_BeforeTool(gate, &a, SVL("read"), SVL("call-1"), &args, &decision);
    StrView seen = Slurp(&a, sinkPath);
    CHECK_ALL(decision.Block, StrView_EqCStr(decision.Reason, "denied by policy"),
              StrView_Contains(seen, "\"hook_event_name\":\"PreToolUse\""),
              StrView_Contains(seen, "\"tool_name\":\"read\""),
              StrView_Contains(seen, "\"tool_call_id\":\"call-1\""),
              StrView_Contains(seen, "\"tool_input\":{\"path\":\"a.txt\"}"),
              StrView_Contains(seen, "\"interactive\":true"));
  }

  // A structured verdict is honoured even though the child exited 0.
  {
    remove(sinkPath);
    StrView commands[1] = {SelfCommand(&a, "--deny-json-stdin", SV(sinkPath))};
    HookConfig config = {};
    HookGate* gate = MakeGate(&a, &config, GateDocument(&a, commands, 1, nullptr));
    HookGate_BeforeTool(gate, &a, SVL("read"), SVL("call-2"), &args, &decision);
    CHECK_ALL(decision.Block, StrView_EqCStr(decision.Reason, "structured no"),
              decision.Terminate);
  }

  // ... and an allow is honoured even though the child exited non-zero.
  {
    remove(sinkPath);
    StrView commands[1] = {SelfCommand(&a, "--allow-json-stdin", SV(sinkPath))};
    HookConfig config = {};
    HookGate* gate = MakeGate(&a, &config, GateDocument(&a, commands, 1, nullptr));
    HookGate_BeforeTool(gate, &a, SVL("read"), SVL("call-3"), &args, &decision);
    CHECK(!decision.Block);
  }

  // The first block short-circuits: the second command must never spawn, so
  // exactly one payload reaches the sink.
  {
    remove(sinkPath);
    StrView commands[2] = {SelfCommand(&a, "--deny-stdin", SV(sinkPath)),
                           SelfCommand(&a, "--sink-stdin", SV(sinkPath))};
    HookConfig config = {};
    HookGate* gate = MakeGate(&a, &config, GateDocument(&a, commands, 2, nullptr));
    HookGate_BeforeTool(gate, &a, SVL("read"), SVL("call-4"), &args, &decision);
    CHECK_ALL(decision.Block, CountLines(Slurp(&a, sinkPath)) == 1);
  }

  // A matcher narrows the gate by tool name: an unmatched tool spawns nothing.
  {
    remove(sinkPath);
    StrView commands[1] = {SelfCommand(&a, "--deny-stdin", SV(sinkPath))};
    HookConfig config = {};
    HookGate* gate = MakeGate(&a, &config, GateDocument(&a, commands, 1, "shell|write"));
    CHECK(gate != nullptr);
    HookGate_BeforeTool(gate, &a, SVL("read"), SVL("call-5"), &args, &decision);
    CHECK_ALL(!decision.Block, CountLines(Slurp(&a, sinkPath)) == 0);
    HookGate_BeforeTool(gate, &a, SVL("shell"), SVL("call-6"), &args, &decision);
    CHECK_ALL(decision.Block, CountLines(Slurp(&a, sinkPath)) == 1);
  }

  // A hook that outlives its timeout blocks rather than being waited out.
  {
    HookConfig config = {};
    StrBuf document = StrBuf_Make(&a, 512);
    StrBuf_AppendCStr(&document,
                      "{\"hooks\":{\"PreToolUse\":[{\"hooks\":[{\"type\":\"command\","
                      "\"timeout\":1,\"command\":");
    Json_WriteString(&document, SelfCommand(&a, "--hang", SVL("4000")));
    StrBuf_AppendCStr(&document, "}]}]}}");
    HookGate* gate = MakeGate(&a, &config, StrBuf_View(&document));
    HookGate_BeforeTool(gate, &a, SVL("read"), SVL("call-7"), &args, &decision);
    CHECK_ALL(decision.Block, StrView_Contains(decision.Reason, "timed out"));
  }

  // No PreToolUse command at all means no gate, so an unconfigured install
  // spawns nothing and pays nothing.
  {
    HookConfig config = {};
    CHECK(HookConfig_Parse(
              &a, SVL("{\"hooks\":{\"PostToolUse\":[{\"hooks\":[{\"type\":\"command\","
                      "\"command\":\"echo\"}]}]}}"),
              &config) == HookConfigLoadResult::Success);
    HookSessionInfo session = {};
    CHECK(HookGate_Create(&a, &config, &session, nullptr) == nullptr);
    // A null gate is the seam's allow-everything default.
    HookGate_BeforeTool(nullptr, &a, SVL("read"), SVL("call-8"), &args, &decision);
    CHECK(!decision.Block);
  }

  remove(sinkPath);
  ArenaRelease(&a);
}

}  // namespace

void TestHooks() {
  TestMatcher();
  TestParse();
  TestParseTolerance();
  TestLoad();
  TestPayload();
  TestRunnerLifecycle();
  TestRunnerDelivery();
  TestRunnerSessionSwitch();
  TestSelectionIsNotCapped();
  TestRunnerShutdownIsBounded();
  TestExecInputTimeout();
  TestExecInputCapture();
  TestExecInputTimeoutIsNotSpurious();
  TestGateDecision();
  TestGateDelivery();
}
