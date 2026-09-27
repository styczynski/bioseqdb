/**
 * @file
 * Implement sequence length inspection through the shared header validator.
 */
module;
#include <bioseq/pg_compat.h>

module bioseq.postgres.types.compact_sequence.header_probe;

import std;
import bioseq.postgres.types.compact_sequence;

namespace bioseq::postgres {

[[nodiscard]] std::uint64_t compact_sequence_header_bp_length(Datum input) {
    return classify_and_describe_sequence_header(input).bp_length;
}

} // namespace bioseq::postgres
