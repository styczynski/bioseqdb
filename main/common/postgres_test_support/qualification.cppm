module;

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

export module bioseq.test.postgres:qualification;

import bioseq.common.memory_budget;
import :supervision;

export namespace bioseq::postgres_test {

inline constexpr std::uint64_t qualification_gibibyte =
    1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t qualification_mebibyte =
    1024ULL * 1024ULL;

/// @brief One finite cgroup memory limit and the usage charged to it.
struct cgroup_memory_limit_sample {
    std::uint64_t limit_bytes{};
    std::uint64_t current_bytes{};
};

/// @brief One finite cgroup CPU bandwidth limit.
struct cgroup_cpu_quota_sample {
    std::uint64_t quota_microseconds{};
    std::uint64_t period_microseconds{};
};

/// @brief Pure memory-capacity reduction used by host qualification.
struct effective_memory_capacity {
    std::uint64_t total_bytes{};
    std::uint64_t available_bytes{};
    std::optional<std::size_t> total_limit_sample;
    std::optional<std::size_t> available_limit_sample;
};

/// @brief Pure CPU-capacity reduction used by host qualification.
struct effective_cpu_capacity {
    double cores{};
    std::optional<std::size_t> quota_sample;
};

/// @brief Effective NUMA memory-node relationship for the current task.
struct linux_memory_node_scope {
    unsigned int allowed_nodes{};
    unsigned int online_nodes{};
    bool restricted{};
};

enum class cgroup_controller_version {
    none,
    v1,
    v2,
};

/// @brief Strictly parses a bounded classic-locale finite real.
/// @return nullopt for empty, overlong, partial, non-finite, or overflow input.
[[nodiscard]] std::optional<double>
parse_finite_real(std::string_view text);

/// @brief Parses a cgroup memory limit; `max` is represented by nullopt.
[[nodiscard]] std::optional<std::uint64_t>
parse_cgroup_memory_limit(std::string_view text);

/// @brief Parses cgroup v2 cpu.max; an unlimited quota is nullopt.
[[nodiscard]] std::optional<cgroup_cpu_quota_sample>
parse_cgroup_cpu_max(std::string_view text);

/// @brief Counts the exact CPU IDs in a Linux cpuset list.
[[nodiscard]] std::optional<unsigned int>
parse_cgroup_cpuset_count(std::string_view text);

/// @brief Applies all finite ancestor cgroup limits to host memory evidence.
[[nodiscard]] effective_memory_capacity constrain_memory_capacity(
    std::uint64_t host_total_bytes,
    std::uint64_t host_available_bytes,
    std::span<const cgroup_memory_limit_sample> limits);

/// @brief Applies affinity, cpuset, and ancestor quotas to host CPU evidence.
[[nodiscard]] effective_cpu_capacity constrain_cpu_capacity(
    unsigned int host_cores,
    std::optional<unsigned int> affinity_cores,
    std::optional<unsigned int> cpuset_cores,
    std::span<const cgroup_cpu_quota_sample> quotas);

/// @brief Classifies the task's allowed NUMA nodes against online nodes.
[[nodiscard]] linux_memory_node_scope classify_linux_memory_node_scope(
    std::string_view allowed_nodes,
    std::string_view online_nodes);

/// @brief Extracts the unique Mems_allowed_list value from task status.
[[nodiscard]] std::string parse_linux_mems_allowed_list(
    std::string_view task_status);

/// @brief Selects v2 only when its controller interface is observable.
[[nodiscard]] cgroup_controller_version select_cgroup_controller_version(
    bool v2_interface_observed,
    bool v1_interface_observed) noexcept;

/// @brief Rejects unresolved namespace-relative cgroup coordinates.
[[nodiscard]] bool cgroup_namespace_path_is_safe(
    std::string_view text) noexcept;

/// @brief Tests whether a membership can be located below a mount root.
[[nodiscard]] bool cgroup_membership_is_resolvable(
    std::string_view membership,
    std::string_view mount_root) noexcept;

/// @brief True only for the Linux affinity error that requests a larger mask.
[[nodiscard]] bool scheduler_affinity_error_requires_resize(
    int error_number) noexcept;

/// @brief Unsuccessful acceptance result caused by host infrastructure.
class infrastructure_inconclusive : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Capacity evidence for one exact resolved filesystem path.
struct filesystem_qualification_report {
    std::string role;
    std::filesystem::path path;
    std::uint64_t capacity_bytes{};
    std::uint64_t available_bytes{};
    std::uint64_t required_available_bytes{};
    std::uint64_t device_id{};
    bool distinct_from_data{};
    bool passed{};

    bool operator==(
        const filesystem_qualification_report&) const = default;
};

/// @brief Uncached sequential write/read evidence from the target filesystem.
struct io_qualification_report {
    std::filesystem::path path;
    std::uint64_t bytes{};
    std::chrono::milliseconds write_duration{};
    std::chrono::milliseconds read_duration{};
    double write_mib_per_second{};
    double read_mib_per_second{};
    std::string cache_eviction_method;
    bool cache_eviction_succeeded{};
    bool passed{};

    bool operator==(const io_qualification_report&) const = default;
};

enum class host_qualification_status {
    passed,
    infrastructure_inconclusive,
};

/// @brief Structured pre-initdb host qualification result.
struct host_qualification_report {
    std::uint64_t ordinal{};
    std::string test_case;
    host_qualification_status status{
        host_qualification_status::infrastructure_inconclusive};
    std::string detail;
    /// Effective aliases retained for the original report schema.
    std::uint64_t total_ram_bytes{};
    std::uint64_t available_ram_bytes{};
    unsigned int logical_cores{};
    std::uint64_t host_total_ram_bytes{};
    std::uint64_t host_available_ram_bytes{};
    std::uint64_t effective_total_ram_bytes{};
    std::uint64_t effective_available_ram_bytes{};
    std::uint64_t cgroup_memory_total_limit_bytes{};
    std::uint64_t cgroup_memory_headroom_limit_bytes{};
    std::uint64_t cgroup_memory_current_bytes{};
    std::uint64_t cgroup_memory_headroom_bytes{};
    std::string cgroup_memory_version;
    std::string cgroup_memory_total_source;
    std::string cgroup_memory_headroom_source;
    unsigned int host_logical_cores{};
    unsigned int affinity_logical_cores{};
    unsigned int cpuset_logical_cores{};
    std::uint64_t cgroup_cpu_quota_microseconds{};
    std::uint64_t cgroup_cpu_period_microseconds{};
    double cgroup_cpu_quota_cores{};
    double effective_cpu_cores{};
    std::string cgroup_cpu_version;
    std::string cgroup_cpu_source;
    std::uint64_t required_total_ram_bytes{};
    std::uint64_t required_available_ram_bytes{};
    unsigned int required_logical_cores{};
    double required_io_mib_per_second{};
    std::vector<filesystem_qualification_report> filesystems;
    io_qualification_report io;
    std::chrono::milliseconds duration{};

