# KairoScheduler

KairoScheduler is the deliberate multi-threaded scheduling foundation for the
Kairo ML stack.

Phase 1 scope:

- thread-pool worker runtime,
- `ParallelFor` range scheduling,
- deterministic scalar fallback,
- small API surface suitable for tensor kernels.

Later phases should add work stealing, task graphs, NUMA-aware scheduling, and
profiling hooks.

Build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build
./build/KairoSchedulerSmoke
```
