/**
 * @file materialization_contract.cppm
 * @brief Declares compact-sequence materialization categories shared across subsystems.
 */

export module bioseq.postgres.types.compact_sequence.materialization_contract;

import std;

/// Mechanism that produced one compact-sequence materialization.
export enum class materialization_mechanism : std::uint8_t {
    backing_copy = 0u,
    packed_full,
    packed_region,
    text,
    binary,
    letter_array,
};

/// Consumer boundary that required a compact-sequence materialization.
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
