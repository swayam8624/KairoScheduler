module;

#include <algorithm>
#include <atomic>
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
        {
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

    private:
        std::vector<std::thread> m_workers;
        std::deque<std::function<void()>> m_tasks;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::condition_variable m_idleCv;
        std::size_t m_pending = 0;
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

        const std::size_t workerChunks = pool.WorkerCount() * 4;
        const std::size_t chunkCount = std::min(workerChunks, (count + minChunkSize - 1) / minChunkSize);
        const std::size_t chunkSize = (count + chunkCount - 1) / chunkCount;

        for (std::size_t begin = 0; begin < count; begin += chunkSize)
        {
            const std::size_t end = std::min(begin + chunkSize, count);
            pool.Submit([range = Range{ begin, end }, &fn]
            {
                fn(range);
            });
        }
        pool.WaitIdle();
    }
}
