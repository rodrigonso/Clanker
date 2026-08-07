// Clank test harness.
//
// A single CLI entry point for every test suite in the project. Humans and
// agents select what to run by name or substring instead of always paying for
// the full sweep:
//
//   clank_tests                    run every default suite
//   clank_tests --list             print suite names and tags
//   clank_tests json session       run the named suites
//   clank_tests --filter tui       run suites whose name contains "tui"
//   clank_tests --all              include network-tagged suites
//   clank_tests --benchmark ...    run the opt-in agent-loop benchmark
//   clank_tests --help             usage
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/arena.h"
#include "base/process_file_lock.h"
#include "base/thread.h"
#include "json/json.h"
#include "tests.h"

int g_checks = 0;
int g_failures = 0;
const char* g_test_executable = nullptr;

namespace {

// Suites tagged Network perform live I/O and are skipped unless explicitly
// requested (by name, by --filter, or via --all), so the default run stays
// offline and deterministic.
enum SuiteTag { TagDefault, TagNetwork };

struct Suite {
  const char* Name;
  void (*Fn)();
  SuiteTag Tag;
  const char* Desc;
};

const Suite kSuites[] = {
    {"arena", TestArena, TagDefault, "arena allocator"},
    {"array", TestArray, TagDefault, "dynamic array"},
    {"str", TestStr, TagDefault, "string views and builders"},
    {"hostinfo", TestHostInfo, TagDefault, "host/system info"},
    {"types", TestTypes, TagDefault, "core type invariants"},
    {"json", TestJson, TagDefault, "json parser and writer"},
    {"session", TestSession, TagDefault, "durable sessions and replay"},
    {"harness", TestHarness, TagDefault, "agent harness loop"},
    {"compaction", TestCompaction, TagDefault, "compaction policy and cut points"},
    {"event_json", TestEventJson, TagDefault, "NDJSON agent event stream"},
    {"anthropic", TestAnthropic, TagDefault, "anthropic proto and SSE"},
    {"openai", TestOpenAI, TagDefault, "openai Responses protocol and SSE"},
    {"tools", TestTools, TagDefault, "tool implementations"},
    {"infra", TestInfra, TagDefault, "glob, diff, fs, locks"},
    {"crypto", TestCrypto, TagDefault, "sha256 and random"},
    {"auth", TestAuth, TagDefault, "credential store and OAuth"},
    {"command", TestCommand, TagDefault, "slash command parsing"},
    {"hooks", TestHooks, TagDefault, "external lifecycle hooks"},
    {"prompt_context", TestPromptContext, TagDefault, "system prompt and project context"},
    {"project_trust", TestProjectTrust, TagDefault, "hierarchical project trust"},
    {"loopback", TestLoopback, TagDefault, "OAuth loopback server"},
    {"provider_registry", TestProviderRegistry, TagDefault, "provider registry"},
    {"settings", TestSettings, TagDefault, "settings load/save"},
    {"thread", TestThread, TagDefault, "threads and sync"},
    {"keys", TestKeys, TagDefault, "key decoding"},
    {"screen", TestScreen, TagDefault, "screen buffer and diffing"},
    {"markdown", TestMarkdown, TagDefault, "markdown rendering"},
    {"sanitize", TestSanitize, TagDefault, "output sanitization"},
    {"stream_reveal", TestStreamReveal, TagDefault, "streaming reveal"},
    {"linemodel", TestLineModel, TagDefault, "line model and wrapping"},
    {"lineedit", TestLineEdit, TagDefault, "multi-line editor"},
    {"ui", TestUi, TagDefault, "frame-built terminal components"},
    {"ui_widgets", TestUiWidgets, TagDefault, "reusable terminal widgets"},
    {"ui_demo", TestUiDemo, TagDefault, "standalone component screen"},
    {"tui", TestTui, TagDefault, "headless TUI controller"},
    {"http", TestHttp, TagDefault, "HTTP transport and retry policy"},
    {"http_live", TestHttpLive, TagNetwork, "live TLS/HTTP request (network)"},
};
const u32 kSuiteCount = sizeof(kSuites) / sizeof(kSuites[0]);

bool Contains(const char* haystack, const char* needle) {
  return strstr(haystack, needle) != nullptr;
}

void PrintUsage(const char* exe) {
  printf("usage: %s [options] [suite...]\n\n", exe);
  printf("Runs Clank's test suites. With no suite arguments, every suite\n");
  printf("except network-tagged ones is run.\n\n");
  printf("options:\n");
  printf("  --list            list available suites and exit\n");
  printf("  --filter <text>   run suites whose name contains <text>\n");
  printf("  --all             include network-tagged suites\n");
  printf("  --benchmark ...   run the agent-loop latency benchmark\n");
  printf("  -h, --help        show this help\n\n");
  printf("examples:\n");
  printf("  %s                       # full offline sweep\n", exe);
  printf("  %s json session          # just those two suites\n", exe);
  printf("  %s --filter line         # linemodel + lineedit\n", exe);
  printf("  %s --all                 # everything, including network\n", exe);
}

void PrintList() {
  printf("%-20s %-8s %s\n", "SUITE", "TAG", "DESCRIPTION");
  for (u32 i = 0; i < kSuiteCount; i++) {
    printf("%-20s %-8s %s\n", kSuites[i].Name,
           kSuites[i].Tag == TagNetwork ? "network" : "default",
           kSuites[i].Desc);
  }
}

// Reserved subcommand: the process file lock test re-execs this binary to
// verify a second process observes the lock as busy. Must run before any
// suite selection so it never gets treated as a suite name.
int RunLockProbe(const char* path) {
  Arena arena = ArenaCreate();
  ProcessFileLock lock = {};
  ProcessFileLockStatus status =
      ProcessFileLock_TryAcquire(&arena, SV(path), &lock);
  if (status == ProcessFileLockStatus::Acquired)
    ProcessFileLock_Release(&lock, SV(path));
  ArenaRelease(&arena);
  return status == ProcessFileLockStatus::Busy ? 0 : 1;
}

bool HasArg(int argc, char** argv, const char* expected) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], expected) == 0) return true;
  }
  return false;
}

