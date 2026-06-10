//******************************************************************************
//*       ██╗  ██╗██╗██████╗  ██████╗     ██╗  ██╗    ██████╗                  *
//*       ██║  ██║██║██╔══██╗██╔═══██╗    ██║  ██║   ██╔═████╗                 *
//*       ███████║██║██████╔╝██║   ██║    ███████║   ██║██╔██║                 *
//*       ██╔══██║██║██╔═══╝ ██║   ██║    ╚════██║   ████╔╝██║                 *
//*       ██║  ██║██║██║     ╚██████╔╝         ██║██╗╚██████╔╝                 *
//*       ╚═╝  ╚═╝╚═╝╚═╝      ╚═════╝          ╚═╝╚═╝ ╚═════╝                  *
//************************ Jefferson National Lab (2017) ***********************
/// 
///  @file chain.h
///  @brief Chain multiple HIPO files for sequential or parallel processing
/// 
///  Provides a modern C++ interface for processing multiple HIPO files with:
///  - Range-based iteration across all files
///  - Parallel processing with configurable thread pools
///  - Lazy metadata loading for minimal memory overhead
///  - Progress tracking and statistics
/// 
///  @code
///  // Simple sequential iteration
///  hipo::chain ch;
///  ch.add("file1.hipo");
///  ch.add("file2.hipo");
///  for (auto& [event, file_idx, event_idx] : ch) {
///      auto& particles = event.getBank("REC::Particle");
///  }
/// 
///  // Parallel processing
///  ch.process([](auto& event, int file_idx, long event_idx) {
///      // Process event...
///  });
///  @endcode
///

#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include "reader.h"
#include "record.h"
#include "dictionary.h"
#include "bank.h"
#include "event.h"
#include "progresstracker.hpp"
#include "threadpool.hpp"

namespace hipo {

//=============================================================================
// FileInfo - Metadata for a single file in the chain
//=============================================================================

/// 
/// @brief Metadata container for a file in the chain
/// 
struct FileInfo {
    std::string filename;
    int index{0};
    long total_events{-1};   // -1 = not yet loaded
    int num_records{-1};     // -1 = not yet loaded
    std::uintmax_t file_size{0};
    bool is_valid{true};
    std::string error_message;

    FileInfo() = default;
    FileInfo(std::string_view name, int idx)
        : filename(name), index(idx) {}

    /// Check if metadata has been loaded
    [[nodiscard]] bool metadata_loaded() const noexcept {
        return total_events >= 0;
    }

    /// Get human-readable file size string
    [[nodiscard]] std::string size_string() const {
        if (file_size > 1024ULL * 1024 * 1024)
            return fmt::format("{:.1f} GB", file_size / (1024.0 * 1024 * 1024));
        if (file_size > 1024 * 1024)
            return fmt::format("{:.1f} MB", file_size / (1024.0 * 1024));
        if (file_size > 1024)
            return fmt::format("{:.1f} KB", file_size / 1024.0);
        return fmt::format("{} B", file_size);
    }
};

// Legacy alias
using fileinfo = FileInfo;

//=============================================================================
// ChainStatistics - Processing statistics
//=============================================================================

/// 
/// @brief Thread-safe statistics for chain processing
/// 
struct ChainStatistics {
    std::atomic<long> total_events{0};
    std::atomic<long> events_processed{0};
    std::atomic<long> events_skipped{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;

    void reset() noexcept {
        total_events = 0;
        events_processed = 0;
        events_skipped = 0;
        start_time = std::chrono::steady_clock::now();
    }

    [[nodiscard]] double elapsed_seconds() const noexcept {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        return elapsed.count() / 1000.0;
    }

    [[nodiscard]] double throughput() const noexcept {
        double elapsed = elapsed_seconds();
        return elapsed > 0 ? static_cast<double>(events_processed) / elapsed : 0.0;
    }
};

// Legacy alias
using chainstatistics = ChainStatistics;

//=============================================================================
// chain_event - Event wrapper with dictionary for bank access
//=============================================================================

/// 
/// @brief Event wrapper that provides bank access via dictionary
/// 
class chain_event {
public:
    chain_event() : m_event(nullptr), m_dict(nullptr) {}
    chain_event(event* ev, dictionary* dict) : m_event(ev), m_dict(dict) {}

    /// Get a bank by name, reading it from the event
    bank getBank(const std::string& name) {
        if (!m_event || !m_dict) {
            throw std::runtime_error("Invalid chain_event (no event data)");
        }
        bank b(m_dict->getSchema(name.c_str()));
        m_event->read(b);
        return b;
    }

    /// Alias for getBank (snake_case)
    bank get_bank(const std::string& name) { return getBank(name); }

    /// Read event data into an existing banklist
    void readBanks(banklist& list) {
        if (!m_event) {
            throw std::runtime_error("Invalid chain_event (no event data)");
        }
        for (auto& b : list) {
            m_event->read(b);
        }
    }

    /// Alias for readBanks (snake_case)
    void read_banks(banklist& list) { readBanks(list); }

