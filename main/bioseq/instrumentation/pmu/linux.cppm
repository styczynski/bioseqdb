/**
 * @file instrumentation/pmu/linux.cppm
 * @brief Linux perf_event_open backend.
 *
 * Reads cycles / instructions / branches / branch_misses via the perf
 * subsystem. The four events are opened as a *group* on the calling
 * thread so a single read() syscall returns all four counter values
 * atomically — important when we want IPC and branch-miss-rate to be
 * derived from the same time window.
 *
 * Per-thread file descriptors live in thread-local storage; the kernel
 * automatically frees them when the thread exits or when the program
 * exits. read() returns the counters as raw monotonically-increasing
 * values; the caller subtracts a baseline taken at scope-begin to
 * obtain the delta.
 *
 * No special permissions are required for the default (user-mode-only)
 * configuration as long as `perf_event_paranoid <= 2` (this is the
 * default on most distros). When opening fails we cache the failure
 * thread-locally and the public read() returns zeros so the rest of
 * the pipeline continues to work.
 */
module;

#include <cstdint>
#include <cstring>

#if defined(__linux__)
#  include <asm/unistd.h>
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

export module bioseq.instrumentation.pmu.linux;

export namespace bioseq::instrumentation::pmu::linux {

/// Canonical 4-event sample. Mirrors the public counter_sample in pmu.cppm.
struct counter_sample {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};
static_assert(sizeof(counter_sample) == 32);

#if defined(__linux__)

namespace detail {

/// One file descriptor per event. group_leader_fd is also cycles_fd by
/// convention — every other event's parent group is the cycles_fd.
struct per_thread_file_descriptors {
    int  cycles_fd        = -1;
    int  instructions_fd  = -1;
    int  branches_fd      = -1;
    int  branch_misses_fd = -1;
    bool is_initialized = false;
    bool has_failed = false;
};

/// Read-format buffer layout used when PERF_FORMAT_GROUP is set:
///   nr
///   value0
///   id0     (when PERF_FORMAT_ID is set — we don't ask for it)
///   value1
///   ...
/// We ask only for GROUP, so the layout is { nr, value0, value1, ... }.
struct group_read_without_ids {
    std::uint64_t event_count;
    std::uint64_t time_enabled;
    std::uint64_t time_running;
    std::uint64_t cycles;
    std::uint64_t instructions;
    std::uint64_t branches;
    std::uint64_t branch_misses;
};
static_assert(sizeof(group_read_without_ids) == 7 * sizeof(std::uint64_t));

/// Wrapper around the perf_event_open syscall (no glibc wrapper exists).
[[nodiscard]] inline long perf_event_open_syscall(
    perf_event_attr* attributes,
    pid_t process_id,
    int cpu,
    int group_file_descriptor,
    unsigned long flags) noexcept
{
    return ::syscall(
        __NR_perf_event_open,
        attributes,
        process_id,
        cpu,
        group_file_descriptor,
        flags);
}

/// Open one perf hardware counter. `group_fd == -1` opens a new group
/// (the leader); positive group_fd attaches to an existing group.
[[nodiscard]] inline int open_counter(
    std::uint32_t hardware_event,
    int group_file_descriptor) noexcept
{
    perf_event_attr attributes{};
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.size = sizeof(attributes);
    attributes.type = PERF_TYPE_HARDWARE;
    attributes.config = hardware_event;
    attributes.disabled = group_file_descriptor == -1 ? 1 : 0;
    attributes.exclude_kernel = 1;
    attributes.exclude_hv = 1;
    // Only the leader needs the read-format set; sibling events inherit
    // the group's read interpretation when read() is issued on the
    // leader. We set it on the leader only.
    if (group_file_descriptor == -1) {
        attributes.read_format =
            PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
            PERF_FORMAT_TOTAL_TIME_RUNNING;
    }
    // pid=0 -> calling thread; cpu=-1 -> any CPU the thread runs on.
    return static_cast<int>(
        perf_event_open_syscall(&attributes, 0, -1, group_file_descriptor, 0));
}

inline thread_local per_thread_file_descriptors thread_file_descriptors{};

inline void close_group(per_thread_file_descriptors& file_descriptors) noexcept {
    if (file_descriptors.cycles_fd >= 0) {
        ::close(file_descriptors.cycles_fd);
        file_descriptors.cycles_fd = -1;
    }
    if (file_descriptors.instructions_fd >= 0) {
        ::close(file_descriptors.instructions_fd);
        file_descriptors.instructions_fd = -1;
    }
    if (file_descriptors.branches_fd >= 0) {
        ::close(file_descriptors.branches_fd);
        file_descriptors.branches_fd = -1;
    }
    if (file_descriptors.branch_misses_fd >= 0) {
        ::close(file_descriptors.branch_misses_fd);
        file_descriptors.branch_misses_fd = -1;
    }
}

/// Open all 4 events as a group on the calling thread. Returns true on
/// success; on failure cleans up any partially-opened fds and leaves
/// tls_fds.failed = true.
[[nodiscard]] inline bool open_group(per_thread_file_descriptors& file_descriptors) noexcept {
    file_descriptors.cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES, -1);
    if (file_descriptors.cycles_fd < 0) return false;

