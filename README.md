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
- `ParallelFor`: range splitting for tensor kernels.
- `PartitionRange`: deterministic chunk planning.
- `Scheduler`: owned facade with reusable defaults.
- `SchedulerStats`: visibility into submitted, completed, and pending work.
- `ExecutionPolicy`: switch between sequential and parallel execution.

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
- smoke test.

Planned next:

- cancellation tokens,
- task groups,
- dependency DAG scheduler,
- work stealing,
- affinity/NUMA hints,
- profiling timestamps,
- integration into `Kairo.Foundation.Math.Tensor`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build
./build/KairoSchedulerSmoke
```

## Enterprise Role

This repo is deliberately separate because scheduling is infrastructure, not a
math helper. It needs independent testing, profiling, and versioning. Once the
ML library grows to datasets, inference services, GPU command generation, and
visual dashboards, a shared scheduler prevents hidden thread pools from fighting
each other.
