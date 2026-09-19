/**
 * @file
 * @brief Declares custom-scan runtime buffer sizing without importing runtime state.
 */
module;
#include <cstddef>

export module bioseq.postgres.table:custom_scan_runtime_buffer_contracts;

/*
 * Initial reservation hint passed to tuple_dispatch_buffer when a
 * bioseq_scan_runtime is constructed. Sized for the typical small-result
 * scan; the buffer reallocates linearly past this if a query produces
 * more buffered tuples.
 */
export inline constexpr std::size_t k_bioseq_runtime_buffer_reserve = 8192;
