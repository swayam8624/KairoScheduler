# KairoScheduler

KairoScheduler is the CPU execution substrate for the Kairo ML stack. Its job is
to make tensor kernels, dataloaders, graph executors, and visual-analysis jobs
parallel without each subsystem inventing its own thread management.

## Problem

ML runtimes fail quickly when parallelism is bolted onto individual kernels:

- matrix multiply wants chunked row/column work,
- dataloaders want background decode and normalization,
- graph execution wants dependency-aware tasks,
- profiling wants consistent task statistics,
- debugging wants deterministic sequential fallback.

If every layer manages threads directly, the runtime becomes hard to profile,
hard to cancel, and easy to oversubscribe.

## Solution

KairoScheduler provides one scheduling vocabulary:

- `ThreadPool`: fixed worker pool with submit/wait semantics.
- `TaskGroup`: scoped task completion and worker-exception propagation.
- `ParallelFor`: range splitting for tensor kernels.
- `PartitionRange`: deterministic chunk planning.
- `Scheduler`: owned facade with reusable defaults.
- `SchedulerStats`: visibility into submitted, completed, and pending work.
- `ExecutionPolicy`: switch between sequential and parallel execution.
- `CancellationSource`/`CancellationToken`: shared cooperative stop state.
- `TaskGraph`: dependency-validated tasks executed in deterministic parallel
  topological waves.

This keeps higher-level packages focused on their domain:

- `KairoMath::Tensor` uses it for elementwise kernels, reductions, matmul, and
  convolution.
- `KairoSIMD` can be used inside each scheduled range.
- `MLLibrary` uses it for batched training and evaluation.
- Future `KairoGPU` paths can use it for CPU-side command preparation and
  readback work.

## Current Status

Implemented now:

- C++23 module package: `import Kairo.Scheduler;`
- fixed-size thread pool,
- range partitioning,
- parallel and sequential execution policies,
- scheduler facade,
- worker exceptions are rethrown by `TaskGroup::Wait` or `ThreadPool::WaitIdle`,
- dependency-aware task graphs with cancellation between execution waves,
- cooperative cancellable parallel ranges,
- worker utilization plus total/max/average task timing statistics,
- deterministic machine-readable benchmark output,
- smoke coverage for cancellation, exception propagation and timing.

Post-v1 experiments:

- true per-worker work stealing when benchmarks justify it,
- affinity/NUMA hints on platforms where they are measurable.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build
ctest --test-dir build --output-on-failure
./build/KairoSchedulerSmoke
```

## Enterprise Role

This repo is deliberately separate because scheduling is infrastructure, not a
math helper. It needs independent testing, profiling, and versioning. Once the
ML library grows to datasets, inference services, GPU command generation, and
visual dashboards, a shared scheduler prevents hidden thread pools from fighting
each other.


## Benchmark And Telemetry

`SchedulerStats` reports submitted/completed/pending tasks, active and peak
worker counts, total task nanoseconds and maximum task nanoseconds. These are
host-observed CPU timings intended for regressions and higher-level engine
profiling.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DKAIRO_SCHEDULER_BUILD_BENCHMARK=ON
cmake --build build --target KairoSchedulerBenchmark
./build/KairoSchedulerBenchmark
```

The executable emits `kairo.scheduler.benchmark.v1` JSON. KairoMath already
has an opt-in Scheduler/SIMD Tensor execution boundary; Scheduler itself stays
independent of Tensor types.

See [STATUS.md](STATUS.md) for the frozen Wave-B v1 scope.