void WriteFakeGrepMatch(StrView path, u32 lineNumber, StrView lineText) {
  Arena arena = ArenaCreate((usize)1 << 20);
  StrBuf json = StrBuf_Make(&arena, path.Length + lineText.Length + 128);
  StrBuf_AppendCStr(&json, "{\"type\":\"match\",\"data\":{\"path\":{\"text\":");
  Json_WriteString(&json, path);
  StrBuf_AppendCStr(&json, "},\"lines\":{\"text\":");
  Json_WriteString(&json, lineText);
  StrBuf_AppendCStr(&json, "},\"line_number\":");
  StrBuf_AppendU64(&json, lineNumber);
  StrBuf_AppendCStr(&json, "}}\n");
  fwrite(json.Data, 1, json.Length, stdout);
  ArenaRelease(&arena);
}

int RunFakeRipgrep(int argc, char** argv) {
  int separator = -1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      separator = i;
      break;
    }
  }
  if (separator < 0 || separator + 2 >= argc) return 2;
  const char* pattern = argv[separator + 1];
  const char* root = argv[separator + 2];

  if (strcmp(pattern, "[") == 0) {
    fprintf(stderr, "regex parse error: invalid repetition operator\n");
    return 2;
  }
  if (strcmp(pattern, "definitely-no-match") == 0) return 1;
  if (strcmp(pattern, "BETA") == 0 &&
      (!HasArg(argc, argv, "--json") || !HasArg(argc, argv, "--line-number") ||
       !HasArg(argc, argv, "--color=never") || !HasArg(argc, argv, "--hidden") ||
       !HasArg(argc, argv, "--ignore-case") || !HasArg(argc, argv, "--fixed-strings") ||
       !HasArg(argc, argv, "--glob"))) {
    fprintf(stderr, "expected grep flags were not forwarded\n");
    return 9;
  }

  Arena arena = ArenaCreate((usize)1 << 20);
  StrBuf path = StrBuf_Make(&arena, (u32)strlen(root) + 16);
  StrBuf_AppendCStr(&path, root);
#if defined(_WIN32)
  StrBuf_AppendChar(&path, '\\');
#else
  StrBuf_AppendChar(&path, '/');
