#include <atomic>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

import Kairo.Scheduler;

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
    return 0;
}
