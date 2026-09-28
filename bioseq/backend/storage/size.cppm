/**
 * @file
 * @brief Shared type for logical sequence positions and lengths.
 */
module;

#include <cstdint>

export module bioseq.storage.size;

export namespace bioseq::storage {

/**
 * Unsigned 64-bit type for  sequence positions and lengths.
 *
 * Check the range before converting to narrower storage fields or signed SQL
 * integers.
 */
using sequence_size_t = std::uint64_t;

}  // namespace bioseq::storage
