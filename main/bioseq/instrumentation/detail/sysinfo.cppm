/**
 * @file instrumentation/detail/sysinfo.cppm
 * @brief System info — page size, CPU count, NUMA hints.
 *
 * Used by the calibration and pool-sizing logic. None of these functions
 * are called on the hot path.
 */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>

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

export module bioseq.instr.detail.sysinfo;

export namespace bioseq::instr::detail::sysinfo {

[[nodiscard]]
inline std::size_t page_size() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    long ps = ::sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<std::size_t>(ps) : 4096;
#elif defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<std::size_t>(si.dwPageSize);
#else
    return 4096;
#endif
}

[[nodiscard]]
inline unsigned hardware_concurrency() noexcept {
    auto n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
}

// ---------------------------------------------------------------------------
// Runtime memory probe (DOC0276). A small, dependency-free replacement for the
// vcpkg `hwinfo` path the bench `MemoryBudget` used: on ARM macOS that path
// routes "currently free" through `host_statistics64`, which the vcpkg build
// returns -1 (failure) for — so the bench framework saw free==0 and fell back
// to total, i.e. "memory estimates always zero". These call the mach/POSIX
// APIs directly, which DO work, and are NO_POSTGRES-safe (no PG symbols) so the
// production extension can consult them too.
// ---------------------------------------------------------------------------

/// Physical memory footprint of THIS process in bytes — the number Activity
/// Monitor shows as "Memory" on macOS (`phys_footprint`). 0 if unavailable.
[[nodiscard]]
inline std::uint64_t process_footprint_bytes() noexcept {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const kern_return_t kr = ::task_info(::mach_task_self(), TASK_VM_INFO,
                                         reinterpret_cast<task_info_t>(&info), &count);
    if (kr == KERN_SUCCESS && count >= TASK_VM_INFO_REV1_COUNT) {
        return static_cast<std::uint64_t>(info.phys_footprint);
    }
    // Older kernels: fall back to resident set size.
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t bcount = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&basic), &bcount) == KERN_SUCCESS) {
        return static_cast<std::uint64_t>(basic.resident_size);
    }
    return 0;
#elif defined(__linux__)
    // /proc/self/statm fields are in pages: size resident shared ...
    unsigned long long size_pages = 0, resident_pages = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r"); f != nullptr) {
        const int got = std::fscanf(f, "%llu %llu", &size_pages, &resident_pages);
        std::fclose(f);
        if (got == 2) {
            return static_cast<std::uint64_t>(resident_pages)
                 * static_cast<std::uint64_t>(page_size());
        }
    }
    return 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    return 0;
#endif
}

/// Total physical RAM in bytes. 0 if unavailable.
[[nodiscard]]
inline std::uint64_t system_total_bytes() noexcept {
#if defined(__APPLE__)
    std::uint64_t mem = 0;
    std::size_t len = sizeof(mem);
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    if (::sysctl(mib, 2, &mem, &len, nullptr, 0) == 0) return mem;
    return 0;
#elif defined(__linux__)
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long ps    = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && ps > 0) return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(ps);
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX st{}; st.dwLength = sizeof(st);
    if (::GlobalMemoryStatusEx(&st)) return static_cast<std::uint64_t>(st.ullTotalPhys);
    return 0;
#else
    return 0;
#endif
}

/// Best-effort "available to allocate without paging" bytes. On macOS this is
/// (free + inactive + speculative + purgeable) pages — what the kernel can hand
/// back under pressure without swapping. 0 if unavailable.
[[nodiscard]]
inline std::uint64_t system_available_bytes() noexcept {
#if defined(__APPLE__)
    const mach_port_t host = ::mach_host_self();
    vm_size_t page = 0;
    if (::host_page_size(host, &page) != KERN_SUCCESS || page == 0) {
        page = static_cast<vm_size_t>(page_size());
    }
    vm_statistics64_data_t vmstat{};
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(host, HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&vmstat), &cnt) == KERN_SUCCESS) {
        const std::uint64_t reclaimable =
            static_cast<std::uint64_t>(vmstat.free_count)
            + vmstat.inactive_count
            + vmstat.speculative_count
            + vmstat.purgeable_count;
        return reclaimable * static_cast<std::uint64_t>(page);
    }
    return 0;
#elif defined(__linux__)
    // Parse MemAvailable from /proc/meminfo (kB).
    if (std::FILE* f = std::fopen("/proc/meminfo", "r"); f != nullptr) {
        char label[64];
        unsigned long long value_kb = 0;
        char unit[16];
        while (std::fscanf(f, "%63s %llu %15s", label, &value_kb, unit) >= 2) {
            if (label[0] == 'M' && label[1] == 'e' && label[2] == 'm' &&
                label[3] == 'A') {  // "MemAvailable:"
                std::fclose(f);
                return value_kb * 1024ull;
            }
        }
        std::fclose(f);
    }
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX st{}; st.dwLength = sizeof(st);
    if (::GlobalMemoryStatusEx(&st)) return static_cast<std::uint64_t>(st.ullAvailPhys);
    return 0;
