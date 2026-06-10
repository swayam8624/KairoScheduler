module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

export module Kairo.Scheduler;

export namespace kairo::scheduler
{
    /// Execution policy used by parallel range helpers.
    enum class ExecutionPolicy
    {
        Sequential,
        Parallel
    };

    /// Inclusive-exclusive range chunk passed to worker callbacks.
    struct Range final
    {
        std::size_t begin = 0;
        std::size_t end = 0;

        [[nodiscard]]
        std::size_t Size() const noexcept
        {
            return end - begin;
        }
    };

    struct SchedulerConfig final
    {
        // Zero means "use hardware_concurrency fallback".
        std::size_t workerCount = 0;
        std::size_t defaultMinChunkSize = 1024;
        bool spinBeforeSleep = false;
    };

    struct SchedulerStats final
    {
        std::size_t workerCount = 0;
        std::size_t submittedTasks = 0;
        std::size_t completedTasks = 0;
        std::size_t pendingTasks = 0;
    };

    [[nodiscard]]
    inline std::vector<Range> PartitionRange(
        std::size_t count,
        std::size_t minChunkSize,
        std::size_t maxChunks)
    {
        std::vector<Range> ranges;
        if (count == 0)
        {
            return ranges;
        }

        minChunkSize = std::max<std::size_t>(minChunkSize, 1);
        maxChunks = std::max<std::size_t>(maxChunks, 1);

        const std::size_t requestedChunks = (count + minChunkSize - 1) / minChunkSize;
        const std::size_t chunkCount = std::min(maxChunks, requestedChunks);
        const std::size_t chunkSize = (count + chunkCount - 1) / chunkCount;

        ranges.reserve(chunkCount);
        for (std::size_t begin = 0; begin < count; begin += chunkSize)
        {
            ranges.push_back({ begin, std::min(begin + chunkSize, count) });
        }
        return ranges;
    }

    /// A compact fixed-size thread pool for CPU tensor kernels.
    ///
    /// Input: tasks submitted through `Submit` or `ParallelFor`.
    /// Output: tasks run on worker threads and synchronized by `WaitIdle`.
    /// Task: provide a reusable scheduling substrate without leaking thread
    /// management into ML operators.
    class ThreadPool final
    {
    public:
        explicit ThreadPool(std::size_t workerCount = DefaultWorkerCount())
            : ThreadPool(SchedulerConfig{ .workerCount = workerCount })
        {
        }

        explicit ThreadPool(SchedulerConfig config)
            : m_config(config)
        {
            std::size_t workerCount = config.workerCount == 0 ? DefaultWorkerCount() : config.workerCount;
            workerCount = std::max<std::size_t>(workerCount, 1);
            m_workers.reserve(workerCount);
            for (std::size_t i = 0; i < workerCount; ++i)
            {
                m_workers.emplace_back([this] { WorkerLoop(); });
            }
        }

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        ~ThreadPool()
        {
            {
                std::scoped_lock lock(m_mutex);
                m_stopping = true;
            }
            m_cv.notify_all();
            for (std::thread& worker : m_workers)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
        }

        [[nodiscard]]
        static std::size_t DefaultWorkerCount() noexcept
        {
            const unsigned count = std::thread::hardware_concurrency();
            return count == 0 ? 1 : static_cast<std::size_t>(count);
        }

        [[nodiscard]]
        std::size_t WorkerCount() const noexcept
        {
            return m_workers.size();
        }

        void Submit(std::function<void()> task)
        {
            if (!task)
            {
                return;
            }
            {
                std::scoped_lock lock(m_mutex);
                if (m_stopping)
                {
                    throw std::logic_error("ThreadPool::Submit called after shutdown.");
                }
                m_tasks.push_back(std::move(task));
                ++m_pending;
                ++m_submitted;
            }
            m_cv.notify_one();
        }

        void WaitIdle()
        {
            std::unique_lock lock(m_mutex);
            m_idleCv.wait(lock, [this]
            {
                return m_pending == 0 && m_tasks.empty();
            });
        }

        [[nodiscard]]
        SchedulerStats Stats() const
        {
            std::scoped_lock lock(m_mutex);
            return {
                .workerCount = m_workers.size(),
                .submittedTasks = m_submitted,
                .completedTasks = m_completed,
                .pendingTasks = m_pending
            };
        }

        [[nodiscard]]
        const SchedulerConfig& Config() const noexcept
        {
            return m_config;
        }

    private:
        SchedulerConfig m_config;
        std::vector<std::thread> m_workers;
        std::deque<std::function<void()>> m_tasks;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::condition_variable m_idleCv;
        std::size_t m_pending = 0;
        std::size_t m_submitted = 0;
        std::size_t m_completed = 0;
        bool m_stopping = false;

        void WorkerLoop()
        {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, [this]
                    {
                        return m_stopping || !m_tasks.empty();
                    });

                    if (m_stopping && m_tasks.empty())
                    {
                        return;
                    }

                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                }

                task();

                {
                    std::scoped_lock lock(m_mutex);
                    --m_pending;
                    ++m_completed;
                    if (m_pending == 0 && m_tasks.empty())
                    {
                        m_idleCv.notify_all();
                    }
                }
            }
        }
    };

    /// Input: item count, minimum chunk size, callback accepting Range.
    /// Output: callback invoked across all items.
    /// Task: central range splitting primitive for tensor kernels.
    template<typename Fn>
    void ParallelFor(
        ThreadPool& pool,
        std::size_t count,
        std::size_t minChunkSize,
        Fn&& fn,
        ExecutionPolicy policy = ExecutionPolicy::Parallel)
    {
        if (count == 0)
        {
            return;
        }

        minChunkSize = std::max<std::size_t>(minChunkSize, 1);
        if (policy == ExecutionPolicy::Sequential || pool.WorkerCount() <= 1 || count <= minChunkSize)
        {
            fn(Range{ 0, count });
            return;
        }

        const std::vector<Range> ranges = PartitionRange(count, minChunkSize, pool.WorkerCount() * 4);
        for (Range range : ranges)
        {
            pool.Submit([range, &fn]
            {
                fn(range);
            });
        }
        pool.WaitIdle();
    }

    template<typename Fn>
    void SerialFor(std::size_t count, Fn&& fn)
    {
        if (count == 0)
        {
            return;
        }
        fn(Range{ 0, count });
    }

    /// A higher-level scheduler facade that owns a pool and applies config
    /// defaults. Tensor and dataloader code should depend on this facade rather
    /// than directly constructing pools everywhere.
    class Scheduler final
    {
    public:
        explicit Scheduler(SchedulerConfig config = {})
            : m_pool(config)
        {
        }

        template<typename Fn>
        void ParallelFor(std::size_t count, Fn&& fn)
        {
            kairo::scheduler::ParallelFor(
                m_pool,
                count,
                m_pool.Config().defaultMinChunkSize,
                std::forward<Fn>(fn),
                ExecutionPolicy::Parallel);
        }

        template<typename Fn>
        void For(std::size_t count, Fn&& fn, ExecutionPolicy policy)
        {
            kairo::scheduler::ParallelFor(
                m_pool,
                count,
                m_pool.Config().defaultMinChunkSize,
                std::forward<Fn>(fn),
                policy);
        }

        [[nodiscard]]
        SchedulerStats Stats() const
        {
            return m_pool.Stats();
        }

    private:
        ThreadPool m_pool;
    };
}
