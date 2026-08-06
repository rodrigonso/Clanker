#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "agent/harness.h"
#include "base/arena.h"
#include "base/thread.h"
#include "script_provider.h"
#include "session/memory_repository.h"

namespace {

constexpr u32 kDefaultWarmup = 20;
constexpr u32 kDefaultIterations = 100;
constexpr u32 kMaxBenchmarkValue = 1000000;

struct BenchArgs {
  const char* Workload;
  u32* Cycles;
  u32 CycleCount;
  u32* Tools;
  u32 ToolCount;
  u32 Warmup;
  u32 Iterations;
};

struct BenchTiming {
  Mutex Lock;
  double* ProviderStarts;
  double* ToolEnds;
  u32 ProviderCapacity;
  u32 ToolCapacity;
  u32 ProviderCount;
  u32 ToolEndCount;
};

struct BenchProvider {
  ScriptProvider Script;
  BenchTiming* Timing;
};

struct BenchRun {
  double FullRunMs;
  double* GapsMs;
  u32 GapCount;
};

struct BenchDistribution {
  double MedianMs;
  double P95Ms;
  u32 Samples;
};

struct BenchCase {
  const char* Workload;
  u32 Size;
  bool Parallel;
  BenchDistribution FullRun;
  BenchDistribution Gap;
};

double BenchNowMs() {
#if defined(_WIN32)
  static LARGE_INTEGER frequency = {};
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  LARGE_INTEGER value = {};
  QueryPerformanceCounter(&value);
  return (double)value.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
  timespec value = {};
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
#endif
}

void RecordProviderStart(BenchTiming* timing) {
  ScopedLock lock(&timing->Lock);
  if (timing->ProviderCount < timing->ProviderCapacity)
    timing->ProviderStarts[timing->ProviderCount++] = BenchNowMs();
}

void RecordToolEnd(BenchTiming* timing) {
  ScopedLock lock(&timing->Lock);
  if (timing->ToolEndCount < timing->ToolCapacity)
    timing->ToolEnds[timing->ToolEndCount++] = BenchNowMs();
}

bool BenchProviderStream(void* context, Arena* arena, const Model* model,
                         const LlmContext* llm, const StreamOptions* options,
                         StreamSink sink, Message* out) {
  BenchProvider* provider = (BenchProvider*)context;
  RecordProviderStart(provider->Timing);
  return ScriptProvider_Stream(&provider->Script, arena, model, llm, options, sink, out);
}

bool BenchToolExecute(void* context, Arena* arena, StrView, const ToolArgs*,
                      volatile i32*, const ToolUpdateSink*, ToolResult* out,
                      StrBuf*) {
  (void)context;
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("ok")));
  return true;
}

void BenchAfterTool(void* user, Arena*, StrView, const ToolArgs*,
                    ToolOutcome*) {
  RecordToolEnd((BenchTiming*)user);
}

SessionResult CreateBenchSession(SessionRepository* repository, Arena* arena,
                                 SessionLog* out) {
  SessionCreateOptions options = {};
  options.ProjectKey = SVL("agent-benchmark");
  options.WorkingDirectory = SVL("benchmark");
  options.Label = SVL("benchmark");
  SessionInfo info = {};
  return repository->Create(repository->Ctx, arena, &options, out, &info);
}

StrView BenchToolId(Arena* arena, const char* prefix, u32 index) {
  StrBuf id = StrBuf_Make(arena, 32);
  StrBuf_AppendCStr(&id, prefix);
  StrBuf_AppendU64(&id, index);
  return StrBuf_View(&id);
}

void BuildSequentialScript(Arena* arena, u32 cycles, BenchProvider* provider) {
  ScriptStep* steps = ArenaPushArray(arena, ScriptStep, cycles + 1);
  for (u32 i = 0; i < cycles; i++) {
    ScriptToolCall* call = ArenaPushStruct(arena, ScriptToolCall);
    call->Name = SVL("bench_tool");
    call->Id = BenchToolId(arena, "cycle-", i);
    call->ArgsJson = SVL("{\"n\":1}");
    steps[i].Tools = call;
    steps[i].ToolCount = 1;
  }
  steps[cycles].Text = SVL("done");
  steps[cycles].Stop = StopReason::Stop;
  provider->Script.Steps = steps;
  provider->Script.Count = cycles + 1;
}

