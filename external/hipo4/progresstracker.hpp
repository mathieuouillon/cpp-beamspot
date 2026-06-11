#pragma once

#include <fmt/core.h>
#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace detail {
/// @brief Returns the global recursive mutex used to synchronize stdout access.
/// A recursive mutex is used to allow nested locking from the same thread.
inline std::recursive_mutex& get_stdout_mutex() {
    static std::recursive_mutex s_stdout_mutex;
    return s_stdout_mutex;
}
}  // namespace detail

/// 
///  @brief Thread-safe progress tracker with visual progress bar.
///
class ProgressTracker {
   public:
    struct Config {
        std::size_t bar_width;
        bool show_eta;
        bool show_rate;
        bool use_colors;
        bool use_gradient;
        std::string label;
        std::chrono::milliseconds update_interval;

        Config()
            : bar_width(50), show_eta(true), show_rate(true), use_colors(true), use_gradient(true), label("Processing"), update_interval(100) {}
    };

    ///
    ///  @brief Constructs a progress tracker.
    ///  @param total Total number of items to track
    ///  @param config Display configuration
    ///  @throws std::invalid_argument if total is 0
    ///
    explicit ProgressTracker(std::size_t total, Config config = {});

    ~ProgressTracker() noexcept;

    // Non-copyable, move-only
    ProgressTracker(const ProgressTracker&) = delete;
    ProgressTracker& operator=(const ProgressTracker&) = delete;
    ProgressTracker(ProgressTracker&&) noexcept;
    ProgressTracker& operator=(ProgressTracker&&) noexcept;

    ///
    ///  @brief Increments the progress counter (thread-safe, no immediate display update).
    ///
    auto increment() noexcept -> void;

    ///
    ///  @brief Adds multiple items to progress (thread-safe, no immediate display update).
    ///
    auto add(std::size_t count) noexcept -> void;

    ///
    ///  @brief Sets the absolute progress count (thread-safe, no immediate display update).
    ///
    ///  Useful when progress is driven from an external source (e.g. the sum of
    ///  per-worker counters in the multi-process fill) rather than incremented locally.
    ///
    auto set_processed(std::size_t value) noexcept -> void {
        m_processed.store(value, std::memory_order_release);
    }

    ///
    ///  @brief Gets current progress count (thread-safe).
    ///
    [[nodiscard]] auto get_processed() const noexcept -> std::size_t {
        return m_processed.load(std::memory_order_acquire);
    }

    ///
    ///  @brief Gets total item count (thread-safe).
    ///
    [[nodiscard]] auto get_total() const noexcept -> std::size_t {
        return m_total;
    }

    ///
    ///  @brief Gets current progress ratio [0.0, 1.0] (thread-safe).
    ///
    [[nodiscard]] auto get_progress() const noexcept -> double;

    ///
    /// @brief Starts the background update thread and shows initial display.
    ///
    auto start() noexcept -> void;

    ///
    /// @brief Stops the update thread and displays final completion message.
    ///
    auto finish() noexcept -> void;

    ///
    /// @brief Clears the current progress line.
    ///
    static auto clear_line() noexcept -> void;

    ///
    /// @brief Formats a duration given in seconds into a human-readable string.
    ///
    /// Converts the provided std::chrono::seconds duration into a string representation,
    /// typically in the format "HH:MM:SS" or similar, for display purposes.
    ///
    /// @param duration The duration to format, expressed as std::chrono::seconds.
    /// @return A std::string containing the formatted duration.
    ///
    /// @note This function does not throw exceptions.
    ///
    [[nodiscard]] static auto format_duration(std::chrono::seconds duration) noexcept -> std::string;

   private:
    // ANSI escape codes
    struct AnsiCodes {
        static constexpr std::string_view CLEAR_LINE = "\r\033[K";
        static constexpr std::string_view RESET = "\033[0m";
        static constexpr std::string_view BOLD = "\033[1m";
        static constexpr std::string_view DIM = "\033[2m";

        // Standard colors
        static constexpr std::string_view GREEN = "\033[32m";
        static constexpr std::string_view CYAN = "\033[36m";
        static constexpr std::string_view YELLOW = "\033[33m";
        static constexpr std::string_view MAGENTA = "\033[35m";
        static constexpr std::string_view WHITE = "\033[37m";
        static constexpr std::string_view BLUE = "\033[34m";
        static constexpr std::string_view RED = "\033[31m";

