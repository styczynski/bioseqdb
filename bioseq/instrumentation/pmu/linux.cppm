/**
 * @file instrumentation/pmu/linux.cppm
 * @brief Linux perf_event_open backend.
 *
 * A thread-local perf group returns four coherent raw counters in one read.
 * Failed setup is cached and reported as an unavailable zero sample.
 */
module;

#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

export module bioseq.instrumentation.pmu.linux_backend;

export namespace bioseq::instrumentation::pmu::linux_backend {

/**
 * @brief Hold one Linux backend sample in the public counter order.
 *
 * This backend-local type deliberately mirrors `pmu::counter_sample` without
 * coupling the platform module to the public dispatch module.
 */
struct counter_sample {
    std::uint64_t cycles = 0;
    std::uint64_t instructions = 0;
    std::uint64_t branches = 0;
    std::uint64_t branch_misses = 0;
};
static_assert(sizeof(counter_sample) == 32);

#if defined(__linux__)

namespace detail {

/**
 * @brief Own the calling thread's four perf-event file descriptors.
 *
 * `cycles_fd` is the group leader. The initialization flags cache successful
 * and failed setup independently so a denied host does not retry on a hot path.
 */
struct per_thread_file_descriptors {
    int cycles_fd = -1;
    int instructions_fd = -1;
    int branches_fd = -1;
    int branch_misses_fd = -1;
    bool is_initialized = false;
    bool has_failed = false;
};

/**
 * @brief Match the exact byte layout returned by a grouped perf `read()`.
 *
 * The leader requests GROUP plus TOTAL_TIME_ENABLED and TOTAL_TIME_RUNNING,
 * but not PERF_FORMAT_ID. The kernel therefore writes the event count, the two
 * timing fields, then cycles, instructions, branches, and branch misses in the
 * event-open order. The timing fields remain present even though raw values are
 * intentionally reported without multiplex scaling.
 */
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

/**
 * @brief Invoke `perf_event_open` through its Linux syscall boundary.
 *
 * @param[in] attributes Fully initialized kernel event attributes.
 * @param[in] process_id Thread/process selector passed to the kernel.
 * @param[in] cpu CPU selector passed to the kernel.
 * @param[in] group_file_descriptor Group leader fd, or `-1` to create one.
 * @param[in] flags Kernel syscall flags.
 * @return The kernel's file descriptor or negative error result.
 */
[[nodiscard]] inline long perf_event_open_syscall(perf_event_attr* attributes,
                                                  pid_t process_id,
                                                  int cpu,
                                                  int group_file_descriptor,
                                                  unsigned long flags) noexcept {
    return ::syscall(
        __NR_perf_event_open, attributes, process_id, cpu, group_file_descriptor, flags);
}

/**
 * @brief Open one user-mode perf hardware counter for the calling thread.
 *
 * @param[in] hardware_event Linux `PERF_COUNT_HW_*` identifier.
 * @param[in] group_file_descriptor `-1` for the leader, otherwise its fd.
 * @return The opened descriptor, or a negative value on failure.
 */
[[nodiscard]] inline int open_counter(std::uint32_t hardware_event,
                                      int group_file_descriptor) noexcept {
    perf_event_attr attributes{};
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.size = sizeof(attributes);
    attributes.type = PERF_TYPE_HARDWARE;
    attributes.config = hardware_event;
    attributes.disabled = group_file_descriptor == -1 ? 1 : 0;
    attributes.exclude_kernel = 1;
    attributes.exclude_hv = 1;
    // Only the leader needs the read format. Sibling events inherit
    // the group's read interpretation when read() is issued on the
    // leader. We set it on the leader only.
    if (group_file_descriptor == -1) {
        attributes.read_format =
            PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    }
    // A zero pid selects the calling thread, and cpu -1 allows any CPU.
    return static_cast<int>(perf_event_open_syscall(&attributes, 0, -1, group_file_descriptor, 0));
}

inline thread_local per_thread_file_descriptors thread_file_descriptors{};

/**
 * @brief Close every descriptor in leader-to-sibling order.
 *
 * @param[in,out] file_descriptors Calling-thread descriptor state to clear.
 */
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

/**
 * @brief Open and enable all four events as one calling-thread group.
 *
 * Event creation remains cycles, instructions, branches, then branch misses.
 * Reset and enable are issued on the leader only after every sibling opens.
 * Partial failure closes all descriptors in the same stable order.
 *
 * @param[in,out] file_descriptors Calling-thread descriptor state to populate.
 * @return `true` only after the complete group is reset and enabled.
 */
[[nodiscard]] inline bool open_group(per_thread_file_descriptors& file_descriptors) noexcept {
    file_descriptors.cycles_fd = open_counter(PERF_COUNT_HW_CPU_CYCLES, -1);
    if (file_descriptors.cycles_fd < 0) {
        return false;
    }

    file_descriptors.instructions_fd =
        open_counter(PERF_COUNT_HW_INSTRUCTIONS, file_descriptors.cycles_fd);
    if (file_descriptors.instructions_fd < 0) {
        close_group(file_descriptors);
        return false;
    }

    file_descriptors.branches_fd =
        open_counter(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, file_descriptors.cycles_fd);
    if (file_descriptors.branches_fd < 0) {
        close_group(file_descriptors);
        return false;
    }

    file_descriptors.branch_misses_fd =
        open_counter(PERF_COUNT_HW_BRANCH_MISSES, file_descriptors.cycles_fd);
    if (file_descriptors.branch_misses_fd < 0) {
        close_group(file_descriptors);
        return false;
    }

    // Reset and enable the entire group via one ioctl on the leader.
    if (::ioctl(file_descriptors.cycles_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0) {
        close_group(file_descriptors);
        return false;
    }
    if (::ioctl(file_descriptors.cycles_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
        close_group(file_descriptors);
        return false;
    }
    return true;
}

} // namespace detail

/**
 * @brief Confirm process-level initialization for the per-thread Linux backend.
 *
 * Linux needs no process-wide setup because each thread owns its perf group.
 *
 * @return Always `true` on Linux.
 */
[[nodiscard]] inline bool initialize_process() noexcept {
    return true;
}

/**
 * @brief Lazily initialize the calling thread's four-event perf group.
 *
 * Successful and failed outcomes are cached thread-locally, so subsequent
 * calls do not issue more syscalls until a successful group is shut down.
 *
 * @return `true` when the complete group is available.
 */
[[nodiscard]] inline bool initialize_thread() noexcept {
    auto& file_descriptors = detail::thread_file_descriptors;
    if (file_descriptors.is_initialized) {
        return true;
    }
    if (file_descriptors.has_failed) {
        return false;
    }

    if (!detail::open_group(file_descriptors)) {
        file_descriptors.has_failed = true;
        return false;
    }
    file_descriptors.is_initialized = true;
    return true;
}

/** @brief Close the calling thread's successfully initialized perf group. */
inline void shutdown_thread() noexcept {
    auto& file_descriptors = detail::thread_file_descriptors;
    if (!file_descriptors.is_initialized) {
        return;
    }
    detail::close_group(file_descriptors);
    file_descriptors.is_initialized = false;
}

/**
 * @brief Read the calling thread's four counters with one grouped syscall.
 *
 * Raw values are returned without multiplex scaling, preserving the counter
 * contract. A short read or unexpected event count yields zeros.
 *
 * @return Counters in cycles, instructions, branches, branch-misses order.
 */
[[nodiscard]] inline counter_sample read_counters() noexcept {
    auto& file_descriptors = detail::thread_file_descriptors;
    if (!file_descriptors.is_initialized) {
        return counter_sample{};
    }

    detail::group_read_without_ids read_buffer{};
    const ssize_t bytes_read =
        ::read(file_descriptors.cycles_fd, &read_buffer, sizeof(read_buffer));
    if (bytes_read != static_cast<ssize_t>(sizeof(read_buffer)) || read_buffer.event_count != 4) {
        return counter_sample{};
    }
    return counter_sample{
        .cycles = read_buffer.cycles,
        .instructions = read_buffer.instructions,
        .branches = read_buffer.branches,
        .branch_misses = read_buffer.branch_misses,
    };
}

/**
 * @brief Report whether the calling thread has an initialized perf group.
 *
 * @return The thread-local initialization state.
 */
[[nodiscard]] inline bool are_thread_counters_available() noexcept {
    return detail::thread_file_descriptors.is_initialized;
}

#else // !defined(__linux__)

/** @brief Report that process-level Linux PMU setup is unavailable. */
[[nodiscard]] inline bool initialize_process() noexcept {
    return false;
}

/** @brief Report that calling-thread Linux PMU setup is unavailable. */
[[nodiscard]] inline bool initialize_thread() noexcept {
    return false;
}

/** @brief Perform the no-op shutdown for a non-Linux translation. */
inline void shutdown_thread() noexcept {}

/** @brief Return an all-zero sample when the Linux backend is unavailable. */
[[nodiscard]] inline counter_sample read_counters() noexcept {
    return counter_sample{};
}

/** @brief Report that calling-thread Linux counters are unavailable. */
[[nodiscard]] inline bool are_thread_counters_available() noexcept {
    return false;
}

#endif

} // namespace bioseq::instrumentation::pmu::linux_backend
