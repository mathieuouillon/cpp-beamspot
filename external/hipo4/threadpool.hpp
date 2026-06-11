#pragma once

#include <fmt/format.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

///
/// @brief A ThreadPool implementation.
///
///
class ThreadPool {
   public:
    enum class ShutdownResult {
        Graceful,       ///< All tasks completed within timeout
        Forced,         ///< Timeout exceeded, threads detached
        AlreadyStopped  ///< Pool was already stopped
    };

    struct Statistics {
        std::atomic<std::size_t> tasks_submitted{0};
        std::atomic<std::size_t> tasks_completed{0};
        std::atomic<std::size_t> tasks_failed{0};
        std::atomic<std::size_t> tasks_pending{0};
        std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};

        // Custom move operations for atomic members
        Statistics() = default;
        Statistics(const Statistics&) = delete;
        Statistics& operator=(const Statistics&) = delete;
        Statistics(Statistics&& other) noexcept;
        Statistics& operator=(Statistics&& other) noexcept;

        [[nodiscard]] auto get_throughput() const noexcept -> double;
        [[nodiscard]] auto get_uptime() const noexcept -> std::chrono::duration<double>;
    };

    ///
    /// @brief Constructs a ThreadPool with specified number of worker threads.
    /// @param num_threads Number of threads (0 = hardware_concurrency)
    /// @param thread_name_prefix Prefix for thread names
    /// @throws std::system_error if thread creation fails
    /// @throws std::invalid_argument if num_threads > hardware limit
    ///
    explicit ThreadPool(std::size_t num_threads = 0, std::string_view thread_name_prefix = "ThreadPool");

    ///
    /// @brief Destructor ensures graceful shutdown with reasonable timeout.
    ///
    ~ThreadPool() noexcept;

    // Non-copyable, move-only
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) noexcept;
    ThreadPool& operator=(ThreadPool&&) noexcept;

    ///
    /// @brief Submits a task for asynchronous execution.
    /// @param callable Function/callable object to execute
    /// @param args Arguments to pass to the callable
    /// @return Future for the result
    /// @throws std::runtime_error if pool is stopped
    ///
    template <typename Callable, typename... Args>
    [[nodiscard]] auto submit(Callable&& callable, Args&&... args) -> std::future<std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>>;

    ///
    /// @brief Initiates graceful shutdown with specified timeout.
    /// @param timeout Maximum time to wait for completion
    ///
    auto shutdown(std::chrono::milliseconds timeout = std::chrono::seconds{5}) noexcept -> void;

    ///
    /// @brief Waits for all pending tasks to complete.
    /// @param timeout Maximum time to wait
    /// @return true if all tasks completed within timeout
    ///
    [[nodiscard]] auto wait_for_idle(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const -> bool;

    ///
    /// @brief Gets the number of worker threads.
    ///
    [[nodiscard]] auto thread_count() const noexcept -> std::size_t { return m_workers.size(); }

    ///
    /// @brief Gets the number of pending tasks.
    ///
    [[nodiscard]] auto pending_tasks() const noexcept -> std::size_t { return m_stats.tasks_pending.load(std::memory_order_acquire); }

    ///
    /// @brief Checks if the pool is running (not stopped).
    ///
    [[nodiscard]] auto is_running() const noexcept -> bool { return !m_stop_requested.load(std::memory_order_acquire); }

    ///
    /// @brief Gets current statistics snapshot.
    ///
    [[nodiscard]] auto get_statistics() const noexcept -> const Statistics& { return m_stats; }

   private:
    auto worker_loop(std::size_t worker_id) noexcept -> void;
    auto set_thread_name(std::string_view name) const noexcept -> void;

    mutable std::mutex m_queue_mutex;
    std::condition_variable m_task_available;
    mutable std::condition_variable m_all_idle;
    std::queue<std::function<void()>> m_task_queue;

    std::vector<std::thread> m_workers;
    std::atomic<bool> m_stop_requested{false};
    std::string m_thread_name_prefix;
    mutable Statistics m_stats;
};