void BuildParallelScript(Arena* arena, u32 count, BenchProvider* provider) {
  ScriptStep* steps = ArenaPushArray(arena, ScriptStep, 2);
  ScriptToolCall* calls = ArenaPushArray(arena, ScriptToolCall, count);
  for (u32 i = 0; i < count; i++) {
    calls[i].Name = SVL("bench_tool");
    calls[i].Id = BenchToolId(arena, "parallel-", i);
    calls[i].ArgsJson = SVL("{\"n\":1}");
  }
  steps[0].Tools = calls;
  steps[0].ToolCount = count;
  steps[1].Text = SVL("done");
  steps[1].Stop = StopReason::Stop;
  provider->Script.Steps = steps;
  provider->Script.Count = 2;
}

bool RunOnce(u32 size, bool parallel, BenchRun* out) {
  Arena arena = ArenaCreate();
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  SessionLog log = {};
  SessionResult created = CreateBenchSession(&repository, &arena, &log);
  if (!SessionResult_IsOk(created)) {
    fprintf(stderr, "error: benchmark session creation failed: %.*s\n",
            (int)created.Message.Length, created.Message.Data);
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
    return false;
  }

  BenchTiming timing = {};
  timing.ProviderCapacity = parallel ? 2 : size + 1;
  timing.ToolCapacity = size;
  timing.ProviderStarts = ArenaPushArray(&arena, double, timing.ProviderCapacity);
  timing.ToolEnds = ArenaPushArray(&arena, double, timing.ToolCapacity);
  Mutex_Init(&timing.Lock);

  BenchProvider provider = {};
  provider.Timing = &timing;
  if (parallel)
    BuildParallelScript(&arena, size, &provider);
  else
    BuildSequentialScript(&arena, size, &provider);

  Tool tool = {};
  tool.Name = SVL("bench_tool");
  tool.Label = SVL("Bench Tool");
  tool.Description = SVL("Deterministic in-process benchmark tool.");
  tool.ParamsJsonSchema =
      SVL("{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"number\"}},"
          "\"required\":[\"n\"]}");
  tool.Ctx = &timing;
  tool.Execute = BenchToolExecute;

  AgentHarnessConfig config = {};
  config.SystemPrompt = SVL("bench");
  config.ModelInfo.Id = SVL("script");
  config.ModelInfo.Provider = SVL("script");
  config.Tools = &tool;
  config.ToolCount = 1;
  config.ProviderImpl.Ctx = &provider;
  config.ProviderImpl.Stream = BenchProviderStream;
  config.ToolExec = parallel ? ToolExecution::Parallel : ToolExecution::Sequential;
  config.MaxParallelTools = parallel ? size : 0;
  config.ToolHooks.User = &timing;
  config.ToolHooks.AfterTool = BenchAfterTool;

  AgentHarness harness = {};
  SessionResult initialized = AgentHarness_Init(&harness, log, &config);
  if (!SessionResult_IsOk(initialized)) {
    fprintf(stderr, "error: benchmark harness initialization failed: %.*s\n",
            (int)initialized.Message.Length, initialized.Message.Data);
    Mutex_Destroy(&timing.Lock);
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
    return false;
  }

  double started = BenchNowMs();
  SessionResult ran = AgentHarness_Run(&harness, SVL("go"));
  out->FullRunMs = BenchNowMs() - started;

  if (SessionResult_IsOk(ran)) {
    out->GapCount = parallel ? 1 : size;
    out->GapsMs = ArenaPushArray(&arena, double, out->GapCount);
    if (parallel) {
      double batchEnd = 0;
      for (u32 i = 0; i < timing.ToolEndCount; i++)
        if (timing.ToolEnds[i] > batchEnd) batchEnd = timing.ToolEnds[i];
      out->GapsMs[0] = timing.ProviderStarts[1] - batchEnd;
    } else {
      for (u32 i = 0; i < out->GapCount; i++)
        out->GapsMs[i] = timing.ProviderStarts[i + 1] - timing.ToolEnds[i];
    }
  } else {
    fprintf(stderr, "error: benchmark run failed: %.*s\n",
            (int)ran.Message.Length, ran.Message.Data);
  }

  bool valid = SessionResult_IsOk(ran) &&
               timing.ProviderCount == (parallel ? 2u : size + 1) &&
               timing.ToolEndCount == size;
  if (!valid && SessionResult_IsOk(ran))
    fprintf(stderr, "error: benchmark produced incomplete timing events\n");
  AgentHarness_Close(&harness);
  Mutex_Destroy(&timing.Lock);
  repository.Close(repository.Ctx);

  if (valid) {
    double* copied = (double*)malloc(sizeof(double) * out->GapCount);
    if (!copied) valid = false;
    if (copied) {
      memcpy(copied, out->GapsMs, sizeof(double) * out->GapCount);
      out->GapsMs = copied;
    }
  }
  ArenaRelease(&arena);
  return valid;
}

