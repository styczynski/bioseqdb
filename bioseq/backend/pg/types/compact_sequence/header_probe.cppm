/**
 * @file
 * @brief Exposes metadata-only sequence length inspection without the compact-sequence type graph.
 */
module;
#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.types.compact_sequence.header_probe;

export namespace bioseq::postgres {

/**
 * @brief Reads the logical sequence length through the compact-sequence header validator.
 * @param input PostgreSQL datum containing a bioseq sequence value.
 * @return The validated logical length in letters.
 */
[[nodiscard]] std::uint64_t compact_sequence_header_bp_length(Datum input);

} // namespace bioseq::postgres
