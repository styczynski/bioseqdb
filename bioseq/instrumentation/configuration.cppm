/**
 * @file instrumentation/configuration.cppm
 * @brief Compile-time switches for profiling features.
 *
 * Instrumentation templates use these flags to remove disabled measurements
 * from the compiled code.
 */
module;

#include <concepts>
#include <type_traits>

export module bioseq.instrumentation.configuration;

export namespace bioseq::instrumentation {

/**
 * @brief Required flags and capacities for a profiling configuration.
 * @tparam p_configuration Trivially default-constructible type with feature flags
 * and unsigned-compatible trace  and aggregation capacities.
 */
template <class p_configuration>
concept profiling_configuration = requires {
    { p_configuration::enable_profiling } -> std::convertible_to<bool>;
    { p_configuration::enable_counters } -> std::convertible_to<bool>;
    { p_configuration::enable_tracing } -> std::convertible_to<bool>;
    { p_configuration::enable_sampling } -> std::convertible_to<bool>;
    { p_configuration::enable_arena_tracking } -> std::convertible_to<bool>;
    { p_configuration::trace_capacity } -> std::convertible_to<unsigned>;
    { p_configuration::aggregate_block_size } -> std::convertible_to<unsigned>;
} && std::is_trivially_default_constructible_v<p_configuration>;

/**
 * @brief Disable optional profiling features.
 *
 * Trace capacity is zero. A nonzero aggregate block size keeps the storage
 * templates valid even when measurements are disabled.
 */
struct profiling_disabled_configuration {
    static constexpr bool enable_profiling = false;
    static constexpr bool enable_counters = false;
    static constexpr bool enable_tracing = false;
    static constexpr bool enable_sampling = false;
    static constexpr bool enable_arena_tracking = false;
    static constexpr unsigned trace_capacity = 0;
    static constexpr unsigned aggregate_block_size = 256;
};
static_assert(profiling_configuration<profiling_disabled_configuration>);

/**
 * @brief Enable all profiling features for production.
 *
 * Capacities count trace events and measurement slots.
 */
struct production_profiling_configuration {
    static constexpr bool enable_profiling = true;
    static constexpr bool enable_counters = true;
    static constexpr bool enable_tracing = true;
    static constexpr bool enable_sampling = true;
    static constexpr bool enable_arena_tracking = true;
    static constexpr unsigned trace_capacity = 256;
    static constexpr unsigned aggregate_block_size = 256;
};
static_assert(profiling_configuration<production_profiling_configuration>);

/**
 * @brief Profiling configuration with separate state for benchmarks.
 *
 * It enables the same features as production. Its distinct type lets benchmark
 * code install its own thread-local profiler.
 */
struct benchmark_profiling_configuration {
    static constexpr bool enable_profiling = true;
    static constexpr bool enable_counters = true;
    static constexpr bool enable_tracing = true;
    static constexpr bool enable_sampling = true;
    static constexpr bool enable_arena_tracking = true;
    static constexpr unsigned trace_capacity = 256;
    static constexpr unsigned aggregate_block_size = 256;
};
static_assert(profiling_configuration<benchmark_profiling_configuration>);

} // namespace bioseq::instrumentation
