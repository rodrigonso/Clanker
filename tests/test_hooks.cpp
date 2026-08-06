#include <stdio.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app/hook_config.h"
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
  const char* dir = "clank_hooks_test";
  const char* path = "clank_hooks_test/hooks.json";
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
  session.TranscriptPath = SVL("C:\\home\\.clank\\sessions\\v2\\repo-1\\s.jsonl");

  StrBuf payload = StrBuf_Make(&a, 256);
  HookRunner_BuildPayload(&payload, HookEvent::PreToolUse, &session, SVL("read"));
  StrView text = StrBuf_View(&payload);
  CHECK_ALL(StrView_Contains(text, "\"hook_event_name\":\"PreToolUse\""),
            StrView_Contains(text, "\"session_id\":\"0123456789abcdefghijkl\""),
            StrView_Contains(text, "\"tool_name\":\"read\""),
            // Backslashes must survive as JSON escapes, not raw bytes.
            StrView_Contains(text, "C:\\\\work\\\\repo"),
            StrView_Contains(text, "s.jsonl"));

  // Events with no tool subject omit the key rather than sending an empty one.
  StrBuf sessionStart = StrBuf_Make(&a, 256);
  HookRunner_BuildPayload(&sessionStart, HookEvent::SessionStart, &session, StrView{"", 0});
  CHECK_ALL(StrView_Contains(StrBuf_View(&sessionStart),
                             "\"hook_event_name\":\"SessionStart\""),
            !StrView_Contains(StrBuf_View(&sessionStart), "tool_name"));

  // A session with nothing to report still produces a well-formed object.
  HookSessionInfo empty = {};
  StrBuf bare = StrBuf_Make(&a, 128);
  HookRunner_BuildPayload(&bare, HookEvent::SessionEnd, &empty, StrView{"", 0});
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
  const char* sinkPath = "clank_hooks_live.txt";
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
  const char* sinkPath = "clank_hooks_bounded.txt";
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

// A session switch must be reflected in later payloads, and must not accumulate
// state. The identity used to be duplicated into a fixed 1 MiB arena on every
// switch, and ArenaAlloc traps rather than failing softly once that fills.
void TestRunnerSessionSwitch() {
  Arena a = ArenaCreate();
  const char* sinkPath = "clank_hooks_switch.txt";
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
  first.WorkingDirectory = SVL("C:\\Users\\dev\\projects\\clank");
  first.TranscriptPath = SVL("C:\\Users\\dev\\.clank\\sessions\\v2\\clank-c838f21a\\first.jsonl");
  HookRunner* runner = HookRunner_Create(&a, &config, &first);
  CHECK(runner != nullptr);

  // Far more switches than the old per-switch duplication could have survived,
  // at the identity sizes a real session actually reports.
  HookSessionInfo second = {};
  second.SessionId = SVL("second-session");
  second.WorkingDirectory = SVL("C:\\Users\\dev\\projects\\clank");
  second.TranscriptPath =
      SVL("C:\\Users\\dev\\.clank\\sessions\\v2\\clank-c838f21a\\second.jsonl");
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
  const char* sinkPath = "clank_hooks_fanout.txt";
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
}
