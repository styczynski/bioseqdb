module;

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/proc_info.h>
#endif

export module bioseq.cache.benchmark;

import bioseq.test.postgres;

export namespace bioseq::cache_benchmark {

/// @brief Private-cluster workload owned by one benchmark executable.
enum class scenario {
    standalone,
    bioseq_2gbp,
};

}  // namespace bioseq::cache_benchmark

namespace bioseq::cache_benchmark {

using namespace std::chrono_literals;

constexpr std::uint64_t total_input_bases = 2'000'000'000ULL;
constexpr std::uint64_t query_bases = 5'000'000ULL;
constexpr std::uint64_t target_bases =
    total_input_bases - query_bases;
static_assert(query_bases > 0);
static_assert(query_bases + target_bases == total_input_bases);
constexpr std::uint64_t standalone_fixture_rows = 20'000ULL;
constexpr std::string_view report_schema{"bioseq-cache-benchmark-v1"};
constexpr std::chrono::milliseconds rss_sample_interval{50};
constexpr std::chrono::milliseconds temp_sample_interval{1000};
constexpr std::string_view standalone_query{
    "SELECT id, group_id, optional_value, payload "
    "FROM cache_benchmark_rows "
    "ORDER BY group_id, id, optional_value, payload"};
constexpr std::string_view bioseq_query{
    "SELECT id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id) "
    "FROM cache_bioseq_2gbp "
    "WHERE mmsearch(target, query) "
    "ORDER BY id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id)"};

struct benchmark_options {
    scenario selected_scenario{scenario::standalone};
    std::filesystem::path postgres_bin_dir;
    std::filesystem::path postgres_stage_root;
    std::filesystem::path postgres_stage_manifest;
    std::filesystem::path cache_control;
    std::filesystem::path cache_install_sql;
    std::filesystem::path cache_dso;
    std::optional<std::filesystem::path> bioseq_control;
    std::optional<std::filesystem::path> bioseq_install_sql;
    std::optional<std::filesystem::path> bioseq_dso;
    std::filesystem::path report;
    std::uint32_t samples{7};
    std::uint32_t warmup_samples{1};
};

struct cache_counters {
    std::int64_t produced{};
    std::int64_t completed_hits{};
    std::int64_t inflight_waits{};
    std::int64_t reuse_hits{};
    std::int64_t fallbacks{};
    std::int64_t invalidations{};
    std::int64_t evictions{};
    std::int64_t logical_bytes{};
    std::int64_t stored_bytes{};
};

struct benchmark_sample {
    std::string phase;
    std::uint32_t ordinal{};
    std::uint64_t wall_time_ns{};
    std::uint64_t time_to_first_row_ns{};
    std::uint64_t rows{};
    std::string correctness_digest;
    cache_counters counter_delta;
};

struct fixture_evidence {
    std::string relation;
    std::uint64_t source_rows{};
    std::uint64_t reference_base_count{};
    std::uint64_t query_base_count{};
    std::uint64_t target_base_count{};
    std::uint64_t aggregate_input_base_count{};
    std::string query;
};

struct build_identity_evidence {
    std::filesystem::path executable_path;
    std::string executable_sha256;
    std::string compiler;
    std::string compiler_version;
    std::uint64_t cxx_standard{};
    std::string build_mode;
    std::string identity_sha256;
};

struct host_identity_evidence {
    std::string system_name;
    std::string node_name;
    std::string release;
    std::string version;
    std::string machine;
    std::uint32_t logical_cpu_count{};
    std::uint64_t page_size_bytes{};
};

struct benchmark_resource_report {
    std::uint64_t peak_postgres_rss_bytes{};
    std::uint64_t peak_temp_bytes{};
    std::uint64_t rss_samples{};
    std::uint64_t temp_samples{};
    std::uint64_t rss_sampling_failures{};
    std::uint64_t temp_sampling_failures{};
};

struct benchmark_report {
    std::string status{"passed"};
    std::string error;
    scenario selected_scenario{scenario::standalone};
    std::string executable;
    build_identity_evidence build_identity;
    host_identity_evidence host_identity;
    std::string postgres_server_version;
    std::uint32_t configured_samples{};
    std::uint32_t configured_warmup_samples{};
    fixture_evidence fixture;
    std::string correctness_digest;
    std::optional<benchmark_sample> cold_producer;
    std::vector<benchmark_sample> samples;
    cache_counters counter_deltas;
    std::uint64_t logical_bytes{};
    std::uint64_t stored_bytes{};
    benchmark_resource_report resources;
    std::vector<postgres_test::artifact_report> artifacts;
};

struct measured_result {
    libpq::result rows;
    std::uint64_t wall_time_ns{};
    std::uint64_t time_to_first_row_ns{};
};

[[nodiscard]] std::filesystem::path current_executable_path(
    std::string_view argument_zero) {
    std::filesystem::path executable;
#if defined(__linux__)
    std::error_code link_error;
    executable =
        std::filesystem::read_symlink("/proc/self/exe", link_error);
    if (link_error) {
        executable.clear();
    }
#elif defined(__APPLE__)
    std::uint32_t capacity = 1024;
    std::vector<char> buffer(capacity);
    if (::_NSGetExecutablePath(buffer.data(), &capacity) != 0) {
        buffer.resize(capacity);
        if (::_NSGetExecutablePath(buffer.data(), &capacity) != 0) {
            buffer.clear();
        }
    }
    if (!buffer.empty()) {
        executable = buffer.data();
    }
#endif
    if (executable.empty()) {
        if (argument_zero.empty()) {
            throw std::runtime_error(
                "cannot identify the benchmark executable");
        }
        executable = std::filesystem::absolute(argument_zero);
    }
    std::error_code canonical_error;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(executable, canonical_error);
    return canonical_error ? std::filesystem::absolute(executable)
                           : canonical;
}

[[nodiscard]] std::string compiler_name() {
#if defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string compiler_version() {
#if defined(__clang__)
    return __clang_version__;
#elif defined(__GNUC__)
    return __VERSION__;
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string build_mode() {
#if defined(NDEBUG)
    return "ndebug";
#else
    return "assertions-enabled";
#endif
}

[[nodiscard]] build_identity_evidence build_identity(
    std::string_view argument_zero) {
    const std::filesystem::path executable =
        current_executable_path(argument_zero);
    return build_identity_evidence{
        .executable_path = executable,
        .executable_sha256 =
            postgres_test::sha256_file(executable).hex(),
        .compiler = compiler_name(),
        .compiler_version = compiler_version(),
        .cxx_standard = static_cast<std::uint64_t>(__cplusplus),
        .build_mode = build_mode()};
}

[[nodiscard]] host_identity_evidence host_identity() {
    utsname information{};
    if (::uname(&information) != 0) {
        throw std::runtime_error(
            "uname failed while capturing benchmark host identity");
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    return host_identity_evidence{
        .system_name = information.sysname,
        .node_name = information.nodename,
        .release = information.release,
        .version = information.version,
        .machine = information.machine,
        .logical_cpu_count = std::thread::hardware_concurrency(),
        .page_size_bytes =
            page_size > 0 ? static_cast<std::uint64_t>(page_size) : 0};
}

void finalize_build_identity(benchmark_report& report) {
    std::string material = report.build_identity.executable_sha256;
    material.append("|");
    material.append(report.build_identity.compiler);
    material.append("|");
    material.append(report.build_identity.compiler_version);
    material.append("|");
    material.append(std::to_string(report.build_identity.cxx_standard));
    material.append("|");
    material.append(report.build_identity.build_mode);
    for (const postgres_test::artifact_report& artifact : report.artifacts) {
        material.append("|");
        material.append(artifact.name);
        material.append("|");
        material.append(artifact.stage_manifest_sha256);
        material.append("|");
        material.append(artifact.control_sha256);
        material.append("|");
        material.append(artifact.install_sql_sha256);
        material.append("|");
        material.append(artifact.dso_sha256);
        material.append("|");
        material.append(artifact.staged_control_sha256);
    }
    report.build_identity.identity_sha256 =
        postgres_test::sha256(material).hex();
}

namespace resource_detail {

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
    const std::filesystem::path& stat_path, std::uint64_t pid,
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
        return process_snapshot_result{};
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
        if (std::optional<process_entry> process =
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
    return process_snapshot_result{
        .processes = std::move(processes),
        .complete = complete};
}

#elif defined(__APPLE__)

[[nodiscard]] process_snapshot_result process_snapshot(
    std::uint64_t postmaster_pid) {
    const int process_capacity = ::proc_listallpids(nullptr, 0);
    if (process_capacity <= 0) {
        return process_snapshot_result{};
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
            .parent_pid =
                static_cast<std::uint64_t>(basic.pbi_ppid),
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
        for (int index = 0;
             index < child_count
             && static_cast<std::size_t>(index) < child_buffer.size();
             ++index) {
            const pid_t child = child_buffer[
                static_cast<std::size_t>(index)];
            if (child > 1
                && observed.insert(
                       static_cast<std::uint64_t>(child)).second) {
                pending.push_back(
                    static_cast<std::uint64_t>(child));
            }
        }
    }
    return process_snapshot_result{
        .processes = std::move(processes),
        .complete = complete};
}

#else

[[nodiscard]] process_snapshot_result process_snapshot(
    std::uint64_t) {
    return process_snapshot_result{};
}

#endif

[[nodiscard]] std::optional<std::uint64_t>
postgres_cluster_rss_bytes(std::uint64_t postmaster_pid) {
    process_snapshot_result snapshot =
        process_snapshot(postmaster_pid);
    if (!snapshot.complete) {
        return std::nullopt;
    }
    const std::vector<process_entry>& processes = snapshot.processes;
    const bool root_present = std::any_of(
        processes.begin(), processes.end(),
        [postmaster_pid](const process_entry& process) {
            return process.pid == postmaster_pid;
        });
    if (!root_present) {
        return std::nullopt;
    }
    std::unordered_set<std::uint64_t> descendants{postmaster_pid};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const process_entry& process : processes) {
            if (!descendants.contains(process.pid)
                && descendants.contains(process.parent_pid)) {
                descendants.insert(process.pid);
                changed = true;
            }
        }
    }
    std::uint64_t aggregate{};
    for (const process_entry& process : processes) {
        if (!descendants.contains(process.pid)) {
            continue;
        }
        if (process.rss_bytes
            > std::numeric_limits<std::uint64_t>::max() - aggregate) {
            return std::nullopt;
        }
        aggregate += process.rss_bytes;
    }
    return aggregate;
}

[[nodiscard]] std::uint64_t read_postmaster_pid(
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
    std::atomic<std::uint64_t>& peak, std::uint64_t value) noexcept {
    std::uint64_t current = peak.load(std::memory_order_relaxed);
    while (current < value
           && !peak.compare_exchange_weak(
               current, value, std::memory_order_relaxed)) {
    }
}

}  // namespace resource_detail

class benchmark_resource_tracker {
public:
    benchmark_resource_tracker(
        std::filesystem::path cluster_root,
        const std::filesystem::path& data_directory,
        benchmark_resource_report& destination)
        : cluster_root_(std::move(cluster_root)),
          postmaster_pid_(
              resource_detail::read_postmaster_pid(data_directory)),
          destination_(&destination) {
        sample_rss_once();
        sample_temp_once();
        rss_thread_ = std::jthread(
            [this](std::stop_token stop_token) noexcept {
                while (!stop_token.stop_requested()) {
                    std::this_thread::sleep_for(rss_sample_interval);
                    if (!stop_token.stop_requested()) {
                        sample_rss_once();
                    }
                }
            });
        temp_thread_ = std::jthread(
            [this](std::stop_token stop_token) noexcept {
                while (!stop_token.stop_requested()) {
                    std::this_thread::sleep_for(temp_sample_interval);
                    if (!stop_token.stop_requested()) {
                        sample_temp_once();
                    }
                }
            });
    }

