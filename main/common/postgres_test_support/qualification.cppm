module;

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
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
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

export module bioseq.test.postgres:qualification;

export namespace bioseq::postgres_test {

inline constexpr std::uint64_t qualification_gibibyte =
    1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t qualification_mebibyte =
    1024ULL * 1024ULL;

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
    std::uint64_t total_ram_bytes{};
    std::uint64_t available_ram_bytes{};
    unsigned int logical_cores{};
    std::uint64_t required_total_ram_bytes{};
    std::uint64_t required_available_ram_bytes{};
    unsigned int required_logical_cores{};
    double required_io_mib_per_second{};
    std::vector<filesystem_qualification_report> filesystems;
    io_qualification_report io;
    std::chrono::milliseconds duration{};
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

[[nodiscard]] std::uint64_t checked_product(
    std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs != 0
        && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw std::overflow_error("host memory byte count overflow");
    }
    return lhs * rhs;
}

[[nodiscard]] memory_capacity memory() {
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
    const std::uint64_t available_pages =
        static_cast<std::uint64_t>(statistics.free_count)
        + static_cast<std::uint64_t>(statistics.inactive_count)
        + static_cast<std::uint64_t>(statistics.speculative_count);
    return {
        .total = total,
        .available = checked_product(
            available_pages,
            static_cast<std::uint64_t>(page_size))};
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
        const qualification_detail::memory_capacity memory =
            qualification_detail::memory();
        report.total_ram_bytes = memory.total;
        report.available_ram_bytes = memory.available;
        report.logical_cores = std::thread::hardware_concurrency();
        if (report.total_ram_bytes
            < requirements.minimum_total_ram_bytes) {
            qualification_detail::append_failure(
                report.detail, "total RAM is below the required minimum");
        }
        if (report.available_ram_bytes
            < requirements.minimum_available_ram_bytes) {
            qualification_detail::append_failure(
                report.detail,
                "available RAM is below the required minimum");
        }
        if (report.logical_cores
            < requirements.minimum_logical_cores) {
            qualification_detail::append_failure(
                report.detail,
                "logical core count is below the required minimum");
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
