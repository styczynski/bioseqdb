/**
 * @file
 * Expose sequence length inspection without importing the  sequence implementation.
 */
module;
#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.types.compact_sequence.header_probe;

export namespace bioseq::postgres {

/**
 * @brief Validate the sequence header and read its length without reading letters.
 * @param input PostgreSQL Datum containing a sequence.
 * @return Number of letters recorded in the validated header
 */
[[nodiscard]] std::uint64_t compact_sequence_header_bp_length(Datum input);

} // namespace bioseq::postgres
