import { performance } from "node:perf_hooks";
import path from "node:path";
import process from "node:process";
import { pathToFileURL } from "node:url";
import { Type } from "typebox";

const DEFAULT_WARMUP = 20;
const DEFAULT_ITERATIONS = 100;

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(2);
}

function positiveInteger(value, option) {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < 1)
    fail(`${option} requires a positive integer`);
  return parsed;
}

function integerList(value, option) {
  const values = value.split(",").map((item) => positiveInteger(item, option));
  if (values.length === 0) fail(`${option} requires at least one value`);
  return values;
}

function parseArgs(argv) {
  const args = {
    piCheckout: ".pi-checkout",
    workload: "all",
    cycles: [5, 20, 50, 100],
    tools: [1, 2, 4, 8, 16],
    warmup: DEFAULT_WARMUP,
    iterations: DEFAULT_ITERATIONS,
  };
  for (let i = 0; i < argv.length; i++) {
    const option = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) fail(`${option} requires a value`);
      return argv[++i];
    };
    switch (option) {
      case "--pi-checkout": args.piCheckout = next(); break;
      case "--workload": args.workload = next(); break;
      case "--cycles": args.cycles = integerList(next(), option); break;
      case "--tools": args.tools = integerList(next(), option); break;
      case "--warmup": args.warmup = positiveInteger(next(), option); break;
      case "--iterations":
        args.iterations = positiveInteger(next(), option);
        break;
      case "--help":
      case "-h":
        console.log(
          "usage: node run-bench.mjs [options]\n\n" +
          "  --pi-checkout PATH       built pinned Pi checkout (default: .pi-checkout)\n" +
          "  --workload NAME          all, shallow, deep, or parallel (default: all)\n" +
          "  --cycles N[,N...]        sequential tool-cycle depths\n" +
          "  --tools N[,N...]         parallel tools per turn\n" +
          `  --warmup N               warmup runs (default: ${DEFAULT_WARMUP})\n` +
          `  --iterations N           measured runs (default: ${DEFAULT_ITERATIONS})`
        );
        process.exit(0);
      default: fail(`unknown option '${option}'`);
    }
  }
  if (!["all", "shallow", "deep", "parallel"].includes(args.workload))
    fail("--workload must be all, shallow, deep, or parallel");
  return args;
}

async function loadPi(checkout) {
  const aiUrl = pathToFileURL(
    path.join(checkout, "packages", "ai", "dist", "index.js")
  ).href;
  const agentUrl = pathToFileURL(
    path.join(checkout, "packages", "agent", "dist", "index.js")
  ).href;
  try {
    const [ai, agent] = await Promise.all([import(aiUrl), import(agentUrl)]);
    return { ai, agent };
  } catch (error) {
    fail(`could not import the built Pi packages: ${error.message}`);
  }
}

function makeBenchTool() {
  return {
    name: "bench_tool",
    label: "Bench Tool",
    description: "Deterministic in-process benchmark tool.",
    parameters: Type.Object({ n: Type.Number() }),
    async execute(_toolCallId, params) {
      return {
        content: [{ type: "text", text: `ok:${params.n}` }],
        details: { n: params.n },
      };
    },
  };
}

function sequentialResponses(ai, cycles) {
  const responses = [];
  for (let i = 0; i < cycles; i++) {
    responses.push(
      ai.fauxAssistantMessage(
        [ai.fauxToolCall("bench_tool", { n: i }, { id: `cycle-${i}` })],
        { stopReason: "toolUse" }
      )
    );
  }
  responses.push(
    ai.fauxAssistantMessage(ai.fauxText("done"), { stopReason: "stop" })
  );
  return responses;
}

function parallelResponses(ai, count) {
  const calls = Array.from({ length: count }, (_, i) =>
    ai.fauxToolCall("bench_tool", { n: i }, { id: `parallel-${i}` })
  );
  return [
    ai.fauxAssistantMessage(calls, { stopReason: "toolUse" }),
    ai.fauxAssistantMessage(ai.fauxText("done"), { stopReason: "stop" }),
  ];
}

function percentile(sorted, fraction) {
  if (sorted.length === 0) return null;
  const index = Math.max(
    0,
    Math.min(sorted.length - 1, Math.ceil(fraction * sorted.length) - 1)
  );
  return sorted[index];
}

