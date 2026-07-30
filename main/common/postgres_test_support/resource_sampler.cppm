module;

#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

export module bioseq.test.postgres:resource_sampler;

import :supervision;

export namespace bioseq::postgres_test {

/// @brief Exact sample taken at a caller-defined workload phase boundary.
struct resource_phase_boundary {
    std::uint64_t ordinal{};
    std::string phase;
    std::chrono::milliseconds elapsed{};
    std::optional<std::uint64_t> postmaster_process_tree_rss_bytes;
    std::optional<std::uint64_t> owned_root_bytes;
};

/// @brief Process-tree and marker-owned filesystem sampling evidence.
struct resource_sampling_report {
    std::uint64_t peak_postmaster_process_tree_rss_bytes{};
    std::uint64_t peak_owned_root_bytes{};
    std::uint64_t rss_samples{};
    std::uint64_t owned_root_samples{};
    std::uint64_t rss_sampling_failures{};
    std::uint64_t owned_root_sampling_failures{};
    std::vector<resource_phase_boundary> phase_boundaries;
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::resource_detail {

struct process_entry {
    std::uint64_t pid{};
    std::uint64_t parent_pid{};
    std::uint64_t rss_bytes{};
};

struct process_snapshot_result {
    std::vector<process_entry> processes;
    bool complete{};
};

#if defined(__linux__)

[[nodiscard]] std::optional<process_entry> linux_process_entry(
    const std::filesystem::path& stat_path,
    std::uint64_t pid,
    std::uint64_t page_size) {
    std::ifstream input(stat_path);
    std::string line;
    if (!input || !std::getline(input, line)) {
        return std::nullopt;
    }
    const std::size_t command_end = line.rfind(')');
    if (command_end == std::string::npos
        || command_end + 2 >= line.size()) {
        return std::nullopt;
    }
    std::istringstream fields(line.substr(command_end + 2));
    char state{};
    std::uint64_t parent_pid{};
    fields >> state >> parent_pid;
    std::int64_t ignored{};
    for (int field = 5; field <= 23 && fields; ++field) {
        fields >> ignored;
    }
    std::int64_t rss_pages{};
    fields >> rss_pages;
    if (!fields || rss_pages < 0) {
        return std::nullopt;
    }
    const std::uint64_t pages =
        static_cast<std::uint64_t>(rss_pages);
    if (page_size != 0
        && pages > std::numeric_limits<std::uint64_t>::max() / page_size) {
        return std::nullopt;
    }
    return process_entry{
        .pid = pid,
        .parent_pid = parent_pid,
        .rss_bytes = pages * page_size};
}

[[nodiscard]] process_snapshot_result process_snapshot(
    std::uint64_t postmaster_pid) {
    const long native_page_size = ::sysconf(_SC_PAGESIZE);
    if (native_page_size <= 0) {
        return {};
    }
    const std::uint64_t page_size =
        static_cast<std::uint64_t>(native_page_size);
    std::vector<process_entry> processes;
    std::vector<std::uint64_t> pending{postmaster_pid};
    std::unordered_set<std::uint64_t> observed{postmaster_pid};
    bool complete = true;
    while (!pending.empty()) {
        const std::uint64_t pid = pending.back();
        pending.pop_back();
        const std::filesystem::path process_root =
            std::filesystem::path("/proc") / std::to_string(pid);
        if (const std::optional<process_entry> process =
                linux_process_entry(
                    process_root / "stat", pid, page_size)) {
            processes.push_back(*process);
        } else {
            complete = false;
        }
        std::ifstream children(
            process_root / "task" / std::to_string(pid) / "children");
        if (!children) {
            complete = false;
            continue;
        }
        std::uint64_t child{};
        while (children >> child) {
            if (child > 1 && observed.insert(child).second) {
                pending.push_back(child);
            }
        }
        if (!children.eof()) {
            complete = false;
        }
    }
    return {
        .processes = std::move(processes),
        .complete = complete};
}

#elif defined(__APPLE__)

[[nodiscard]] process_snapshot_result process_snapshot(
    std::uint64_t postmaster_pid) {
    const int process_capacity = ::proc_listallpids(nullptr, 0);
    if (process_capacity <= 0) {
        return {};
    }
    std::vector<pid_t> child_buffer(
        static_cast<std::size_t>(process_capacity) + 32U);
    std::vector<process_entry> processes;
    std::vector<std::uint64_t> pending{postmaster_pid};
    std::unordered_set<std::uint64_t> observed{postmaster_pid};
    bool complete = true;
    while (!pending.empty()) {
        const std::uint64_t unsigned_pid = pending.back();
        pending.pop_back();
        const pid_t pid = static_cast<pid_t>(unsigned_pid);
        proc_bsdinfo basic{};
        proc_taskinfo task{};
        if (::proc_pidinfo(
                pid, PROC_PIDTBSDINFO, 0, &basic, sizeof(basic))
                != static_cast<int>(sizeof(basic))
            || ::proc_pidinfo(
                   pid, PROC_PIDTASKINFO, 0, &task, sizeof(task))
                != static_cast<int>(sizeof(task))) {
            complete = false;
            continue;
        }
        processes.push_back(process_entry{
            .pid = unsigned_pid,
            .parent_pid = static_cast<std::uint64_t>(basic.pbi_ppid),
            .rss_bytes =
                static_cast<std::uint64_t>(task.pti_resident_size)});
        const int child_count = ::proc_listchildpids(
            pid, child_buffer.data(),
            static_cast<int>(child_buffer.size() * sizeof(pid_t)));
        if (child_count < 0
            || static_cast<std::size_t>(child_count)
                > child_buffer.size()) {
            complete = false;
            continue;
        }
        for (int index = 0; index < child_count; ++index) {
            const pid_t child =
                child_buffer[static_cast<std::size_t>(index)];
            if (child > 1
                && observed.insert(
                       static_cast<std::uint64_t>(child)).second) {
                pending.push_back(static_cast<std::uint64_t>(child));
            }
        }
    }
    return {
        .processes = std::move(processes),
        .complete = complete};
}

#else

[[nodiscard]] process_snapshot_result process_snapshot(std::uint64_t) {
    return {};
}

#endif

[[nodiscard]] std::optional<std::uint64_t> process_tree_rss_bytes(
    std::uint64_t postmaster_pid) {
    process_snapshot_result snapshot = process_snapshot(postmaster_pid);
    if (!snapshot.complete) {
        return std::nullopt;
    }
    const bool root_present = std::any_of(
        snapshot.processes.begin(), snapshot.processes.end(),
        [postmaster_pid](const process_entry& process) {
            return process.pid == postmaster_pid;
        });
    if (!root_present) {
        return std::nullopt;
    }
    std::uint64_t total{};
    for (const process_entry& process : snapshot.processes) {
        if (process.rss_bytes
            > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::nullopt;
        }
        total += process.rss_bytes;
    }
    return total;
}

[[nodiscard]] std::optional<std::uint64_t> strict_tree_bytes(
    const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        return std::nullopt;
    }
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, error);
    if (error) {
        return std::nullopt;
    }
    const std::filesystem::recursive_directory_iterator end;
    std::uint64_t total{};
    while (iterator != end) {
        const bool regular = iterator->is_regular_file(error);
        if (error) {
            return std::nullopt;
        }
        if (regular) {
            const std::uintmax_t bytes = iterator->file_size(error);
            if (error
                || bytes
                    > std::numeric_limits<std::uint64_t>::max() - total) {
                return std::nullopt;
            }
            total += static_cast<std::uint64_t>(bytes);
        }
        iterator.increment(error);
        if (error) {
            return std::nullopt;
        }
    }
    return total;
}

void update_peak(
    std::uint64_t& peak, std::uint64_t value) noexcept {
    peak = std::max(peak, value);
}

[[nodiscard]] inline std::mutex& journal_mutex() {
    static std::mutex mutex;
    return mutex;
}

void append_journal(
    std::string_view kind,
    std::optional<std::uint64_t> value = std::nullopt,
    std::string_view phase = {},
    std::chrono::milliseconds elapsed = {},
    std::optional<std::uint64_t> boundary_owned_root_bytes =
        std::nullopt) noexcept {
    try {
        const auto directory = supervision_state_directory();
        if (!directory) {
            return;
        }
        std::scoped_lock lock(journal_mutex());
        std::ofstream output(
            *directory / "resources.tsv", std::ios::app);
        output << kind << '\t';
        if (value) {
            output << *value;
        } else {
            output << '-';
        }
        output << '\t' << supervision_hex_encode(phase)
               << '\t' << elapsed.count();
        if (kind == "phase") {
            output << '\t';
            if (boundary_owned_root_bytes) {
                output << *boundary_owned_root_bytes;
            } else {
                output << '-';
            }
        }
        output << '\n';
        output.flush();
    } catch (...) {
    }
}

}  // namespace bioseq::postgres_test::resource_detail

