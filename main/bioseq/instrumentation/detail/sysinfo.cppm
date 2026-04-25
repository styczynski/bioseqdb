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
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
#  include <unistd.h>
#endif

#if defined(_WIN32)
#  include <windows.h>
#endif

export module bioseq_instr.detail.sysinfo;

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

}  // namespace bioseq::instr::detail::sysinfo