int CompareDouble(const void* a, const void* b) {
  double left = *(const double*)a;
  double right = *(const double*)b;
  return left < right ? -1 : left > right ? 1 : 0;
}

BenchDistribution Distribution(double* values, u32 count) {
  BenchDistribution result = {};
  result.Samples = count;
  if (!count) return result;
  qsort(values, count, sizeof(double), CompareDouble);
  u32 medianIndex = (count + 1) / 2 - 1;
  u32 p95Index = (95 * count + 99) / 100 - 1;
  result.MedianMs = values[medianIndex];
  result.P95Ms = values[p95Index];
  return result;
}

bool BenchmarkCase(Arena* arena, u32 size, bool parallel, u32 warmup,
                   u32 iterations, BenchCase* out) {
  for (u32 i = 0; i < warmup; i++) {
    BenchRun run = {};
    if (!RunOnce(size, parallel, &run)) return false;
    free(run.GapsMs);
  }

  double* fullRuns = ArenaPushArray(arena, double, iterations);
  u64 gapCapacity64 = (u64)iterations * (parallel ? 1u : size);
  if (gapCapacity64 > UINT_MAX) {
    fprintf(stderr, "error: benchmark sample count is too large\n");
    return false;
  }
  u32 gapCapacity = (u32)gapCapacity64;
  double* gaps = ArenaPushArray(arena, double, gapCapacity);
  u32 gapCount = 0;
  for (u32 i = 0; i < iterations; i++) {
    BenchRun run = {};
    if (!RunOnce(size, parallel, &run)) return false;
    fullRuns[i] = run.FullRunMs;
    memcpy(gaps + gapCount, run.GapsMs, sizeof(double) * run.GapCount);
    gapCount += run.GapCount;
    free(run.GapsMs);
  }

  out->Size = size;
  out->Parallel = parallel;
  out->Workload = parallel ? "parallel" : size <= 5 ? "shallow" : "deep";
  out->FullRun = Distribution(fullRuns, iterations);
  out->Gap = Distribution(gaps, gapCount);
  return true;
}

bool ParsePositive(const char* value, const char* option, u32* out) {
  if (!value || !*value || *value == '-') {
    fprintf(stderr, "error: %s requires a positive integer\n", option);
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (errno == ERANGE || *end || parsed < 1 ||
      parsed > kMaxBenchmarkValue) {
    fprintf(stderr, "error: %s requires a positive integer\n", option);
    return false;
  }
  *out = (u32)parsed;
  return true;
}

bool ParseList(Arena* arena, const char* value, const char* option,
               u32** outValues, u32* outCount) {
  u32 count = 1;
  for (const char* at = value; *at; at++)
    if (*at == ',') count++;
  u32* values = ArenaPushArray(arena, u32, count);
  const char* at = value;
  for (u32 i = 0; i < count; i++) {
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = strtoull(at, &end, 10);
    if (errno == ERANGE || end == at || parsed < 1 ||
        parsed > kMaxBenchmarkValue ||
        (*end != ',' && *end != '\0')) {
      fprintf(stderr, "error: %s requires comma-separated positive integers\n",
              option);
      return false;
    }
    values[i] = (u32)parsed;
    at = *end == ',' ? end + 1 : end;
  }
  *outValues = values;
  *outCount = count;
  return true;
}

void PrintBenchmarkUsage() {
  printf("usage: clank_tests --benchmark [options]\n\n");
  printf("  --workload NAME          all, shallow, deep, or parallel (default: all)\n");
  printf("  --cycles N[,N...]        sequential tool-cycle depths\n");
  printf("  --tools N[,N...]         parallel tools per turn\n");
  printf("  --warmup N               warmup runs (default: %u)\n", kDefaultWarmup);
  printf("  --iterations N           measured runs (default: %u)\n", kDefaultIterations);
}

bool ParseArgs(Arena* arena, int argc, char** argv, BenchArgs* out) {
  static u32 defaultCycles[] = {5, 20, 50, 100};
  static u32 defaultTools[] = {1, 2, 4, 8, 16};
  out->Workload = "all";
  out->Cycles = defaultCycles;
  out->CycleCount = sizeof(defaultCycles) / sizeof(defaultCycles[0]);
  out->Tools = defaultTools;
  out->ToolCount = sizeof(defaultTools) / sizeof(defaultTools[0]);
  out->Warmup = kDefaultWarmup;
  out->Iterations = kDefaultIterations;

  for (int i = 1; i < argc; i++) {
    const char* option = argv[i];
    if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
      PrintBenchmarkUsage();
      return false;
    }
    if (i + 1 >= argc) {
      fprintf(stderr, "error: %s requires a value\n", option);
      return false;
    }
    const char* value = argv[++i];
    if (strcmp(option, "--workload") == 0) {
      out->Workload = value;
    } else if (strcmp(option, "--cycles") == 0) {
      if (!ParseList(arena, value, option, &out->Cycles, &out->CycleCount))
        return false;
    } else if (strcmp(option, "--tools") == 0) {
      if (!ParseList(arena, value, option, &out->Tools, &out->ToolCount))
        return false;
    } else if (strcmp(option, "--warmup") == 0) {
      if (!ParsePositive(value, option, &out->Warmup)) return false;
    } else if (strcmp(option, "--iterations") == 0) {
      if (!ParsePositive(value, option, &out->Iterations)) return false;
    } else {
      fprintf(stderr, "error: unknown benchmark option '%s'\n", option);
      return false;
    }
  }
  if (strcmp(out->Workload, "all") != 0 &&
      strcmp(out->Workload, "shallow") != 0 &&
      strcmp(out->Workload, "deep") != 0 &&
      strcmp(out->Workload, "parallel") != 0) {
    fprintf(stderr,
            "error: --workload must be all, shallow, deep, or parallel\n");
    return false;
  }
  return true;
}

