/**
 * @file instrumentation/profiler_state.cppm
 * @brief Typed per-thread storage and scoped installation.
 *
 * The state remains separate from pipeline context objects so enabling
 * instrumentation does not alter their layout. A scoped installer restores the
 * previous pointer, which keeps nested dispatch safe.
 */
module;

#include <array>
#include <cstdint>

export module bioseq.instrumentation.profiler_state;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.aggregation;

export namespace bioseq::instrumentation {

/** @brief Allocation arena currently attributed to the calling thread. */
enum class arena_kind : std::uint8_t {
    none = 0,
    scratch = 1,
    chain = 2,
    align = 3,
    result = 4,
    hash = 5,
};

inline constexpr std::size_t arena_kind_count = 6;

/** @brief Allocation metrics for one arena category. */
struct arena_counter {
    std::uint64_t bytes_live  = 0;
    std::uint64_t bytes_total = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t largest_allocation_bytes = 0;
};

/// Per-pipeline profiler state. Owned on the stack by the dispatch fork
/// in BatchProcessor::process_sequences when configuration_type::enable_profiling is
/// true; not constructed at all when false.
template<profiling_configuration configuration_type>
struct profiler_state {
    region_aggregate_table<configuration_type>          aggregates;
    loop_aggregate_table<configuration_type>            loop_aggregates;
    arena_kind active_arena = arena_kind::none;
    std::array<arena_counter, arena_kind_count> arena_counters{};
    std::uint16_t stack_depth = 0;
    std::uint16_t stack_error_count = 0;

    [[nodiscard, gnu::always_inline]]
    arena_kind active_arena_kind() const noexcept { return active_arena; }

    [[gnu::always_inline]]
    void set_active_arena(arena_kind kind) noexcept { active_arena = kind; }

    [[nodiscard, gnu::always_inline]]
    arena_counter& counter_for(arena_kind kind) noexcept {
        return arena_counters[static_cast<unsigned>(kind)];
    }
};

/// Typed thread-local pointer to the currently-installed profiler_state.
/// One slot per distinct configuration_type type the program instantiates. When no
/// profiler is installed for that configuration_type, the pointer is null.
template<profiling_configuration configuration_type>
inline thread_local profiler_state<configuration_type>* active_thread_profiler = nullptr;

/// RAII installer. Construct on the stack alongside a stack-local
/// profiler_state<configuration_type>; the constructor stores the address in
/// active_thread_profiler<configuration_type>; the destructor restores the prior value.
template<profiling_configuration configuration_type>
struct [[nodiscard]] installed_profiler_scope {
    profiler_state<configuration_type>* previous_profiler = nullptr;

    [[gnu::always_inline]]
    explicit installed_profiler_scope(profiler_state<configuration_type>& profiler) noexcept {
        previous_profiler = active_thread_profiler<configuration_type>;
        active_thread_profiler<configuration_type> = &profiler;
    }

    [[gnu::always_inline]]
    ~installed_profiler_scope() noexcept {
        active_thread_profiler<configuration_type> = previous_profiler;
    }

    installed_profiler_scope(const installed_profiler_scope&)            = delete;
    installed_profiler_scope& operator=(const installed_profiler_scope&) = delete;
    installed_profiler_scope(installed_profiler_scope&&)                 = delete;
    installed_profiler_scope& operator=(installed_profiler_scope&&)      = delete;
};

}  // namespace bioseq::instrumentation

template bioseq::instrumentation::profiler_state<bioseq::instrumentation::profiling_disabled_configuration>*
    bioseq::instrumentation::active_thread_profiler<bioseq::instrumentation::profiling_disabled_configuration>;
template bioseq::instrumentation::profiler_state<bioseq::instrumentation::production_profiling_configuration>*
    bioseq::instrumentation::active_thread_profiler<bioseq::instrumentation::production_profiling_configuration>;
template bioseq::instrumentation::profiler_state<bioseq::instrumentation::benchmark_profiling_configuration>*
    bioseq::instrumentation::active_thread_profiler<bioseq::instrumentation::benchmark_profiling_configuration>;
