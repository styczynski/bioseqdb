/**
 * @file common/memory_budget/index.cppm
 * @brief Dependency-free operating-system memory probes and budget policy.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#if defined(__APPLE__) || defined(__linux__)
#  include <unistd.h>
#endif

#if defined(__APPLE__)
#  include <mach/mach.h>
#  include <mach/task.h>
#  include <mach/task_info.h>
#  include <mach/vm_statistics.h>
#  include <sys/sysctl.h>
#endif

#if defined(__linux__)
#  include <cstdio>
#endif

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#endif

export module bioseq.common.memory_budget;

export namespace bioseq::common::memory {

/** A single coherent sample used by pure admission arithmetic. */
struct memory_snapshot {
    std::uint64_t process_footprint_bytes = 0;
    std::uint64_t system_available_bytes = 0;
    std::uint64_t system_total_bytes = 0;
};

/**
 * Convert Darwin's disjoint free and inactive page queues to bytes.
 *
 * `vm_statistics64_data_t::speculative_count` is already included in
 * `free_count`; accepting it here would double-count reclaimable capacity.
 * A missing value is a fail-closed arithmetic or page-size observation.
 */
[[nodiscard]] inline std::optional<std::uint64_t>
darwin_conservative_available_page_bytes(
    std::uint64_t free_pages,
    std::uint64_t inactive_pages,
    std::uint64_t bytes_per_page) noexcept {
    if (bytes_per_page == 0
        || free_pages
            > (std::numeric_limits<std::uint64_t>::max)()
                - inactive_pages) {
        return std::nullopt;
    }
    const std::uint64_t available_pages =
        free_pages + inactive_pages;
    if (available_pages
        > (std::numeric_limits<std::uint64_t>::max)()
            / bytes_per_page) {
        return std::nullopt;
    }
    return available_pages * bytes_per_page;
}

/** Return the operating-system page size, or a conservative fallback. */
[[nodiscard]] inline std::size_t page_size() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    const long value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : 4096u;
#elif defined(_WIN32)
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
#else
    return 4096u;
#endif
}

/** Return this process's resident/physical footprint, or zero if unavailable. */
[[nodiscard]] inline std::uint64_t process_footprint_bytes() noexcept {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const kern_return_t status = ::task_info(
        ::mach_task_self(), TASK_VM_INFO,
        reinterpret_cast<task_info_t>(&info), &count);
    if (status == KERN_SUCCESS && count >= TASK_VM_INFO_REV1_COUNT) {
        return static_cast<std::uint64_t>(info.phys_footprint);
    }
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t basic_count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(
            ::mach_task_self(), MACH_TASK_BASIC_INFO,
            reinterpret_cast<task_info_t>(&basic), &basic_count)
        == KERN_SUCCESS) {
        return static_cast<std::uint64_t>(basic.resident_size);
    }
    return 0;
#elif defined(__linux__)
    unsigned long long size_pages = 0;
    unsigned long long resident_pages = 0;
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    const int fields = std::fscanf(file, "%llu %llu", &size_pages, &resident_pages);
    std::fclose(file);
    return fields == 2
        ? static_cast<std::uint64_t>(resident_pages)
            * static_cast<std::uint64_t>(page_size())
        : 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(
            ::GetCurrentProcess(), &counters, sizeof(counters))) {
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
    return 0;
#else
    return 0;
#endif
}

/** Return total physical memory, or zero if unavailable. */
[[nodiscard]] inline std::uint64_t system_total_bytes() noexcept {
#if defined(__APPLE__)
    std::uint64_t memory = 0;
    std::size_t length = sizeof(memory);
    int selectors[2] = {CTL_HW, HW_MEMSIZE};
    return ::sysctl(selectors, 2, &memory, &length, nullptr, 0) == 0
        ? memory
        : 0;
#elif defined(__linux__)
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long bytes_per_page = ::sysconf(_SC_PAGESIZE);
    return pages > 0 && bytes_per_page > 0
        ? static_cast<std::uint64_t>(pages)
            * static_cast<std::uint64_t>(bytes_per_page)
        : 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return ::GlobalMemoryStatusEx(&status)
        ? static_cast<std::uint64_t>(status.ullTotalPhys)
        : 0;
#else
    return 0;
#endif
}

/** Return a conservative OS-specific availability sample, or zero. */
[[nodiscard]] inline std::uint64_t system_available_bytes() noexcept {
#if defined(__APPLE__)
    const mach_port_t host = ::mach_host_self();
    vm_size_t bytes_per_page = 0;
    if (::host_page_size(host, &bytes_per_page) != KERN_SUCCESS
        || bytes_per_page == 0) {
        bytes_per_page = static_cast<vm_size_t>(page_size());
    }
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(
            host, HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&statistics), &count)
        != KERN_SUCCESS) {
        return 0;
    }
    return darwin_conservative_available_page_bytes(
               static_cast<std::uint64_t>(statistics.free_count),
               static_cast<std::uint64_t>(statistics.inactive_count),
               static_cast<std::uint64_t>(bytes_per_page))
        .value_or(0);