    bool operator==(const host_qualification_report&) const = default;
};

/// @brief Acceptance thresholds and an optional distinct cache/temp path.
struct host_qualification_requirements {
    std::string test_case;
    std::uint64_t minimum_total_ram_bytes{
        16ULL * qualification_gibibyte};
    std::uint64_t minimum_available_ram_bytes{
        8ULL * qualification_gibibyte};
    std::uint64_t minimum_data_available_bytes{
        32ULL * qualification_gibibyte};
    std::uint64_t minimum_distinct_cache_temp_available_bytes{
        8ULL * qualification_gibibyte};
    unsigned int minimum_logical_cores{8};
    std::uint64_t io_probe_bytes{qualification_gibibyte};
    double minimum_io_mib_per_second{150.0};
    std::optional<std::filesystem::path> cache_temp_directory;
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::qualification_parse_detail {

[[nodiscard]] std::string_view trim_ascii(std::string_view text) {
    while (!text.empty()
           && (text.front() == ' ' || text.front() == '\t'
               || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty()
           && (text.back() == ' ' || text.back() == '\t'
               || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::uint64_t parse_unsigned(
    std::string_view text, std::string_view field) {
    text = trim_ascii(text);
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{}
        || end != text.data() + text.size()) {
        throw std::invalid_argument(
            "invalid cgroup " + std::string(field));
    }
    return value;
}

[[nodiscard]] std::vector<std::string_view> whitespace_fields(
    std::string_view text) {
    std::vector<std::string_view> fields;
    text = trim_ascii(text);
    while (!text.empty()) {
        const std::size_t separator =
            text.find_first_of(" \t\r\n");
        fields.push_back(text.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator);
        text = trim_ascii(text);
    }
    return fields;
}

using index_range = std::pair<std::uint64_t, std::uint64_t>;

[[nodiscard]] std::vector<index_range> normalized_index_ranges(
    std::string_view text,
    std::string_view field) {
    text = trim_ascii(text);
    if (text.empty()) {
        return {};
    }
    std::vector<index_range> ranges;
    while (!text.empty()) {
        const std::size_t separator = text.find(',');
        const std::string_view token =
            trim_ascii(text.substr(0, separator));
        if (token.empty()) {
            throw std::invalid_argument(
                "invalid empty cgroup " + std::string(field)
                + " item");
        }
        const std::size_t dash = token.find('-');
        const std::uint64_t first =
            parse_unsigned(token.substr(0, dash), field);
        const std::uint64_t last =
            dash == std::string_view::npos
            ? first
            : parse_unsigned(token.substr(dash + 1), field);
        if (dash != std::string_view::npos
            && token.find('-', dash + 1) != std::string_view::npos) {
            throw std::invalid_argument(
                "invalid cgroup " + std::string(field) + " range");
        }
        if (last < first) {
            throw std::invalid_argument(
                "descending cgroup " + std::string(field) + " range");
        }
        ranges.emplace_back(first, last);
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
        if (text.empty()) {
            throw std::invalid_argument(
                "invalid trailing cgroup " + std::string(field)
                + " separator");
        }
    }
    std::ranges::sort(ranges);
    std::vector<index_range> merged;
    merged.reserve(ranges.size());
    for (const auto [first, last] : ranges) {
        if (merged.empty()) {
            merged.emplace_back(first, last);
            continue;
        }
        auto& previous = merged.back();
        const bool adjacent =
            previous.second
                != std::numeric_limits<std::uint64_t>::max()
            && first == previous.second + 1;
        if (first <= previous.second || adjacent) {
            previous.second = std::max(previous.second, last);
        } else {
            merged.emplace_back(first, last);
        }
    }
    return merged;
}

[[nodiscard]] unsigned int index_range_count(
    std::span<const index_range> ranges,
    std::string_view field) {
    std::uint64_t count{};
    for (const auto [first, last] : ranges) {
        const std::uint64_t width = last - first;
        if (width >= std::numeric_limits<unsigned int>::max()
            || count
                > std::numeric_limits<unsigned int>::max()
                    - (width + 1)) {
            throw std::overflow_error(
                "cgroup " + std::string(field) + " count overflow");
        }
        count += width + 1;
    }
    return static_cast<unsigned int>(count);
}

[[nodiscard]] bool ranges_include(
    std::span<const index_range> superset,
    std::span<const index_range> subset) {
    std::size_t superset_index{};
    for (const auto [first, last] : subset) {
        while (superset_index < superset.size()
               && superset[superset_index].second < first) {
            ++superset_index;
        }
        if (superset_index == superset.size()
            || superset[superset_index].first > first
            || superset[superset_index].second < last) {
            return false;
        }
    }
    return true;
}

}  // namespace bioseq::postgres_test::qualification_parse_detail

export namespace bioseq::postgres_test {

[[nodiscard]] inline std::optional<std::uint64_t>
parse_cgroup_memory_limit(std::string_view text) {
    text = qualification_parse_detail::trim_ascii(text);
    if (text == "max") {
        return std::nullopt;
    }
    return qualification_parse_detail::parse_unsigned(
        text, "memory limit");
}

[[nodiscard]] inline std::optional<cgroup_cpu_quota_sample>
parse_cgroup_cpu_max(std::string_view text) {
    const auto fields =
        qualification_parse_detail::whitespace_fields(text);
    if (fields.size() != 2) {
        throw std::invalid_argument(
            "invalid cgroup cpu.max field count");
    }
    const std::uint64_t period =
        qualification_parse_detail::parse_unsigned(
            fields[1], "CPU period");
    if (period == 0) {
        throw std::invalid_argument(
            "cgroup CPU period must be positive");
    }
    if (fields[0] == "max") {
        return std::nullopt;
    }
    const std::uint64_t quota =
        qualification_parse_detail::parse_unsigned(
            fields[0], "CPU quota");
    if (quota == 0) {
        throw std::invalid_argument(
            "cgroup CPU quota must be positive");
    }
    return cgroup_cpu_quota_sample{
        .quota_microseconds = quota,
        .period_microseconds = period};
}

[[nodiscard]] inline std::optional<unsigned int>
parse_cgroup_cpuset_count(std::string_view text) {
    const auto ranges =
        qualification_parse_detail::normalized_index_ranges(
            text, "cpuset CPU");
    if (ranges.empty()) {
        return std::nullopt;
    }
    return qualification_parse_detail::index_range_count(
        ranges, "cpuset CPU");
}

[[nodiscard]] inline linux_memory_node_scope
classify_linux_memory_node_scope(
    std::string_view allowed_nodes,
    std::string_view online_nodes) {
    const auto allowed =
        qualification_parse_detail::normalized_index_ranges(
            allowed_nodes, "allowed memory node");
    const auto online =
        qualification_parse_detail::normalized_index_ranges(
            online_nodes, "online memory node");
    if (allowed.empty() || online.empty()) {
        throw std::invalid_argument(
            "Linux memory-node evidence is empty");
    }
    if (!qualification_parse_detail::ranges_include(
            online, allowed)) {
        throw std::invalid_argument(
            "allowed Linux memory nodes are not online");
    }
    const unsigned int allowed_count =
        qualification_parse_detail::index_range_count(
            allowed, "allowed memory node");
    const unsigned int online_count =
        qualification_parse_detail::index_range_count(
            online, "online memory node");
    return linux_memory_node_scope{
        .allowed_nodes = allowed_count,
        .online_nodes = online_count,
        .restricted = allowed != online};
}

[[nodiscard]] inline std::string parse_linux_mems_allowed_list(
    std::string_view task_status) {
    constexpr std::string_view key{"Mems_allowed_list:"};
    std::optional<std::string> result;
    while (!task_status.empty()) {
        const std::size_t newline = task_status.find('\n');
        const std::string_view line =
            task_status.substr(0, newline);
        if (line.starts_with(key)) {
            if (result) {
                throw std::invalid_argument(
                    "duplicate Mems_allowed_list task evidence");
            }
            const std::string_view value =
                qualification_parse_detail::trim_ascii(
                    line.substr(key.size()));
            if (value.empty()) {
                throw std::invalid_argument(
                    "empty Mems_allowed_list task evidence");
            }
            result = std::string(value);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        task_status.remove_prefix(newline + 1);
    }
    if (!result) {
        throw std::invalid_argument(
            "missing Mems_allowed_list task evidence");
    }
    return *result;
}

[[nodiscard]] inline effective_memory_capacity
constrain_memory_capacity(
    std::uint64_t host_total_bytes,
    std::uint64_t host_available_bytes,
    std::span<const cgroup_memory_limit_sample> limits) {
    effective_memory_capacity result{
        .total_bytes = host_total_bytes,
        .available_bytes =
            std::min(host_available_bytes, host_total_bytes)};
    std::uint64_t smallest_limit =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t smallest_headroom =
        std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < limits.size(); ++index) {
        const cgroup_memory_limit_sample& sample = limits[index];
        const std::uint64_t headroom =
            sample.current_bytes >= sample.limit_bytes
            ? 0 : sample.limit_bytes - sample.current_bytes;
        if (!result.total_limit_sample
            || sample.limit_bytes < smallest_limit) {
            smallest_limit = sample.limit_bytes;
            result.total_limit_sample = index;
        }
        if (!result.available_limit_sample
            || headroom < smallest_headroom) {
            smallest_headroom = headroom;
            result.available_limit_sample = index;
        }
        result.total_bytes =
            std::min(result.total_bytes, sample.limit_bytes);
        result.available_bytes =
            std::min(result.available_bytes, headroom);
    }
    result.available_bytes =
        std::min(result.available_bytes, result.total_bytes);
    return result;
}

[[nodiscard]] inline effective_cpu_capacity constrain_cpu_capacity(
    unsigned int host_cores,
    std::optional<unsigned int> affinity_cores,
    std::optional<unsigned int> cpuset_cores,
    std::span<const cgroup_cpu_quota_sample> quotas) {
    effective_cpu_capacity result{
        .cores = static_cast<double>(host_cores)};
    bool initialized = host_cores != 0;
    const auto apply_integral_limit =
        [&result, &initialized](std::optional<unsigned int> limit) {
            if (!limit) {
                return;
            }
            const double value = static_cast<double>(*limit);
            result.cores =
                initialized ? std::min(result.cores, value) : value;
            initialized = true;
        };
    apply_integral_limit(affinity_cores);
    apply_integral_limit(cpuset_cores);

    double smallest_quota =
        std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < quotas.size(); ++index) {
        const cgroup_cpu_quota_sample& sample = quotas[index];
        if (sample.quota_microseconds == 0
            || sample.period_microseconds == 0) {
            throw std::invalid_argument(
                "cgroup CPU quota and period must be positive");
        }
        const double cores =
            static_cast<double>(sample.quota_microseconds)
            / static_cast<double>(sample.period_microseconds);
        if (!result.quota_sample || cores < smallest_quota) {
            smallest_quota = cores;
            result.quota_sample = index;
        }
        result.cores =
            initialized ? std::min(result.cores, cores) : cores;
        initialized = true;
    }
    if (!initialized) {
        result.cores = 0.0;
    }
    return result;
}

[[nodiscard]] inline cgroup_controller_version
select_cgroup_controller_version(
    bool v2_interface_observed,
    bool v1_interface_observed) noexcept {
    if (v2_interface_observed) {
        return cgroup_controller_version::v2;
    }
    if (v1_interface_observed) {
        return cgroup_controller_version::v1;
    }
    return cgroup_controller_version::none;
}

[[nodiscard]] inline bool cgroup_namespace_path_is_safe(
    std::string_view text) noexcept {
    try {
        const std::filesystem::path path{std::string(text)};
        if (!path.is_absolute()) {
            return false;
        }
        for (const std::filesystem::path& component : path) {
            if (component == "..") {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] inline bool scheduler_affinity_error_requires_resize(
    int error_number) noexcept {
#if defined(__linux__)
    return error_number == EINVAL;
#else
    static_cast<void>(error_number);
    return false;
#endif
}

[[nodiscard]] inline bool cgroup_membership_is_resolvable(
    std::string_view membership,
    std::string_view mount_root) noexcept {
    try {
        if (!cgroup_namespace_path_is_safe(membership)
            || !cgroup_namespace_path_is_safe(mount_root)) {
            return false;
        }
        const std::filesystem::path normalized_membership =
            std::filesystem::path{std::string(membership)}
                .lexically_normal();
        const std::filesystem::path normalized_root =
            std::filesystem::path{std::string(mount_root)}
                .lexically_normal();
        if (normalized_membership == "/") {
            return normalized_root == "/";
        }
        const std::filesystem::path relative =
            normalized_membership.lexically_relative(normalized_root);
        return !relative.empty()
            && *relative.begin() != "..";
    } catch (...) {
        return false;
    }
}

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::qualification_snapshot_detail {

[[nodiscard]] std::vector<std::string_view> fields(
    std::string_view line) {
    std::vector<std::string_view> result;
    std::size_t begin{};
    while (begin <= line.size()) {
        const std::size_t end = line.find('\t', begin);
        result.push_back(line.substr(
            begin, end == std::string_view::npos
                ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

template <class Integer>
[[nodiscard]] std::optional<Integer> integer(
    std::string_view text) {
    Integer value{};
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{}
        || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<double> real(std::string_view text) {
    constexpr std::size_t max_real_text_size = 128;
    if (text.empty() || text.size() > max_real_text_size) {
        return std::nullopt;
    }
    double value{};
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    if (!input || input.peek() != std::char_traits<char>::eof()
        || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<bool> boolean(std::string_view text) {
    if (text == "0") {
        return false;
    }
    if (text == "1") {
        return true;
    }
    return std::nullopt;
}

[[nodiscard]] bool header(
    std::istringstream& input, std::size_t& count) {
    std::string line;
    if (!std::getline(input, line)) {
        return false;
    }
    const auto values = fields(line);
    const auto parsed_count =
        values.size() == 3
        ? integer<std::size_t>(values[2]) : std::nullopt;
    if (values.size() != 3
        || values[0] != "bioseq-watchdog-v1"
        || values[1] != "qualifications"
        || !parsed_count || *parsed_count > 10'000U) {
        return false;
    }
    count = *parsed_count;
    return true;
}

}  // namespace bioseq::postgres_test::qualification_snapshot_detail

export namespace bioseq::postgres_test {

[[nodiscard]] std::optional<double>
parse_finite_real(std::string_view text) {
    return qualification_snapshot_detail::real(text);
}

/// @brief Publishes the complete current host-qualification registry.
[[nodiscard]] inline bool
publish_supervision_host_qualification_reports(
    std::span<const host_qualification_report> reports) noexcept {
    try {
        if (reports.size() > 10'000U) {
            return false;
        }
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(
            std::numeric_limits<double>::max_digits10);
        output << "bioseq-watchdog-v1\tqualifications\t"
               << reports.size() << '\n';
        for (const host_qualification_report& report : reports) {
            output << "Q\t"
                   << report.ordinal << '\t'
                   << supervision_hex_encode(report.test_case) << '\t'
                   << (report.status == host_qualification_status::passed
                       ? 0 : 1) << '\t'
                   << supervision_hex_encode(report.detail) << '\t'
                   << report.total_ram_bytes << '\t'
                   << report.available_ram_bytes << '\t'
                   << report.logical_cores << '\t'
                   << report.host_total_ram_bytes << '\t'
                   << report.host_available_ram_bytes << '\t'
                   << report.effective_total_ram_bytes << '\t'
                   << report.effective_available_ram_bytes << '\t'
                   << report.cgroup_memory_total_limit_bytes << '\t'
                   << report.cgroup_memory_headroom_limit_bytes << '\t'
                   << report.cgroup_memory_current_bytes << '\t'
                   << report.cgroup_memory_headroom_bytes << '\t'
                   << supervision_hex_encode(
                          report.cgroup_memory_version) << '\t'
                   << supervision_hex_encode(
                          report.cgroup_memory_total_source) << '\t'
                   << supervision_hex_encode(
                          report.cgroup_memory_headroom_source) << '\t'
                   << report.host_logical_cores << '\t'
                   << report.affinity_logical_cores << '\t'
                   << report.cpuset_logical_cores << '\t'
                   << report.cgroup_cpu_quota_microseconds << '\t'
                   << report.cgroup_cpu_period_microseconds << '\t'
                   << report.cgroup_cpu_quota_cores << '\t'
                   << report.effective_cpu_cores << '\t'
                   << supervision_hex_encode(
                          report.cgroup_cpu_version) << '\t'
                   << supervision_hex_encode(
                          report.cgroup_cpu_source) << '\t'
                   << report.required_total_ram_bytes << '\t'
                   << report.required_available_ram_bytes << '\t'
                   << report.required_logical_cores << '\t'
                   << report.required_io_mib_per_second << '\t'
                   << report.duration.count() << '\t'
                   << report.filesystems.size() << '\n';
            for (const filesystem_qualification_report& filesystem :
                 report.filesystems) {
                output << "F\t"
                       << supervision_hex_encode(filesystem.role) << '\t'
                       << supervision_hex_encode(
                              filesystem.path.string()) << '\t'
                       << filesystem.capacity_bytes << '\t'
                       << filesystem.available_bytes << '\t'
                       << filesystem.required_available_bytes << '\t'
                       << filesystem.device_id << '\t'
                       << (filesystem.distinct_from_data ? 1 : 0) << '\t'
                       << (filesystem.passed ? 1 : 0) << '\n';
            }
            output << "I\t"
                   << supervision_hex_encode(
                          report.io.path.string()) << '\t'
                   << report.io.bytes << '\t'
                   << report.io.write_duration.count() << '\t'
                   << report.io.read_duration.count() << '\t'
                   << report.io.write_mib_per_second << '\t'
                   << report.io.read_mib_per_second << '\t'
                   << supervision_hex_encode(
                          report.io.cache_eviction_method) << '\t'
                   << (report.io.cache_eviction_succeeded ? 1 : 0) << '\t'
                   << (report.io.passed ? 1 : 0) << '\n';
        }
        return publish_supervision_snapshot(
            "qualifications.snapshot.tsv", output.str());
    } catch (...) {
        return false;
    }
}

/// @brief Loads one complete, strictly bounded qualification snapshot.
[[nodiscard]] inline
std::optional<std::vector<host_qualification_report>>
load_supervision_host_qualification_reports(
    const std::filesystem::path& directory) noexcept {
    try {
        const auto payload = load_supervision_snapshot(
            directory, "qualifications.snapshot.tsv");
        if (!payload) {
            return std::nullopt;
        }
        std::istringstream input(*payload);
        input.imbue(std::locale::classic());
        std::size_t count{};
        if (!qualification_snapshot_detail::header(input, count)) {
            return std::nullopt;
        }
        std::vector<host_qualification_report> reports;
        reports.reserve(count);
        std::string line;
        for (std::size_t report_index = 0;
             report_index < count; ++report_index) {
            if (!std::getline(input, line)) {
                return std::nullopt;
            }
            const auto values =
                qualification_snapshot_detail::fields(line);
            if (values.size() != 34 || values[0] != "Q") {
                return std::nullopt;
            }
            const auto ordinal =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[1]);
            const auto test_case =
                supervision_hex_decode(values[2]);
            const auto status =
                qualification_snapshot_detail::integer<
                    unsigned int>(values[3]);
            const auto detail =
                supervision_hex_decode(values[4]);
            const auto total_ram =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[5]);
            const auto available_ram =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[6]);
            const auto logical_cores =
                qualification_snapshot_detail::integer<
                    unsigned int>(values[7]);
            const auto host_total =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[8]);
            const auto host_available =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[9]);
            const auto effective_total =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[10]);
            const auto effective_available =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[11]);
            const auto memory_total_limit =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[12]);
            const auto memory_headroom_limit =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[13]);
            const auto memory_current =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[14]);
            const auto memory_headroom =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[15]);
            const auto memory_version =
                supervision_hex_decode(values[16]);
            const auto memory_total_source =
                supervision_hex_decode(values[17]);
            const auto memory_headroom_source =
                supervision_hex_decode(values[18]);
            const auto host_cores =
                qualification_snapshot_detail::integer<
                    unsigned int>(values[19]);
            const auto affinity_cores =
                qualification_snapshot_detail::integer<
                    unsigned int>(values[20]);
            const auto cpuset_cores =
                qualification_snapshot_detail::integer<
                    unsigned int>(values[21]);
            const auto cpu_quota =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[22]);
            const auto cpu_period =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[23]);
            const auto cpu_quota_cores =
                qualification_snapshot_detail::real(values[24]);
            const auto effective_cpu_cores =
                qualification_snapshot_detail::real(values[25]);
            const auto cpu_version =
                supervision_hex_decode(values[26]);
            const auto cpu_source =
                supervision_hex_decode(values[27]);
            const auto required_total =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[28]);
            const auto required_available =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(values[29]);
            const auto required_cores =
                qualification_snapshot_detail::integer<
                    unsigned int>(values[30]);
            const auto required_io =
                qualification_snapshot_detail::real(values[31]);
            const auto duration =
                qualification_snapshot_detail::integer<
                    std::int64_t>(values[32]);
            const auto filesystem_count =
                qualification_snapshot_detail::integer<
                    std::size_t>(values[33]);
            if (!ordinal || !test_case || !status || *status > 1
                || !detail || !total_ram || !available_ram
                || !logical_cores || !host_total || !host_available
                || !effective_total || !effective_available
                || !memory_total_limit || !memory_headroom_limit
                || !memory_current || !memory_headroom
                || !memory_version || !memory_total_source
                || !memory_headroom_source || !host_cores
                || !affinity_cores || !cpuset_cores || !cpu_quota
                || !cpu_period || !cpu_quota_cores
                || !effective_cpu_cores || !cpu_version
                || !cpu_source || !required_total
                || !required_available || !required_cores
                || !required_io || !duration || *duration < 0
                || !filesystem_count || *filesystem_count > 10'000U) {
                return std::nullopt;
            }

            host_qualification_report report;
            report.ordinal = *ordinal;
            report.test_case = std::move(*test_case);
            report.status = *status == 0
                ? host_qualification_status::passed
                : host_qualification_status::
                    infrastructure_inconclusive;
            report.detail = std::move(*detail);
            report.total_ram_bytes = *total_ram;
            report.available_ram_bytes = *available_ram;
            report.logical_cores = *logical_cores;
            report.host_total_ram_bytes = *host_total;
            report.host_available_ram_bytes = *host_available;
            report.effective_total_ram_bytes = *effective_total;
            report.effective_available_ram_bytes =
                *effective_available;
            report.cgroup_memory_total_limit_bytes =
                *memory_total_limit;
            report.cgroup_memory_headroom_limit_bytes =
                *memory_headroom_limit;
            report.cgroup_memory_current_bytes = *memory_current;
            report.cgroup_memory_headroom_bytes = *memory_headroom;
            report.cgroup_memory_version = std::move(*memory_version);
            report.cgroup_memory_total_source =
                std::move(*memory_total_source);
            report.cgroup_memory_headroom_source =
                std::move(*memory_headroom_source);
            report.host_logical_cores = *host_cores;
            report.affinity_logical_cores = *affinity_cores;
            report.cpuset_logical_cores = *cpuset_cores;
            report.cgroup_cpu_quota_microseconds = *cpu_quota;
            report.cgroup_cpu_period_microseconds = *cpu_period;
            report.cgroup_cpu_quota_cores = *cpu_quota_cores;
            report.effective_cpu_cores = *effective_cpu_cores;
            report.cgroup_cpu_version = std::move(*cpu_version);
            report.cgroup_cpu_source = std::move(*cpu_source);
            report.required_total_ram_bytes = *required_total;
            report.required_available_ram_bytes =
                *required_available;
            report.required_logical_cores = *required_cores;
            report.required_io_mib_per_second = *required_io;
            report.duration = std::chrono::milliseconds(*duration);
            report.filesystems.reserve(*filesystem_count);

            for (std::size_t filesystem_index = 0;
                 filesystem_index < *filesystem_count;
                 ++filesystem_index) {
                if (!std::getline(input, line)) {
                    return std::nullopt;
                }
                const auto filesystem_values =
                    qualification_snapshot_detail::fields(line);
                if (filesystem_values.size() != 9
                    || filesystem_values[0] != "F") {
                    return std::nullopt;
                }
                const auto role =
                    supervision_hex_decode(filesystem_values[1]);
                const auto path =
                    supervision_hex_decode(filesystem_values[2]);
                const auto capacity =
                    qualification_snapshot_detail::integer<
                        std::uint64_t>(filesystem_values[3]);
                const auto available =
                    qualification_snapshot_detail::integer<
                        std::uint64_t>(filesystem_values[4]);
                const auto required =
                    qualification_snapshot_detail::integer<
                        std::uint64_t>(filesystem_values[5]);
                const auto device =
                    qualification_snapshot_detail::integer<
                        std::uint64_t>(filesystem_values[6]);
                const auto distinct =
                    qualification_snapshot_detail::boolean(
                        filesystem_values[7]);
                const auto passed =
                    qualification_snapshot_detail::boolean(
                        filesystem_values[8]);
                if (!role || !path || !capacity || !available
                    || !required || !device || !distinct || !passed) {
                    return std::nullopt;
                }
                report.filesystems.push_back(
                    filesystem_qualification_report{
                        .role = std::move(*role),
                        .path =
                            std::filesystem::path{
                                std::move(*path)},
                        .capacity_bytes = *capacity,
                        .available_bytes = *available,
                        .required_available_bytes = *required,
                        .device_id = *device,
                        .distinct_from_data = *distinct,
                        .passed = *passed});
            }

            if (!std::getline(input, line)) {
                return std::nullopt;
            }
            const auto io_values =
                qualification_snapshot_detail::fields(line);
            if (io_values.size() != 10 || io_values[0] != "I") {
                return std::nullopt;
            }
            const auto io_path =
                supervision_hex_decode(io_values[1]);
            const auto io_bytes =
                qualification_snapshot_detail::integer<
                    std::uint64_t>(io_values[2]);
            const auto write_duration =
                qualification_snapshot_detail::integer<
                    std::int64_t>(io_values[3]);
            const auto read_duration =
                qualification_snapshot_detail::integer<
                    std::int64_t>(io_values[4]);
            const auto write_speed =
                qualification_snapshot_detail::real(io_values[5]);
            const auto read_speed =
                qualification_snapshot_detail::real(io_values[6]);
            const auto eviction_method =
                supervision_hex_decode(io_values[7]);
            const auto eviction_succeeded =
                qualification_snapshot_detail::boolean(io_values[8]);
            const auto io_passed =
                qualification_snapshot_detail::boolean(io_values[9]);
            if (!io_path || !io_bytes || !write_duration
                || *write_duration < 0 || !read_duration
                || *read_duration < 0 || !write_speed || !read_speed
                || !eviction_method || !eviction_succeeded
                || !io_passed) {
                return std::nullopt;
            }
            report.io = io_qualification_report{
                .path =
                    std::filesystem::path{
                        std::move(*io_path)},
                .bytes = *io_bytes,
                .write_duration =
                    std::chrono::milliseconds(*write_duration),
                .read_duration =
                    std::chrono::milliseconds(*read_duration),
                .write_mib_per_second = *write_speed,
                .read_mib_per_second = *read_speed,
                .cache_eviction_method =
                    std::move(*eviction_method),
                .cache_eviction_succeeded = *eviction_succeeded,
                .passed = *io_passed};
            reports.push_back(std::move(report));
        }
        if (std::getline(input, line)) {
            return std::nullopt;
        }
        return reports;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::qualification_detail {

struct qualification_state {
    std::mutex mutex;
    std::vector<host_qualification_report> reports;
    std::uint64_t next_ordinal{};
};

[[nodiscard]] inline qualification_state& process_state() {
    static qualification_state state;
    return state;
}

struct memory_capacity {
    std::uint64_t total{};
    std::uint64_t available{};
};

struct cgroup_mount {
    std::filesystem::path root;
    std::filesystem::path mount_point;
    std::string filesystem;
    std::string super_options;
};

struct cgroup_memberships {
    std::optional<std::filesystem::path> unified;
    std::optional<std::filesystem::path> memory;
    std::optional<std::filesystem::path> cpu;
    std::optional<std::filesystem::path> cpuset;
};

struct cgroup_layout {
    std::optional<std::filesystem::path> unified;
    std::optional<std::filesystem::path> unified_mount;
    std::optional<std::filesystem::path> memory;
    std::optional<std::filesystem::path> memory_mount;
    std::optional<std::filesystem::path> cpu;
    std::optional<std::filesystem::path> cpu_mount;
    std::optional<std::filesystem::path> cpuset;
    std::optional<std::filesystem::path> cpuset_mount;
};

struct discovered_memory_limits {
    std::string version{"none"};
    std::vector<cgroup_memory_limit_sample> samples;
    std::vector<std::filesystem::path> sources;
};

struct discovered_cpu_limits {
    std::string version{"none"};
    std::vector<cgroup_cpu_quota_sample> samples;
    std::vector<std::filesystem::path> sources;
    std::optional<unsigned int> cpuset_cores;
};

struct qualified_memory_capacity {
    memory_capacity host;
    effective_memory_capacity effective;
    discovered_memory_limits cgroup;
};

struct qualified_cpu_capacity {
    unsigned int host_cores{};
    std::optional<unsigned int> affinity_cores;
    discovered_cpu_limits cgroup;
    effective_cpu_capacity effective;
};

[[nodiscard]] std::uint64_t checked_product(
    std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs != 0
        && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw std::overflow_error("host memory byte count overflow");
    }
    return lhs * rhs;
}

[[nodiscard]] memory_capacity host_memory() {
#if defined(__APPLE__)
    std::uint64_t total{};
    std::size_t total_size = sizeof(total);
    if (::sysctlbyname(
            "hw.memsize", &total, &total_size, nullptr, 0) != 0) {
        throw std::runtime_error("cannot read hw.memsize");
    }
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t statistics{};
    if (::host_statistics64(
            mach_host_self(), HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&statistics), &count)
        != KERN_SUCCESS) {
        throw std::runtime_error("cannot read host VM statistics");
    }
    vm_size_t page_size{};
    if (::host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS) {
        throw std::runtime_error("cannot read host page size");
    }
    const auto available =
        bioseq::common::memory::
            darwin_conservative_available_page_bytes(
                static_cast<std::uint64_t>(statistics.free_count),
                static_cast<std::uint64_t>(statistics.inactive_count),
                static_cast<std::uint64_t>(page_size));
    if (!available) {
        throw std::overflow_error(
            "invalid or overflowing host available-memory page observation");
    }
    return {
        .total = total,
        .available = *available};
#elif defined(__linux__)
    std::ifstream input("/proc/meminfo");
    std::uint64_t total_kib{};
    std::uint64_t available_kib{};
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t value{};
        std::string unit;
        fields >> key >> value >> unit;
        if (!fields) {
            continue;
        }
        if (key == "MemTotal:") {
            total_kib = value;
        } else if (key == "MemAvailable:") {
            available_kib = value;
        }
    }
    if (total_kib == 0 || available_kib == 0) {
        throw std::runtime_error(
            "cannot read MemTotal/MemAvailable from /proc/meminfo");
    }
    return {
        .total = checked_product(total_kib, 1024U),
        .available = checked_product(available_kib, 1024U)};
#else
    throw std::runtime_error(
        "host available-memory qualification is unsupported");
#endif
}

[[nodiscard]] std::optional<std::string> read_optional_text(
    const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        if (error) {
            throw std::runtime_error(
                "cannot inspect cgroup file " + path.string()
                + ": " + error.message());
        }
        return std::nullopt;
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            "cannot read cgroup file " + path.string());
    }
    std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        throw std::runtime_error(
            "cannot complete cgroup read " + path.string());
    }
    return text;
}