export namespace bioseq::postgres_test {

/// @brief Reads the postmaster PID from an initialized, running PGDATA.
[[nodiscard]] inline std::uint64_t read_postmaster_pid_file(
    const std::filesystem::path& data_directory) {
    std::ifstream input(data_directory / "postmaster.pid");
    std::uint64_t pid{};
    input >> pid;
    if (!input || pid <= 1) {
        throw std::runtime_error(
            "cannot identify the private PostgreSQL postmaster");
    }
    return pid;
}

/// @brief Samples a postmaster and all descendants as one RSS observation.
[[nodiscard]] inline std::optional<std::uint64_t>
sample_postmaster_process_tree_rss(std::uint64_t postmaster_pid) {
    return resource_detail::process_tree_rss_bytes(postmaster_pid);
}

/// @brief Returns a complete postmaster process tree for watchdog cleanup.
[[nodiscard]] inline std::optional<std::vector<std::uint64_t>>
sample_postmaster_process_tree_pids(std::uint64_t postmaster_pid) {
    resource_detail::process_snapshot_result snapshot =
        resource_detail::process_snapshot(postmaster_pid);
    if (!snapshot.complete) {
        return std::nullopt;
    }
    std::vector<std::uint64_t> pids;
    pids.reserve(snapshot.processes.size());
    for (const resource_detail::process_entry& process :
         snapshot.processes) {
        pids.push_back(process.pid);
    }
    return pids;
}

/// @brief Samples all regular-file bytes under one marker-owned root.
[[nodiscard]] inline std::optional<std::uint64_t>
sample_owned_root_bytes(const std::filesystem::path& root) {
    return resource_detail::strict_tree_bytes(root);
}

/// @brief Truncates crash-resilient resource evidence for one case.
inline void initialize_supervision_resource_journal() noexcept {
    try {
        const auto directory = supervision_state_directory();
        if (!directory) {
            return;
        }
        std::scoped_lock lock(resource_detail::journal_mutex());
        std::ofstream output(
            *directory / "resources.tsv", std::ios::trunc);
        output.close();
    } catch (...) {
    }
}

/// @brief Loads complete samples recorded before runner termination.
[[nodiscard]] inline resource_sampling_report
load_supervision_resource_report(
    const std::filesystem::path& directory) {
    resource_sampling_report report;
    std::ifstream input(directory / "resources.tsv");
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t first = line.find('\t');
        const std::size_t second =
            first == std::string::npos
            ? std::string::npos : line.find('\t', first + 1U);
        const std::size_t third =
            second == std::string::npos
            ? std::string::npos : line.find('\t', second + 1U);
        const std::size_t fourth =
            third == std::string::npos
            ? std::string::npos : line.find('\t', third + 1U);
        if (first == std::string::npos || second == std::string::npos
            || third == std::string::npos) {
            continue;
        }
        const std::string_view kind =
            std::string_view(line).substr(0, first);
        const std::string_view value_text =
            std::string_view(line).substr(
                first + 1U, second - first - 1U);
        const std::string_view phase_text =
            std::string_view(line).substr(
                second + 1U, third - second - 1U);
        const std::string_view elapsed_text =
            std::string_view(line).substr(
                third + 1U,
                fourth == std::string::npos
                    ? std::string::npos : fourth - third - 1U);
        const std::string_view boundary_owned_root_text =
            fourth == std::string::npos
            ? std::string_view{}
            : std::string_view(line).substr(fourth + 1U);
        std::optional<std::uint64_t> value;
        if (value_text != "-") {
            std::uint64_t parsed{};
            const auto [end, error] = std::from_chars(
                value_text.data(),
                value_text.data() + value_text.size(), parsed);
            if (error != std::errc{}
                || end != value_text.data() + value_text.size()) {
                continue;
            }
            value = parsed;
        }
        if (kind == "rss") {
            if (value) {
                ++report.rss_samples;
                report.peak_postmaster_process_tree_rss_bytes =
                    std::max(
                        report.peak_postmaster_process_tree_rss_bytes,
                        *value);
            } else {
                ++report.rss_sampling_failures;
            }
        } else if (kind == "root") {
            if (value) {
                ++report.owned_root_samples;
                report.peak_owned_root_bytes =
                    std::max(report.peak_owned_root_bytes, *value);
            } else {
                ++report.owned_root_sampling_failures;
            }
        } else if (kind == "phase") {
            const auto phase = supervision_hex_decode(phase_text);
            std::int64_t elapsed{};
            const auto [end, error] = std::from_chars(
                elapsed_text.data(),
                elapsed_text.data() + elapsed_text.size(), elapsed);
            if (!phase || error != std::errc{}
                || end != elapsed_text.data() + elapsed_text.size()) {
                continue;
            }
            std::optional<std::uint64_t> boundary_owned_root_bytes;
            if (!boundary_owned_root_text.empty()
                && boundary_owned_root_text != "-") {
                std::uint64_t parsed{};
                const auto [root_end, root_error] = std::from_chars(
                    boundary_owned_root_text.data(),
                    boundary_owned_root_text.data()
                        + boundary_owned_root_text.size(),
                    parsed);
                if (root_error != std::errc{}
                    || root_end
                        != boundary_owned_root_text.data()
                            + boundary_owned_root_text.size()) {
                    continue;
                }
                boundary_owned_root_bytes = parsed;
            }
            report.phase_boundaries.push_back(resource_phase_boundary{
                .ordinal = report.phase_boundaries.size(),
                .phase = *phase,
                .elapsed = std::chrono::milliseconds(elapsed),
                .postmaster_process_tree_rss_bytes = value,
                .owned_root_bytes = boundary_owned_root_bytes});
        }
    }
    return report;
}

