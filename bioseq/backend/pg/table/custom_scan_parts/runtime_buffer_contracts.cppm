/**
 * @file
 * @brief Defines the row-count estimate used when allocating custom-scan buffers.
 */
module;
#include <cstddef>

export module bioseq.postgres.table:custom_scan_runtime_buffer_contracts;

/* Row-count estimate passed to custom-scan dispatch buffers at construction
 * It matches the 8192-row dispatch batch size. The buffer uses this estimate
 * to reserve storage before rows arrive. */
export inline constexpr std::size_t k_bioseq_runtime_buffer_reserve = 8192;