void enforce_linux_memory_node_scope() {
#if defined(__linux__)
    const auto task_status =
        read_optional_text("/proc/thread-self/status");
    if (!task_status) {
        throw std::runtime_error(
            "cannot establish task Mems_allowed_list");
    }
    const std::string allowed =
        parse_linux_mems_allowed_list(*task_status);
    const auto online_text = read_optional_text(
        "/sys/devices/system/node/online");
    if (!online_text) {
        throw std::runtime_error(
            "cannot establish online Linux memory nodes");
    }
    const std::string online(
        qualification_parse_detail::trim_ascii(*online_text));
    const linux_memory_node_scope scope =
        classify_linux_memory_node_scope(allowed, online);
    if (scope.restricted) {
        throw std::runtime_error(
            "strict NUMA memory-node scope cannot be conservatively "
            "qualified: Mems_allowed_list="
            + allowed + ", online=" + online);
    }
#endif
}

[[nodiscard]] std::string decode_mount_field(std::string_view field) {
    std::string decoded;
    decoded.reserve(field.size());
    for (std::size_t index = 0; index < field.size(); ++index) {
        if (field[index] != '\\' || index + 3 >= field.size()
            || field[index + 1] < '0' || field[index + 1] > '7'
            || field[index + 2] < '0' || field[index + 2] > '7'
            || field[index + 3] < '0' || field[index + 3] > '7') {
            decoded.push_back(field[index]);
            continue;
        }
        const unsigned int value =
            static_cast<unsigned int>(field[index + 1] - '0') * 64U
            + static_cast<unsigned int>(field[index + 2] - '0') * 8U
            + static_cast<unsigned int>(field[index + 3] - '0');
        decoded.push_back(static_cast<char>(value));
        index += 3;
    }
    return decoded;
}

