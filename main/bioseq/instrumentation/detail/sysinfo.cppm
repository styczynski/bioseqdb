/**
 * @file instrumentation/detail/sysinfo.cppm
 * @brief Compatibility facade for shared system-memory probes.
 */
module;

#include <cstddef>
#include <cstdint>
#include <thread>

export module bioseq.instr.detail.sysinfo;
import bioseq.common.memory_budget;

export namespace bioseq::instr::detail::sysinfo {

/** Forward the page-size probe to its single common implementation. */
[[nodiscard]] inline std::size_t page_size() noexcept {
    return bioseq::common::memory::page_size();
}

/** Return usable hardware concurrency, never zero. */
[[nodiscard]] inline unsigned hardware_concurrency() noexcept {
    const unsigned count = std::thread::hardware_concurrency();
    return count == 0 ? 1u : count;
}

/** Forward the process-footprint probe to its single common implementation. */
[[nodiscard]] inline std::uint64_t process_footprint_bytes() noexcept {
    return bioseq::common::memory::process_footprint_bytes();
}

/** Forward the total-memory probe to its single common implementation. */
[[nodiscard]] inline std::uint64_t system_total_bytes() noexcept {
    return bioseq::common::memory::system_total_bytes();
}

/** Forward the available-memory probe to its single common implementation. */
[[nodiscard]] inline std::uint64_t system_available_bytes() noexcept {
    return bioseq::common::memory::system_available_bytes();
}

} // namespace bioseq::instr::detail::sysinfo

export namespace bioseq::memory {

using MemoryBudget = bioseq::common::memory::MemoryBudget;
using MemorySnapshot = bioseq::common::memory::MemorySnapshot;

/** Preserve the established source API while sharing one implementation. */
[[nodiscard]] inline MemoryBudget& default_budget() noexcept {
    return bioseq::common::memory::default_budget();
}

} // namespace bioseq::memory