    benchmark_resource_tracker(const benchmark_resource_tracker&) = delete;
    benchmark_resource_tracker& operator=(
        const benchmark_resource_tracker&) = delete;

    ~benchmark_resource_tracker() noexcept {
        if (!finished_) {
            try {
                static_cast<void>(finish());
            } catch (...) {
                // A failing benchmark retains partial resource evidence;
                // the original workload error remains authoritative.
            }
        }
    }

    void observe_phase_boundary() noexcept {
        sample_rss_once();
        sample_temp_once();
    }

    [[nodiscard]] benchmark_resource_report finish() {
        rss_thread_.request_stop();
        temp_thread_.request_stop();
        if (rss_thread_.joinable()) {
            rss_thread_.join();
        }
        if (temp_thread_.joinable()) {
            temp_thread_.join();
        }
        sample_rss_once();
        sample_temp_once();
        const benchmark_resource_report report{
            .peak_postgres_rss_bytes =
                peak_postgres_rss_bytes_.load(std::memory_order_relaxed),
            .peak_temp_bytes =
                peak_temp_bytes_.load(std::memory_order_relaxed),
            .rss_samples =
                rss_samples_.load(std::memory_order_relaxed),
            .temp_samples =
                temp_samples_.load(std::memory_order_relaxed),
            .rss_sampling_failures =
                rss_sampling_failures_.load(std::memory_order_relaxed),
            .temp_sampling_failures =
                temp_sampling_failures_.load(std::memory_order_relaxed)};
        *destination_ = report;
        finished_ = true;
        if (report.rss_samples < 2
            || report.peak_postgres_rss_bytes == 0
            || report.rss_sampling_failures > report.rss_samples) {
            throw std::runtime_error(
                "PostgreSQL cluster-process RSS sampling was incomplete");
        }
        if (report.temp_samples < 2 || report.peak_temp_bytes == 0
            || report.temp_sampling_failures > report.temp_samples) {
            throw std::runtime_error(
                "private-cluster temp-byte sampling was incomplete");
        }
        return report;
    }

private:
    void sample_rss_once() noexcept {
        try {
            const std::optional<std::uint64_t> bytes =
                resource_detail::postgres_cluster_rss_bytes(
                    postmaster_pid_);
            if (!bytes) {
                rss_sampling_failures_.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            resource_detail::update_peak(
                peak_postgres_rss_bytes_, *bytes);
            rss_samples_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            rss_sampling_failures_.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    void sample_temp_once() noexcept {
        try {
            const std::optional<std::uint64_t> bytes =
                resource_detail::strict_tree_bytes(cluster_root_);
            if (!bytes) {
                temp_sampling_failures_.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            resource_detail::update_peak(
                peak_temp_bytes_, *bytes);
            temp_samples_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            temp_sampling_failures_.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    std::filesystem::path cluster_root_;
    std::uint64_t postmaster_pid_{};
    std::atomic<std::uint64_t> peak_postgres_rss_bytes_{};
    std::atomic<std::uint64_t> peak_temp_bytes_{};
    std::atomic<std::uint64_t> rss_samples_{};
    std::atomic<std::uint64_t> temp_samples_{};
    std::atomic<std::uint64_t> rss_sampling_failures_{};
    std::atomic<std::uint64_t> temp_sampling_failures_{};
    std::jthread rss_thread_;
    std::jthread temp_thread_;
    benchmark_resource_report* destination_{};
    bool finished_{};
};

[[nodiscard]] std::string_view scenario_name(scenario value) noexcept {
    switch (value) {
    case scenario::standalone:
        return "standalone";
    case scenario::bioseq_2gbp:
        return "bioseq-2gbp";
    }
    return "unknown";
}

[[nodiscard]] scenario parse_scenario(std::string_view value) {
    if (value == "standalone") {
        return scenario::standalone;
    }
    if (value == "bioseq-2gbp") {
        return scenario::bioseq_2gbp;
    }
    throw std::invalid_argument(
        "scenario must be standalone or bioseq-2gbp");
}

template <class Integer>
[[nodiscard]] Integer parse_integer(
    std::string_view value, std::string_view option, bool allow_zero) {
    Integer parsed{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()
        || (!allow_zero && parsed == 0)) {
        throw std::invalid_argument(
            "invalid numeric value for " + std::string(option));
    }
    return parsed;
}

[[nodiscard]] std::string_view required_option_value(
    int argc, char** argv, int& index, std::string_view option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(
            "missing value for " + std::string(option));
    }
    return argv[++index];
}

[[nodiscard]] benchmark_options parse_options(
    int argc, char** argv, scenario required_scenario) {
    benchmark_options options;
    options.selected_scenario = required_scenario;
    options.samples =
        required_scenario == scenario::bioseq_2gbp ? 3U : 7U;
    bool scenario_seen = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto assign_path =
            [argc, argv, &index, argument](
                std::filesystem::path& destination,
                std::string_view option) {
                destination = required_option_value(
                    argc, argv, index, option);
            };
        if (argument == "--scenario") {
            options.selected_scenario = parse_scenario(
                required_option_value(argc, argv, index, argument));
            scenario_seen = true;
        } else if (argument.starts_with("--scenario=")) {
            options.selected_scenario =
                parse_scenario(argument.substr(11));
            scenario_seen = true;
        } else if (argument == "--postgres-bin-dir") {
            assign_path(options.postgres_bin_dir, argument);
        } else if (argument.starts_with("--postgres-bin-dir=")) {
            options.postgres_bin_dir = argument.substr(19);
        } else if (argument == "--postgres-stage-root") {
            assign_path(options.postgres_stage_root, argument);
        } else if (argument.starts_with("--postgres-stage-root=")) {
            options.postgres_stage_root = argument.substr(22);
        } else if (argument == "--postgres-stage-manifest") {
            assign_path(options.postgres_stage_manifest, argument);
        } else if (argument.starts_with("--postgres-stage-manifest=")) {
            options.postgres_stage_manifest = argument.substr(26);
        } else if (argument == "--cache-control") {
            assign_path(options.cache_control, argument);
        } else if (argument.starts_with("--cache-control=")) {
            options.cache_control = argument.substr(16);
        } else if (argument == "--cache-install-sql") {
            assign_path(options.cache_install_sql, argument);
        } else if (argument.starts_with("--cache-install-sql=")) {
            options.cache_install_sql = argument.substr(20);
        } else if (argument == "--cache-dso") {
            assign_path(options.cache_dso, argument);
        } else if (argument.starts_with("--cache-dso=")) {
            options.cache_dso = argument.substr(12);
        } else if (argument == "--bioseq-control") {
            std::filesystem::path path;
            assign_path(path, argument);
            options.bioseq_control = std::move(path);
        } else if (argument.starts_with("--bioseq-control=")) {
            options.bioseq_control =
                std::filesystem::path(argument.substr(17));
        } else if (argument == "--bioseq-install-sql") {
            std::filesystem::path path;
            assign_path(path, argument);
            options.bioseq_install_sql = std::move(path);
        } else if (argument.starts_with("--bioseq-install-sql=")) {
            options.bioseq_install_sql =
                std::filesystem::path(argument.substr(21));
        } else if (argument == "--bioseq-dso") {
            std::filesystem::path path;
            assign_path(path, argument);
            options.bioseq_dso = std::move(path);
        } else if (argument.starts_with("--bioseq-dso=")) {
            options.bioseq_dso =
                std::filesystem::path(argument.substr(13));
        } else if (argument == "--report") {
            assign_path(options.report, argument);
        } else if (argument.starts_with("--report=")) {
            options.report = argument.substr(9);
        } else if (argument == "--samples") {
            options.samples = parse_integer<std::uint32_t>(
                required_option_value(argc, argv, index, argument),
                argument, false);
        } else if (argument.starts_with("--samples=")) {
            options.samples = parse_integer<std::uint32_t>(
                argument.substr(10), "--samples", false);
        } else if (argument == "--warmup-samples") {
            options.warmup_samples = parse_integer<std::uint32_t>(
                required_option_value(argc, argv, index, argument),
                argument, true);
        } else if (argument.starts_with("--warmup-samples=")) {
            options.warmup_samples = parse_integer<std::uint32_t>(
                argument.substr(17), "--warmup-samples", true);
        } else {
            throw std::invalid_argument(
                "unknown benchmark argument: " + std::string(argument));
        }
    }

    if (!scenario_seen || options.selected_scenario != required_scenario) {
        throw std::invalid_argument(
            "benchmark executable was invoked with the wrong scenario");
    }
    for (const auto& [path, name] :
         std::vector<std::pair<std::filesystem::path, std::string_view>>{
             {options.postgres_bin_dir, "--postgres-bin-dir"},
             {options.postgres_stage_root, "--postgres-stage-root"},
             {options.postgres_stage_manifest,
              "--postgres-stage-manifest"},
             {options.cache_control, "--cache-control"},
             {options.cache_install_sql, "--cache-install-sql"},
             {options.cache_dso, "--cache-dso"},
             {options.report, "--report"}}) {
        if (path.empty()) {
            throw std::invalid_argument(
                "required benchmark option is missing: "
                + std::string(name));
        }
    }
    if (required_scenario == scenario::bioseq_2gbp) {
        if (!options.bioseq_control || !options.bioseq_install_sql
            || !options.bioseq_dso) {
            throw std::invalid_argument(
                "bioseq-2gbp requires all Bioseq artifact paths");
        }
        if (options.samples < 3) {
            throw std::invalid_argument(
                "bioseq-2gbp requires at least three measured warm samples");
        }
    }
    return options;
}

[[nodiscard]] std::optional<std::filesystem::path> report_path_hint(
    int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--report" && index + 1 < argc) {
            return std::filesystem::path(argv[index + 1]);
        }
        if (argument.starts_with("--report=")) {
            return std::filesystem::path(argument.substr(9));
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::int64_t parse_i64(std::string_view value) {
    std::int64_t parsed{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("PostgreSQL counter is not an int64");
    }
    return parsed;
}

[[nodiscard]] cache_counters read_cache_counters(
    libpq::connection& connection,
    std::chrono::milliseconds timeout = 30s) {
    const libpq::result result = connection.exec(
        "SELECT produced, completed_hits, inflight_waits, reuse_hits, "
        "fallbacks, invalidations, evictions, logical_bytes, stored_bytes "
        "FROM bioseq_cache.stats()",
        timeout);
    if (result.rows() != 1 || result.columns() != 9) {
        throw std::runtime_error(
            "bioseq_cache.stats() returned an unexpected shape");
    }
    for (int column = 0; column < result.columns(); ++column) {
        if (result.is_null(0, column)) {
            throw std::runtime_error(
                "bioseq_cache.stats() returned a null counter");
        }
    }
    return cache_counters{
        .produced = parse_i64(result.value(0, 0)),
        .completed_hits = parse_i64(result.value(0, 1)),
        .inflight_waits = parse_i64(result.value(0, 2)),
        .reuse_hits = parse_i64(result.value(0, 3)),
        .fallbacks = parse_i64(result.value(0, 4)),
        .invalidations = parse_i64(result.value(0, 5)),
        .evictions = parse_i64(result.value(0, 6)),
        .logical_bytes = parse_i64(result.value(0, 7)),
        .stored_bytes = parse_i64(result.value(0, 8))};
}

[[nodiscard]] cache_counters subtract_counters(
    const cache_counters& after, const cache_counters& before) {
    const auto delta =
        [](std::int64_t newer, std::int64_t older,
           std::string_view name) -> std::int64_t {
            if (newer < older) {
                throw std::runtime_error(
                    "cache counter moved backwards: " + std::string(name));
            }
            return newer - older;
        };
    return cache_counters{
        .produced = delta(after.produced, before.produced, "produced"),
        .completed_hits =
            delta(after.completed_hits, before.completed_hits,
                  "completed_hits"),
        .inflight_waits =
            delta(after.inflight_waits, before.inflight_waits,
                  "inflight_waits"),
        .reuse_hits =
            delta(after.reuse_hits, before.reuse_hits, "reuse_hits"),
        .fallbacks = delta(after.fallbacks, before.fallbacks, "fallbacks"),
        .invalidations =
            delta(after.invalidations, before.invalidations, "invalidations"),
        .evictions =
            delta(after.evictions, before.evictions, "evictions"),
        .logical_bytes =
            delta(after.logical_bytes, before.logical_bytes, "logical_bytes"),
        .stored_bytes =
            delta(after.stored_bytes, before.stored_bytes, "stored_bytes")};
}

void execute(
    libpq::connection& connection, std::string_view sql,
    std::chrono::milliseconds timeout) {
    static_cast<void>(connection.exec(sql, timeout));
}

[[nodiscard]] measured_result measure(
    libpq::connection& connection, std::string_view sql,
    std::chrono::milliseconds timeout) {
    libpq::single_row_observed_result observed =
        connection.exec_single_row_observed(
            sql,
            timeout,
            libpq::execution_limits{
                .max_rows = 1'000'000,
                .max_columns = 32,
                .max_cell_bytes = 64ULL * 1024ULL * 1024ULL,
                .max_total_bytes = 1ULL * 1024ULL * 1024ULL * 1024ULL,
                .max_result_sets = 1});
    if (observed.rows.rows() > 0 && !observed.time_to_first_row) {
        throw std::runtime_error(
            "libpq returned rows without a first-row observation");
    }
    if (observed.time_to_first_row
        && *observed.time_to_first_row > observed.elapsed) {
        throw std::runtime_error(
            "libpq first-row observation exceeds completion time");
    }
    return measured_result{
        .rows = std::move(observed.rows),
        .wall_time_ns = static_cast<std::uint64_t>(
            observed.elapsed.count()),
        .time_to_first_row_ns =
            observed.time_to_first_row
                ? static_cast<std::uint64_t>(
                      observed.time_to_first_row->count())
                : 0};
}

void configure_cpu_path(libpq::connection& connection) {
    execute(
        connection,
        "SET bioseq.aligner.vulkan_enabled = off; "
        "SET bioseq.aligner.vulkan_seed_hits_enabled = off; "
        "SET bioseq.aligner.vulkan_index_cache_enabled = off; "
        "SET bioseq.aligner.vulkan_fused_frontend_enabled = off; "
        "SET bioseq.aligner.vulkan_gpu_index_build_enabled = off; "
        "SET bioseq.aligner.vulkan_extend_enabled = off; "
        "SET bioseq.enable_custom_scan = on; "
        "SET bioseq.enable_column_cache = on; "
        "SET bioseq.read_sequence_index = on",
        30s);
}

void create_standalone_fixture(libpq::connection& connection) {
    execute(
        connection,
        "CREATE TABLE cache_benchmark_rows("
        "id bigint PRIMARY KEY, "
        "group_id integer NOT NULL, "
        "optional_value integer, "
        "payload text NOT NULL); "
        "INSERT INTO cache_benchmark_rows "
        "SELECT id, id % 101, "
        "CASE WHEN id % 3 = 0 THEN NULL ELSE id % 1000 END, "
        "repeat(md5(id::text), 4) "
        "FROM generate_series(1, 20000) AS generated(id); "
        "ANALYZE cache_benchmark_rows",
        2min);
}

[[nodiscard]] fixture_evidence create_bioseq_fixture(
    libpq::connection& connection) {
    configure_cpu_path(connection);
    execute(
        connection,
        "SET bioseq.update_index_on_insert = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_bioseq_2gbp', 1, 5000000, 0.0, 1995000000, "
        "0.001, 0.0, 0.0, 1.0, 1.0, 0.5, 3219876)",
        40min);
    const libpq::result measured_input = connection.exec(
        "SELECT count(*)::bigint, "
        "sum(query_locator.total_bp_size)::bigint, "
        "sum(target_locator.total_bp_size)::bigint, "
        "sum(query_locator.total_bp_size "
        "    + target_locator.total_bp_size)::bigint "
        "FROM cache_bioseq_2gbp AS sample "
        "CROSS JOIN LATERAL "
        "bioseq.inspect_locator(sample.query) AS query_locator "
        "CROSS JOIN LATERAL "
        "bioseq.inspect_locator(sample.target) AS target_locator",
        5min);
    if (measured_input.rows() != 1
        || measured_input.columns() != 4) {
        throw std::runtime_error(
            "bioseq-2gbp stored-input measurement has wrong shape");
    }
    for (int column = 0; column < measured_input.columns(); ++column) {
        if (measured_input.is_null(0, column)) {
            throw std::runtime_error(
                "bioseq-2gbp stored-input measurement is null");
        }
    }
    const std::int64_t measured_source_rows =
        parse_i64(measured_input.value(0, 0));
    const std::int64_t measured_query_bases =
        parse_i64(measured_input.value(0, 1));
    const std::int64_t measured_target_bases =
        parse_i64(measured_input.value(0, 2));
    const std::int64_t measured_total_input_bases =
        parse_i64(measured_input.value(0, 3));
    if (measured_source_rows != 1
        || measured_query_bases
            != static_cast<std::int64_t>(query_bases)
        || measured_query_bases <= 0
        || measured_target_bases
            != static_cast<std::int64_t>(target_bases)
        || measured_total_input_bases
            != static_cast<std::int64_t>(total_input_bases)
        || measured_query_bases + measured_target_bases
            != measured_total_input_bases) {
        throw std::runtime_error(
            "bioseq-2gbp stored input is not exactly "
            "5,000,000 query bases plus 1,995,000,000 target bases");
    }
    execute(
        connection,
        "SELECT bioseq.update_index("
        "'cache_bioseq_2gbp'::regclass, 'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_bioseq_2gbp'::regclass, 'minimizers_per_fragment')",
        40min);
    return fixture_evidence{
        .relation = "cache_bioseq_2gbp",
        .source_rows =
            static_cast<std::uint64_t>(measured_source_rows),
        .reference_base_count =
            static_cast<std::uint64_t>(measured_target_bases),
        .query_base_count =
            static_cast<std::uint64_t>(measured_query_bases),
        .target_base_count =
            static_cast<std::uint64_t>(measured_target_bases),
        .aggregate_input_base_count =
            static_cast<std::uint64_t>(measured_total_input_bases),
        .query = std::string(bioseq_query)};
}

[[nodiscard]] std::vector<postgres_test::extension_artifact>
extension_artifacts(const benchmark_options& options) {
    std::vector<postgres_test::extension_artifact> artifacts{
        postgres_test::extension_artifact{
            .name = "bioseq_cache",
            .control_file = options.cache_control,
            .install_sql = options.cache_install_sql,
            .dso = options.cache_dso}};
    if (options.selected_scenario == scenario::bioseq_2gbp) {
        artifacts.push_back(postgres_test::extension_artifact{
            .name = "bioseq",
            .control_file = *options.bioseq_control,
            .install_sql = *options.bioseq_install_sql,
            .dso = *options.bioseq_dso});
    }
    return artifacts;
}

[[nodiscard]] postgres_test::cluster_options cluster_options(
    const benchmark_options& options) {
    std::vector<std::string> preloads{"bioseq_cache"};
    if (options.selected_scenario == scenario::bioseq_2gbp) {
        preloads.emplace_back("bioseq");
    }
    return postgres_test::cluster_options{
        .postgres_bin_dir = options.postgres_bin_dir,
        .preinstalled_stage_root = options.postgres_stage_root,
        .preinstalled_stage_manifest = options.postgres_stage_manifest,
        .extensions = extension_artifacts(options),
        .allowed_runtime_dso_overrides = {},
        .preload_libraries = std::move(preloads),
        .startup_gucs = {
            {"max_parallel_workers", "0"},
            {"max_parallel_workers_per_gather", "0"}},
        .startup_timeout = 90s,
        .stop_timeout = 30s};
}

[[nodiscard]] benchmark_sample run_sample(
    libpq::connection& data, libpq::connection& admin,
    std::string_view query, std::chrono::milliseconds timeout,
    std::string phase, std::uint32_t ordinal,
    const postgres_test::sha256_digest& expected_digest,
    std::uint64_t expected_rows) {
    const cache_counters before = read_cache_counters(admin, timeout);
    measured_result measured = measure(data, query, timeout);
    const cache_counters after = read_cache_counters(admin, timeout);
    const auto digest = postgres_test::exact_row_hash(measured.rows);
    if (digest != expected_digest
        || static_cast<std::uint64_t>(measured.rows.rows()) != expected_rows) {
        throw std::runtime_error(
            "benchmark result differs from the disabled oracle");
    }
    return benchmark_sample{
        .phase = std::move(phase),
        .ordinal = ordinal,
        .wall_time_ns = measured.wall_time_ns,
        .time_to_first_row_ns = measured.time_to_first_row_ns,
        .rows = expected_rows,
        .correctness_digest = digest.hex(),
        .counter_delta = subtract_counters(after, before)};
}

void require_producer(const benchmark_sample& sample) {
    if (sample.counter_delta.produced != 1
        || sample.counter_delta.completed_hits != 0
        || sample.counter_delta.fallbacks != 0) {
        throw std::runtime_error(
            "cold sample did not produce exactly one cache entry");
    }
}

void require_hit(const benchmark_sample& sample) {
    if (sample.counter_delta.produced != 0
        || sample.counter_delta.completed_hits != 1
        || sample.counter_delta.fallbacks != 0) {
        throw std::runtime_error(
            "warm sample was not exactly one completed cache hit");
    }
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> entry_sizes(
    libpq::connection& connection, std::chrono::milliseconds timeout) {
    const libpq::result result = connection.exec(
        "SELECT COALESCE(sum(logical_bytes), 0)::bigint, "
        "COALESCE(sum(stored_bytes), 0)::bigint "
        "FROM bioseq_cache.entries() "
        "WHERE state = 'COMPLETE'",
        timeout);
    if (result.rows() != 1 || result.columns() != 2
        || result.is_null(0, 0) || result.is_null(0, 1)) {
        throw std::runtime_error(
            "bioseq_cache.entries() returned an unexpected size shape");
    }
    const std::int64_t logical = parse_i64(result.value(0, 0));
    const std::int64_t stored = parse_i64(result.value(0, 1));
    if (logical <= 0 || stored <= 0) {
        throw std::runtime_error(
            "completed cache entry has no measured storage");
    }
    return {
        static_cast<std::uint64_t>(logical),
        static_cast<std::uint64_t>(stored)};
}

[[nodiscard]] benchmark_report make_base_report(
    scenario selected_scenario, std::string executable,
    std::string_view argument_zero) {
    return benchmark_report{
        .selected_scenario = selected_scenario,
        .executable = std::move(executable),
        .build_identity = build_identity(argument_zero),
        .host_identity = host_identity()};
}

void execute_benchmark(
    const benchmark_options& options, benchmark_report& report) {
    report.configured_samples = options.samples;
    report.configured_warmup_samples = options.warmup_samples;

    postgres_test::postgres_fixture fixture{cluster_options(options)};
    report.artifacts = postgres_test::artifact_reports(
        fixture.cluster().staged_artifacts());
    finalize_build_identity(report);
    auto& admin = fixture.admin();
    auto data = fixture.connect(90s);
    report.postgres_server_version = data.server_version();
    benchmark_resource_tracker resource_tracker{
        fixture.cluster().root_directory(),
        fixture.cluster().data_directory(),
        report.resources};
    const bool combined =
        options.selected_scenario == scenario::bioseq_2gbp;
    const std::chrono::milliseconds query_timeout =
        combined ? std::chrono::duration_cast<std::chrono::milliseconds>(30min)
                 : std::chrono::duration_cast<std::chrono::milliseconds>(2min);
    const std::string_view query =
        combined ? bioseq_query : standalone_query;

    if (combined) {
        report.fixture = create_bioseq_fixture(admin);
        configure_cpu_path(data);
    } else {
        create_standalone_fixture(admin);
        report.fixture = fixture_evidence{
            .relation = "cache_benchmark_rows",
            .source_rows = standalone_fixture_rows,
            .reference_base_count = 0,
            .query_base_count = 0,
            .target_base_count = 0,
            .aggregate_input_base_count = 0,
            .query = std::string(query)};
    }
    resource_tracker.observe_phase_boundary();

    execute(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'",
        30s);
    measured_result oracle = measure(data, query, query_timeout);
    if (oracle.rows.rows() <= 0) {
        throw std::runtime_error(
            "disabled benchmark oracle returned no rows");
    }
    const postgres_test::sha256_digest oracle_digest =
        postgres_test::exact_row_hash(oracle.rows);
    const std::uint64_t oracle_rows =
        static_cast<std::uint64_t>(oracle.rows.rows());
    report.correctness_digest = oracle_digest.hex();
    resource_tracker.observe_phase_boundary();

    execute(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()",
        30s);
    execute(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SET bioseq.query_cache_ttl = 3600000",
        30s);
    const cache_counters before_all = read_cache_counters(admin, 30s);

    benchmark_sample producer = run_sample(
        data, admin, query, query_timeout, "producer", 0,
        oracle_digest, oracle_rows);
    require_producer(producer);
    report.cold_producer = std::move(producer);
    resource_tracker.observe_phase_boundary();

    for (std::uint32_t warmup = 0;
         warmup < options.warmup_samples; ++warmup) {
        benchmark_sample discarded = run_sample(
            data, admin, query, query_timeout, "warmup", warmup,
            oracle_digest, oracle_rows);
        require_hit(discarded);
        resource_tracker.observe_phase_boundary();
    }

    for (std::uint32_t sample = 0; sample < options.samples; ++sample) {
        benchmark_sample warm = run_sample(
            data, admin, query, query_timeout, "warm", sample,
            oracle_digest, oracle_rows);
        require_hit(warm);
        report.samples.push_back(std::move(warm));
        resource_tracker.observe_phase_boundary();
        if (combined && sample == 0) {
            execute(
                data, "SET bioseq.enable_column_cache = off", 30s);
        }
    }

    const cache_counters after_all = read_cache_counters(admin, 30s);
    report.counter_deltas = subtract_counters(after_all, before_all);
    if (report.counter_deltas.produced != 1
        || report.counter_deltas.completed_hits
            != static_cast<std::int64_t>(
                options.warmup_samples + options.samples)
        || report.counter_deltas.fallbacks != 0) {
        throw std::runtime_error(
            "aggregate cache counters do not match benchmark phases");
    }
    const auto [logical_bytes, stored_bytes] =
        entry_sizes(admin, query_timeout);
    report.logical_bytes = logical_bytes;
    report.stored_bytes = stored_bytes;
    report.resources = resource_tracker.finish();
}

namespace json {

void string(std::ostream& output, std::string_view value) {
    constexpr std::string_view digits{"0123456789abcdef"};
    output.put('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u00"
                       << digits[(character >> 4U) & 0x0fU]
                       << digits[character & 0x0fU];
            } else {
                output.put(static_cast<char>(character));
            }
        }
    }
    output.put('"');
}

void key(std::ostream& output, std::string_view value) {
    string(output, value);
    output.put(':');
}

void counters(std::ostream& output, const cache_counters& value) {
    output << "{\"produced\":" << value.produced
           << ",\"completed_hits\":" << value.completed_hits
           << ",\"inflight_waits\":" << value.inflight_waits
           << ",\"reuse_hits\":" << value.reuse_hits
           << ",\"fallbacks\":" << value.fallbacks
           << ",\"invalidations\":" << value.invalidations
           << ",\"evictions\":" << value.evictions
           << ",\"logical_bytes\":" << value.logical_bytes
           << ",\"stored_bytes\":" << value.stored_bytes << '}';
}

void sample(std::ostream& output, const benchmark_sample& value) {
    output.put('{');
    key(output, "phase");
    string(output, value.phase);
    output.put(',');
    key(output, "ordinal");
    output << value.ordinal;
    output.put(',');
    key(output, "wall_time_ns");
    output << value.wall_time_ns;
    output.put(',');
    key(output, "time_to_first_row_ns");
    output << value.time_to_first_row_ns;
    output.put(',');
    key(output, "rows");
    output << value.rows;
    output.put(',');
    key(output, "correctness_digest");
    string(output, value.correctness_digest);
    output.put(',');
    key(output, "cache_counter_delta");
    counters(output, value.counter_delta);
    output.put('}');
}

[[nodiscard]] std::uint64_t percentile(
    std::vector<std::uint64_t> values,
    std::uint64_t numerator, std::uint64_t denominator) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::uint64_t rank =
        (numerator * values.size() + denominator - 1U) / denominator;
    return values[static_cast<std::size_t>(
        std::max<std::uint64_t>(rank, 1U) - 1U)];
}

void write(std::ostream& output, const benchmark_report& report) {
    output.put('{');
    key(output, "schema_version");
    string(output, report_schema);
    output.put(',');
    key(output, "metadata");
    output.put('{');
    key(output, "status");
    string(output, report.status);
    output.put(',');
    key(output, "error");
    string(output, report.error);
    output.put(',');
    key(output, "executable");
    string(output, report.executable);
    output.put(',');
    key(output, "build_identity");
    output.put('{');
    key(output, "executable_path");
    string(output, report.build_identity.executable_path.string());
    output.put(',');
    key(output, "executable_sha256");
    string(output, report.build_identity.executable_sha256);
    output.put(',');
    key(output, "compiler");
    string(output, report.build_identity.compiler);
    output.put(',');
    key(output, "compiler_version");
    string(output, report.build_identity.compiler_version);
    output.put(',');
    key(output, "cxx_standard");
    output << report.build_identity.cxx_standard;
    output.put(',');
    key(output, "build_mode");
    string(output, report.build_identity.build_mode);
    output.put(',');
    key(output, "identity_sha256");
    string(output, report.build_identity.identity_sha256);
    output.put('}');
    output.put(',');
    key(output, "host_identity");
    output.put('{');
    key(output, "system_name");
    string(output, report.host_identity.system_name);
    output.put(',');
    key(output, "node_name");
    string(output, report.host_identity.node_name);
    output.put(',');
    key(output, "release");
    string(output, report.host_identity.release);
    output.put(',');
    key(output, "version");
    string(output, report.host_identity.version);
    output.put(',');
    key(output, "machine");
    string(output, report.host_identity.machine);
    output.put(',');
    key(output, "logical_cpu_count");
    output << report.host_identity.logical_cpu_count;
    output.put(',');
    key(output, "page_size_bytes");
    output << report.host_identity.page_size_bytes;
    output.put('}');
    output.put(',');
    key(output, "postgres_server_version");
    string(output, report.postgres_server_version);
    output.put(',');
    key(output, "configured_samples");
    output << report.configured_samples;
    output.put(',');
    key(output, "configured_warmup_samples");
    output << report.configured_warmup_samples;
    output.put(',');
    key(output, "first_row_measurement");
    string(
        output,
        "PQsetSingleRowMode first PGRES_SINGLE_TUPLE arrival");
    output.put(',');
    key(output, "resource_sampling");
    output.put('{');
    key(output, "measurement_window");
    string(
        output,
        "from the running private-cluster fixture before workload setup "
        "through the final measured cache-hit phase; excludes initdb, "
        "server startup, shutdown, and cleanup");
    output.put(',');
    key(output, "rss_scope");
    string(
        output,
        "sum of current RSS for the owned private PostgreSQL postmaster "
        "and all descendant processes");
    output.put(',');
    key(output, "rss_sample_interval_ms");
    output << rss_sample_interval.count();
    output.put(',');
    key(output, "rss_samples");
    output << report.resources.rss_samples;
    output.put(',');
    key(output, "rss_sampling_failures");
    output << report.resources.rss_sampling_failures;
    output.put(',');
    key(output, "temp_scope");
    string(
        output,
        "all regular-file bytes below the owned private-cluster root");
    output.put(',');
    key(output, "temp_sample_interval_ms");
    output << temp_sample_interval.count();
    output.put(',');
    key(output, "temp_samples");
    output << report.resources.temp_samples;
    output.put(',');
    key(output, "temp_sampling_failures");
    output << report.resources.temp_sampling_failures;
    output.put('}');
    output.put(',');
    key(output, "artifacts");
    output.put('[');
    for (std::size_t index = 0; index < report.artifacts.size(); ++index) {
        if (index != 0) {
            output.put(',');
        }
        const postgres_test::artifact_report& artifact =
            report.artifacts[index];
        output.put('{');
        key(output, "name");
        string(output, artifact.name);
        output.put(',');
        key(output, "preinstalled_stage_root");
        string(output, artifact.preinstalled_stage_root.string());
        output.put(',');
        key(output, "stage_manifest");
        string(output, artifact.stage_manifest.string());
        output.put(',');
        key(output, "stage_manifest_sha256");
        string(output, artifact.stage_manifest_sha256);
        output.put(',');
        key(output, "control_file");
        string(output, artifact.control_file.string());
        output.put(',');
        key(output, "install_sql");
        string(output, artifact.install_sql.string());
        output.put(',');
        key(output, "dso");
        string(output, artifact.dso.string());
        output.put(',');
        key(output, "staged_control_file");
        string(output, artifact.staged_control_file.string());
        output.put(',');
        key(output, "staged_install_sql");
        string(output, artifact.staged_install_sql.string());
        output.put(',');
        key(output, "staged_dso");
        string(output, artifact.staged_dso.string());
        output.put(',');
        key(output, "control_sha256");
        string(output, artifact.control_sha256);
        output.put(',');
        key(output, "install_sql_sha256");
        string(output, artifact.install_sql_sha256);
        output.put(',');
        key(output, "dso_sha256");
        string(output, artifact.dso_sha256);
        output.put(',');
        key(output, "staged_control_sha256");
        string(output, artifact.staged_control_sha256);
        output.put('}');
    }
    output << "]}";
    output.put(',');
    key(output, "scenario");
    string(output, scenario_name(report.selected_scenario));
    output.put(',');
    key(output, "fixture");
    output.put('{');
    key(output, "relation");
    string(output, report.fixture.relation);
    output.put(',');
    key(output, "source_rows");
    output << report.fixture.source_rows;
    output.put(',');
    key(output, "reference_bases");
    output << report.fixture.reference_base_count;
    output.put(',');
    key(output, "query_bases");
    output << report.fixture.query_base_count;
    output.put(',');
    key(output, "target_bases");
    output << report.fixture.target_base_count;
    output.put(',');
    key(output, "aggregate_input_bases");
    output << report.fixture.aggregate_input_base_count;
    output.put(',');
    key(output, "query");
    string(output, report.fixture.query);
    output.put('}');
    output.put(',');
    key(output, "correctness_digest");
    string(output, report.correctness_digest);
    output.put(',');
    key(output, "cold_producer");
    if (report.cold_producer) {
        sample(output, *report.cold_producer);
    } else {
        output << "null";
    }
    output.put(',');
    key(output, "samples");
    output.put('[');
    std::vector<std::uint64_t> wall_samples;
    wall_samples.reserve(report.samples.size());
    for (std::size_t index = 0; index < report.samples.size(); ++index) {
        if (index != 0) {
            output.put(',');
        }
        const benchmark_sample& warm_sample = report.samples[index];
        wall_samples.push_back(warm_sample.wall_time_ns);
        sample(output, warm_sample);
    }
    output.put(']');
    output.put(',');
    key(output, "summary");
    output.put('{');
    key(output, "population");
    string(output, "warm samples only");
    output.put(',');
    key(output, "sample_count");
    output << wall_samples.size();
    output.put(',');
    key(output, "p50");
    output << percentile(wall_samples, 50, 100);
    output.put(',');
    key(output, "p95");
    output << percentile(wall_samples, 95, 100);
    output.put(',');
    key(output, "min");
    output << (wall_samples.empty()
                   ? 0
                   : *std::min_element(
                         wall_samples.begin(), wall_samples.end()));
    output.put(',');
    key(output, "max");
    output << (wall_samples.empty()
                   ? 0
                   : *std::max_element(
                         wall_samples.begin(), wall_samples.end()));
    output.put('}');
    output.put(',');
    key(output, "cache_counter_deltas");
    counters(output, report.counter_deltas);
    output.put(',');
    key(output, "logical_bytes");
    output << report.logical_bytes;
    output.put(',');
    key(output, "stored_bytes");
    output << report.stored_bytes;
    output.put(',');
    key(output, "peak_rss_bytes");
    output << report.resources.peak_postgres_rss_bytes;
    output.put(',');
    key(output, "peak_temp_bytes");
    output << report.resources.peak_temp_bytes;
    output.put('}');
}

}  // namespace json

[[nodiscard]] std::string render_contract_probe(
    bool passed, std::filesystem::path preinstalled_stage_root,
    std::filesystem::path stage_manifest,
    std::string stage_manifest_sha256) {
    const std::string digest(64, 'a');
    benchmark_report report{
        .status = passed ? "passed" : "failed",
        .error = passed ? "" : "contract probe failure",
        .selected_scenario = scenario::standalone,
        .executable = "bioseq_cache_report_contract_probe",
        .build_identity =
            build_identity_evidence{
                .executable_path = "/build/cache-benchmark-probe",
                .executable_sha256 = digest,
                .compiler = "contract-compiler",
                .compiler_version = "1.0",
                .cxx_standard = 202302,
                .build_mode = "ndebug",
                .identity_sha256 = {}},
        .host_identity =
            host_identity_evidence{
                .system_name = "contract-system",
                .node_name = "contract-node",
                .release = "contract-release",
                .version = "contract-version",
                .machine = "contract-machine",
                .logical_cpu_count = 8,
                .page_size_bytes = 4096},
        .postgres_server_version = "PostgreSQL contract",
        .configured_samples = 3,
        .configured_warmup_samples = 1,
        .fixture =
            fixture_evidence{
                .relation = "contract_fixture",
                .source_rows = 4,
                .reference_base_count = 0,
                .query_base_count = 0,
                .target_base_count = 0,
                .aggregate_input_base_count = 0,
                .query = "SELECT contract"},
        .correctness_digest = passed ? digest : "",
        .resources =
            benchmark_resource_report{
                .peak_postgres_rss_bytes = 8192,
                .peak_temp_bytes = 16384,
                .rss_samples = 4,
                .temp_samples = 3}};
    report.artifacts.push_back(postgres_test::artifact_report{
        .name = "bioseq_cache",
        .preinstalled_stage_root = std::move(preinstalled_stage_root),
        .stage_manifest = std::move(stage_manifest),
        .stage_manifest_sha256 = std::move(stage_manifest_sha256),
        .control_file = "/source/bioseq_cache.control",
        .install_sql = "/source/bioseq_cache--1.0.sql",
        .dso = "/source/bioseq_cache.so",
        .staged_control_file = "/stage/bioseq_cache.control",
        .staged_install_sql = "/stage/bioseq_cache--1.0.sql",
        .staged_dso = "/stage/bioseq_cache.so",
        .control_sha256 = digest,
        .install_sql_sha256 = digest,
        .dso_sha256 = digest,
        .staged_control_sha256 = digest});
    if (passed) {
        report.cold_producer = benchmark_sample{
            .phase = "producer",
            .ordinal = 0,
            .wall_time_ns = 999,
            .time_to_first_row_ns = 500,
            .rows = 4,
            .correctness_digest = digest,
            .counter_delta = cache_counters{.produced = 1}};
        for (const std::uint64_t wall_time : {30U, 10U, 20U}) {
            report.samples.push_back(benchmark_sample{
                .phase = "warm",
                .ordinal =
                    static_cast<std::uint32_t>(report.samples.size()),
                .wall_time_ns = wall_time,
                .time_to_first_row_ns = wall_time / 2U,
                .rows = 4,
                .correctness_digest = digest,
                .counter_delta =
                    cache_counters{.completed_hits = 1}});
        }
        report.counter_deltas = cache_counters{
            .produced = 1,
            .completed_hits = 4};
        report.logical_bytes = 2048;
        report.stored_bytes = 1024;
    }
    finalize_build_identity(report);
    std::ostringstream output;
    json::write(output, report);
    return output.str();
}

void write_report(
    const std::filesystem::path& path,
    const benchmark_report& report) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(path);
    std::filesystem::create_directories(absolute.parent_path());
    const std::filesystem::path temporary =
        absolute.parent_path()
        / (absolute.filename().string() + ".partial");
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "cannot create benchmark report");
    }
    json::write(output, report);
    output.put('\n');
    output.close();
    if (!output) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        throw std::runtime_error(
            "cannot finish benchmark report");
    }
    std::error_code error;
    std::filesystem::rename(temporary, absolute, error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        throw std::runtime_error(
            "cannot publish benchmark report: " + error.message());
    }
}

