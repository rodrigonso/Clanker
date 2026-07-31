# Pinned Pi agent-loop benchmark

This fixture measures Pi's exported durable `AgentHarness` with
`InMemorySessionStorage`, its faux provider, and an in-process no-op tool. It
does not copy Pi source into Clank and performs no network access during
measured runs.

The fixture pins `earendil-works/pi` to
`74caa2649f10ed71b4378ce69f5d9fbfd2466ca5`, the reference used for Clank's
latency-parity work.

## Setup

From this directory in PowerShell:

```powershell
git clone https://github.com/earendil-works/pi.git .pi-checkout
git -C .pi-checkout checkout 74caa2649f10ed71b4378ce69f5d9fbfd2466ca5
npm --prefix .pi-checkout ci
npm --prefix .pi-checkout run build:offline --workspace=@earendil-works/pi-ai
npm --prefix .pi-checkout run build --workspace=@earendil-works/pi-agent-core
npm install
```

Setup requires network access. The package builds and all benchmark runs after
setup are local.

## Run

```powershell
node run-bench.mjs --workload shallow --cycles 5
node run-bench.mjs --workload deep --cycles 20,50,100
node run-bench.mjs --workload parallel --tools 1,2,4,8,16
node run-bench.mjs --workload all --warmup 20 --iterations 100
```

Output is JSON containing median and p95 milliseconds for:

- the complete `Agent.prompt()` run; and
- the last tool completion in a batch to the next faux-provider invocation.

Pi's faux provider traverses an asynchronous event stream while Clank's faux
provider invokes synchronous callbacks. Absolute cross-runtime times are
therefore directional; compare scaling across cycle depth and batch width, and
use the same warmup/iteration counts for both runtimes.

Run the matching native Clank workload from the repository root with a Release
build:

```powershell
.\build\clank_tests.exe --benchmark --workload shallow --cycles 5
.\build\clank_tests.exe --benchmark --workload deep --cycles 20,50,100
.\build\clank_tests.exe --benchmark --workload parallel --tools 1,2,4,8,16
.\build\clank_tests.exe --benchmark --workload all --warmup 20 --iterations 100
```

The Clank command emits the same JSON fields as `run-bench.mjs`.
