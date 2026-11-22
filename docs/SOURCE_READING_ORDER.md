# Teaching source map — KairoScheduler

CPU task execution and deterministic bounded parallel work contracts.

**Start with the worked guide:** [CPU worker pools, task-group errors, dependency waves and cancellation](TASK_SCHEDULING_WALKTHROUGH.md).

Follow source links sequentially. For each API/function, identify its input contract, owning data structure, called modules, failure modes, outputs, test and downstream consumers.

## Read in this order

1. [`CMakeLists.txt`](../CMakeLists.txt)
2. [`README.md`](../README.md)
3. [`src`](../src)
4. [`tests`](../tests)
5. [`benchmarks`](../benchmarks)
6. [`research`](../research)

## How this repository interacts with others

MLLibrary and Kairo compute libraries can schedule independent tasks here. Study cancellation, exception propagation, task lifetime and worker coordination before optimizing throughput.

## Verification and study rules

- Read the public C++23 module/entrypoint first, then its implementation and tests.
- Explain invariants, ownership, error states, concurrency or numerical assumptions in comments, not trivial syntax.
- A passing CI job covers only tests actually run on that commit/host; source presence is not runtime certification.
- Check [STATUS.md](../STATUS.md) and [STATUS.yaml](../STATUS.yaml) for current explicit verification limitations.
- Original work may be modified and extended under the repository license; third-party content retains upstream terms.


## Complete source-file inventory

After the curated teaching path, follow this deterministic inventory of first-party source, scripts, shaders, and tests. Grouping is a suggested learning order, not an indication that every function is already annotated. Vendored dependencies, ROMs, binaries, data, media, and generated evidence are excluded.

### Entry points and build (1)

1. [`CMakeLists.txt`](../CMakeLists.txt)

### Interfaces and modules (1)

1. [`src/KairoScheduler.cppm`](../src/KairoScheduler.cppm)

### Research and benchmarks (1)

1. [`benchmarks/benchmark.cpp`](../benchmarks/benchmark.cpp)

### Verification and tests (1)

1. [`tests/smoke.cpp`](../tests/smoke.cpp)

Snapshot totals: 4 first-party source/build/test files, 12 tracked files overall. Re-audit when files are renamed or added.