        // Bright colors
        static constexpr std::string_view BRIGHT_GREEN = "\033[92m";
        static constexpr std::string_view BRIGHT_CYAN = "\033[96m";
        static constexpr std::string_view BRIGHT_YELLOW = "\033[93m";
        static constexpr std::string_view BRIGHT_MAGENTA = "\033[95m";
        static constexpr std::string_view BRIGHT_WHITE = "\033[97m";

        // Background colors for gradient effect
        static constexpr std::string_view BG_CYAN = "\033[46m";
        static constexpr std::string_view BG_BLUE = "\033[44m";
        static constexpr std::string_view BG_MAGENTA = "\033[45m";
    };

    // Progress bar block characters (8 levels of granularity)
    static constexpr std::array<std::string_view, 9> BLOCK_CHARS = {
        " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};

    // Spinner animation characters
    static constexpr std::array<std::string_view, 10> SPINNER_CHARS = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

    auto update_display() noexcept -> void;
    auto render_bar(double progress) const noexcept -> void;
    auto update_loop() noexcept -> void;
    auto stop_update_thread() noexcept -> void;

    [[nodiscard]] static auto format_number(std::size_t num) noexcept -> std::string;
    [[nodiscard]] auto get_elapsed() const noexcept -> std::chrono::seconds;
    [[nodiscard]] auto calculate_eta() const noexcept -> std::chrono::seconds;
    [[nodiscard]] auto calculate_rate() const noexcept -> double;
    [[nodiscard]] auto calculate_instant_rate() const noexcept -> double;
    [[nodiscard]] static auto format_rate(double rate) noexcept -> std::string;
    [[nodiscard]] auto get_progress_color() const noexcept -> std::string_view;

    const std::size_t m_total;
    std::atomic<std::size_t> m_processed{0};
    std::chrono::steady_clock::time_point m_start_time;
    Config m_config;
    mutable std::mutex m_display_mutex;
    bool m_finished{false};
    mutable std::size_t m_spinner_index{0};

    // Sliding-window state for the instantaneous rate (sampled in update_display)
    mutable std::chrono::steady_clock::time_point m_last_sample_time;
    mutable std::size_t m_last_sample_count{0};
    mutable double m_instant_rate{0.0};

    // Background update thread
    std::atomic<bool> m_should_stop{false};
    std::thread m_update_thread;
};

inline ProgressTracker::ProgressTracker(std::size_t total, Config config)
    : m_total{total}, m_start_time{std::chrono::steady_clock::now()}, m_config{config}, m_last_sample_time{m_start_time} {
    if (total == 0) throw std::invalid_argument("ProgressTracker: total must be greater than 0");
}

inline ProgressTracker::~ProgressTracker() noexcept {
    stop_update_thread();
    if (!m_finished) clear_line();
}

inline ProgressTracker::ProgressTracker(ProgressTracker&& other) noexcept
    : m_total{other.m_total}, m_processed{other.m_processed.load(std::memory_order_acquire)}, m_start_time{other.m_start_time}, m_config{other.m_config}, m_finished{other.m_finished} {
    other.stop_update_thread();
    other.m_finished = true;
}

inline ProgressTracker& ProgressTracker::operator=(ProgressTracker&& other) noexcept {
    if (this != &other) {
        stop_update_thread();
        if (!m_finished) clear_line();
        other.stop_update_thread();

        const_cast<std::size_t&>(m_total) = other.m_total;
        m_processed.store(other.m_processed.load(std::memory_order_acquire), std::memory_order_release);
        m_start_time = other.m_start_time;
        m_config = other.m_config;
        m_finished = other.m_finished;

        other.m_finished = true;
    }
    return *this;
}

inline auto ProgressTracker::increment() noexcept -> void {
    m_processed.fetch_add(1, std::memory_order_acq_rel);
}

inline auto ProgressTracker::add(std::size_t count) noexcept -> void {
    m_processed.fetch_add(count, std::memory_order_acq_rel);
}

inline auto ProgressTracker::get_progress() const noexcept -> double {
    const auto current = m_processed.load(std::memory_order_acquire);
    return m_total > 0 ? static_cast<double>(current) / static_cast<double>(m_total) : 0.0;
}

inline auto ProgressTracker::get_elapsed() const noexcept -> std::chrono::seconds {
    const auto elapsed = std::chrono::steady_clock::now() - m_start_time;
    return std::chrono::duration_cast<std::chrono::seconds>(elapsed);
}

inline auto ProgressTracker::calculate_rate() const noexcept -> double {
    const auto elapsed = get_elapsed().count();
    const auto current = m_processed.load(std::memory_order_acquire);
    return elapsed > 0 ? static_cast<double>(current) / static_cast<double>(elapsed) : 0.0;
}

inline auto ProgressTracker::calculate_instant_rate() const noexcept -> double {
    // Rate over the interval since the previous display tick, smoothed with an
    // exponential moving average so it doesn't jitter. Sampled here because
    // update_display() is the single periodic caller (guarded by m_display_mutex).
    const auto now = std::chrono::steady_clock::now();
    const auto current = m_processed.load(std::memory_order_acquire);
    const double dt = std::chrono::duration<double>(now - m_last_sample_time).count();
    if (dt >= 0.05) {
        const double delta = static_cast<double>(current) - static_cast<double>(m_last_sample_count);
        const double sample = delta > 0.0 ? delta / dt : 0.0;
        m_instant_rate = m_instant_rate > 0.0 ? 0.6 * sample + 0.4 * m_instant_rate : sample;
        m_last_sample_time = now;
        m_last_sample_count = current;
    }
    return m_instant_rate;
}

inline auto ProgressTracker::format_rate(double rate) noexcept -> std::string {
    if (rate < 1000.0) return fmt::format("{:.0f}/s", rate);
    if (rate < 1000000.0) return fmt::format("{:.1f}k/s", rate / 1000.0);
    return fmt::format("{:.2f}M/s", rate / 1000000.0);
}

inline auto ProgressTracker::calculate_eta() const noexcept -> std::chrono::seconds {
    const auto current = m_processed.load(std::memory_order_acquire);
    // Prefer the (smoothed) instantaneous rate so the ETA reacts to the current
    // pace; fall back to the cumulative average until the first sample lands.
    const auto instant = calculate_instant_rate();
    const auto rate = instant > 0.0 ? instant : calculate_rate();

    if (rate > 0.0 && current < m_total) {
        const auto remaining = m_total - current;
        return std::chrono::seconds{static_cast<long>(remaining / rate)};
    }
    return std::chrono::seconds{0};
}

inline auto ProgressTracker::format_duration(std::chrono::seconds duration) noexcept -> std::string {
    const auto m_totalseconds = duration.count();

    if (m_totalseconds < 60) return fmt::format("{}s", m_totalseconds);
    if (m_totalseconds < 3600) {
        const auto minutes = m_totalseconds / 60;
        const auto seconds = m_totalseconds % 60;
        return fmt::format("{}m {:02d}s", minutes, seconds);
    }

    const auto hours = m_totalseconds / 3600;
    const auto minutes = (m_totalseconds % 3600) / 60;
    return fmt::format("{}h {:02d}m", hours, minutes);
}

inline auto ProgressTracker::format_number(std::size_t num) noexcept -> std::string {
    if (num < 1000) return fmt::format("{}", num);
    if (num < 1000000) return fmt::format("{:.1f}k", num / 1000.0);
    return fmt::format("{:.1f}M", num / 1000000.0);
}

inline auto ProgressTracker::clear_line() noexcept -> void {
    std::lock_guard lock{detail::get_stdout_mutex()};
    try {
        fmt::print("{}", AnsiCodes::CLEAR_LINE);
        std::cout.flush();
    } catch (...) {
    }
}

inline auto ProgressTracker::get_progress_color() const noexcept -> std::string_view {
    if (!m_config.use_colors || !m_config.use_gradient) return AnsiCodes::GREEN;
    return AnsiCodes::BRIGHT_GREEN;
}

inline auto ProgressTracker::render_bar(double progress) const noexcept -> void {
    std::lock_guard lock{detail::get_stdout_mutex()};
    try {
        const auto& colors = m_config.use_colors;
        progress = std::clamp(progress, 0.0, 1.0);

        const auto filled_width = m_config.bar_width * progress;
        const auto full_blocks = static_cast<std::size_t>(filled_width);
        const auto remainder = filled_width - static_cast<double>(full_blocks);
        const auto partial_block_idx = static_cast<std::size_t>(remainder * 8.0);

        // Left bracket with color
        if (colors) fmt::print("{}", AnsiCodes::DIM);
        fmt::print("╢");
        if (colors) fmt::print("{}", AnsiCodes::RESET);

        // Render progress bar with gradient
        for (std::size_t i = 0; i < m_config.bar_width; ++i) {
            if (i < full_blocks) {
                if (colors) {
                    fmt::print("{}", get_progress_color());
                }
                fmt::print("{}", BLOCK_CHARS[8]);
            } else if (i == full_blocks) {
                if (colors) {
                    fmt::print("{}", get_progress_color());
                }
                fmt::print("{}", BLOCK_CHARS[partial_block_idx]);
            } else {
                if (colors) fmt::print("{}", AnsiCodes::DIM);
                fmt::print("░");
            }
        }

        if (colors) fmt::print("{}", AnsiCodes::RESET);

        // Right bracket
        if (colors) fmt::print("{}", AnsiCodes::DIM);
        fmt::print("╟");
        if (colors) fmt::print("{}", AnsiCodes::RESET);
        fmt::print(" ");

    } catch (...) {
    }
}

inline auto ProgressTracker::update_display() noexcept -> void {
    std::lock_guard lock{m_display_mutex};

    if (m_finished) return;

    try {
        const auto current = m_processed.load(std::memory_order_acquire);
        const auto progress = get_progress();
        const auto elapsed = get_elapsed();
        const auto& colors = m_config.use_colors;

        // Lock stdout mutex for all print operations
        std::lock_guard stdout_lock{detail::get_stdout_mutex()};

        clear_line();

        // Spinner animation
        if (progress < 1.0) {
            if (colors) fmt::print("{}", AnsiCodes::BRIGHT_CYAN);
            fmt::print("{}", SPINNER_CHARS[m_spinner_index % SPINNER_CHARS.size()]);
            m_spinner_index++;
            if (colors) fmt::print("{}", AnsiCodes::RESET);
            fmt::print(" ");
        } else {
            if (colors) fmt::print("{}", AnsiCodes::BRIGHT_GREEN);
            fmt::print("✓");
            if (colors) fmt::print("{}", AnsiCodes::RESET);
            fmt::print(" ");
        }

        // Label
        if (!m_config.label.empty()) {
            if (colors) fmt::print("{}", AnsiCodes::BOLD);
            fmt::print("{}", m_config.label);
            if (colors) fmt::print("{}", AnsiCodes::RESET);
            fmt::print(" ");
        }

        render_bar(progress);

        // Percentage with enhanced styling
        if (progress >= 1.0) {
            if (colors) fmt::print("{}{}", AnsiCodes::BRIGHT_GREEN, AnsiCodes::BOLD);
            fmt::print("100%");
            if (colors) fmt::print("{}", AnsiCodes::RESET);
        } else {
            if (colors) fmt::print("{}", get_progress_color());
            fmt::print("{:>3.0f}%", progress * 100.0);
            if (colors) fmt::print("{}", AnsiCodes::RESET);
        }

        // Count with dividers
        fmt::print(" ");
        if (colors) fmt::print("{}", AnsiCodes::DIM);
        fmt::print("│");
        if (colors) fmt::print("{}", AnsiCodes::RESET);
        fmt::print(" ");

        if (colors) fmt::print("{}", AnsiCodes::BRIGHT_WHITE);
        fmt::print("{}", format_number(current));
        if (colors) fmt::print("{}", AnsiCodes::RESET);

        if (colors) fmt::print("{}", AnsiCodes::DIM);
        fmt::print("/");
        if (colors) fmt::print("{}", AnsiCodes::RESET);

        if (colors) fmt::print("{}", AnsiCodes::BRIGHT_CYAN);
        fmt::print("{}", format_number(m_total));
        if (colors) fmt::print("{}", AnsiCodes::RESET);

        // Elapsed time
        fmt::print(" ");
        if (colors) fmt::print("{}", AnsiCodes::DIM);
        fmt::print("│ ⏱");
        if (colors) fmt::print("{}", AnsiCodes::RESET);
        fmt::print(" ");
        if (colors) fmt::print("{}", AnsiCodes::MAGENTA);
        fmt::print("{}", format_duration(elapsed));
        if (colors) fmt::print("{}", AnsiCodes::RESET);

        // ETA
        if (m_config.show_eta && current < m_total) {
            const auto eta = calculate_eta();
            if (eta.count() > 0) {
                fmt::print(" ");
                if (colors) fmt::print("{}", AnsiCodes::DIM);
                fmt::print("│ ⏳");
                if (colors) fmt::print("{}", AnsiCodes::RESET);
                fmt::print(" ");
                if (colors) fmt::print("{}", AnsiCodes::YELLOW);
                fmt::print("{}", format_duration(eta));
                if (colors) fmt::print("{}", AnsiCodes::RESET);
            }
        }

        // Rate: instantaneous (⚡) with the running average (⌀) alongside
        if (m_config.show_rate) {
            const auto instant = calculate_instant_rate();
            const auto average = calculate_rate();
            if (instant > 0.0 || average > 0.0) {
                fmt::print(" ");
                if (colors) fmt::print("{}", AnsiCodes::DIM);
                fmt::print("│ ⚡");
                if (colors) fmt::print("{}", AnsiCodes::RESET);
                fmt::print(" ");
                if (colors) fmt::print("{}", AnsiCodes::BRIGHT_MAGENTA);
                fmt::print("{}", format_rate(instant));
                if (colors) fmt::print("{}", AnsiCodes::RESET);

                fmt::print(" ");
                if (colors) fmt::print("{}", AnsiCodes::DIM);
                fmt::print("(⌀ {})", format_rate(average));
                if (colors) fmt::print("{}", AnsiCodes::RESET);
            }
        }

        std::cout.flush();

    } catch (...) {
    }
}

inline auto ProgressTracker::update_loop() noexcept -> void {
    while (!m_should_stop.load(std::memory_order_acquire)) {
        update_display();
        if (m_processed.load(std::memory_order_acquire) >= m_total) break;
        std::this_thread::sleep_for(m_config.update_interval);
    }

    update_display();
}

inline auto ProgressTracker::start() noexcept -> void {
    if (!m_update_thread.joinable() && !m_finished) {
        m_should_stop.store(false, std::memory_order_release);
        update_display();
        m_update_thread = std::thread(&ProgressTracker::update_loop, this);
    }
}

inline auto ProgressTracker::stop_update_thread() noexcept -> void {
    if (m_update_thread.joinable()) {
        m_should_stop.store(true, std::memory_order_release);
        m_update_thread.join();
    }
}

inline auto ProgressTracker::finish() noexcept -> void {
    stop_update_thread();

    std::lock_guard lock{m_display_mutex};

    if (m_finished) return;
    m_finished = true;

    try {
        const auto elapsed = get_elapsed();
        const auto& colors = m_config.use_colors;

        // Lock stdout mutex for all print operations
        std::lock_guard stdout_lock{detail::get_stdout_mutex()};

        clear_line();

        // Completion line
        if (colors) fmt::print("{}", AnsiCodes::BRIGHT_GREEN);
        fmt::print(" {} Complete!   │   ", m_config.label);
        if (colors) fmt::print("{}", AnsiCodes::RESET);

        // Files processed
        fmt::print(" Items: ");
        if (colors) fmt::print("{}", AnsiCodes::BRIGHT_CYAN);
        fmt::print("{}", format_number(m_total));
        if (colors) fmt::print("{}", AnsiCodes::RESET);
        fmt::print("  │  ");

        // Time elapsed
        fmt::print("⏱  Time: ");
        if (colors) fmt::print("{}", AnsiCodes::MAGENTA);
        fmt::print("{}", format_duration(elapsed));
        if (colors) fmt::print("{}", AnsiCodes::RESET);

        fmt::print("\n");
        std::cout.flush();

    } catch (...) {
    }
}