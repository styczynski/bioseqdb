/**
 * @file materialization_contract.cppm
 * Name the output formats and reasons recorded when sequence contents are copied.
 */
module;

#include <cstdint>

export module bioseq.postgres.types.compact_sequence.materialization_contract;

/// Kind of sequence data produced by a copy or conversion.
export enum class materialization_mechanism : std::uint8_t {
    backing_copy = 0u,
    packed_full,
    packed_region,
    text,
    binary,
    letter_array,
};

/// Operation that needed sequence contents in an allocated buffer.
export enum class materialization_reason : std::uint8_t {
    executor_copy = 0u,
    heap_tuple,
    index_tuple,
    mmsearch_target,
    mmsearch_query,
    resident_algorithm,
    sql_conversion,
    client_output,
};