#elif defined(__linux__)
    std::FILE* file = std::fopen("/proc/meminfo", "r");
    if (file == nullptr) {
        return 0;
    }
    char label[64]{};
    char unit[16]{};
    unsigned long long value_kb = 0;
    while (std::fscanf(file, "%63s %llu %15s", label, &value_kb, unit) >= 2) {
        if (label[0] == 'M' && label[1] == 'e' && label[2] == 'm'
            && label[3] == 'A') {
            std::fclose(file);
            return static_cast<std::uint64_t>(value_kb) * 1024u;
        }
    }
    std::fclose(file);
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return ::GlobalMemoryStatusEx(&status)
        ? static_cast<std::uint64_t>(status.ullAvailPhys)
        : 0;
#else
    return 0;
#endif
}

/** Policy governing process memory growth against live system memory. */
struct memory_budget {
    double cap_fraction = 0.80;
    std::uint64_t min_free_bytes = std::uint64_t{2} << 30;

    /** Obtain a coherent best-effort snapshot from the operating system. */
    [[nodiscard]] memory_snapshot snapshot() const noexcept {
        return memory_snapshot{
            .process_footprint_bytes = process_footprint_bytes(),
            .system_available_bytes = system_available_bytes(),
            .system_total_bytes = system_total_bytes(),
        };
    }

    /** Return this process's current footprint. */
    [[nodiscard]] std::uint64_t footprint_bytes() const noexcept {
        return process_footprint_bytes();
    }

    /** Return currently available system memory. */
    [[nodiscard]] std::uint64_t available_bytes() const noexcept {
        return system_available_bytes();
    }

    /** Return total system memory. */
    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return system_total_bytes();
    }

    /** Compute the footprint ceiling for a known total. */
    [[nodiscard]] std::uint64_t soft_cap_bytes(
        std::uint64_t total) const noexcept {
        if (total == 0) {
            return 0;
        }
        const double bounded_fraction = std::clamp(cap_fraction, 0.0, 1.0);
        return static_cast<std::uint64_t>(
            static_cast<double>(total) * bounded_fraction);
    }

    /** Compute the footprint ceiling from a live sample. */
    [[nodiscard]] std::uint64_t soft_cap_bytes() const noexcept {
        return soft_cap_bytes(total_bytes());
    }

    /** Compute allocatable bytes from a coherent snapshot. */
    [[nodiscard]] std::uint64_t allocatable_bytes(
        const memory_snapshot& sample) const noexcept {
        if (sample.system_total_bytes == 0
            || sample.system_available_bytes == 0) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        const std::uint64_t cap = soft_cap_bytes(sample.system_total_bytes);
        const std::uint64_t by_cap = sample.process_footprint_bytes < cap
            ? cap - sample.process_footprint_bytes
            : 0;
        const std::uint64_t by_free =
            sample.system_available_bytes > min_free_bytes
            ? sample.system_available_bytes - min_free_bytes
            : 0;
        return std::min(by_cap, by_free);
    }

    /** Compute currently allocatable bytes, treating unavailable probes as unbounded. */
    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        return allocatable_bytes(snapshot());
    }

    /** Report whether the supplied snapshot is beyond either safety boundary. */
    [[nodiscard]] bool under_pressure(
        const memory_snapshot& sample) const noexcept {
        if (sample.system_total_bytes == 0
            || sample.system_available_bytes == 0) {
            return false;
        }
        return sample.process_footprint_bytes
                > soft_cap_bytes(sample.system_total_bytes)
            || sample.system_available_bytes < min_free_bytes;
    }

    /** Report whether the live system is beyond either safety boundary. */
    [[nodiscard]] bool under_pressure() const noexcept {
        return under_pressure(snapshot());
    }

    /** Clamp a requested working set while preserving a progress floor. */
    [[nodiscard]] std::uint64_t clamp_working_set(
        std::uint64_t desired,
        std::uint64_t floor,
        std::uint64_t ceiling) const noexcept {
        const std::uint64_t normalized_ceiling = std::max(floor, ceiling);
        const std::uint64_t budget = allocatable_bytes();
        const std::uint64_t admitted = std::min(
            std::min(desired, normalized_ceiling), budget);
        return std::max(admitted, floor);
    }
};

/** Return the process-wide default memory-budget policy. */
[[nodiscard]] inline memory_budget& default_budget() noexcept {
    static memory_budget budget{};
    return budget;
}

} // namespace bioseq::common::memory