[[nodiscard]] bool comma_field_contains(
    std::string_view fields, std::string_view expected) {
    while (!fields.empty()) {
        const std::size_t separator = fields.find(',');
        if (fields.substr(0, separator) == expected) {
            return true;
        }
        if (separator == std::string_view::npos) {
            return false;
        }
        fields.remove_prefix(separator + 1);
    }
    return false;
}

[[nodiscard]] cgroup_memberships parse_cgroup_memberships(
    std::string_view text) {
    cgroup_memberships memberships;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line =
            text.substr(0, newline);
        const std::size_t first = line.find(':');
        const std::size_t second =
            first == std::string_view::npos
            ? std::string_view::npos : line.find(':', first + 1);
        if (first != std::string_view::npos
            && second != std::string_view::npos
            && second + 1 < line.size()) {
            const std::string_view controllers =
                line.substr(first + 1, second - first - 1);
            const std::filesystem::path membership{
                std::string(line.substr(second + 1))};
            if (controllers.empty()) {
                memberships.unified = membership;
            } else {
                std::string_view remaining = controllers;
                while (!remaining.empty()) {
                    const std::size_t separator =
                        remaining.find(',');
                    const std::string_view controller =
                        remaining.substr(0, separator);
                    if (controller == "memory") {
                        memberships.memory = membership;
                    } else if (controller == "cpu") {
                        memberships.cpu = membership;
                    } else if (controller == "cpuset") {
                        memberships.cpuset = membership;
                    }
                    if (separator == std::string_view::npos) {
                        break;
                    }
                    remaining.remove_prefix(separator + 1);
                }
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1);
    }
    return memberships;
}