// Template implementation
template <typename Callable, typename... Args>
auto ThreadPool::submit(Callable&& callable, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>> {

    using ReturnType = std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>;

    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [callable = std::forward<Callable>(callable),
         args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> ReturnType {
            return std::apply(std::move(callable), std::move(args_tuple));
        });

    auto future = task->get_future();

    std::lock_guard lock{m_queue_mutex};
    if (m_stop_requested.load(std::memory_order_acquire)) throw std::runtime_error("Cannot submit task to stopped ThreadPool");
    m_task_queue.emplace([task = std::move(task)]() noexcept {try {(*task)();} catch (...) {} });

    m_stats.tasks_submitted.fetch_add(1, std::memory_order_acq_rel);
    m_stats.tasks_pending.fetch_add(1, std::memory_order_acq_rel);

    m_task_available.notify_one();
    return future;
}

namespace {
constexpr std::size_t MAX_REASONABLE_THREADS = 100;
constexpr auto DEFAULT_SHUTDOWN_TIMEOUT = std::chrono::seconds{5};
}  // anonymous namespace

inline auto ThreadPool::Statistics::get_throughput() const noexcept -> double {
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const auto seconds = std::chrono::duration<double>(elapsed).count();
    return seconds > 0.0 ? static_cast<double>(tasks_completed.load(std::memory_order_acquire)) / seconds : 0.0;
}

inline auto ThreadPool::Statistics::get_uptime() const noexcept -> std::chrono::duration<double> {
    return std::chrono::steady_clock::now() - start_time;
}

inline ThreadPool::Statistics::Statistics(Statistics&& other) noexcept
    : tasks_submitted{other.tasks_submitted.load(std::memory_order_acquire)},
      tasks_completed{other.tasks_completed.load(std::memory_order_acquire)},
      tasks_failed{other.tasks_failed.load(std::memory_order_acquire)},
      tasks_pending{other.tasks_pending.load(std::memory_order_acquire)},
      start_time{other.start_time} {
    // Reset other's atomic values
    other.tasks_submitted.store(0, std::memory_order_release);
    other.tasks_completed.store(0, std::memory_order_release);
    other.tasks_failed.store(0, std::memory_order_release);
    other.tasks_pending.store(0, std::memory_order_release);
}

// Statistics move assignment operator
inline ThreadPool::Statistics& ThreadPool::Statistics::operator=(Statistics&& other) noexcept {
    if (this != &other) {
        tasks_submitted.store(other.tasks_submitted.load(std::memory_order_acquire), std::memory_order_release);
        tasks_completed.store(other.tasks_completed.load(std::memory_order_acquire), std::memory_order_release);
        tasks_failed.store(other.tasks_failed.load(std::memory_order_acquire), std::memory_order_release);
        tasks_pending.store(other.tasks_pending.load(std::memory_order_acquire), std::memory_order_release);
        start_time = other.start_time;

        // Reset other's atomic values
        other.tasks_submitted.store(0, std::memory_order_release);
        other.tasks_completed.store(0, std::memory_order_release);
        other.tasks_failed.store(0, std::memory_order_release);
        other.tasks_pending.store(0, std::memory_order_release);
    }
    return *this;
}

inline ThreadPool::ThreadPool(std::size_t num_threads, std::string_view thread_name_prefix)
    : m_thread_name_prefix{thread_name_prefix} {

    if (num_threads == 0) throw std::invalid_argument("ThreadPool: num_threads must be greater than 0");
    if (num_threads > MAX_REASONABLE_THREADS) throw std::invalid_argument("ThreadPool: Requested thread count exceeds reasonable limit");

    m_workers.reserve(num_threads);

    try {
        for (std::size_t i = 0; i < num_threads; ++i) {
            m_workers.emplace_back(&ThreadPool::worker_loop, this, i);
        }
    } catch (const std::system_error& e) {
        m_stop_requested.store(true, std::memory_order_release);
        m_task_available.notify_all();

        for (auto& worker : m_workers) {
            if (worker.joinable()) worker.join();
        }
        throw;
    }
}

inline ThreadPool::~ThreadPool() noexcept {
    if (is_running()) shutdown(DEFAULT_SHUTDOWN_TIMEOUT);
}

inline ThreadPool::ThreadPool(ThreadPool&& other) noexcept
    : m_queue_mutex{},     // Cannot move mutex, create new
      m_task_available{},  // Cannot move condition_variable, create new
      m_all_idle{},        // Cannot move condition_variable, create new
      m_task_queue{std::move(other.m_task_queue)},
      m_workers{std::move(other.m_workers)},
      m_stop_requested{other.m_stop_requested.load(std::memory_order_acquire)},
      m_thread_name_prefix{std::move(other.m_thread_name_prefix)},
      m_stats{std::move(other.m_stats)} {

    other.m_stop_requested.store(true, std::memory_order_release);
}

inline ThreadPool& ThreadPool::operator=(ThreadPool&& other) noexcept {
    if (this != &other) {
        if (is_running()) shutdown(DEFAULT_SHUTDOWN_TIMEOUT);

        // Move data (note: mutexes and condition_variables cannot be moved)
        std::lock_guard lock{m_queue_mutex};
        std::lock_guard other_lock{other.m_queue_mutex};

        m_task_queue = std::move(other.m_task_queue);
        m_workers = std::move(other.m_workers);
        m_stop_requested.store(other.m_stop_requested.load(std::memory_order_acquire), std::memory_order_release);
        m_thread_name_prefix = std::move(other.m_thread_name_prefix);
        m_stats = std::move(other.m_stats);

        // Mark other as moved-from
        other.m_stop_requested.store(true, std::memory_order_release);
    }
    return *this;
}

inline auto ThreadPool::shutdown(std::chrono::milliseconds timeout) noexcept -> void {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    bool expected = false;
    if (!m_stop_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) fmt::print(stderr, "Pool was already stopped\n");

    // Notify all workers to stop
    m_task_available.notify_all();

    bool all_joined = true;
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            if (std::chrono::steady_clock::now() < deadline) {
                try {
                    worker.join();
                } catch (const std::system_error&) {
                    worker.detach();
                    all_joined = false;
                }
            } else {
                worker.detach();
                all_joined = false;
            }
        }
    }

    m_workers.clear();

    // Clear remaining tasks
    std::lock_guard lock{m_queue_mutex};
    std::queue<std::function<void()>> empty;
    m_task_queue.swap(empty);
    m_stats.tasks_pending.store(0, std::memory_order_release);

    ShutdownResult shutdown_result = all_joined ? ShutdownResult::Graceful : ShutdownResult::Forced;

    switch (shutdown_result) {
        case ThreadPool::ShutdownResult::Graceful:
            break;  // normal path: stay quiet (was noise on the progress line)
        case ThreadPool::ShutdownResult::Forced:
            fmt::print(stderr, "Pool shutdown was forced (timeout)\n");
            break;
        case ThreadPool::ShutdownResult::AlreadyStopped:
            fmt::print(stderr, "Pool was already stopped\n");
            break;
    }
}