    /// Access underlying event
    event* raw() { return m_event; }
    const event* raw() const { return m_event; }

    /// Check if valid
    explicit operator bool() const { return m_event != nullptr && m_dict != nullptr; }

private:
    event* m_event;
    dictionary* m_dict;
};

//=============================================================================
// ChainIterator - Iterator for sequential access across all files
//=============================================================================

class chain;  // Forward declaration

///
/// @brief Iterator for traversing events across all files in a chain
///
/// Provides sequential access to events with file and event index tracking.
///
class ChainIterator {
public:
    struct EventData {
        chain_event event;
        int file_index;
        long event_index;
    };

    using iterator_category = std::input_iterator_tag;
    using value_type = EventData;
    using difference_type = std::ptrdiff_t;
    using pointer = EventData*;
    using reference = EventData&;

    ChainIterator() : m_chain(nullptr), m_exhausted(true) {}

    ChainIterator(chain* ch, bool at_end);

    ChainIterator& operator++();
    void operator++(int) { ++(*this); }

    [[nodiscard]] EventData operator*() {
        return {chain_event(&m_current_event, m_current_dict.get()),
                m_current_file_idx, m_current_event_idx};
    }

    [[nodiscard]] bool operator==(const ChainIterator& other) const noexcept {
        if (m_exhausted && other.m_exhausted) return true;
        if (m_exhausted != other.m_exhausted) return false;
        return m_current_file_idx == other.m_current_file_idx &&
               m_current_event_idx == other.m_current_event_idx;
    }

    [[nodiscard]] bool operator!=(const ChainIterator& other) const noexcept {
        return !(*this == other);
    }

private:
    chain* m_chain;
    std::unique_ptr<reader> m_current_reader;
    std::unique_ptr<dictionary> m_current_dict;
    event m_current_event;
    int m_current_file_idx{0};
    long m_current_event_idx{0};
    bool m_exhausted{false};

    bool advance_to_next_event();
    bool open_next_file();
};

//=============================================================================
// chain - Main class for chaining multiple HIPO files
//=============================================================================

/// 
///  @brief Chain multiple HIPO files for unified processing
/// 
///  The chain class provides:
///  - File management (add, remove, pattern matching)
///  - Sequential iteration via range-based for loops
///  - Parallel processing with configurable thread pools
///  - Lazy metadata loading for minimal memory overhead
///  - Progress tracking and statistics
/// 
///  @code
///  hipo::chain ch(4);  // 4 threads
///  ch.add_pattern("data/*.hipo");
/// 
///  // Sequential iteration
///  for (auto& [event, file_idx, event_idx] : ch) {
///      auto particles = event.getBank("REC::Particle");
///  }
/// 
///  // Parallel processing
///  ch.process([](auto& event, int file_idx, long event_idx) {
///      // Thread-safe processing...
///  });
///  @endcode
///
class chain {
public:
    using iterator = ChainIterator;
    using const_iterator = ChainIterator;

    //=========================================================================
    // Construction
    //=========================================================================

    /// 
    ///  @brief Construct a chain with specified thread count
    ///  @param threads Number of threads (0 = auto-detect hardware concurrency)
    ///  @param progress Show progress bar during processing
    ///  @param verbose Enable verbose output
    /// 
    explicit chain(int threads = 0, bool progress = true, bool verbose = false)
        : m_num_threads(threads == 0 ? static_cast<int>(std::thread::hardware_concurrency()) : threads),
          m_show_progress(progress),
          m_verbose(verbose),
          m_thread_pool(m_num_threads, "ChainWorker") {
        if (m_verbose) {
            fmt::print("[chain] Initialized with {} threads\n", m_num_threads);
        }
    }

    //=========================================================================
    // File Management
    //=========================================================================

    /// 
    ///  @brief Add a single file to the chain
    ///  @param filename Path to the HIPO file
    ///  @return Number of files in chain after addition
    /// 
    int add(std::string_view filename) {
        if (!std::filesystem::exists(filename)) {
            fmt::print(stderr, "[chain] Warning: File not found: {}\n", filename);
            return static_cast<int>(m_files.size());
        }
        std::lock_guard lock(m_mutex);
        m_files.emplace_back(filename, static_cast<int>(m_files.size()));
        m_metadata_loaded = false;
        if (m_verbose) {
            fmt::print("[chain] Added file {}: {}\n", m_files.size(), filename);
        }
        return static_cast<int>(m_files.size());
    }

    /// 
    ///  @brief Add multiple files to the chain
    ///  @param filenames Vector of file paths
    ///  @return Number of files in chain after addition
    /// 
    int add(std::vector<std::string>& filenames) {
        for (const auto& f : filenames) add(f);
        return static_cast<int>(m_files.size());
    }

