/**
 * @file
 * @brief Canonical logical sequence coordinate type.
 */
module;

#include <cstdint>

export module bioseq.storage.size;

export namespace bioseq::storage {

/**
 * Unsigned type for logical sequence positions and lengths.
 *
 * SQL and storage boundaries must validate narrowing explicitly; this module
 * deliberately provides no unchecked signed conversion helper.
 */
using sequence_size_t = std::uint64_t;

}  // namespace bioseq::storage
