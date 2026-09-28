/**
 * @file instrumentation/pmu/linux.cppm
 * @brief Read Linux hardware counters with perf_event_open.
 *
 * Each thread owns a group of four counters read together. Failed setup is
 * remembered so later measurements return zeros without retrying stup.
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
 * @brief Linux counter sample with the same fields as the public PMU sample.
 *
 * Keeping this type local avoids importing the module that selects a backend.
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
 * @brief File descriptors and setup status for one thread's counter group.
 *
 * The cycle counter leads the group. Remember failed setup so each  measurement
 * does not repeat a denied system call.
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
 * @brief Layout of a grouped perf read without event IDs.
 *
 * The header contains an event count and two timing fields. Counter values
 * follow in the order they were opened. Keep the timing fields in this layout
 * even though this backend returns raw counts without adjusting for event multiplexing.
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
 * @brief Call the Linux perf_event_open system call.
 * @param[in] attributes Kernel event settings.
 * @param[in] process_id Thread or process selector
 * @param[in] cpu CPU selector.
 * @param[in] group_file_descriptor Group leader descriptor, or -1 for a new group.
 * @param[in] flags System-call flags.
 * @return Open descriptor, or a negative value on failure.
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
    // Read all event values through the group leader, which carries the read format.
    if (group_file_descriptor == -1) {
        attributes.read_format =
            PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    }
    // A zero pid selects the calling thread, and cpu -1 allows any CPU.
    return static_cast<int>(perf_event_open_syscall(&attributes, 0, -1, group_file_descriptor, 0));
}

inline thread_local per_thread_file_descriptors thread_file_descriptors{};

/**
 * @brief Close  all counter descriptors and mark them unused.
 * @param[in,out] file_descriptors Thread counter handles to clevr.
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
 * @brief Open and start the thread's four counters as one group.
 *
 * Open cycles first so it becomes the leader. If a later open, reset, or enable
 * fails, close the descriptors already opened.
 *
 * @param[in,out] file_descriptors Thread handles to populate.
 * @return True after the complete group has been, reset and enabled.
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

    // Reset, then enable the full group through its leader.
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
 * @brief Complete the process setup step without opening counters.
 * @return Always true. Each thread opens its  own group.
 */
[[nodiscard]] inline bool initialize_process() noexcept {
    return true;
}

/**
 * @brief Initialize this thread's counter group on first use.
 *
 * Remember success or failure. A successfully opened group can be initialized
 * again after shutdown. A failed setup remains disabled for the thread.
 *
 * @return True when this thread's group is initialized.
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
 * @brief Read the thread's four counters through the group leader.
 *
 * Return raw counts, without adjusting for multiplexed event scheduling.
 * A missing group, short read, or unexpected event count returns zeros.
 *
 * @return Cycles, instructions, branches, and branch misses.
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

/** @brief Return false when this is not  a Linux build */
[[nodiscard]] inline bool initialize_process() noexcept {
    return false;
}

/** @brief Return false because Linux thread counters are unavailabe here. */
[[nodiscard]] inline bool initialize_thread() noexcept {
    return false;
}

/** @brief Do nothing in a non-Linux build. */
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