    /// 
    ///  @brief Add files matching a glob pattern
    ///  @param pattern Glob pattern (e.g., "data/*.hipo", "run_???.hipo")
    ///  @return Number of files matched and added
    /// 
    int add_pattern(std::string_view pattern) {
        int added = 0;
        std::filesystem::path pattern_path(pattern);
        std::filesystem::path dir = pattern_path.parent_path();
        if (dir.empty()) dir = ".";

        std::string filename_pattern = pattern_path.filename().string();
        std::string regex_pattern = std::regex_replace(filename_pattern,
            std::regex(R"(\*)"), ".*");
        regex_pattern = std::regex_replace(regex_pattern,
            std::regex(R"(\?)"), ".");

        try {
            std::regex file_regex(regex_pattern);
            std::vector<std::string> matched_files;

            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::string fname = entry.path().filename().string();
                    if (std::regex_match(fname, file_regex)) {
                        matched_files.push_back(entry.path().string());
                    }
                }
            }

            // Sort for deterministic ordering
            std::sort(matched_files.begin(), matched_files.end());
            for (const auto& f : matched_files) {
                add(f);
                added++;
            }
        } catch (const std::exception& e) {
            fmt::print(stderr, "[chain] Error scanning directory: {}\n", e.what());
        }

        if (m_verbose) {
            fmt::print("[chain] Pattern '{}' matched {} files\n", pattern, added);
        }
        return added;
    }

    /// 
    /// @brief Remove all files from the chain
    /// 
    void clear() {
        std::lock_guard lock(m_mutex);
        m_files.clear();
        m_stats.reset();
        m_metadata_loaded = false;
        if (m_verbose) fmt::print("[chain] Cleared all files\n");
    }

    //=========================================================================
    // File Information
    //=========================================================================

    /// Number of files in the chain
    [[nodiscard]] std::size_t size() const noexcept { return m_files.size(); }

    /// Check if chain is empty
    [[nodiscard]] bool empty() const noexcept { return m_files.empty(); }

    /// Legacy alias for size()
    [[nodiscard]] int get_nb_files() const noexcept {
        return static_cast<int>(m_files.size());
    }

    /// Get file info by index
    [[nodiscard]] const FileInfo& operator[](std::size_t index) const {
        if (index >= m_files.size()) {
            throw std::out_of_range("chain: File index out of range");
        }
        return m_files[index];
    }

    /// Legacy alias for operator[]
    [[nodiscard]] const FileInfo& file_info(int index) const {
        return (*this)[static_cast<std::size_t>(index)];
    }

    /// Get all file infos
    [[nodiscard]] std::vector<FileInfo>& files() noexcept {
        return m_files;
    }

    //=========================================================================
    // Configuration
    //=========================================================================

    /// Set event tags for filtering
    void set_tags(const std::vector<long>& tags) { m_tags = tags; }

    /// Set number of processing threads
    void set_threads(int n) {
        m_num_threads = (n == 0) ? static_cast<int>(std::thread::hardware_concurrency()) : n;
    }

    /// Enable/disable progress display
    void set_progress(bool show) { m_show_progress = show; }

    /// Enable/disable verbose output
    void set_verbose(bool verbose) { m_verbose = verbose; }

    //=========================================================================
    // Validation and Scanning
    //=========================================================================

    /// 
    /// @brief Validate and optionally load metadata for all files
    /// @param validate_all If true, open each file to validate; if false, only check existence
    /// @throws std::runtime_error if no valid files in chain
    ///
    void open(bool validate_all = true) {
        if (m_files.empty()) {
            throw std::runtime_error("chain: No files added to chain");
        }

        if (m_verbose) {
            fmt::print("[chain] Validating {} files...\n", m_files.size());
        }

        int valid_count = 0;
        long total_events = 0;

        for (auto& file : m_files) {
            if (!std::filesystem::exists(file.filename)) {
                file.is_valid = false;
                file.error_message = "File not found";
                continue;
            }

            file.file_size = std::filesystem::file_size(file.filename);

            if (validate_all) {
                try {
                    reader temp_reader;
                    temp_reader.setTags(m_tags);
                    temp_reader.open(file.filename.c_str());

                    file.total_events = temp_reader.getEntries();
                    file.num_records = temp_reader.getNRecords();
                    total_events += file.total_events;
                    valid_count++;

                    if (m_verbose) {
                        fmt::print("  [{}] {} - OK ({} events)\n",
                            file.index, file.filename, file.total_events);
                    }
                } catch (const std::exception& e) {
                    file.is_valid = false;
                    file.error_message = e.what();
                }
            } else {
                valid_count++;
            }
        }

        m_stats.total_events = total_events;
        m_metadata_loaded = validate_all;

        if (m_verbose) {
            fmt::print("[chain] Valid files: {}/{}, Total events: {}\n",
                valid_count, m_files.size(), total_events);
        }

        if (valid_count == 0) {
            throw std::runtime_error("chain: No valid files in chain");
        }
    }

