module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
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
        std::size_t activeWorkers = 0;
        std::size_t peakActiveWorkers = 0;
        std::uint64_t totalTaskNanoseconds = 0;
        std::uint64_t maxTaskNanoseconds = 0;

        [[nodiscard]] double AverageTaskNanoseconds() const noexcept
        {
            return completedTasks == 0
                ? 0.0
                : static_cast<double>(totalTaskNanoseconds) /
                    static_cast<double>(completedTasks);
        }
    };

    class CancellationToken final
    {
    public:
        CancellationToken() = default;
        [[nodiscard]] bool StopRequested() const noexcept
        {
            return state_ && state_->load(std::memory_order_acquire);
        }

    private:
        explicit CancellationToken(std::shared_ptr<std::atomic<bool>> state)
            : state_(std::move(state)) {}
        std::shared_ptr<std::atomic<bool>> state_;
        friend class CancellationSource;
    };

    class CancellationSource final
    {
    public:
        CancellationSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}
        [[nodiscard]] CancellationToken Token() const { return CancellationToken(state_); }
        void RequestStop() noexcept { state_->store(true, std::memory_order_release); }

    private:
        std::shared_ptr<std::atomic<bool>> state_;
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
            std::exception_ptr exception;
            {
                std::unique_lock lock(m_mutex);
                m_idleCv.wait(lock, [this]
                {
                    return m_pending == 0 && m_tasks.empty();
                });
                exception = m_unhandledException;
                m_unhandledException = nullptr;
            }
            if (exception)
            {
                std::rethrow_exception(exception);
            }
        }

        [[nodiscard]]
        SchedulerStats Stats() const
        {
            std::scoped_lock lock(m_mutex);
            return {
                .workerCount = m_workers.size(),
                .submittedTasks = m_submitted,
                .completedTasks = m_completed,
                .pendingTasks = m_pending,
                .activeWorkers = m_activeWorkers,
                .peakActiveWorkers = m_peakActiveWorkers,
                .totalTaskNanoseconds = m_totalTaskNanoseconds,
                .maxTaskNanoseconds = m_maxTaskNanoseconds
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
        std::size_t m_activeWorkers = 0;
        std::size_t m_peakActiveWorkers = 0;
        std::uint64_t m_totalTaskNanoseconds = 0;
        std::uint64_t m_maxTaskNanoseconds = 0;
        std::exception_ptr m_unhandledException;
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
                    ++m_activeWorkers;
                    m_peakActiveWorkers = std::max(m_peakActiveWorkers, m_activeWorkers);
                }

                const auto started = std::chrono::steady_clock::now();
                try
                {
                    task();
                }
                catch (...)
                {
                    std::scoped_lock lock(m_mutex);
                    if (!m_unhandledException)
                    {
                        m_unhandledException = std::current_exception();
                    }
                }
                const auto finished = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finished - started).count();

                {
                    std::scoped_lock lock(m_mutex);
                    --m_pending;
                    --m_activeWorkers;
                    ++m_completed;
                    const std::uint64_t elapsedNs = elapsed > 0
                        ? static_cast<std::uint64_t>(elapsed)
                        : 0u;
                    m_totalTaskNanoseconds += elapsedNs;
                    m_maxTaskNanoseconds = std::max(m_maxTaskNanoseconds, elapsedNs);
                    if (m_pending == 0 && m_tasks.empty())
                    {
                        m_idleCv.notify_all();
                    }
                }
            }
        }
    };

    /// Owns a bounded set of pool tasks and transports the first worker
    /// exception to the submitting thread. A TaskGroup must outlive its tasks;
    /// call Wait before destruction or before inspecting task output.
    class TaskGroup final
    {
    public:
        explicit TaskGroup(ThreadPool& pool)
            : m_pool(pool)
        {
        }

        TaskGroup(const TaskGroup&) = delete;
        TaskGroup& operator=(const TaskGroup&) = delete;

        template<typename Fn>
        void Submit(Fn&& fn)
        {
            {
                std::scoped_lock lock(m_mutex);
                if (m_waitStarted)
                {
                    throw std::logic_error("TaskGroup::Submit called after Wait.");
                }
                ++m_remaining;
            }
            m_pool.Submit([this, task = std::forward<Fn>(fn)]() mutable
            {
                try
                {
                    task();
                }
                catch (...)
                {
                    std::scoped_lock lock(m_mutex);
                    if (!m_exception)
                    {
                        m_exception = std::current_exception();
                    }
                }
                {
                    std::scoped_lock lock(m_mutex);
                    --m_remaining;
                    if (m_remaining == 0)
                    {
                        m_doneCv.notify_all();
                    }
                }
            });
        }

        void Wait()
        {
            std::exception_ptr exception;
            {
                std::unique_lock lock(m_mutex);
                m_waitStarted = true;
                m_doneCv.wait(lock, [this] { return m_remaining == 0; });
                exception = m_exception;
            }
            if (exception)
            {
                std::rethrow_exception(exception);
            }
        }

    private:
        ThreadPool& m_pool;
        std::mutex m_mutex;
        std::condition_variable m_doneCv;
        std::size_t m_remaining = 0;
        bool m_waitStarted = false;
        std::exception_ptr m_exception;
    };

    struct TaskHandle final
    {
        std::size_t index = 0;
        friend bool operator==(TaskHandle, TaskHandle) = default;
    };

    /// Validated dependency graph executed in deterministic topological waves.
    /// Independent tasks in each wave run concurrently; a cycle, invalid
    /// dependency, worker exception, or cancellation terminates execution
    /// predictably.
    class TaskGraph final
    {
    public:
        [[nodiscard]] TaskHandle Add(
            std::function<void()> task,
            std::span<const TaskHandle> dependencies = {})
        {
            if (!task) throw std::invalid_argument("TaskGraph requires a callable task.");
            Node node;
            node.task = std::move(task);
            node.dependencies.reserve(dependencies.size());
            for (TaskHandle dependency : dependencies)
            {
                if (dependency.index >= nodes_.size())
                    throw std::out_of_range("TaskGraph dependency must reference an existing task.");
                if (std::find(
                    node.dependencies.begin(), node.dependencies.end(), dependency.index)
                    != node.dependencies.end())
                    throw std::invalid_argument("TaskGraph dependency cannot be duplicated.");
                node.dependencies.push_back(dependency.index);
            }
            nodes_.push_back(std::move(node));
            return { nodes_.size() - 1 };
        }

        [[nodiscard]] std::size_t Size() const noexcept { return nodes_.size(); }

        /// Input: reusable pool and optional cooperative cancellation token.
        /// Output: every task runs once after all dependencies complete.
        /// Cancellation stops before the next topological wave.
        void Execute(ThreadPool& pool, CancellationToken cancellation = {}) const
        {
            if (nodes_.empty()) return;
            std::vector<std::size_t> indegree(nodes_.size(), 0);
            std::vector<std::vector<std::size_t>> dependents(nodes_.size());
            for (std::size_t index = 0; index < nodes_.size(); ++index)
            {
                indegree[index] = nodes_[index].dependencies.size();
                for (std::size_t dependency : nodes_[index].dependencies)
                    dependents[dependency].push_back(index);
            }
            std::vector<std::size_t> ready;
            for (std::size_t index = 0; index < nodes_.size(); ++index)
                if (indegree[index] == 0) ready.push_back(index);
            std::size_t completed = 0;
            while (!ready.empty())
            {
                if (cancellation.StopRequested()) return;
                TaskGroup group(pool);
                for (std::size_t index : ready)
                    group.Submit([this, index, cancellation]
                    {
                        if (!cancellation.StopRequested()) nodes_[index].task();
                    });
                group.Wait();
                completed += ready.size();
                std::vector<std::size_t> next;
                for (std::size_t index : ready)
                    for (std::size_t dependent : dependents[index])
                        if (--indegree[dependent] == 0) next.push_back(dependent);
                std::sort(next.begin(), next.end());
                ready = std::move(next);
            }
            if (completed != nodes_.size() && !cancellation.StopRequested())
                throw std::logic_error("TaskGraph contains a dependency cycle.");
        }

    private:
        struct Node final
        {
            std::function<void()> task;
            std::vector<std::size_t> dependencies;
        };
        std::vector<Node> nodes_;
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
        TaskGroup group(pool);
        for (Range range : ranges)
        {
            group.Submit([range, &fn]
            {
                fn(range);
            });
        }
        group.Wait();
    }

    /// Cooperative cancellable range execution. Already-running chunks are
    /// allowed to finish; no new callback work begins after cancellation is
    /// observed. This keeps teardown bounded without forcibly terminating
    /// worker threads.
    template<typename Fn>
    void ParallelForCancellable(
        ThreadPool& pool,
        std::size_t count,
        std::size_t minChunkSize,
        CancellationToken cancellation,
        Fn&& fn,
        ExecutionPolicy policy = ExecutionPolicy::Parallel)
    {
        if (count == 0 || cancellation.StopRequested()) return;
        minChunkSize = std::max<std::size_t>(minChunkSize, 1);

        if (policy == ExecutionPolicy::Sequential ||
            pool.WorkerCount() <= 1 ||
            count <= minChunkSize)
        {
            if (!cancellation.StopRequested())
                fn(Range{ 0, count });
            return;
        }

        const std::vector<Range> ranges =
            PartitionRange(count, minChunkSize, pool.WorkerCount() * 4);
        TaskGroup group(pool);
        for (Range range : ranges)
        {
            group.Submit([range, cancellation, &fn]
            {
                if (!cancellation.StopRequested())
                    fn(range);
            });
        }
        group.Wait();
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
