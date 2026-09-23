#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>

import Kairo.Scheduler;

int main()
{
    using namespace kairo::scheduler;

    ThreadPool pool;
    constexpr std::size_t itemCount = 1u << 20u;
    constexpr std::size_t iterations = 64u;
    std::atomic<std::uint64_t> checksum = 0u;

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
    {
        ParallelFor(pool, itemCount, 4096u,
            [&checksum](Range range)
            {
                std::uint64_t local = 0u;
                for (std::size_t index = range.begin; index < range.end; ++index)
                    local += static_cast<std::uint64_t>((index * 2654435761u) ^ (index >> 3u));
                checksum.fetch_add(local, std::memory_order_relaxed);
            });
    }
    const auto stop = std::chrono::steady_clock::now();
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        stop - start).count();
    const SchedulerStats stats = pool.Stats();

    std::cout
        << "{\"schema\":\"kairo.scheduler.benchmark.v1\","
        << "\"workers\":" << stats.workerCount << ","
        << "\"items\":" << itemCount << ","
        << "\"iterations\":" << iterations << ","
        << "\"submitted_tasks\":" << stats.submittedTasks << ","
        << "\"peak_active_workers\":" << stats.peakActiveWorkers << ","
        << "\"elapsed_ns\":" << elapsedNs << ","
        << "\"total_task_ns\":" << stats.totalTaskNanoseconds << ","
        << "\"max_task_ns\":" << stats.maxTaskNanoseconds << ","
        << "\"checksum\":" << checksum.load(std::memory_order_relaxed)
        << "}\n";
}
