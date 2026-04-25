/**
 * @file instrumentation/pmu/pmu_linux.cppm
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

export module bioseq_instr.pmu.linux;

export namespace bioseq::instr::pmu::linux_backend {

/// Canonical 4-event sample. Mirrors the public counters_t in pmu.cppm.
struct counters_t {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};

#if defined(__linux__)

namespace detail {

/// One file descriptor per event. group_leader_fd is also cycles_fd by
/// convention — every other event's parent group is the cycles_fd.
struct PerThreadFds {
    int  cycles_fd        = -1;
    int  instructions_fd  = -1;
    int  branches_fd      = -1;
    int  branch_misses_fd = -1;
    bool initialised      = false;
    bool failed           = false;
};

/// Read-format buffer layout used when PERF_FORMAT_GROUP is set:
///   nr
///   value0
///   id0     (when PERF_FORMAT_ID is set — we don't ask for it)
///   value1
///   ...
/// We ask only for GROUP, so the layout is { nr, value0, value1, ... }.
struct ReadGroupNoId {
    std::uint64_t nr;
    std::uint64_t cycles;
    std::uint64_t instructions;
    std::uint64_t branches;
    std::uint64_t branch_misses;
};

/// Wrapper around the perf_event_open syscall (no glibc wrapper exists).
[[nodiscard]] inline long perf_event_open_syscall(
    perf_event_attr* attr, pid_t pid, int cpu,
    int group_fd, unsigned long flags) noexcept
{
    return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/// Open one perf hardware counter. `group_fd == -1` opens a new group
/// (the leader); positive group_fd attaches to an existing group.
[[nodiscard]] inline int open_counter(
    std::uint32_t config, int group_fd) noexcept
{
    perf_event_attr attr{};
    std::memset(&attr, 0, sizeof(attr));
    attr.size           = sizeof(attr);
    attr.type           = PERF_TYPE_HARDWARE;
    attr.config         = config;
    attr.disabled       = (group_fd == -1) ? 1 : 0;  // leader starts disabled
    attr.exclude_kernel = 1;
    attr.exclude_hv     = 1;
    // Only the leader needs the read-format set; sibling events inherit
    // the group's read interpretation when read() is issued on the
    // leader. We set it on the leader only.
    if (group_fd == -1) {
        attr.read_format =
            PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
            PERF_FORMAT_TOTAL_TIME_RUNNING;
        // Strictly we don't *need* time enabled/running for the simple
        // counter use — they let downstream callers compensate for
        // counter multiplexing. We don't surface them in the public
        // counters_t and don't read them in fast_read(); harmless.
    }
    // pid=0 -> calling thread; cpu=-1 -> any CPU the thread runs on.
    return static_cast<int>(perf_event_open_syscall(&attr, 0, -1, group_fd, 0));
}

inline thread_local PerThreadFds tls_fds{};

/// Open all 4 events as a group on the calling thread. Returns true on
/// success; on failure cleans up any partially-opened fds and leaves
/// tls_fds.failed = true.
[[nodiscard]] inline bool open_group(PerThreadFds& fds) noexcept {
    fds.cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES, -1);
    if (fds.cycles_fd < 0) return false;

    fds.instructions_fd = open_counter(PERF_COUNT_HW_INSTRUCTIONS, fds.cycles_fd);
    if (fds.instructions_fd < 0) {
        ::close(fds.cycles_fd);
        fds.cycles_fd = -1;
        return false;
    }

    fds.branches_fd = open_counter(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, fds.cycles_fd);
    if (fds.branches_fd < 0) {
        ::close(fds.cycles_fd);
        ::close(fds.instructions_fd);
        fds.cycles_fd = fds.instructions_fd = -1;
        return false;
    }

    fds.branch_misses_fd = open_counter(PERF_COUNT_HW_BRANCH_MISSES, fds.cycles_fd);
    if (fds.branch_misses_fd < 0) {
        ::close(fds.cycles_fd);
        ::close(fds.instructions_fd);
        ::close(fds.branches_fd);
        fds.cycles_fd = fds.instructions_fd = fds.branches_fd = -1;
        return false;
    }

    // Reset and enable the entire group via one ioctl on the leader.
    if (::ioctl(fds.cycles_fd, PERF_EVENT_IOC_RESET,
                PERF_IOC_FLAG_GROUP) != 0) {
        return false;
    }
    if (::ioctl(fds.cycles_fd, PERF_EVENT_IOC_ENABLE,
                PERF_IOC_FLAG_GROUP) != 0) {
        return false;
    }
    return true;
}

inline void close_group(PerThreadFds& fds) noexcept {
    if (fds.cycles_fd        >= 0) { ::close(fds.cycles_fd);        fds.cycles_fd        = -1; }
    if (fds.instructions_fd  >= 0) { ::close(fds.instructions_fd);  fds.instructions_fd  = -1; }
    if (fds.branches_fd      >= 0) { ::close(fds.branches_fd);      fds.branches_fd      = -1; }
    if (fds.branch_misses_fd >= 0) { ::close(fds.branch_misses_fd); fds.branch_misses_fd = -1; }
}

}  // namespace detail

/// Process-wide init. No-op on Linux — every thread brings up its own
/// fds via init_thread(). Returns true so the public dispatcher can
/// follow the same gate-then-init shape as macOS.
[[nodiscard]] inline bool process_init() noexcept { return true; }

/// Per-thread init. Lazily opens the 4-event group on first call.
/// Cached thread-locally so the cost is one branch on subsequent calls.
[[nodiscard]] inline bool init_thread() noexcept {
    auto& fds = detail::tls_fds;
    if (fds.initialised) return true;
    if (fds.failed)      return false;

    if (!detail::open_group(fds)) {
        fds.failed = true;
        return false;
    }
    fds.initialised = true;
    return true;
}

inline void shutdown_thread() noexcept {
    auto& fds = detail::tls_fds;
    if (!fds.initialised) return;
    detail::close_group(fds);
    fds.initialised = false;
}

/// Read the current 4-event counter values for this thread. Returns
/// zeros if init_thread() hasn't succeeded for this thread.
[[nodiscard]] inline counters_t read() noexcept {
    auto& fds = detail::tls_fds;
    if (!fds.initialised) return counters_t{};

    detail::ReadGroupNoId buf{};
    const ssize_t got = ::read(fds.cycles_fd, &buf, sizeof(buf));
    if (got != static_cast<ssize_t>(sizeof(buf))) {
        return counters_t{};
    }
    return counters_t{
        .cycles        = buf.cycles,
        .instructions  = buf.instructions,
        .branches      = buf.branches,
        .branch_misses = buf.branch_misses,
    };
}

[[nodiscard]] inline bool counters_available() noexcept {
    return detail::tls_fds.initialised;
}

#else  // !defined(__linux__)

[[nodiscard]] inline bool process_init() noexcept     { return false; }
[[nodiscard]] inline bool init_thread() noexcept      { return false; }
inline void shutdown_thread() noexcept                {}
[[nodiscard]] inline counters_t read() noexcept       { return counters_t{}; }
[[nodiscard]] inline bool counters_available() noexcept { return false; }

#endif

}  // namespace bioseq::instr::pmu::linux_backend