inline auto ThreadPool::wait_for_idle(std::chrono::milliseconds timeout) const -> bool {
    std::unique_lock lock{m_queue_mutex};
    return m_all_idle.wait_for(lock, timeout, [this] {
        return m_task_queue.empty() &&
               m_stats.tasks_pending.load(std::memory_order_acquire) == 0;
    });
}

inline auto ThreadPool::worker_loop(std::size_t worker_id) noexcept -> void {
    set_thread_name(m_thread_name_prefix + "_" + std::to_string(worker_id));

    while (!m_stop_requested.load(std::memory_order_acquire)) {
        std::function<void()> task;
        {  // This scope is VERY IMPORTANT to limit lock duration, don't remove it!
            std::unique_lock lock{m_queue_mutex};

            // Wait for task or stop signal
            m_task_available.wait(lock, [this] {
                return m_stop_requested.load(std::memory_order_acquire) || !m_task_queue.empty();
            });

            // Check if we should exit
            if (m_stop_requested.load(std::memory_order_acquire) && m_task_queue.empty()) {
                break;
            }

            // Get task if available
            if (!m_task_queue.empty()) {
                task = std::move(m_task_queue.front());
                m_task_queue.pop();
            }
            /*
        The braces { } create a new scope. In C++, when a variable goes out of scope, its destructor is called automatically (RAII).
        By wrapping the std::unique_lock in braces, the lock's destructor runs at the closing }, which releases the mutex before the task executes. 
        Without the braces, the lock stayed alive until the end of the entire while loop, keeping the mutex locked during task execution and blocking all other threads.
        It's the difference between "lock → get task → unlock → execute" (parallel) versus "lock → get task → execute → unlock" (serial).
        */
        }  // ← Lock released here!

        // Execute task outside of lock
        if (task) {
            try {
                task();
                m_stats.tasks_completed.fetch_add(1, std::memory_order_acq_rel);
            } catch (const std::exception& e) {
                m_stats.tasks_failed.fetch_add(1, std::memory_order_acq_rel);
            } catch (...) {
                m_stats.tasks_failed.fetch_add(1, std::memory_order_acq_rel);
            }

            // Update pending count and notify if idle
            const auto remaining = m_stats.tasks_pending.fetch_sub(1, std::memory_order_acq_rel);
            if (remaining == 1) {
                std::lock_guard lock{m_queue_mutex};
                if (m_task_queue.empty()) m_all_idle.notify_all();
            }
        }
    }
}

inline auto ThreadPool::set_thread_name(std::string_view name) const noexcept -> void {
    try {
#if defined(__linux__)
        // Linux limits thread names to 15 characters + null terminator
        constexpr std::size_t MAX_NAME_LENGTH = 15;
        std::string truncated_name{name.substr(0, MAX_NAME_LENGTH)};
        pthread_setname_np(pthread_self(), truncated_name.c_str());

#elif defined(_WIN32)
        // Windows thread naming via SetThreadDescription (Windows 10 version 1607+)
        std::wstring wide_name;
        wide_name.reserve(name.size());
        std::transform(name.begin(), name.end(), std::back_inserter(wide_name),
                       [](char c) { return static_cast<wchar_t>(c); });

        SetThreadDescription(GetCurrentThread(), wide_name.c_str());

#elif defined(__APPLE__)
        // macOS allows longer thread names
        std::string name_str{name};
        pthread_setname_np(name_str.c_str());
#endif
    } catch (...) {
    }
}