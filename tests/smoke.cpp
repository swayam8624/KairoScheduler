import Kairo.Scheduler;

#include <atomic>
#include <cassert>
#include <cstddef>
#include <stdexcept>

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
    return 0;
}