    file_descriptors.instructions_fd = open_counter(
        PERF_COUNT_HW_INSTRUCTIONS, file_descriptors.cycles_fd);
    if (file_descriptors.instructions_fd < 0) {
        close_group(file_descriptors);
        return false;
    }

    file_descriptors.branches_fd = open_counter(
        PERF_COUNT_HW_BRANCH_INSTRUCTIONS, file_descriptors.cycles_fd);
    if (file_descriptors.branches_fd < 0) {
        close_group(file_descriptors);
        return false;
    }

    file_descriptors.branch_misses_fd = open_counter(
        PERF_COUNT_HW_BRANCH_MISSES, file_descriptors.cycles_fd);
    if (file_descriptors.branch_misses_fd < 0) {
        close_group(file_descriptors);
        return false;
    }

    // Reset and enable the entire group via one ioctl on the leader.
    if (::ioctl(file_descriptors.cycles_fd, PERF_EVENT_IOC_RESET,
                PERF_IOC_FLAG_GROUP) != 0) {
        close_group(file_descriptors);
        return false;
    }
    if (::ioctl(file_descriptors.cycles_fd, PERF_EVENT_IOC_ENABLE,
                PERF_IOC_FLAG_GROUP) != 0) {
        close_group(file_descriptors);
        return false;
    }
    return true;
}

}  // namespace detail

/// Process-wide init. No-op on Linux — every thread brings up its own
/// fds via initialize_thread(). Returns true so the public dispatcher can
/// follow the same gate-then-init shape as macOS.
[[nodiscard]] inline bool initialize_process() noexcept { return true; }

/// Per-thread init. Lazily opens the 4-event group on first call.
/// Cached thread-locally so the cost is one branch on subsequent calls.
[[nodiscard]] inline bool initialize_thread() noexcept {
    auto& file_descriptors = detail::thread_file_descriptors;
    if (file_descriptors.is_initialized) return true;
    if (file_descriptors.has_failed) return false;

    if (!detail::open_group(file_descriptors)) {
        file_descriptors.has_failed = true;
        return false;
    }
    file_descriptors.is_initialized = true;
    return true;
}

inline void shutdown_thread() noexcept {
    auto& file_descriptors = detail::thread_file_descriptors;
    if (!file_descriptors.is_initialized) return;
    detail::close_group(file_descriptors);
    file_descriptors.is_initialized = false;
}

/// Read the current 4-event counter values for this thread. Returns
/// zeros if initialize_thread() hasn't succeeded for this thread.
[[nodiscard]] inline counter_sample read_counters() noexcept {
    auto& file_descriptors = detail::thread_file_descriptors;
    if (!file_descriptors.is_initialized) return counter_sample{};

    detail::group_read_without_ids read_buffer{};
    const ssize_t bytes_read = ::read(
        file_descriptors.cycles_fd, &read_buffer, sizeof(read_buffer));
    if (bytes_read != static_cast<ssize_t>(sizeof(read_buffer)) ||
        read_buffer.event_count != 4) {
        return counter_sample{};
    }
    return counter_sample{
        .cycles = read_buffer.cycles,
        .instructions = read_buffer.instructions,
        .branches = read_buffer.branches,
        .branch_misses = read_buffer.branch_misses,
    };
}

[[nodiscard]] inline bool are_thread_counters_available() noexcept {
    return detail::thread_file_descriptors.is_initialized;
}

#else  // !defined(__linux__)

[[nodiscard]] inline bool initialize_process() noexcept     { return false; }
[[nodiscard]] inline bool initialize_thread() noexcept      { return false; }
inline void shutdown_thread() noexcept                {}
[[nodiscard]] inline counter_sample read_counters() noexcept       { return counter_sample{}; }
[[nodiscard]] inline bool are_thread_counters_available() noexcept { return false; }

#endif

}  // namespace bioseq::instrumentation::pmu::linux
