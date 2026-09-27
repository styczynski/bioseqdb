/**
 * @file instrumentation/profiler_state.cppm
 * @brief Profiler storage and installation for the current thread.
 *
 * An installed_profiler_scope saves the previous pointer and restores it on exit.
 * This lets nested work temporarily use another profiler.
 */
module;

#include <array>
#include <cstdint>

export module bioseq.instrumentation.profiler_state;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.aggregation;

export namespace bioseq::instrumentation {

/** @brief Memory category used for allocation accounting. */
enum class arena_kind : std::uint8_t {
    none = 0,
    scratch = 1,
    chain = 2,
    align = 3,
    result = 4,
    hash = 5,
};

/** @brief Number of values in arena_kind, including the none sentinel. */
inline constexpr std::size_t arena_kind_count = 6;

/** @brief Allocation metrics for one arena category. */
struct arena_counter {
    std::uint64_t bytes_live  = 0;
    std::uint64_t bytes_total = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t largest_allocation_bytes = 0;
};

/** @brief Per-thread profiler state for one compile-time configuration. */
template<profiling_configuration p_configuration_type>
struct profiler_state {
    region_aggregate_table<p_configuration_type>          aggregates;
    loop_aggregate_table<p_configuration_type>            loop_aggregates;
    arena_kind active_arena = arena_kind::none;
    std::array<arena_counter, arena_kind_count> arena_counters{};
    std::uint16_t stack_depth = 0;
    std::uint16_t stack_error_count = 0;

    /** @brief Return the category receiving allocation events. */
    [[nodiscard, gnu::always_inline]]
    arena_kind active_arena_kind() const noexcept { return active_arena; }

    /** @brief Select the category receiving allocation events. */
    [[gnu::always_inline]]
    void set_active_arena(arena_kind kind) noexcept { active_arena = kind; }

    /** @brief Return counters for an arena category. */
    [[nodiscard, gnu::always_inline]]
    arena_counter& counter_for(arena_kind kind) noexcept {
        return arena_counters[static_cast<unsigned>(kind)];
    }
};

/** @brief Active profiler for this thread and configuration. */
template<profiling_configuration p_configuration_type>
inline thread_local profiler_state<p_configuration_type>* active_thread_profiler = nullptr;

/** @brief Scoped installer that restores the prior thread-local profiler. */
template<profiling_configuration p_configuration_type>
struct [[nodiscard]] installed_profiler_scope {
    profiler_state<p_configuration_type>* previous_profiler = nullptr;

    /** @brief Install a profiler and kave  the previous thread-local pointer. */
    [[gnu::always_inline]]
    explicit installed_profiler_scope(profiler_state<p_configuration_type>& profiler) noexcept {
        previous_profiler = active_thread_profiler<p_configuration_type>;
        active_thread_profiler<p_configuration_type> = &profiler;
    }

    /** @brief Restore the prior thread-local profiler pointer. */
    [[gnu::always_inline]]
    ~installed_profiler_scope() noexcept {
        active_thread_profiler<p_configuration_type> = previous_profiler;
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
