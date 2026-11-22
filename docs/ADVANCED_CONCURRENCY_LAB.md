# Advanced laboratory: reasoning about a C++23 task scheduler

This lesson extends the [source map](SOURCE_READING_ORDER.md) and the [introductory walkthrough](TASK_SCHEDULING_WALKTHROUGH.md). Read the actual [module interface and implementation](../src/KairoScheduler.cppm) alongside it, then check [smoke tests](../tests/smoke.cpp) and [benchmark](../benchmarks/benchmark.cpp).

**Status:** source-level teaching analysis; the examples below are exercises, not claims that the test suite has executed. The scheduler owns CPU threads and queueing, not tensor shape validation, device synchronization, GPU command submission, or automatic thread safety.

## 1. Follow one task through the entire call graph

`Scheduler::ParallelFor(count, fn)` delegates to the free `ParallelFor(pool, count, config.defaultMinChunkSize, fn, Parallel)`. That function either invokes the callback once with `Range{0,count}` or calls `PartitionRange`, constructs a stack-owned `TaskGroup`, and submits each chunk. `TaskGroup::Submit` reserves a group completion slot **before** invoking `ThreadPool::Submit`. The pool takes the queue mutex, stores a `std::function<void()>`, increments the pending/submitted counters, unlocks, and wakes a worker.

The worker dequeues under the mutex, releases the lock, executes the callback, and records duration and completion under the mutex. Its enclosing `TaskGroup` wrapper catches a callback exception, saves the first exception, and decrements the group's outstanding count. `TaskGroup::Wait` waits for that count to become zero and rethrows the exception on the caller's thread. This distinguishes a **group exception** from the pool's separate first **raw-submission exception**, surfaced by `ThreadPool::WaitIdle`.

Why increment first? If a worker finished before the caller recorded the reservation, it could decrement a zero counter. Why roll back the reservation on failed submission? Otherwise `Wait` might wait forever for a callback that was never queued.

## 2. Ownership and happens-before: the real safety argument

`ThreadPool` owns `m_workers`, the deque, condition variables, mutex, counters, and the raw-callback error slot. `TaskGroup` **borrows** a `ThreadPool&` and the callback submitted by the group captures its owning group by `this`. The group must therefore outlive every submitted callback: call `Wait` before the group is destroyed or any referenced local data goes out of scope. Likewise, keep the pool alive until groups are joined.

`std::condition_variable::wait(lock, predicate)` releases the mutex while sleeping and rechecks the predicate under the lock after waking. The same mutex protects the producer's counter updates and the consumer's completion updates. `notify_one` signals queued work; `notify_all` during teardown wakes idle workers; `m_idleCv` wakes callers waiting for **global** pool idleness. A worker only holds the queue mutex while manipulating shared bookkeeping, **not** while running user code. Otherwise independent callbacks would serialize and callbacks submitting additional work might deadlock immediately.

The source currently permits multiple threads to call `TaskGroup::Submit` while submission is open, but a concurrent `Wait` closes the submission gate. Treat submission and the start of waiting as an explicit phase boundary, not as a general dynamic task registry.

## 3. Data races: disjoint ranges are necessary, not sufficient

For `count = 10`, `minChunkSize = 3`, `maxChunks = 4`, `PartitionRange` computes four chunks with a chunk width of three: `[0,3), [3,6), [6,9), [9,10)`. The half-open endpoints cover each index once. This says nothing about the callback's *other* memory accesses.

```cpp
import Kairo.Scheduler;
#include <vector>
using namespace kairo::scheduler;

// Each callback owns different elements. No shared accumulator is mutated.
ThreadPool pool(4);
std::vector<float> input(1024, 2.0f);
std::vector<float> output(input.size());

ParallelFor(pool, input.size(), 128, [&](Range range) {
    for (std::size_t i = range.begin; i < range.end; ++i) {
        output[i] = input[i] * input[i];
    }
});
```

A lambda doing `sum += input[i]` is **not** safe when multiple chunks share `sum`. Use per-chunk storage, atomics (when semantically appropriate), or a separate reduction/combine stage. Floating-point reduction may differ from serial output because addition is not associative in finite precision. The caller also owns aliasing rules: two disjoint index intervals may still touch the same cache line (false sharing), and output views may alias input or other outputs.

## 4. Why nested blocking scheduling can deadlock

Imagine **every** pool worker is currently executing an outer task. Each outer task calls `ParallelFor` using the *same* pool and then blocks in its inner `TaskGroup::Wait`. The inner callbacks are queued, but no worker remains to execute them. The pool has no special worker-assisted waiting or documented reentrant scheduling behavior. Avoid synchronously waiting for nested tasks on the same saturated pool.

