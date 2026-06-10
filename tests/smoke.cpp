import Kairo.Scheduler;

#include <atomic>
#include <cassert>
#include <cstddef>

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
    return 0;
}