[[nodiscard]] std::vector<cgroup_mount> parse_cgroup_mounts(
    std::string_view text) {
    std::vector<cgroup_mount> mounts;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        const std::size_t separator = line.find(" - ");
        if (separator != std::string_view::npos) {
            std::istringstream left{
                std::string(line.substr(0, separator))};
            std::string mount_id;
            std::string parent_id;
            std::string device;
            std::string root;
            std::string mount_point;
            left >> mount_id >> parent_id >> device >> root >> mount_point;
            std::istringstream right{
                std::string(line.substr(separator + 3))};
            std::string filesystem;
            std::string source;
            std::string super_options;
            right >> filesystem >> source >> super_options;
            if (left && right
                && (filesystem == "cgroup2"
                    || filesystem == "cgroup")) {
                mounts.push_back(cgroup_mount{
                    .root = decode_mount_field(root),
                    .mount_point = decode_mount_field(mount_point),
                    .filesystem = std::move(filesystem),
                    .super_options = std::move(super_options)});
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1);
    }
    return mounts;
}

[[nodiscard]] std::optional<std::filesystem::path>
resolve_cgroup_directory(
    const std::filesystem::path& membership,
    const cgroup_mount& mount) {
    if (!cgroup_namespace_path_is_safe(membership.string())
        || !cgroup_namespace_path_is_safe(mount.root.string())
        || !cgroup_namespace_path_is_safe(
            mount.mount_point.string())) {
        throw std::runtime_error(
            "unsafe namespace-relative cgroup path");
    }
    const std::filesystem::path normalized_membership =
        membership.lexically_normal();
    const std::filesystem::path normalized_root =
        mount.root.lexically_normal();
    if (!cgroup_membership_is_resolvable(
            membership.string(), mount.root.string())) {
        return std::nullopt;
    }
    if (normalized_membership == "/") {
        return mount.mount_point.lexically_normal();
    }
    const std::filesystem::path relative =
        normalized_membership.lexically_relative(normalized_root);
    if (relative.empty()
        && normalized_membership != normalized_root) {
        return std::nullopt;
    }
    if (!relative.empty() && *relative.begin() == "..") {
        return std::nullopt;
    }
    return (mount.mount_point / relative).lexically_normal();
}