void PrintDistribution(const BenchDistribution& distribution) {
  printf("{\"medianMs\":%.6f,\"p95Ms\":%.6f,\"samples\":%u}",
         distribution.MedianMs, distribution.P95Ms, distribution.Samples);
}

}  // namespace

int RunAgentBenchmark(int argc, char** argv) {
  if (argc > 1 &&
      (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    PrintBenchmarkUsage();
    return 0;
  }

  Arena arena = ArenaCreate();
  BenchArgs args = {};
  if (!ParseArgs(&arena, argc, argv, &args)) {
    ArenaRelease(&arena);
    return 2;
  }

  u32 capacity = args.CycleCount + args.ToolCount;
  BenchCase* cases = ArenaPushArray(&arena, BenchCase, capacity);
  u32 caseCount = 0;
  if (strcmp(args.Workload, "all") == 0 ||
      strcmp(args.Workload, "shallow") == 0) {
    for (u32 i = 0; i < args.CycleCount; i++) {
      if (strcmp(args.Workload, "all") == 0 && args.Cycles[i] > 5) continue;
      if (!BenchmarkCase(&arena, args.Cycles[i], false, args.Warmup,
                         args.Iterations, &cases[caseCount++])) {
        ArenaRelease(&arena);
        return 1;
      }
    }
  }
  if (strcmp(args.Workload, "all") == 0 ||
      strcmp(args.Workload, "deep") == 0) {
    for (u32 i = 0; i < args.CycleCount; i++) {
      if (strcmp(args.Workload, "all") == 0 && args.Cycles[i] <= 5) continue;
      if (!BenchmarkCase(&arena, args.Cycles[i], false, args.Warmup,
                         args.Iterations, &cases[caseCount++])) {
        ArenaRelease(&arena);
        return 1;
      }
    }
  }
  if (strcmp(args.Workload, "all") == 0 ||
      strcmp(args.Workload, "parallel") == 0) {
    for (u32 i = 0; i < args.ToolCount; i++) {
      if (!BenchmarkCase(&arena, args.Tools[i], true, args.Warmup,
                         args.Iterations, &cases[caseCount++])) {
        ArenaRelease(&arena);
        return 1;
      }
    }
  }

  printf("[\n");
  for (u32 i = 0; i < caseCount; i++) {
    const BenchCase& item = cases[i];
    printf("  {\"runtime\":\"clank\",\"workload\":\"%s\",", item.Workload);
    if (item.Parallel)
      printf("\"toolsPerTurn\":%u,", item.Size);
    else
      printf("\"cycles\":%u,", item.Size);
    printf("\"fullRun\":");
    PrintDistribution(item.FullRun);
    printf(",\"toolEndToNextProviderStart\":");
    PrintDistribution(item.Gap);
    printf("}%s\n", i + 1 == caseCount ? "" : ",");
  }
  printf("]\n");
  ArenaRelease(&arena);
  return 0;
}