#else
    return 0;
#endif
}

}  // namespace bioseq::instr::detail::sysinfo

// ---------------------------------------------------------------------------
// MemoryBudget (DOC0276) — proof-of-concept process-wide memory governor.
//
// The thesis (from the task): the large-sequence read/index paths allocate
// without any awareness of physical memory, so a 6 Gbp run can drive the
// process into swap and stall. This budget gives those paths a cheap,
// live-updated answer to "how many more bytes can I allocate right now without
// risking paging?" so they can SIZE their work (e.g. the windowed minimizer
// decode buffer, the chunk cache) to fit RAM instead of assuming infinity.
//
// It is deliberately tiny and stateless-per-read: each query re-reads the mach
// counters above (a few microseconds), so it is always current — there is no
// stale cached snapshot to go wrong (contrast the bench `MemoryBudget`, which
// cached a value hwinfo never populated on ARM macOS). Callers that consult it
// in a hot loop should throttle their own calls.
// ---------------------------------------------------------------------------
export namespace bioseq::memory {

struct MemoryBudget {
    /// Keep the process footprint below this fraction of total RAM.
    double cap_fraction = 0.80;
    /// Always leave at least this many bytes reported "available" by the OS,
    /// so other processes (and the page cache) are not starved into swap.
    std::uint64_t min_free_bytes = std::uint64_t{2} << 30;  // 2 GiB

    [[nodiscard]] std::uint64_t footprint_bytes() const noexcept {
        return instr::detail::sysinfo::process_footprint_bytes();
    }
    [[nodiscard]] std::uint64_t available_bytes() const noexcept {
        return instr::detail::sysinfo::system_available_bytes();
    }
    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return instr::detail::sysinfo::system_total_bytes();
    }

    /// Footprint ceiling = cap_fraction · total. 0 if total is unknown.
    [[nodiscard]] std::uint64_t soft_cap_bytes() const noexcept {
        const std::uint64_t t = total_bytes();
        if (t == 0) return 0;
        return static_cast<std::uint64_t>(static_cast<double>(t) * cap_fraction);
    }

    /// Bytes we can still allocate without crossing the footprint cap OR
    /// dropping OS-available below `min_free_bytes`. Returns UINT64_MAX when the
    /// platform probe is unavailable (total/available read as 0) so callers
    /// degrade to their own static defaults rather than to "zero budget".
    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        const std::uint64_t total = total_bytes();
        const std::uint64_t avail = available_bytes();
        if (total == 0 || avail == 0) {
            return (std::numeric_limits<std::uint64_t>::max)();  // probe unavailable
        }
        const std::uint64_t cap  = soft_cap_bytes();
        const std::uint64_t fp   = footprint_bytes();
        const std::uint64_t by_cap  = fp   < cap            ? cap   - fp            : 0;
        const std::uint64_t by_free = avail > min_free_bytes ? avail - min_free_bytes : 0;
        return by_cap < by_free ? by_cap : by_free;
    }

    /// True when we are already at/over the footprint cap or below the free
    /// floor — a signal for caches to evict rather than grow.
    [[nodiscard]] bool under_pressure() const noexcept {
        const std::uint64_t total = total_bytes();
        const std::uint64_t avail = available_bytes();
        if (total == 0 || avail == 0) return false;  // probe unavailable → don't panic
        return footprint_bytes() > soft_cap_bytes() || avail < min_free_bytes;
    }

    /// Clamp a desired working-set size to the live budget, keeping it within
    /// [floor, ceil]. Used to size a streaming window so it both (a) stays
    /// within RAM and (b) never collapses below a useful minimum.
    [[nodiscard]] std::uint64_t clamp_working_set(std::uint64_t desired,
                                                  std::uint64_t floor,
                                                  std::uint64_t ceil) const noexcept {
        std::uint64_t budget = allocatable_bytes();
        std::uint64_t want = desired < ceil ? desired : ceil;
        if (want > budget) want = budget;
        if (want < floor) want = floor;  // floor wins — we must make progress
        return want;
    }
};

/// Process-wide default budget. POC default: footprint ≤ 80 % RAM, leave 2 GiB
/// free. Callers read `bioseq::memory::default_budget()` so tuning lives in one
/// place; it is a function-local static so there is no static-init-order issue.
[[nodiscard]] inline MemoryBudget& default_budget() noexcept {
    static MemoryBudget budget{};
    return budget;
}

}  // namespace bioseq::memory
