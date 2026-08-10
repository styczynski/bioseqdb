/**
 * @file instrumentation/configuration.cppm
 * @brief Compile-time feature configurations for instrumentation.
 *
 * Every hot-path entry point is constrained by profiling_configuration.
 * Its static feature fields let the compiler remove disabled instrumentation.
 */
module;

#include <concepts>
#include <type_traits>

export module bioseq.instrumentation.configuration;

export namespace bioseq::instrumentation {

/** @brief Required static interface for a profiling feature configuration. */
template<typename p_configuration>
concept profiling_configuration = requires {
    { p_configuration::enable_profiling }      -> std::convertible_to<bool>;
    { p_configuration::enable_counters }       -> std::convertible_to<bool>;
    { p_configuration::enable_tracing }        -> std::convertible_to<bool>;
    { p_configuration::enable_sampling }       -> std::convertible_to<bool>;
    { p_configuration::enable_arena_tracking } -> std::convertible_to<bool>;
    { p_configuration::trace_capacity }        -> std::convertible_to<unsigned>;
    { p_configuration::aggregate_block_size }  -> std::convertible_to<unsigned>;
} && std::is_trivially_default_constructible_v<p_configuration>;

/** @brief Configuration whose hot-path instrumentation compiles away. */
struct profiling_disabled_configuration {
    static constexpr bool     enable_profiling      = false;
    static constexpr bool     enable_counters       = false;
    static constexpr bool     enable_tracing        = false;
    static constexpr bool     enable_sampling       = false;
    static constexpr bool     enable_arena_tracking = false;
    static constexpr unsigned trace_capacity        = 0;
    static constexpr unsigned aggregate_block_size  = 256;
};
static_assert(profiling_configuration<profiling_disabled_configuration>);

/** @brief Production configuration with all current instrumentation enabled. */
struct production_profiling_configuration {
    static constexpr bool     enable_profiling      = true;
    static constexpr bool     enable_counters       = true;
    static constexpr bool     enable_tracing        = true;
    static constexpr bool     enable_sampling       = true;
    static constexpr bool     enable_arena_tracking = true;
    static constexpr unsigned trace_capacity        = 256;
    static constexpr unsigned aggregate_block_size  = 256;
};
static_assert(profiling_configuration<production_profiling_configuration>);

/**
 * @brief Benchmark-owned profiling configuration.
 *
 * This remains a distinct type because its thread-local state is installed by
 * benchmark dispatch independently of production telemetry. Its current
 * feature set deliberately matches production profiling.
 */
struct benchmark_profiling_configuration {
    static constexpr bool     enable_profiling      = true;
    static constexpr bool     enable_counters       = true;
    static constexpr bool     enable_tracing        = true;
    static constexpr bool     enable_sampling       = true;
    static constexpr bool     enable_arena_tracking = true;
    static constexpr unsigned trace_capacity        = 256;
    static constexpr unsigned aggregate_block_size  = 256;
};
static_assert(profiling_configuration<benchmark_profiling_configuration>);

}  // namespace bioseq::instrumentation