[[nodiscard]] cgroup_layout discover_cgroup_layout() {
    cgroup_layout layout;
#if defined(__linux__)
    const auto membership_text =
        read_optional_text("/proc/self/cgroup");
    const auto mount_text =
        read_optional_text("/proc/self/mountinfo");
    if (!membership_text || !mount_text
        || qualification_parse_detail::trim_ascii(
               *membership_text).empty()
        || qualification_parse_detail::trim_ascii(
               *mount_text).empty()) {
        throw std::runtime_error(
            "complete cgroup proc metadata is unavailable");
    }
    {
        const cgroup_memberships memberships =
            parse_cgroup_memberships(*membership_text);
        if (!memberships.unified && !memberships.memory
            && !memberships.cpu && !memberships.cpuset) {
            throw std::runtime_error(
                "cgroup proc membership metadata is malformed");
        }
        const std::vector<cgroup_mount> mounts =
            parse_cgroup_mounts(*mount_text);
        for (const cgroup_mount& mount : mounts) {
            if (mount.filesystem == "cgroup2"
                && memberships.unified && !layout.unified) {
                layout.unified =
                    resolve_cgroup_directory(
                        *memberships.unified, mount);
                if (layout.unified) {
                    layout.unified_mount = mount.mount_point;
                }
                continue;
            }
            if (mount.filesystem != "cgroup") {
                continue;
            }
            const auto assign =
                [&mount](
                    const std::optional<std::filesystem::path>& membership,
                    std::optional<std::filesystem::path>& directory,
                    std::optional<std::filesystem::path>& mount_point) {
                    if (!membership || directory) {
                        return;
                    }
                    directory =
                        resolve_cgroup_directory(*membership, mount);
                    if (directory) {
                        mount_point = mount.mount_point;
                    }
                };
            if (comma_field_contains(
                    mount.super_options, "memory")) {
                assign(
                    memberships.memory, layout.memory,
                    layout.memory_mount);
            }
            if (comma_field_contains(
                    mount.super_options, "cpu")) {
                assign(
                    memberships.cpu, layout.cpu,
                    layout.cpu_mount);
            }
            if (comma_field_contains(
                    mount.super_options, "cpuset")) {
                assign(
                    memberships.cpuset, layout.cpuset,
                    layout.cpuset_mount);
            }
        }
        if (memberships.unified && !layout.unified) {
            throw std::runtime_error(
                "cgroup v2 membership has no safe visible mount");
        }
        if (memberships.memory && !layout.memory) {
            throw std::runtime_error(
                "cgroup v1 memory membership has no safe visible mount");
        }
        if (memberships.cpu && !layout.cpu) {
            throw std::runtime_error(
                "cgroup v1 CPU membership has no safe visible mount");
        }
        if (memberships.cpuset && !layout.cpuset) {
            throw std::runtime_error(
                "cgroup v1 cpuset membership has no safe visible mount");
        }
        return layout;
    }
#endif
    return layout;
}

[[nodiscard]] std::vector<std::filesystem::path>
cgroup_ancestors(
    const std::filesystem::path& leaf,
    const std::filesystem::path& mount_point) {
    const std::filesystem::path normalized_leaf =
        leaf.lexically_normal();
    const std::filesystem::path normalized_mount =
        mount_point.lexically_normal();
    const std::filesystem::path relative =
        normalized_leaf.lexically_relative(normalized_mount);
    if ((!relative.empty() && *relative.begin() == "..")
        || (relative.empty() && normalized_leaf != normalized_mount)) {
        throw std::runtime_error(
            "resolved cgroup path escapes its mount");
    }
    std::vector<std::filesystem::path> ancestors;
    std::filesystem::path current = normalized_leaf;
    for (;;) {
        ancestors.push_back(current);
        if (current == normalized_mount) {
            break;
        }
        const std::filesystem::path parent =
            current.parent_path();
        if (parent == current || parent.empty()) {
            throw std::runtime_error(
                "cgroup ancestry did not reach its mount");
        }
        current = parent;
    }
    return ancestors;
}

[[nodiscard]] std::int64_t parse_signed_cgroup_value(
    std::string_view text, std::string_view field) {
    text = qualification_parse_detail::trim_ascii(text);
    std::int64_t value{};
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{}
        || end != text.data() + text.size()) {
        throw std::invalid_argument(
            "invalid cgroup " + std::string(field));
    }
    return value;
}

[[nodiscard]] discovered_memory_limits discover_memory_limits(
    const cgroup_layout& layout) {
    discovered_memory_limits limits;
#if defined(__linux__)
    if (layout.unified && layout.unified_mount) {
        bool controller_observed = false;
        for (const auto& directory :
             cgroup_ancestors(
                 *layout.unified, *layout.unified_mount)) {
            const auto maximum =
                read_optional_text(directory / "memory.max");
            if (!maximum) {
                continue;
            }
            controller_observed = true;
            const auto parsed =
                parse_cgroup_memory_limit(*maximum);
            if (!parsed) {
                continue;
            }
            const auto current =
                read_optional_text(directory / "memory.current");
            if (!current) {
                throw std::runtime_error(
                    "finite memory.max has no memory.current at "
                    + directory.string());
            }
            limits.samples.push_back(
                cgroup_memory_limit_sample{
                    .limit_bytes = *parsed,
                    .current_bytes =
                        qualification_parse_detail::parse_unsigned(
                            *current, "memory.current")});
            limits.sources.push_back(directory / "memory.max");
        }
        if (select_cgroup_controller_version(
                controller_observed,
                layout.memory.has_value()
                    && layout.memory_mount.has_value())
            == cgroup_controller_version::v2) {
            limits.version = "v2";
            return limits;
        }
    }
    if (select_cgroup_controller_version(
            false,
            layout.memory.has_value()
                && layout.memory_mount.has_value())
        == cgroup_controller_version::v1) {
        limits.version = "v1";
        for (const auto& directory :
             cgroup_ancestors(
                 *layout.memory, *layout.memory_mount)) {
            const auto maximum = read_optional_text(
                directory / "memory.limit_in_bytes");
            if (!maximum) {
                continue;
            }
            const auto current = read_optional_text(
                directory / "memory.usage_in_bytes");
            if (!current) {
                throw std::runtime_error(
                    "finite memory.limit_in_bytes has no usage at "
                    + directory.string());
            }
            limits.samples.push_back(
                cgroup_memory_limit_sample{
                    .limit_bytes =
                        qualification_parse_detail::parse_unsigned(
                            *maximum, "memory.limit_in_bytes"),
                    .current_bytes =
                        qualification_parse_detail::parse_unsigned(
                            *current, "memory.usage_in_bytes")});
            limits.sources.push_back(
                directory / "memory.limit_in_bytes");
        }
    }
#endif
    return limits;
}