    /// @brief Scan and display detailed information about all files
    void scan() {
        if (m_files.empty()) {
            fmt::print("[chain] No files in chain\n");
            return;
        }

        std::unique_ptr<ProgressTracker> progress;
        if (m_show_progress) {
            ProgressTracker::Config config;
            config.label = "Scanning files";
            config.show_eta = false;
            progress = std::make_unique<ProgressTracker>(m_files.size(), config);
            progress->start();
        }

        long total_events = 0;
        int total_records = 0;
        std::uintmax_t total_size = 0;
        int valid_files = 0;

        for (auto& file : m_files) {
            try {
                if (!std::filesystem::exists(file.filename)) {
                    std::lock_guard stdout_lock{detail::get_stdout_mutex()};
                    fmt::print("  [{}] {} - FILE NOT FOUND\n", file.index, file.filename);
                    continue;
                }

                file.file_size = std::filesystem::file_size(file.filename);
                total_size += file.file_size;

                reader temp_reader;
                temp_reader.setTags(m_tags);
                temp_reader.open(file.filename.c_str());

                file.num_records = temp_reader.getNRecords();
                file.total_events = temp_reader.getEntries();
                total_records += file.num_records;
                total_events += file.total_events;
                valid_files++;

                {
                    std::lock_guard stdout_lock{detail::get_stdout_mutex()};
                    fmt::print("  [{}] {}\n      Records: {}, Events: {}, Size: {}\n",
                        file.index, file.filename, file.num_records,
                        file.total_events, file.size_string());
                }

            } catch (const std::exception& e) {
                std::lock_guard stdout_lock{detail::get_stdout_mutex()};
                fmt::print("  [{}] {} - ERROR: {}\n", file.index, file.filename, e.what());
            }

            if (progress) progress->increment();
        }

        if (progress) progress->finish();

        fmt::print("\nScan Summary\n");
        fmt::print("  Total files:   {}\n", m_files.size());
        fmt::print("  Valid files:   {}\n", valid_files);
        fmt::print("  Total records: {}\n", total_records);
        fmt::print("  Total events:  {}\n", total_events);

        FileInfo dummy;
        dummy.file_size = total_size;
        fmt::print("  Total size:    {}\n", dummy.size_string());

        if (valid_files > 0) {
            fmt::print("  Avg events/file: {}\n", total_events / valid_files);
        }
    }

    /// Print list of files in chain
    void list() const {
        fmt::print("Chain file list ({} files)\n", m_files.size());
        for (const auto& file : m_files) {
            fmt::print("  [{}] {}", file.index, file.filename);
            if (file.metadata_loaded()) {
                fmt::print(" ({} events, {} records)", file.total_events, file.num_records);
            }
            if (!file.is_valid) {
                fmt::print(" [INVALID: {}]", file.error_message);
            }
            fmt::print("\n");
        }
    }

    //=========================================================================
    // Bank Creation
    //=========================================================================

    ///
    ///  @brief Create a banklist from bank names using the first file's dictionary
    ///
    ///  Creates bank objects with the correct schemas, ready to be filled
    ///  by chain_event::readBanks(). The chain must have at least one file added.
    ///
    banklist getBanks(const std::vector<std::string>& names) {
        if (m_files.empty()) {
            throw std::runtime_error("[chain] No files added, cannot create banklist");
        }
        reader temp_reader;
        temp_reader.open(m_files[0].filename.c_str());
        return temp_reader.getBanks(names);
    }

    /// Alias for getBanks (snake_case)
    banklist get_banks(const std::vector<std::string>& names) { return getBanks(names); }

    //=========================================================================
    // Range-based Iteration
    //=========================================================================

    /// 
    ///  @brief Get iterator to first event
    /// 
    ///  Enables range-based for loops:
    ///  @code
    ///  for (auto& [event, file_idx, event_idx] : chain) {
    ///      // Process event...
    ///  }
    ///  @endcode
    /// 
    [[nodiscard]] iterator begin() { return ChainIterator(this, false); }

    /// Get end iterator
    [[nodiscard]] iterator end() { return ChainIterator(this, true); }

    //=========================================================================
    // Parallel Processing
    //=========================================================================

    ///
    ///  @brief Process events in parallel across all files (record-level parallelism)
    ///
    ///  Uses record-level parallelism for efficient I/O: each thread grabs entire
    ///  records and processes all events within them sequentially. This minimizes
    ///  random access and maximizes cache efficiency.
    ///
    ///  @param process_func Callable with signature: void(chain_event&, int file_idx, long event_idx)
    ///  @param percentage Percentage of events to process (0-100)
    ///
    ///  @code
    ///  chain.process([](auto& event, int file_idx, long event_idx) {
    ///      auto particles = event.getBank("REC::Particle");
    ///      // Thread-safe processing...
    ///  });
    ///  @endcode
    ///
    template<typename ProcessFunc>
    void process(ProcessFunc&& process_func, double percentage = 100.0) {
        load_metadata_if_needed();
        long target = static_cast<long>(
            m_stats.total_events * std::clamp(percentage, 0.0, 100.0) / 100.0);
        process_impl(std::forward<ProcessFunc>(process_func), target,
                     fmt::format("Processing ({:.1f}%)", std::clamp(percentage, 0.0, 100.0)));
    }