void write_failure_report(
    const std::optional<std::filesystem::path>& path,
    std::optional<benchmark_report>& report,
    scenario selected_scenario, const std::string& executable,
    std::string_view argument_zero, std::string_view error) noexcept {
    if (!path) {
        return;
    }
    try {
        if (!report) {
            report.emplace(make_base_report(
                selected_scenario, executable, argument_zero));
        }
        report->status = "failed";
        report->error = error;
        finalize_build_identity(*report);
        write_report(*path, *report);
    } catch (...) {
        // The process still fails closed when its diagnostic report cannot
        // be published.
    }
}

}  // namespace bioseq::cache_benchmark

export namespace bioseq::cache_benchmark {

/// @brief Renders deterministic success/failure documents for schema tests.
[[nodiscard]] std::string render_report_contract_probe(bool passed) {
    return render_contract_probe(
        passed, "/stage", "/stage/stage-manifest.json",
        std::string(64, 'b'));
}

/// @brief Renders a probe with caller-selected stage provenance.
///
/// This overload exists so contract tests can prove that the stage-manifest
/// digest participates in build identity while host-dependent stage paths do
/// not.
[[nodiscard]] std::string
render_report_contract_probe_with_stage_provenance(
    bool passed, std::string_view preinstalled_stage_root,
    std::string_view stage_manifest,
    std::string_view stage_manifest_sha256) {
    return render_contract_probe(
        passed, std::string(preinstalled_stage_root),
        std::string(stage_manifest), std::string(stage_manifest_sha256));
}

/// @brief Parse, execute, validate, and report one fixed native benchmark.
int run(int argc, char** argv, scenario required_scenario) noexcept {
    const std::optional<std::filesystem::path> report_hint =
        report_path_hint(argc, argv);
    const std::string_view argument_zero =
        argc > 0 && argv != nullptr && argv[0] != nullptr
            ? std::string_view(argv[0])
            : std::string_view{};
    const std::string executable =
        required_scenario == scenario::standalone
            ? "bioseq_cache_bench"
            : "bioseq_cache_bioseq_bench";
    std::optional<benchmark_report> report;
    try {
        report.emplace(make_base_report(
            required_scenario, executable, argument_zero));
        const benchmark_options options =
            parse_options(argc, argv, required_scenario);
        execute_benchmark(options, *report);
        write_report(options.report, *report);
        return 0;
    } catch (const std::exception& error) {
        write_failure_report(
            report_hint, report, required_scenario, executable,
            argument_zero, error.what());
        return 1;
    } catch (...) {
        write_failure_report(
            report_hint, report, required_scenario, executable,
            argument_zero, "unknown benchmark failure");
        return 1;
    }
}

}  // namespace bioseq::cache_benchmark
