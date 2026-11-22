# KairoScheduler — from a task function to a safe parallel computation

This guide connects [`src/KairoScheduler.cppm`](../src/KairoScheduler.cppm)
to the tests [`tests/smoke.cpp`](../tests/smoke.cpp) and the
[benchmark](../benchmarks/benchmark.cpp). The important boundary:
a CPU thread pool coordinates work; it does **not** make arbitrary
shared data race-free, create GPU command queues or guarantee speedup.

## 1. The worker pool and its lifetime

`ThreadPool` constructs a fixed set of std::thread workers,
a synchronized task deque, a condition variable, pending-task counters
and statistics. A zero-configured worker count falls back to the CPU's
reported hardware concurrency (at least one).

`Submit` queues a nonempty function under the mutex and wakes one
worker; it refuses new jobs once shutdown begins. The destructor marks
the pool stopping, notifies all workers and joins them. The caller must
not destroy a pool while a TaskGroup borrowed from that pool has
outstanding callbacks.

Each raw worker callback catches exceptions; `WaitIdle` blocks until
queue/pending work is empty and transfers the first stored unhandled
raw task exception to the waiting thread.

## 2. TaskGroup: join and exception safety

A `TaskGroup` is a scoped collection of callbacks that share one
ThreadPool. `Submit` increments its outstanding count before queueing
the callback, and reverses the increment if allocation/queueing fails.
A worker records the first exception and decrements the counter even
after a callback fails. `Wait` stops further submissions, waits for
the counter to reach zero and rethrows that first exception.

```cpp
// Illustrative code: KairoScheduler's public module and namespaces.
import Kairo.Scheduler;
using namespace kairo::scheduler;

ThreadPool pool(4);
TaskGroup group(pool);
group.Submit([] { /* independent unit of work */ });
group.Submit([] { /* second independent unit */ });
group.Wait(); // join all submissions and propagate first exception
```

The key invariant is that `Wait` cannot hang because a task was
counted but never successfully queued.

## 3. Deterministic DAG waves

`TaskGraph::Add` takes an operation and dependencies on already
added handles. `Execute` builds indegrees and dependent edges,
collects zero-indegree nodes, and runs each *wave* in one TaskGroup.
Only when all callbacks in that wave finish does it release the next
wave. Ready nodes are sorted for stable scheduling order.

This gives dependency barriers and a clear cycle/invalid-dependency
failure policy. It is not a fully dynamic work-stealing DAG. Even
if tasks are *submitted* in a consistent order, their completion
order within one wave depends on thread scheduling.

**Exercise:** create tasks A and B with no dependencies, C depending
on both. Record start/end observations and verify C never starts
before both complete, but avoid asserting that A finishes before B.

## 4. Cooperative cancellation

`CancellationSource::RequestStop` uses an atomic flag;
`CancellationToken::StopRequested` reads it. This is a cooperative
signal, not preemption. `TaskGraph` checks cancellation before
starting another wave and worker callbacks may skip themselves if
the signal is set.

Long-running callbacks that never check the token may still run to
completion. Destruction and shutdown still require the worker
lifetime to be joined safely.

**Exercise:** request cancellation while one task is active; verify
later wave behavior and distinguish operations that were never
started from callbacks that had already begun.

## 5. ParallelFor and shared-memory responsibility

`ParallelFor` partitions an index range into disjoint chunks,
falling back to sequential execution for small jobs when appropriate.
One worker may process several chunks. A callback must not write
to memory used concurrently by another chunk without synchronization.

For a pure elementwise tensor operation, output ranges can be
partitioned cleanly. For a reduction, using unsynchronized increments
into one shared accumulator is a race. Use per-chunk partial sums
and a defined combine phase; floating-point combination order
matters if deterministic bitwise results are required.

## 6. Layer interaction and diagnostics

KairoMath's optional TensorExecution module uses KairoScheduler
for selected CPU parallel operations. The scheduler knows nothing
about tensor shape, dtype, aliasing, or a GPU's native command queue;
the caller must establish those constraints before dispatch.

`SchedulerStats` counts workers/submissions/completions/queue activity
for observability. It does not by itself establish speedup. Benchmark
with the same workload, thread counts, input sizes and warmup, comparing
wall-clock latency, CPU contention, memory allocation and numerical
results against sequential reference.

**Evidence status:** source-based documentation. No compiler, concurrency
sanitizer or performance test was executed as part of this pass.