Alternatives for the student to evaluate: execute inner loops sequentially, flatten the work into one outer partitioning operation, or introduce a genuinely nonblocking task-dependency system. Do **not** solve this by indiscriminately increasing worker count: a deeper nesting level can exhaust any finite pool.

## 5. Cancellation has a precise, limited meaning

`CancellationSource` shares an atomic stop flag with copied `CancellationToken` instances. A release store in `RequestStop` and acquire load in `StopRequested` provide synchronized flag visibility when the stop is observed; they do not terminate threads.

`ParallelForCancellable` may queue chunks even if cancellation arrives before they execute. Every chunk checks the token before invoking its callback. **Once the callback has started, it runs unless its own implementation cooperatively checks cancellation.**

`TaskGraph::Execute` constructs indegrees, groups the ready nodes into a wave, waits for that wave, and only then releases dependents. It checks cancellation before scheduling the next wave. A task in a submitted wave may be skipped by the wrapper if cancellation was already requested. The implementation increments its local wave-completion count after the group joins, even if some callbacks skipped. Because the executor returns when the token is set, treat cancellation as **early termination**, not as evidence that all registered tasks ran.

## 6. Determinism is not a single property

There are distinct promises:

- **Partition determinism:** the same `count`, chunk limits, and worker-count arguments produce the same ranges.
- **Dependency determinism:** the next DAG wave is released only after all prior-wave callbacks finish, and the ready-index list is sorted.
- **Execution-order determinism:** *not guaranteed* among ready tasks running on different workers.
- **Result determinism:** only possible when the callbacks, memory accesses, reductions, input data and external effects are designed for it.

A reproducible test should assert dependency ordering and final results, **not** assert which independent worker prints first.

## 7. Exception paths and failure boundaries

If the user callback throws inside a `TaskGroup`, the wrapper catches it so worker threads remain alive; `Wait` rethrows the saved first error after all group callbacks finish. A raw task submitted directly to `ThreadPool` is handled instead by the worker's catch block and `WaitIdle`. The exception slot intentionally stores only one pending raw exception.

A failed queue submission is different from callback failure: `TaskGroup::Submit` reverses its outstanding reservation and rethrows on the producer thread. `TaskGraph::Execute` stops before advancing to the next wave when its group's `Wait` throws.

**Important source-level limitation:** this task group has no destructor that automatically waits. Never substitute scope exit for an explicit join, especially during exception unwinding. A caller whose submission loop throws must still ensure that already-queued group callbacks complete before the group and any captured local references are destroyed. Similarly, `ThreadPool::Submit` uses dynamically allocated type-erased `std::function` storage; allocation failure is a possible submission error.

## 8. Cross-repository boundaries

`KairoMath` decides tensor length, aliasing and correct numerical operations **before** requesting CPU ranges from this module. `KairoSIMD` concerns instructions **within** a chunk; SIMD does not replace the thread scheduler. `MLLibrary` can organize training/evaluation work with this scheduling vocabulary. GPU execution remains managed by device APIs and their queues; using this pool for host preparation does not imply GPU completion.

A library integrating `KairoScheduler` should state whether its callbacks are pure, which ranges own writable memory, how exceptions reach the user, which owner keeps the pool alive, and how cancellation is checked.

## 9. Exercises and evidence ledger

1. **Exact coverage:** for varied counts including zero, one, nonmultiples and chunk-size limits, mark each visited element and assert a count of exactly one. Prefer a unique output slot per index rather than racing on a shared counter.
2. **Exception delivery:** make one grouped task throw and verify `TaskGroup::Wait` receives it. Separately submit a raw throwing task and verify `ThreadPool::WaitIdle` receives it.
3. **DAG barrier:** create A and B independently, C depending on both; assert C never observes either incomplete, while allowing A and B to complete in either order.
4. **Cancellation:** use a controlled gate to request cancellation at a wave boundary and verify downstream side effects do not occur. Avoid timing-based sleeps as the only synchronization.
5. **Worker saturation:** design a **bounded, watchdog-protected external experiment** to illustrate nested-blocking deadlock; do not put an unbounded hanging test in ordinary CI.
6. **Benchmark:** compare against a serial baseline using identical data and correctness checks. Report worker count, chunk size, warmup, repeated wall times, variance and CPU utilization. The existing `SchedulerStats` cannot alone prove speedup.

Before asserting these behaviors as verified on a particular platform, run the documented CMake/CTest build and capture the compiler, host, commit and exact test results.