/// @brief Continuously samples one fixture until finish or destruction.
class resource_sampler {
public:
    resource_sampler(
        std::filesystem::path owned_root,
        const std::filesystem::path& data_directory,
        std::chrono::milliseconds rss_interval =
            std::chrono::milliseconds(50),
        std::chrono::milliseconds root_interval =
            std::chrono::seconds(1))
        : owned_root_(std::move(owned_root)),
          data_directory_(data_directory),
          rss_interval_(rss_interval),
          root_interval_(root_interval),
          started_(std::chrono::steady_clock::now()) {
        if (rss_interval_ <= std::chrono::milliseconds::zero()
            || root_interval_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "resource sampling intervals must be positive");
        }
        static_cast<void>(
            read_postmaster_pid_file(data_directory_));
        sample_rss_once();
        sample_root_once();
        rss_thread_ = std::jthread(
            [this](std::stop_token token) noexcept {
                sample_loop(token, rss_interval_, true);
            });
        root_thread_ = std::jthread(
            [this](std::stop_token token) noexcept {
                sample_loop(token, root_interval_, false);
            });
    }

    resource_sampler(const resource_sampler&) = delete;
    resource_sampler& operator=(const resource_sampler&) = delete;

    ~resource_sampler() noexcept {
        try {
            static_cast<void>(finish());
        } catch (...) {
        }
    }

    /// @brief Forces both samples and records their phase association.
    void observe_phase_boundary(std::string_view phase) noexcept {
        const std::optional<std::uint64_t> rss = sample_rss_once();
        const std::optional<std::uint64_t> root = sample_root_once();
        try {
            std::scoped_lock lock(mutex_);
            report_.phase_boundaries.push_back(resource_phase_boundary{
                .ordinal = next_phase_ordinal_++,
                .phase = std::string(phase),
                .elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started_),
                .postmaster_process_tree_rss_bytes = rss,
                .owned_root_bytes = root});
            resource_detail::append_journal(
                "phase", rss, phase,
                report_.phase_boundaries.back().elapsed, root);
        } catch (...) {
        }
    }

    /// @brief Stops workers and returns all evidence collected so far.
    [[nodiscard]] resource_sampling_report finish() {
        {
            std::scoped_lock lock(mutex_);
            if (finished_) {
                return report_;
            }
            finished_ = true;
        }
        rss_thread_.request_stop();
        root_thread_.request_stop();
        wake_.notify_all();
        if (rss_thread_.joinable()) {
            rss_thread_.join();
        }
        if (root_thread_.joinable()) {
            root_thread_.join();
        }
        sample_rss_once();
        sample_root_once();
        return snapshot();
    }

    /// @brief Returns a thread-safe, non-terminal snapshot.
    [[nodiscard]] resource_sampling_report snapshot() const {
        std::scoped_lock lock(mutex_);
        return report_;
    }