#endif
  StrBuf_AppendCStr(&path, "a.txt");

  if (strcmp(pattern, "alpha|beta|gamma") == 0) {
    WriteFakeGrepMatch(StrBuf_View(&path), 1, SVL("alpha\n"));
    WriteFakeGrepMatch(StrBuf_View(&path), 2, SVL("beta\n"));
    WriteFakeGrepMatch(StrBuf_View(&path), 3, SVL("gamma\n"));
  } else if (strcmp(pattern, "x+") == 0) {
    StrBuf longLine = StrBuf_Make(&arena, 640);
    for (u32 i = 0; i < 600; i++) StrBuf_AppendChar(&longLine, 'x');
    StrBuf_AppendChar(&longLine, '\n');
    WriteFakeGrepMatch(StrBuf_View(&path), 2, StrBuf_View(&longLine));
  } else if (pattern[0] == '\0') {
    WriteFakeGrepMatch(StrBuf_View(&path), 1, SVL("alpha\n"));
  } else {
    WriteFakeGrepMatch(StrBuf_View(&path), 2, SVL("beta\r\n"));
  }
  fflush(stdout);
  ArenaRelease(&arena);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (getenv("CLANK_TEST_FAKE_RG")) return RunFakeRipgrep(argc, argv);
  if (argc == 3 && strcmp(argv[1], "--echo-arg") == 0) {
    fwrite(argv[2], 1, strlen(argv[2]), stdout);
    return 0;
  }
  // Hook-runner helpers: a child that drains stdin into a file, and one that
  // ignores stdin and outlives its timeout.
  if (argc == 3 && strcmp(argv[1], "--sink-stdin") == 0) {
    FILE* out = fopen(argv[2], "ab");
    if (!out) return 1;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) fwrite(buf, 1, n, out);
    fputc('\n', out);
    fclose(out);
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "--hang") == 0) {
    Sleep_Ms((u32)atoi(argv[2]));
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "--expect-lock-busy") == 0) {
    return RunLockProbe(argv[2]);
  }
  if (argc >= 2 && strcmp(argv[1], "--benchmark") == 0)
    return RunAgentBenchmark(argc - 1, argv + 1);

  g_test_executable = argv[0];

  bool selected[kSuiteCount] = {};
  bool anySelected = false;
  bool includeNetwork = false;
  bool unknownArg = false;

  for (int i = 1; i < argc; i++) {
    const char* arg = argv[i];
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (strcmp(arg, "--list") == 0) {
      PrintList();
      return 0;
    }
    if (strcmp(arg, "--all") == 0) {
      includeNetwork = true;
      continue;
    }
    if (strcmp(arg, "--filter") == 0) {
      if (i + 1 >= argc) {
        printf("error: --filter requires an argument\n");
        return 2;
      }
      const char* needle = argv[++i];
      bool matched = false;
      for (u32 s = 0; s < kSuiteCount; s++) {
        if (Contains(kSuites[s].Name, needle)) {
          selected[s] = true;
          anySelected = true;
          matched = true;
        }
      }
      if (!matched) {
        printf("error: no suite matches filter '%s'\n", needle);
        unknownArg = true;
      }
      continue;
    }
    if (arg[0] == '-') {
      printf("error: unknown option '%s'\n", arg);
      unknownArg = true;
      continue;
    }
    bool matched = false;
    for (u32 s = 0; s < kSuiteCount; s++) {
      if (strcmp(kSuites[s].Name, arg) == 0) {
        selected[s] = true;
        anySelected = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      printf("error: unknown suite '%s'\n", arg);
      unknownArg = true;
    }
  }

  if (unknownArg) {
    printf("run '%s --list' to see available suites\n", argv[0]);
    return 2;
  }

  // No explicit selection: run everything, network suites only with --all.
  if (!anySelected) {
    for (u32 s = 0; s < kSuiteCount; s++) {
      selected[s] = includeNetwork || kSuites[s].Tag == TagDefault;
    }
  }

  u32 ran = 0;
  for (u32 s = 0; s < kSuiteCount; s++) {
    if (!selected[s]) continue;
    int before = g_failures;
    printf("[ run  ] %s\n", kSuites[s].Name);
    kSuites[s].Fn();
    ran++;
    printf("[ %s ] %s\n", g_failures == before ? " ok  " : "FAIL!",
           kSuites[s].Name);
  }

  printf("tests: %u suites, %d checks, %d failures\n", ran, g_checks,
         g_failures);
  return g_failures ? 1 : 0;
}