    ///
    ///  @brief Process a specific number of events in parallel across all files
    ///
    ///  Same as process(func, percentage) but accepts an absolute event count.
    ///
    ///  @param process_func Callable with signature: void(chain_event&, int file_idx, long event_idx)
    ///  @param num_events Number of events to process
    ///
    ///  @code
    ///  chain.process([](auto& event, int file_idx, long event_idx) {
    ///      auto particles = event.getBank("REC::Particle");
    ///  }, 10000L);
    ///  @endcode
    ///
    template<typename ProcessFunc>
    void process(ProcessFunc&& process_func, long num_events) {
        load_metadata_if_needed();
        long target = std::min(num_events, static_cast<long>(m_stats.total_events.load()));
        process_impl(std::forward<ProcessFunc>(process_func), target,
                     fmt::format("Processing ({} events)", target));
    }

    ///
    ///  @brief Process events in parallel using a banklist (record-level parallelism)
    ///
    ///  Each thread gets its own copy of the banklist for thread safety. For each event,
    ///  the event data is read into the thread-local banklist before calling the user function.
    ///
    ///  @param banks Template banklist (created via chain::getBanks); each thread copies it
    ///  @param process_func Callable with signature: void(banklist&, int file_idx, long event_idx)
    ///  @param percentage Percentage of events to process (0-100)
    ///
    ///  @code
    ///  hipo::banklist banks = chain.getBanks({"REC::Particle", "REC::Event"});
    ///  auto b_particle = hipo::getBanklistIndex(banks, "REC::Particle");
    ///  chain.process(banks, [b_particle](auto& banks, int file_idx, long event_idx) {
    ///      for (auto const& row : banks[b_particle].getRowList()) {
    ///          // process...
    ///      }
    ///  });
    ///  @endcode
    ///
    template<typename ProcessFunc>
    void process(const banklist& banks, ProcessFunc&& process_func, double percentage = 100.0) {
        if (m_files.empty()) {
            fmt::print(stderr, "[chain] Warning: No files to process.\n");
            return;
        }

        percentage = std::clamp(percentage, 0.0, 100.0) / 100.0;

        // Ensure metadata is loaded
        load_metadata_if_needed();

        long target_events = static_cast<long>(m_stats.total_events * percentage);

        // Build work list
        struct WorkItem {
            int file_idx;
            int num_records;
            long max_events;
        };
        std::vector<WorkItem> work_list;
        long events_accumulated = 0;

        for (const auto& file : m_files) {
            if (!file.is_valid || events_accumulated >= target_events) continue;
            long events_needed = target_events - events_accumulated;
            long events_from_file = std::min(events_needed, file.total_events);
            if (events_from_file > 0) {
                work_list.push_back({file.index, file.num_records, events_from_file});
                events_accumulated += events_from_file;
            }
        }

        m_stats.reset();
        m_stats.total_events = events_accumulated;

        if (m_verbose) {
            fmt::print("[chain] Processing {} events from {} files ({} threads, record-level parallelism, banklist)\n",
                events_accumulated, work_list.size(), m_num_threads);
        }

        std::unique_ptr<ProgressTracker> progress;
        if (m_show_progress && m_stats.total_events > 0) {
            ProgressTracker::Config config;
            config.label = fmt::format("Processing ({:.1f}%)", percentage * 100.0);
            config.show_eta = true;
            config.show_rate = true;
            progress = std::make_unique<ProgressTracker>(m_stats.total_events, config);
            progress->start();
        }

        // Process one file at a time with all threads using record-level parallelism
        for (const auto& work : work_list) {
            const auto& file = m_files[work.file_idx];
            std::atomic<int> next_record{0};
            std::atomic<long> events_processed_in_file{0};
            std::vector<std::future<void>> futures;

            for (int t = 0; t < m_num_threads; t++) {
                futures.push_back(m_thread_pool.submit([&, this]() {
                    try {
                        // Each thread has its own reader, record buffer, and banklist copy
                        reader file_reader;
                        file_reader.setTags(m_tags);
                        file_reader.open(file.filename.c_str());

                        record rec;
                        event temp_event;
                        banklist thread_banks = banks; // thread-local copy

                        while (true) {
                            int rec_idx = next_record.fetch_add(1);
                            if (rec_idx >= work.num_records) break;

                            if (events_processed_in_file.load() >= work.max_events) break;

                            if (!file_reader.loadRecord(rec, rec_idx)) continue;

                            int events_in_record = rec.getEventCount();

                            for (int evt_in_rec = 0; evt_in_rec < events_in_record; evt_in_rec++) {
                                long current_count = events_processed_in_file.fetch_add(1);
                                if (current_count >= work.max_events) {
                                    events_processed_in_file.fetch_sub(1);
                                    break;
                                }

                                rec.readHipoEvent(temp_event, evt_in_rec);
                                for (auto& b : thread_banks) {
                                    temp_event.read(b);
                                }
                                process_func(thread_banks, file.index, current_count);
                                m_stats.events_processed++;
                                if (progress) progress->increment();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::lock_guard stdout_lock{detail::get_stdout_mutex()};
                        fmt::print(stderr, "[chain] Thread error: {}\n", e.what());
                    }
                }));
            }

            for (auto& f : futures) f.get();
        }

        m_stats.end_time = std::chrono::steady_clock::now();
        if (progress) progress->finish();
        if (m_verbose) print_statistics();
    }

    ///
    ///  @brief Process events with bank filtering (record-level parallelism)
    /// 
    ///  Only processes events containing all specified banks. Uses record-level
    ///  parallelism for efficient I/O.
    /// 
    ///  @param process_func Callable with signature: void(chain_event&, int file_idx, long event_idx)
    ///  @param required_banks List of bank names that must be present
    ///  @param percentage Percentage of matching events to process
    /// 
    template<typename ProcessFunc>
    void process_filtered(ProcessFunc&& process_func,
                          const std::vector<std::string>& required_banks,
                          double percentage = 100.0) {
        load_metadata_if_needed();
        long target = static_cast<long>(
            m_stats.total_events * std::clamp(percentage, 0.0, 100.0) / 100.0);
        process_filtered_impl(std::forward<ProcessFunc>(process_func),
                              required_banks, target);
    }

    ///
    ///  @brief Process filtered events with an absolute event count
    ///
    ///  Same as process_filtered(func, banks, percentage) but accepts an absolute
    ///  count of matching events to process.
    ///
    ///  @param process_func Callable with signature: void(chain_event&, int file_idx, long event_idx)
    ///  @param required_banks List of bank names that must be present
    ///  @param num_events Number of matching events to process
    ///
    template<typename ProcessFunc>
    void process_filtered(ProcessFunc&& process_func,
                          const std::vector<std::string>& required_banks,
                          long num_events) {
        load_metadata_if_needed();
        process_filtered_impl(std::forward<ProcessFunc>(process_func),
                              required_banks, num_events);
    }

    ///
    ///  @brief Apply a function to each file (for file-level operations)
    ///
    ///  @param func Callable with signature: void(reader&, const FileInfo&)
    ///
    template<typename FileFunc>
    void for_each_file(FileFunc&& func) {
        for (const auto& file : m_files) {
            if (!file.is_valid) continue;
            try {
                reader file_reader;
                file_reader.setTags(m_tags);
                file_reader.open(file.filename.c_str());
                func(file_reader, file);
            } catch (const std::exception& e) {
                fmt::print(stderr, "[chain] Error processing {}: {}\n",
                    file.filename, e.what());
            }
        }
    }

    //=========================================================================
    // Statistics and Information
    //=========================================================================

    /// Get processing statistics
    [[nodiscard]] const ChainStatistics& statistics() const noexcept { return m_stats; }

    /// Get total event count (loads metadata if needed)
    [[nodiscard]] long total_events() {
        load_metadata_if_needed();
        return m_stats.total_events;
    }

    /// Legacy alias
    [[nodiscard]] long total_events_count() const { return m_stats.total_events.load(); }

    /// Print processing statistics
    void print_statistics() const {
        fmt::print("\nChain Statistics\n");
        fmt::print("  Files:            {}\n", m_files.size());
        fmt::print("  Total events:     {}\n", m_stats.total_events.load());
        fmt::print("  Events processed: {}\n", m_stats.events_processed.load());
        fmt::print("  Processing time:  {:.2f} seconds\n", m_stats.elapsed_seconds());
        if (m_stats.elapsed_seconds() > 0) {
            fmt::print("  Throughput:       {:.1f} events/sec\n", m_stats.throughput());
        }
    }

    /// Show detailed info for all files
    void show_all_info() {
        fmt::print("\nChain File Info ({} files)\n", m_files.size());
        fmt::print("================\n");
        for (const auto& file : m_files) {
            fmt::print("\nFile [{}]: {}\n", file.index, file.filename);
            try {
                reader temp_reader;
                temp_reader.setTags(m_tags);
                temp_reader.open(file.filename.c_str());
                temp_reader.about();
            } catch (const std::exception& e) {
                fmt::print("  Error: {}\n", e.what());
            }
        }
    }

    //=========================================================================
    // Configuration Access
    //=========================================================================

    /// Check if any file has a configuration key
    [[nodiscard]] bool any_has_config(std::string_view name) {
        for (const auto& file : m_files) {
            if (!file.is_valid) continue;
            try {
                reader temp_reader;
                temp_reader.setTags(m_tags);
                temp_reader.open(file.filename.c_str());

                std::map<std::string, std::string> config;
                temp_reader.readUserConfig(config);
                if (config.find(std::string(name)) != config.end()) {
                    return true;
                }
            } catch (...) {}
        }
        return false;
    }

    /// Get configuration from first file that has it
    [[nodiscard]] std::optional<std::string> get_config(std::string_view name) {
        for (const auto& file : m_files) {
            if (!file.is_valid) continue;
            try {
                reader temp_reader;
                temp_reader.setTags(m_tags);
                temp_reader.open(file.filename.c_str());

                std::map<std::string, std::string> config;
                temp_reader.readUserConfig(config);
                auto it = config.find(std::string(name));
                if (it != config.end()) {
                    return it->second;
                }
            } catch (...) {}
        }
        return std::nullopt;
    }

    //=========================================================================
    // Advanced Access
    //=========================================================================

    /// Get the thread pool for advanced use
    [[nodiscard]] ThreadPool& threadpool() noexcept { return m_thread_pool; }

private:
    //=========================================================================
    // Private Members
    //=========================================================================

    std::vector<FileInfo> m_files;
    std::vector<long> m_tags;
    int m_num_threads;
    bool m_show_progress;
    bool m_verbose;
    bool m_metadata_loaded{false};
    ThreadPool m_thread_pool;
    ChainStatistics m_stats;
    mutable std::mutex m_mutex;

    //=========================================================================
    // Private Methods
    //=========================================================================

    /// Load metadata for all files if not already loaded
    void load_metadata_if_needed() {
        if (m_metadata_loaded) return;

        if (m_verbose) fmt::print("[chain] Loading file metadata...\n");

        long total = 0;
        for (auto& file : m_files) {
            if (!file.is_valid) continue;
            if (file.total_events < 0) {
                try {
                    reader temp_reader;
                    temp_reader.setTags(m_tags);
                    temp_reader.open(file.filename.c_str());

                    file.total_events = temp_reader.getEntries();
                    file.num_records = temp_reader.getNRecords();
                } catch (const std::exception& e) {
                    file.is_valid = false;
                    file.error_message = e.what();
                    file.total_events = 0;
                }
            }
            total += file.total_events;
        }
        m_stats.total_events = total;
        m_metadata_loaded = true;
    }

    /// Implementation for process() overloads
    template<typename ProcessFunc>
    void process_impl(ProcessFunc&& process_func, long target_events,
                      const std::string& label) {
        if (m_files.empty()) {
            fmt::print(stderr, "[chain] Warning: No files to process.\n");
            return;
        }

        // Build work list - tracking records per file
        struct WorkItem {
            int file_idx;
            int num_records;
            long max_events;
        };
        std::vector<WorkItem> work_list;
        long events_accumulated = 0;

        for (const auto& file : m_files) {
            if (!file.is_valid || events_accumulated >= target_events) continue;
            long events_needed = target_events - events_accumulated;
            long events_from_file = std::min(events_needed, file.total_events);
            if (events_from_file > 0) {
                work_list.push_back({file.index, file.num_records, events_from_file});
                events_accumulated += events_from_file;
            }
        }

        m_stats.reset();
        m_stats.total_events = events_accumulated;

        if (m_verbose) {
            fmt::print("[chain] Processing {} events from {} files ({} threads, record-level parallelism)\n",
                events_accumulated, work_list.size(), m_num_threads);
        }

        std::unique_ptr<ProgressTracker> progress;
        if (m_show_progress && m_stats.total_events > 0) {
            ProgressTracker::Config config;
            config.label = label;
            config.show_eta = true;
            config.show_rate = true;
            progress = std::make_unique<ProgressTracker>(m_stats.total_events, config);
            progress->start();
        }

        for (const auto& work : work_list) {
            const auto& file = m_files[work.file_idx];
            std::atomic<int> next_record{0};
            std::atomic<long> events_processed_in_file{0};
            std::vector<std::future<void>> futures;

            for (int t = 0; t < m_num_threads; t++) {
                futures.push_back(m_thread_pool.submit([&, this]() {
                    try {
                        reader file_reader;
                        file_reader.setTags(m_tags);
                        file_reader.open(file.filename.c_str());

                        dictionary dict;
                        file_reader.readDictionary(dict);

                        record rec;
                        event temp_event;

                        while (true) {
                            int rec_idx = next_record.fetch_add(1);
                            if (rec_idx >= work.num_records) break;
                            if (events_processed_in_file.load() >= work.max_events) break;

                            if (!file_reader.loadRecord(rec, rec_idx)) continue;

                            int events_in_record = rec.getEventCount();

                            for (int evt_in_rec = 0; evt_in_rec < events_in_record; evt_in_rec++) {
                                long current_count = events_processed_in_file.fetch_add(1);
                                if (current_count >= work.max_events) {
                                    events_processed_in_file.fetch_sub(1);
                                    break;
                                }

                                rec.readHipoEvent(temp_event, evt_in_rec);
                                chain_event evt(&temp_event, &dict);
                                process_func(evt, file.index, current_count);
                                m_stats.events_processed++;
                                if (progress) progress->increment();
                            }
                        }
                    } catch (const std::exception& e) {
                        std::lock_guard stdout_lock{detail::get_stdout_mutex()};
                        fmt::print(stderr, "[chain] Thread error: {}\n", e.what());
                    }
                }));
            }

            for (auto& f : futures) f.get();
        }

        m_stats.end_time = std::chrono::steady_clock::now();
        if (progress) progress->finish();
        if (m_verbose) print_statistics();
    }

    /// Implementation for process_filtered() overloads
    template<typename ProcessFunc>
    void process_filtered_impl(ProcessFunc&& process_func,
                               const std::vector<std::string>& required_banks,
                               long target_events) {
        if (m_files.empty()) {
            fmt::print(stderr, "[chain] Warning: No files to process.\n");
            return;
        }

        m_stats.reset();
        m_stats.total_events = target_events;

        if (m_verbose) {
            fmt::print("[chain] Filtered processing: requiring banks [");
            for (size_t i = 0; i < required_banks.size(); i++) {
                fmt::print("{}{}", required_banks[i],
                    i < required_banks.size() - 1 ? ", " : "");
            }
            fmt::print("] (record-level parallelism)\n");
        }

        std::unique_ptr<ProgressTracker> progress;
        if (m_show_progress && target_events > 0) {
            ProgressTracker::Config config;
            config.label = "Filtered processing";
            config.show_eta = true;
            progress = std::make_unique<ProgressTracker>(target_events, config);
            progress->start();
        }

        std::atomic<long> events_found{0};

        for (const auto& file : m_files) {
            if (!file.is_valid || events_found >= target_events) continue;

            std::atomic<int> next_record{0};
            std::vector<std::future<void>> futures;

            for (int t = 0; t < m_num_threads && events_found < target_events; t++) {
                futures.push_back(m_thread_pool.submit([&, this]() {
                    try {
                        reader file_reader;
                        file_reader.setTags(m_tags);
                        file_reader.open(file.filename.c_str());

                        dictionary dict;
                        file_reader.readDictionary(dict);

                        std::vector<bank> banks;
                        banks.reserve(required_banks.size());
                        for (const auto& name : required_banks) {
                            banks.emplace_back(dict.getSchema(name.c_str()));
                        }

                        record rec;
                        event temp_event;

                        while (events_found < target_events) {
                            int rec_idx = next_record.fetch_add(1);
                            if (rec_idx >= file.num_records) break;

                            if (!file_reader.loadRecord(rec, rec_idx)) continue;

                            int events_in_record = rec.getEventCount();

                            for (int evt_in_rec = 0; evt_in_rec < events_in_record; evt_in_rec++) {
                                if (events_found >= target_events) break;

                                rec.readHipoEvent(temp_event, evt_in_rec);

                                bool has_all_banks = true;
                                for (auto& b : banks) {
                                    temp_event.read(b);
                                    if (b.getRows() == 0) {
                                        has_all_banks = false;
                                        break;
                                    }
                                }

                                if (has_all_banks) {
                                    long evt_count = events_found.fetch_add(1);
                                    if (evt_count >= target_events) break;

                                    chain_event evt(&temp_event, &dict);
                                    process_func(evt, file.index, evt_count);
                                    m_stats.events_processed++;
                                    if (progress) progress->increment();
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        std::lock_guard stdout_lock{detail::get_stdout_mutex()};
                        fmt::print(stderr, "[chain] Thread error: {}\n", e.what());
                    }
                }));
            }
            for (auto& f : futures) f.get();
        }

        m_stats.end_time = std::chrono::steady_clock::now();
        if (progress) progress->finish();
        if (m_verbose) {
            fmt::print("[chain] Found {} matching events\n", events_found.load());
            print_statistics();
        }
    }

    // Allow ChainIterator to access private members
    friend class ChainIterator;
};

//=============================================================================
// ChainIterator Implementation
//=============================================================================

inline ChainIterator::ChainIterator(chain* ch, bool at_end)
    : m_chain(ch), m_exhausted(at_end) {
    if (!at_end && ch && !ch->m_files.empty()) {
        if (open_next_file()) {
            advance_to_next_event();
        } else {
            m_exhausted = true;
        }
    }
}

inline ChainIterator& ChainIterator::operator++() {
    if (!m_exhausted) {
        m_current_event_idx++;
        if (!advance_to_next_event()) {
            m_exhausted = true;
        }
    }
    return *this;
}

inline bool ChainIterator::advance_to_next_event() {
    while (m_current_reader) {
        if (m_current_reader->next(m_current_event)) {
            return true;
        }
        // Current file exhausted, try next
        m_current_file_idx++;
        m_current_event_idx = 0;
        if (!open_next_file()) {
            return false;
        }
    }
    return false;
}

inline bool ChainIterator::open_next_file() {
    while (m_current_file_idx < static_cast<int>(m_chain->m_files.size())) {
        const auto& file = m_chain->m_files[m_current_file_idx];
        if (!file.is_valid) {
            m_current_file_idx++;
            continue;
        }
        try {
            m_current_reader = std::make_unique<reader>();
            m_current_reader->setTags(m_chain->m_tags);
            m_current_reader->open(file.filename.c_str());

            m_current_dict = std::make_unique<dictionary>();
            m_current_reader->readDictionary(*m_current_dict);
            return true;
        } catch (const std::exception& e) {
            fmt::print(stderr, "[chain] Error opening {}: {}\n",
                file.filename, e.what());
            m_current_file_idx++;
        }
    }
    m_current_reader.reset();
    m_current_dict.reset();
    return false;
}

}  // namespace hipo