private:
    void sample_loop(
        std::stop_token token,
        std::chrono::milliseconds interval,
        bool rss) noexcept {
        std::mutex wait_mutex;
        std::unique_lock lock(wait_mutex);
        while (!token.stop_requested()) {
            static_cast<void>(wake_.wait_for(
                lock, token, interval, [] { return false; }));
            if (!token.stop_requested()) {
                if (rss) {
                    static_cast<void>(sample_rss_once());
                } else {
                    static_cast<void>(sample_root_once());
                }
            }
        }
    }

    [[nodiscard]] std::optional<std::uint64_t>
    sample_rss_once() noexcept {
        try {
            const std::optional<std::uint64_t> sample =
                sample_postmaster_process_tree_rss(
                    read_postmaster_pid_file(data_directory_));
            std::scoped_lock lock(mutex_);
            if (!sample) {
                ++report_.rss_sampling_failures;
                resource_detail::append_journal("rss");
                return std::nullopt;
            }
            ++report_.rss_samples;
            resource_detail::update_peak(
                report_.peak_postmaster_process_tree_rss_bytes, *sample);
            resource_detail::append_journal("rss", sample);
            return sample;
        } catch (...) {
            std::scoped_lock lock(mutex_);
            ++report_.rss_sampling_failures;
            resource_detail::append_journal("rss");
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<std::uint64_t>
    sample_root_once() noexcept {
        try {
            const std::optional<std::uint64_t> sample =
                sample_owned_root_bytes(owned_root_);
            std::scoped_lock lock(mutex_);
            if (!sample) {
                ++report_.owned_root_sampling_failures;
                resource_detail::append_journal("root");
                return std::nullopt;
            }
            ++report_.owned_root_samples;
            resource_detail::update_peak(
                report_.peak_owned_root_bytes, *sample);
            resource_detail::append_journal("root", sample);
            return sample;
        } catch (...) {
            std::scoped_lock lock(mutex_);
            ++report_.owned_root_sampling_failures;
            resource_detail::append_journal("root");
            return std::nullopt;
        }
    }

    std::filesystem::path owned_root_;
    std::filesystem::path data_directory_;
    std::chrono::milliseconds rss_interval_;
    std::chrono::milliseconds root_interval_;
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    resource_sampling_report report_;
    std::uint64_t next_phase_ordinal_{};
    std::jthread rss_thread_;
    std::jthread root_thread_;
    bool finished_{};
};

}  // namespace bioseq::postgres_test
