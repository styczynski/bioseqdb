/**
 * @file instrumentation/config.cppm
 * @brief is_context_args concept and canonical context_args structs.
 *
 * Compile-time configuration for the profiler. Every templated entry point
 * is constrained by is_context_args; the constant member fields decide
 * which features are emitted by the compiler. The runtime fork in
 * BatchProcessor::process_sequences picks one of the canonical args
 * structs and instantiates the matching pipeline.
 */
module;

#include <concepts>
#include <type_traits>

export module bioseq_instr.config;

export namespace bioseq::instr {

/// is_context_args. Every Args type used as a profiler control tag must
/// satisfy this concept. The concept is consumed by static_assert at the
/// head of every templated entry point and as the constraint of every
/// templated RAII type.
template<typename T>
concept is_context_args = requires {
    { T::enable_profiling }      -> std::convertible_to<bool>;
    { T::enable_counters }       -> std::convertible_to<bool>;
    { T::enable_tracing }        -> std::convertible_to<bool>;
    { T::enable_sampling }       -> std::convertible_to<bool>;
    { T::enable_arena_tracking } -> std::convertible_to<bool>;
    { T::snap_capacity }         -> std::convertible_to<unsigned>;
    { T::trace_capacity }        -> std::convertible_to<unsigned>;
    { T::aggr_block_size }       -> std::convertible_to<unsigned>;
} && std::is_trivially_default_constructible_v<T>;

/// Default profiling-disabled args. Hot path collapses to nothing in this
/// instantiation. context<context_args_no_profiling> has the same memory
/// layout as the legacy untemplated context (the std::monostate field has
/// zero storage cost via [[no_unique_address]]).
struct context_args_no_profiling {
    static constexpr bool     enable_profiling      = false;
    static constexpr bool     enable_counters       = false;
    static constexpr bool     enable_tracing        = false;
    static constexpr bool     enable_sampling       = false;
    static constexpr bool     enable_arena_tracking = false;
    static constexpr unsigned snap_capacity         = 0;
    static constexpr unsigned trace_capacity        = 0;
    static constexpr unsigned aggr_block_size       = 256;
};
static_assert(is_context_args<context_args_no_profiling>);

/// Canonical profiling args: timing + arena tracking on; PMU/tracing/sampling off.
/// Picked by the dispatch fork when bioseq.aligner.enable_profiling = true.
struct context_args_profiling_full {
    static constexpr bool     enable_profiling      = true;
    static constexpr bool     enable_counters       = false;
    static constexpr bool     enable_tracing        = false;
    static constexpr bool     enable_sampling       = false;
    static constexpr bool     enable_arena_tracking = true;
    static constexpr unsigned snap_capacity         = 32;
    static constexpr unsigned trace_capacity        = 0;
    static constexpr unsigned aggr_block_size       = 256;
};
static_assert(is_context_args<context_args_profiling_full>);

}  // namespace bioseq::instr
