# KairoScheduler — source reading guide

This **teaching order** follows data/control flow, not compilation order. Read [README](../README.md), [CMakeLists.txt](../CMakeLists.txt) and the applicable test target before trusting a performance claim or assuming a backend is enabled.

1. [src/KairoScheduler.cppm](../src/KairoScheduler.cppm) — inspect exported declarations, contracts, call paths and tests.
2. [tests/smoke.cpp](../tests/smoke.cpp) — inspect exported declarations, contracts, call paths and tests.
3. [benchmarks/benchmark.cpp](../benchmarks/benchmark.cpp) — inspect exported declarations, contracts, call paths and tests.

## How this repository interacts with Kairo

Tensor/data workload → Scheduler/TaskGroup/ParallelFor or TaskGraph → ThreadPool workers → join/cancel/error handling and stats. Verify lifetimes of captured data and task dependencies.

## Study method

Read the exported C++ module/API, then walk the implementation and its tests. For each function, record its inputs, return type, ownership, error handling, preconditions, numerical assumptions and downstream consumer. Treat configuration/feature flags as constraints on what code is actually compiled.

## Practical exercises

1. Draw the data flow for one smallest supported operation and link every participating module.
2. Find one invalid input and explain whether the API throws, returns an error, or supplies a fallback.
3. Run correctness tests before performance benchmarks; retain compiler/platform/configuration.
4. Locate optional backend paths and distinguish stub/unsupported modes from executed implementations.
5. Identify a dependency ownership boundary and explain when referenced buffers/objects may be destroyed.

## License

The proposed [MIT license](../LICENSE) applies to original project-owned source/documentation. It does not convey rights over external dependencies, datasets, assets or imported code; preserve applicable notices and verify provenance before redistribution.