function distribution(values) {
  values.sort((a, b) => a - b);
  return {
    medianMs: percentile(values, 0.5),
    p95Ms: percentile(values, 0.95),
    samples: values.length,
  };
}

async function runOnce(pi, responses, toolExecution) {
  const faux = pi.ai.fauxProvider({
    provider: "benchmark-faux",
    tokenSize: { min: 1_000_000, max: 1_000_000 },
  });
  faux.setResponses(responses);
  const models = pi.ai.createModels();
  models.setProvider(faux.provider);

  const providerStarts = [];
  const toolBatchEnds = [];
  const pendingToolCalls = new Set();
  const bot = new pi.agent.AgentHarness({
    models,
    session: new pi.agent.Session(new pi.agent.InMemorySessionStorage()),
    model: faux.getModel(),
    systemPrompt: "bench",
    thinkingLevel: "off",
    tools: [makeBenchTool()],
    activeToolNames: ["bench_tool"],
    toolExecution,
  });
  bot.on("before_provider_request", () => {
    providerStarts.push(performance.now());
    return undefined;
  });
  bot.subscribe((event) => {
    if (event.type === "tool_execution_start") {
      pendingToolCalls.add(event.toolCallId);
    } else if (event.type === "tool_execution_end") {
      pendingToolCalls.delete(event.toolCallId);
      if (pendingToolCalls.size === 0) toolBatchEnds.push(performance.now());
    }
  });

  const started = performance.now();
  await bot.prompt("go");
  const fullRunMs = performance.now() - started;
  await bot.shutdown();
  const gapsMs = toolBatchEnds
    .map((ended, index) => providerStarts[index + 1] - ended)
    .filter((value) => Number.isFinite(value) && value >= 0);
  return { fullRunMs, gapsMs };
}

async function benchmark(pi, responses, toolExecution, warmup, iterations) {
  for (let i = 0; i < warmup; i++)
    await runOnce(pi, responses(), toolExecution);

  const fullRuns = [];
  const gaps = [];
  for (let i = 0; i < iterations; i++) {
    const result = await runOnce(pi, responses(), toolExecution);
    fullRuns.push(result.fullRunMs);
    gaps.push(...result.gapsMs);
  }
  return {
    fullRun: distribution(fullRuns),
    toolEndToNextProviderStart: distribution(gaps),
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const pi = await loadPi(path.resolve(args.piCheckout));
  const results = [];

  if (args.workload === "all" || args.workload === "shallow") {
    const cycles =
      args.workload === "all"
        ? args.cycles.filter((value) => value <= 5)
        : args.cycles;
    for (const cycleCount of cycles) {
      results.push({
        runtime: "pi",
        workload: "shallow",
        cycles: cycleCount,
        ...(await benchmark(
          pi,
          () => sequentialResponses(pi.ai, cycleCount),
          "sequential",
          args.warmup,
          args.iterations
        )),
      });
    }
  }
  if (args.workload === "all" || args.workload === "deep") {
    const cycles =
      args.workload === "all"
        ? args.cycles.filter((value) => value > 5)
        : args.cycles;
    for (const cycleCount of cycles) {
      results.push({
        runtime: "pi",
        workload: "deep",
        cycles: cycleCount,
        ...(await benchmark(
          pi,
          () => sequentialResponses(pi.ai, cycleCount),
          "sequential",
          args.warmup,
          args.iterations
        )),
      });
    }
  }
  if (args.workload === "all" || args.workload === "parallel") {
    for (const toolsPerTurn of args.tools) {
      results.push({
        runtime: "pi",
        workload: "parallel",
        toolsPerTurn,
        ...(await benchmark(
          pi,
          () => parallelResponses(pi.ai, toolsPerTurn),
          "parallel",
          args.warmup,
          args.iterations
        )),
      });
    }
  }
  console.log(JSON.stringify(results, null, 2));
  // Same figure Clank's --benchmark reports: OS peak working set for this
  // process, so the two runtimes are compared on the metric a user feels
  // rather than on each runtime's own idea of "heap".
  console.log(
    JSON.stringify({ runtime: "pi", peakRssKb: process.resourceUsage().maxRSS })
  );
}

await main();