[[nodiscard]] std::optional<unsigned int> scheduler_affinity_cores() {
#if defined(__linux__)
    std::size_t cpu_capacity = CPU_SETSIZE;
    constexpr std::size_t maximum_cpu_capacity = 1U << 20U;
    for (;;) {
        cpu_set_t* const affinity = CPU_ALLOC(cpu_capacity);
        if (affinity == nullptr) {
            throw std::bad_alloc();
        }
        const std::size_t bytes = CPU_ALLOC_SIZE(cpu_capacity);
        CPU_ZERO_S(bytes, affinity);
        errno = 0;
        const int result =
            ::sched_getaffinity(0, bytes, affinity);
        const int error_number = errno;
        if (result == 0) {
            const int count = CPU_COUNT_S(bytes, affinity);
            CPU_FREE(affinity);
            if (count <= 0) {
                throw std::runtime_error(
                    "scheduler affinity mask is empty");
            }
            return static_cast<unsigned int>(count);
        }
        CPU_FREE(affinity);
        if (!scheduler_affinity_error_requires_resize(
                error_number)) {
            throw std::system_error(
                error_number, std::generic_category(),
                "sched_getaffinity");
        }
        if (cpu_capacity > maximum_cpu_capacity / 2U) {
            throw std::runtime_error(
                "scheduler affinity mask exceeds safety bound");
        }
        cpu_capacity *= 2U;
    }
#else
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<unsigned int> discover_cpuset_cores(
    const cgroup_layout& layout) {
#if defined(__linux__)
    if (layout.unified && layout.unified_mount) {
        for (const auto& directory :
             cgroup_ancestors(
                 *layout.unified, *layout.unified_mount)) {
            const auto cpus = read_optional_text(
                directory / "cpuset.cpus.effective");
            if (cpus) {
                if (const auto count =
                        parse_cgroup_cpuset_count(*cpus)) {
                    return count;
                }
            }
        }
    }
    if (layout.cpuset && layout.cpuset_mount) {
        for (const auto& directory :
             cgroup_ancestors(
                 *layout.cpuset, *layout.cpuset_mount)) {
            const auto cpus =
                read_optional_text(directory / "cpuset.cpus");
            if (cpus) {
                if (const auto count =
                        parse_cgroup_cpuset_count(*cpus)) {
                    return count;
                }
            }
        }
    }
#else
    static_cast<void>(layout);
#endif
    return std::nullopt;
}

[[nodiscard]] discovered_cpu_limits discover_cpu_limits(
    const cgroup_layout& layout) {
    discovered_cpu_limits limits;
    limits.cpuset_cores = discover_cpuset_cores(layout);
#if defined(__linux__)
    if (layout.unified && layout.unified_mount) {
        bool controller_observed = false;
        for (const auto& directory :
             cgroup_ancestors(
                 *layout.unified, *layout.unified_mount)) {
            const auto maximum =
                read_optional_text(directory / "cpu.max");
            if (!maximum) {
                continue;
            }
            controller_observed = true;
            const auto parsed =
                parse_cgroup_cpu_max(*maximum);
            if (!parsed) {
                continue;
            }
            limits.samples.push_back(*parsed);
            limits.sources.push_back(directory / "cpu.max");
        }
        if (select_cgroup_controller_version(
                controller_observed,
                layout.cpu.has_value()
                    && layout.cpu_mount.has_value())
            == cgroup_controller_version::v2) {
            limits.version = "v2";
            return limits;
        }
    }
    if (select_cgroup_controller_version(
            false,
            layout.cpu.has_value()
                && layout.cpu_mount.has_value())
        == cgroup_controller_version::v1) {
        limits.version = "v1";
        for (const auto& directory :
             cgroup_ancestors(
                 *layout.cpu, *layout.cpu_mount)) {
            const auto quota = read_optional_text(
                directory / "cpu.cfs_quota_us");
            const auto period = read_optional_text(
                directory / "cpu.cfs_period_us");
            if (!quota && !period) {
                continue;
            }
            if (!quota || !period) {
                throw std::runtime_error(
                    "incomplete cgroup v1 CPU bandwidth pair at "
                    + directory.string());
            }
            const std::int64_t parsed_quota =
                parse_signed_cgroup_value(
                    *quota, "cpu.cfs_quota_us");
            if (parsed_quota == -1) {
                continue;
            }
            const std::uint64_t parsed_period =
                qualification_parse_detail::parse_unsigned(
                    *period, "cpu.cfs_period_us");
            if (parsed_quota <= 0 || parsed_period == 0) {
                throw std::invalid_argument(
                    "finite cgroup v1 CPU bandwidth is not positive");
            }
            limits.samples.push_back(cgroup_cpu_quota_sample{
                .quota_microseconds =
                    static_cast<std::uint64_t>(parsed_quota),
                .period_microseconds = parsed_period});
            limits.sources.push_back(
                directory / "cpu.cfs_quota_us");
        }
    }
#endif
    return limits;
}

[[nodiscard]] qualified_memory_capacity qualified_memory(
    const cgroup_layout& layout) {
    qualified_memory_capacity capacity;
    capacity.host = host_memory();
    capacity.cgroup = discover_memory_limits(layout);
    capacity.effective = constrain_memory_capacity(
        capacity.host.total, capacity.host.available,
        capacity.cgroup.samples);
    return capacity;
}

[[nodiscard]] qualified_cpu_capacity qualified_cpu(
    const cgroup_layout& layout) {
    qualified_cpu_capacity capacity;
    capacity.host_cores = std::thread::hardware_concurrency();
    capacity.affinity_cores = scheduler_affinity_cores();
    capacity.cgroup = discover_cpu_limits(layout);
    capacity.effective = constrain_cpu_capacity(
        capacity.host_cores, capacity.affinity_cores,
        capacity.cgroup.cpuset_cores, capacity.cgroup.samples);
    return capacity;
}

[[nodiscard]] std::uint64_t device_id(
    const std::filesystem::path& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        throw std::runtime_error(
            "cannot identify filesystem for " + path.string());
    }
    return static_cast<std::uint64_t>(status.st_dev);
}

class descriptor {
public:
    explicit descriptor(int value = -1) noexcept : value_(value) {}
    descriptor(const descriptor&) = delete;
    descriptor& operator=(const descriptor&) = delete;
    ~descriptor() noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }
    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] int release() noexcept {
        return std::exchange(value_, -1);
    }

private:
    int value_{};
};

class probe_file_guard {
public:
    explicit probe_file_guard(std::filesystem::path path)
        : path_(std::move(path)) {}
    probe_file_guard(const probe_file_guard&) = delete;
    probe_file_guard& operator=(const probe_file_guard&) = delete;
    ~probe_file_guard() noexcept {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

private:
    std::filesystem::path path_;
};

void write_all(int output, const std::byte* data, std::size_t size) {
    std::size_t offset{};
    while (offset < size) {
        const ssize_t written =
            ::write(output, data + offset, size - offset);
        if (written <= 0) {
            throw std::runtime_error(
                "sequential qualification write failed");
        }
        offset += static_cast<std::size_t>(written);
    }
}

void read_all(int input, std::byte* data, std::size_t size) {
    std::size_t offset{};
    while (offset < size) {
        const ssize_t received =
            ::read(input, data + offset, size - offset);
        if (received <= 0) {
            throw std::runtime_error(
                "sequential qualification read failed");
        }
        offset += static_cast<std::size_t>(received);
    }
}

[[nodiscard]] double throughput(
    std::uint64_t bytes,
    std::chrono::steady_clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(bytes)
        / static_cast<double>(qualification_mebibyte) / seconds;
}

[[nodiscard]] io_qualification_report probe_io(
    const std::filesystem::path& directory,
    std::uint64_t probe_bytes,
    double minimum_mib_per_second) {
    const std::filesystem::path probe_path =
        directory / ".bioseq-host-qualification-io-probe";
    probe_file_guard cleanup{probe_path};
    descriptor output{::open(
        probe_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600)};
    if (output.get() < 0) {
        throw std::runtime_error(
            "cannot create host qualification I/O probe");
    }
    constexpr std::size_t block_bytes = 8U * 1024U * 1024U;
    std::vector<std::byte> block(block_bytes, std::byte{0xa5});
    std::uint64_t remaining = probe_bytes;
    const auto write_started = std::chrono::steady_clock::now();
    while (remaining != 0) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, block.size()));
        write_all(output.get(), block.data(), count);
        remaining -= count;
    }
    if (::fsync(output.get()) != 0) {
        throw std::runtime_error(
            "cannot fsync host qualification I/O probe");
    }
    const auto write_finished = std::chrono::steady_clock::now();
    static_cast<void>(::close(output.release()));

    std::string eviction_method;
    bool eviction_succeeded = false;
    {
        descriptor eviction{
            ::open(probe_path.c_str(), O_RDONLY)};
        if (eviction.get() < 0) {
            throw std::runtime_error(
                "cannot reopen host qualification probe for eviction");
        }
#if defined(__linux__) && defined(POSIX_FADV_DONTNEED)
        eviction_method = "posix_fadvise(POSIX_FADV_DONTNEED)";
        eviction_succeeded =
            ::posix_fadvise(
                eviction.get(), 0, 0, POSIX_FADV_DONTNEED) == 0;
#elif defined(__APPLE__) && defined(F_GLOBAL_NOCACHE)
        eviction_method = "fcntl(F_GLOBAL_NOCACHE)+reopen(F_NOCACHE)";
        eviction_succeeded =
            ::fcntl(eviction.get(), F_GLOBAL_NOCACHE, 1) == 0;
        if (!eviction_succeeded) {
            eviction_method = "close+fcntl(F_NOCACHE)+reopen(F_NOCACHE)";
            eviction_succeeded =
                ::fcntl(eviction.get(), F_NOCACHE, 1) == 0;
        }
#else
        eviction_method = "unsupported";
#endif
    }

    descriptor input{::open(probe_path.c_str(), O_RDONLY)};
    if (input.get() < 0) {
        throw std::runtime_error(
            "cannot reopen host qualification I/O probe for reading");
    }
#if defined(__APPLE__) && defined(F_NOCACHE)
    eviction_succeeded =
        eviction_succeeded
        && ::fcntl(input.get(), F_NOCACHE, 1) == 0;
#endif
    remaining = probe_bytes;
    const auto read_started = std::chrono::steady_clock::now();
    while (remaining != 0) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, block.size()));
        read_all(input.get(), block.data(), count);
        remaining -= count;
    }
    const auto read_finished = std::chrono::steady_clock::now();

    const double write_speed =
        throughput(probe_bytes, write_finished - write_started);
    const double read_speed =
        throughput(probe_bytes, read_finished - read_started);
    return {
        .path = probe_path,
        .bytes = probe_bytes,
        .write_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                write_finished - write_started),
        .read_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                read_finished - read_started),
        .write_mib_per_second = write_speed,
        .read_mib_per_second = read_speed,
        .cache_eviction_method = std::move(eviction_method),
        .cache_eviction_succeeded = eviction_succeeded,
        .passed =
            eviction_succeeded
            && write_speed >= minimum_mib_per_second
            && read_speed >= minimum_mib_per_second};
}

void append_failure(std::string& detail, std::string_view failure) {
    if (!detail.empty()) {
        detail += "; ";
    }
    detail += failure;
}

void record(host_qualification_report& report) {
    auto& state = process_state();
    std::scoped_lock lock(state.mutex);
    report.ordinal = state.next_ordinal++;
    state.reports.push_back(report);
    static_cast<void>(
        publish_supervision_host_qualification_reports(state.reports));
}

}  // namespace bioseq::postgres_test::qualification_detail

