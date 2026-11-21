# KairoScheduler: source reading order

Read the C++23 scheduler module and task-group ownership, then smoke and performance tests. Other Kairo runtime and ML packages can use it for job execution but should not import renderer or scene code into scheduling primitives.

## Key APIs and interactions

Study task lifetime, exception propagation, cancellation, queue fairness, work partitioning, race freedom and deterministic tests; `ParallelFor` is a range splitter, not an automatic solution to data races.

## Linked source files (3)

1. [`src/KairoScheduler.cppm`](src/KairoScheduler.cppm) — read public contracts, lifetime ownership, imports/callers, error behavior and associated tests.
2. [`tests/smoke.cpp`](tests/smoke.cpp) — read public contracts, lifetime ownership, imports/callers, error behavior and associated tests.
3. [`benchmarks/benchmark.cpp`](benchmarks/benchmark.cpp) — read public contracts, lifetime ownership, imports/callers, error behavior and associated tests.

For setup consult [README.md](README.md), and for downstream consumers inspect [KairoGameEngine](https://github.com/swayam8624/KairoGameEngine). Original code is [MIT licensed](LICENSE) for study, modification and further development; framework binaries/SDKs retain their original licenses. Function-level commentary and full native tests are separate ongoing verification tasks.
