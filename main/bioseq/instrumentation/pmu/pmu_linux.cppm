/**
 * @file instrumentation/pmu/pmu_linux.cppm
 * @brief Linux perf_event_open backend sketch (Part 14 of the design doc).
 *
 * v1 status: not yet wired into the hot path. The full perf_event_open
 * implementation is documented in the research doc but enabling it
 * requires per-thread file-descriptor lifecycle management hooked into
 * ThreadState — that hookup is deferred. This module compiles to an
 * empty body on non-Linux targets and exists as a placeholder for the
 * future Linux PMU implementation.
 */
module;

#include <cstdint>

#if defined(__linux__)
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

export module bioseq_instr.pmu.linux;

export namespace bioseq::instr::pmu::linux_backend {

#if defined(__linux__)

/// Group-leader file descriptor + the 4 canon events, mmap'd for RDPMC.
struct PerThreadFds {
    int  cycles_fd        = -1;
    int  instructions_fd  = -1;
    int  branches_fd      = -1;
    int  branch_misses_fd = -1;
    void* mmap_page       = nullptr;
};

/// Reserved for future Linux activation. The hot-path read uses the
/// PerThreadFds to call ioctl(ENABLE/DISABLE) + read() or RDPMC. Until
/// activated this returns false and the public bioseq_instr.pmu
/// interface keeps using the stub backend.
[[nodiscard]] inline bool init(PerThreadFds&) noexcept { return false; }
inline void shutdown(PerThreadFds&) noexcept {}

#else

/// Stub on non-Linux. Keeps the symbol surface uniform.
struct PerThreadFds {};
[[nodiscard]] inline bool init(PerThreadFds&) noexcept { return false; }
inline void shutdown(PerThreadFds&) noexcept {}

#endif

}  // namespace bioseq::instr::pmu::linux_backend