export namespace bioseq::postgres_test {

/// @brief Clears structured host evidence before one native test session.
inline void clear_host_qualification_reports() noexcept {
    try {
        auto& state = qualification_detail::process_state();
        std::scoped_lock lock(state.mutex);
        state.reports.clear();
        state.next_ordinal = 0;
    } catch (...) {
    }
}

/// @brief Returns all pre-initdb host qualification observations.
[[nodiscard]] inline std::vector<host_qualification_report>
host_qualification_reports() {
    auto& state = qualification_detail::process_state();
    std::scoped_lock lock(state.mutex);
    return state.reports;
}

/// @brief True when a case has an unsuccessful infrastructure observation.
[[nodiscard]] inline bool infrastructure_inconclusive_for_case(
    std::string_view test_case) {
    const auto reports = host_qualification_reports();
    return std::any_of(
        reports.begin(), reports.end(),
        [test_case](const host_qualification_report& report) {
            return report.test_case == test_case
                && report.status
                    == host_qualification_status::
                        infrastructure_inconclusive;
        });
}

/// @brief Qualifies exact host paths before initdb creates the cluster.
[[nodiscard]] inline host_qualification_report qualify_host_before_initdb(
    const std::filesystem::path& data_directory,
    host_qualification_requirements requirements = {}) {
    const auto started = std::chrono::steady_clock::now();
    host_qualification_report report{
        .test_case = requirements.test_case,
        .required_total_ram_bytes =
            requirements.minimum_total_ram_bytes,
        .required_available_ram_bytes =
            requirements.minimum_available_ram_bytes,
        .required_logical_cores =
            requirements.minimum_logical_cores,
        .required_io_mib_per_second =
            requirements.minimum_io_mib_per_second};
    try {
        std::filesystem::create_directories(data_directory);
        const std::filesystem::path resolved_data =
            std::filesystem::canonical(data_directory);
        const qualification_detail::cgroup_layout cgroups =
            qualification_detail::discover_cgroup_layout();
        const qualification_detail::qualified_memory_capacity memory =
            qualification_detail::qualified_memory(cgroups);

        report.host_total_ram_bytes = memory.host.total;
        report.host_available_ram_bytes = memory.host.available;
        report.effective_total_ram_bytes =
            memory.effective.total_bytes;
        report.effective_available_ram_bytes =
            memory.effective.available_bytes;
        report.total_ram_bytes =
            report.effective_total_ram_bytes;
        report.available_ram_bytes =
            report.effective_available_ram_bytes;
        report.cgroup_memory_version =
            memory.cgroup.version;
        if (memory.effective.total_limit_sample) {
            const std::size_t index =
                *memory.effective.total_limit_sample;
            report.cgroup_memory_total_limit_bytes =
                memory.cgroup.samples[index].limit_bytes;
            report.cgroup_memory_total_source =
                memory.cgroup.sources[index].string();
        }
        if (memory.effective.available_limit_sample) {
            const std::size_t index =
                *memory.effective.available_limit_sample;
            const cgroup_memory_limit_sample& sample =
                memory.cgroup.samples[index];
            report.cgroup_memory_headroom_limit_bytes =
                sample.limit_bytes;
            report.cgroup_memory_current_bytes =
                sample.current_bytes;
            report.cgroup_memory_headroom_bytes =
                sample.current_bytes >= sample.limit_bytes
                ? 0 : sample.limit_bytes - sample.current_bytes;
            report.cgroup_memory_headroom_source =
                memory.cgroup.sources[index].string();
        }

        qualification_detail::enforce_linux_memory_node_scope();
        const qualification_detail::qualified_cpu_capacity cpu =
            qualification_detail::qualified_cpu(cgroups);
        report.host_logical_cores = cpu.host_cores;
        report.affinity_logical_cores =
            cpu.affinity_cores.value_or(0);
        report.cpuset_logical_cores =
            cpu.cgroup.cpuset_cores.value_or(0);
        report.effective_cpu_cores = cpu.effective.cores;
        report.logical_cores =
            cpu.effective.cores
                >= static_cast<double>(
                    std::numeric_limits<unsigned int>::max())
            ? std::numeric_limits<unsigned int>::max()
            : static_cast<unsigned int>(cpu.effective.cores);
        report.cgroup_cpu_version = cpu.cgroup.version;
        if (cpu.effective.quota_sample) {
            const std::size_t index =
                *cpu.effective.quota_sample;
            const cgroup_cpu_quota_sample& sample =
                cpu.cgroup.samples[index];
            report.cgroup_cpu_quota_microseconds =
                sample.quota_microseconds;
            report.cgroup_cpu_period_microseconds =
                sample.period_microseconds;
            report.cgroup_cpu_quota_cores =
                static_cast<double>(sample.quota_microseconds)
                / static_cast<double>(sample.period_microseconds);
            report.cgroup_cpu_source =
                cpu.cgroup.sources[index].string();
        }

        if (report.effective_total_ram_bytes
            < requirements.minimum_total_ram_bytes) {
            qualification_detail::append_failure(
                report.detail,
                "effective total RAM is below the required minimum");
        }
        if (report.effective_available_ram_bytes
            < requirements.minimum_available_ram_bytes) {
            qualification_detail::append_failure(
                report.detail,
                "effective available RAM is below the required minimum");
        }
        if (report.effective_cpu_cores
            < static_cast<double>(
                requirements.minimum_logical_cores)) {
            qualification_detail::append_failure(
                report.detail,
                "effective CPU capacity is below the required minimum");
        }

        const std::filesystem::space_info data_space =
            std::filesystem::space(resolved_data);
        const std::uint64_t data_device =
            qualification_detail::device_id(resolved_data);
        const bool data_passed =
            data_space.available
            >= requirements.minimum_data_available_bytes;
        report.filesystems.push_back(filesystem_qualification_report{
            .role = "data",
            .path = resolved_data,
            .capacity_bytes = data_space.capacity,
            .available_bytes = data_space.available,
            .required_available_bytes =
                requirements.minimum_data_available_bytes,
            .device_id = data_device,
            .distinct_from_data = false,
            .passed = data_passed});
        if (!data_passed) {
            qualification_detail::append_failure(
                report.detail,
                "data filesystem has insufficient available capacity");
        }

        if (requirements.cache_temp_directory) {
            std::filesystem::create_directories(
                *requirements.cache_temp_directory);
            const std::filesystem::path resolved_cache_temp =
                std::filesystem::canonical(
                    *requirements.cache_temp_directory);
            const std::filesystem::space_info cache_temp_space =
                std::filesystem::space(resolved_cache_temp);
            const std::uint64_t cache_temp_device =
                qualification_detail::device_id(resolved_cache_temp);
            const bool distinct = cache_temp_device != data_device;
            const bool capacity_passed =
                cache_temp_space.available
                >= requirements
                    .minimum_distinct_cache_temp_available_bytes;
            report.filesystems.push_back(
                filesystem_qualification_report{
                    .role = "cache-temp",
                    .path = resolved_cache_temp,
                    .capacity_bytes = cache_temp_space.capacity,
                    .available_bytes = cache_temp_space.available,
                    .required_available_bytes =
                        requirements
                            .minimum_distinct_cache_temp_available_bytes,
                    .device_id = cache_temp_device,
                    .distinct_from_data = distinct,
                    .passed = distinct && capacity_passed});
            if (!distinct) {
                qualification_detail::append_failure(
                    report.detail,
                    "configured cache/temp path is not on a distinct "
                    "filesystem");
            }
            if (!capacity_passed) {
                qualification_detail::append_failure(
                    report.detail,
                    "cache/temp filesystem has insufficient available "
                    "capacity");
            }
        }

        report.io = qualification_detail::probe_io(
            resolved_data,
            requirements.io_probe_bytes,
            requirements.minimum_io_mib_per_second);
        if (!report.io.cache_eviction_succeeded) {
            qualification_detail::append_failure(
                report.detail,
                "the uncached read eviction/bypass operation failed");
        }
        if (report.io.write_mib_per_second
            < requirements.minimum_io_mib_per_second) {
            qualification_detail::append_failure(
                report.detail,
                "sequential write throughput is below the minimum");
        }
        if (report.io.read_mib_per_second
            < requirements.minimum_io_mib_per_second) {
            qualification_detail::append_failure(
                report.detail,
                "uncached sequential read throughput is below the minimum");
        }
    } catch (const std::exception& error) {
        qualification_detail::append_failure(report.detail, error.what());
    } catch (...) {
        qualification_detail::append_failure(
            report.detail, "unknown host qualification failure");
    }

    report.duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    report.status = report.detail.empty()
        ? host_qualification_status::passed
        : host_qualification_status::infrastructure_inconclusive;
    if (report.detail.empty()) {
        report.detail = "host qualification passed";
    }
    const bool passed =
        report.status == host_qualification_status::passed;
    const std::string detail = report.detail;
    qualification_detail::record(report);
    if (!passed) {
        throw infrastructure_inconclusive(detail);
    }
    return report;
}

}  // namespace bioseq::postgres_test
