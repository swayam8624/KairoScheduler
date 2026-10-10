// CI builds Release binaries (NDEBUG), but this smoke executable must still
// evaluate every regression assertion and the guarded side effects.
#if defined(NDEBUG)
#undef NDEBUG
#endif

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

import Kairo.Scheduler;

namespace
{
    void TestProfilingAndCancellableRanges();
}

int main()
{
    kairo::scheduler::ThreadPool pool(2);
    std::atomic<std::size_t> sum = 0;
    kairo::scheduler::ParallelFor(pool, 100, 8, [&](kairo::scheduler::Range range)
    {
        for (std::size_t i = range.begin; i < range.end; ++i)
        {
            sum += i;
        }
    });
    assert(sum == 4950);
    assert(pool.Stats().completedTasks > 0);

    auto ranges = kairo::scheduler::PartitionRange(100, 10, 4);
    assert(!ranges.empty());
    assert(ranges.front().begin == 0);
    assert(ranges.back().end == 100);

    // Arithmetic is independent of available RAM: no SIZE_MAX buffer is
    // allocated. This reproduces the previous ceil-division overflow.
    const auto huge = std::numeric_limits<std::size_t>::max();
    const auto oneHugeRange = kairo::scheduler::PartitionRange(huge, huge, 4);
    assert(oneHugeRange.size() == 1u);
    assert(oneHugeRange.front().begin == 0u);
    assert(oneHugeRange.front().end == huge);
    const auto hugeSplit = kairo::scheduler::PartitionRange(huge, 1u, 2u);
    assert(hugeSplit.size() == 2u);
    assert(hugeSplit.front().begin == 0u);
    assert(hugeSplit.front().end == hugeSplit.back().begin);
    assert(hugeSplit.back().end == huge);
    assert(hugeSplit.front().Size() + hugeSplit.back().Size() == huge);

    kairo::scheduler::Scheduler scheduler({ .workerCount = 2, .defaultMinChunkSize = 4 });
    std::atomic<std::size_t> count = 0;
    scheduler.ParallelFor(32, [&](kairo::scheduler::Range range)
    {
        count += range.Size();
    });
    assert(count == 32);

    bool exceptionPropagated = false;
    try
    {
        kairo::scheduler::ParallelFor(pool, 32, 4, [](kairo::scheduler::Range)
        {
            throw std::runtime_error("expected task failure");
        });
    }
    catch (const std::runtime_error&)
    {
        exceptionPropagated = true;
    }
    assert(exceptionPropagated);
    assert(pool.Stats().pendingTasks == 0);

    // If capturing a submitted task throws, TaskGroup must roll back the
    // pending count. Wait() must return rather than waiting for a task that
    // was never queued.
    struct ThrowOnCopy final
    {
        ThrowOnCopy() = default;
        ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("copy denied"); }
        void operator()() const {}
    };
    kairo::scheduler::TaskGroup group(pool);
    ThrowOnCopy uncopyable;
    bool copyFailed = false;
    try
    {
        group.Submit(uncopyable);
    }
    catch (const std::runtime_error&)
    {
        copyFailed = true;
    }
    assert(copyFailed);
    group.Wait();

    pool.Submit([] { throw std::runtime_error("raw task failure"); });
    bool rawExceptionPropagated = false;
    try
    {
        pool.WaitIdle();
    }
    catch (const std::runtime_error&)
    {
        rawExceptionPropagated = true;
    }
    assert(rawExceptionPropagated);

    std::vector<int> execution;
    std::mutex executionMutex;
    kairo::scheduler::TaskGraph graph;
    const auto loadA = graph.Add([&]
    {
        std::scoped_lock lock(executionMutex);
        execution.push_back(1);
    });
    const auto loadB = graph.Add([&]
    {
        std::scoped_lock lock(executionMutex);
        execution.push_back(2);
    });
    const kairo::scheduler::TaskHandle inputs[] = { loadA, loadB };
    const auto combine = graph.Add([&]
    {
        std::scoped_lock lock(executionMutex);
        assert(execution.size() == 2);
        execution.push_back(3);
    }, inputs);
    const auto finalize = graph.Add([&]
    {
        std::scoped_lock lock(executionMutex);
        assert(execution.size() == 3 && execution.back() == 3);
        execution.push_back(4);
    }, std::span<const kairo::scheduler::TaskHandle>(&combine, 1));
    assert(finalize.index == 3);
    graph.Execute(pool);
    assert(execution.size() == 4 && execution.back() == 4);

    kairo::scheduler::CancellationSource cancellation;
    cancellation.RequestStop();
    kairo::scheduler::TaskGraph cancelled;
    bool ran = false;
    const auto cancelledTask = cancelled.Add([&] { ran = true; });
    assert(cancelledTask.index == 0);
    cancelled.Execute(pool, cancellation.Token());
    assert(!ran);

    TestProfilingAndCancellableRanges();
    return 0;
}


namespace
{
    void TestProfilingAndCancellableRanges()
    {
        kairo::scheduler::ThreadPool pool(4);
        std::atomic<std::size_t> visited = 0u;
        kairo::scheduler::ParallelFor(pool, 4096u, 64u,
            [&visited](kairo::scheduler::Range range)
            {
                for (std::size_t index = range.begin; index < range.end; ++index)
                    visited.fetch_add(1u, std::memory_order_relaxed);
            });
        assert(visited == 4096u);

        const auto stats = pool.Stats();
        assert(stats.completedTasks > 0u);
        assert(stats.totalTaskNanoseconds > 0u);
        assert(stats.maxTaskNanoseconds > 0u);
        assert(stats.peakActiveWorkers >= 1u);
        assert(stats.peakActiveWorkers <= stats.workerCount);
        assert(stats.AverageTaskNanoseconds() > 0.0);

        kairo::scheduler::CancellationSource cancellation;
        cancellation.RequestStop();
        std::atomic<std::size_t> cancelledVisits = 0u;
        kairo::scheduler::ParallelForCancellable(
            pool, 4096u, 64u, cancellation.Token(),
            [&cancelledVisits](kairo::scheduler::Range range)
            {
                cancelledVisits.fetch_add(range.Size(), std::memory_order_relaxed);
            });
        assert(cancelledVisits == 0u);
    }
}

