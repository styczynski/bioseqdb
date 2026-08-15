/**
 * @file
 * @brief Defines shared vector-aggregate state and spill contracts.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "nodes/extensible.h"
}
#include <cstdint>

/**
 * Complete PostgreSQL's HashAggSpill declaration in the global module
 * fragment so its layout and linkage match execnodes.h.
 */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL ABI type name
struct HashAggSpill {
    /** Number of spill partitions. */
    int npartitions;
    /** Logical tape for each spill partition. */
    LogicalTape** partitions;
    /** Tuple count for each spill partition. */
    int64_t* ntuples;
    /** Hash mask selecting a spill partition. */
    uint32_t mask;
    /** Hash shift applied after the partition mask. */
    int shift;
    /** Cardinality estimate for each spill partition. */
    hyperLogLogState* hll_card;
};

export module bioseq.postgres.vectorization.aggregator_node:support;

/** Module-visible alias for PostgreSQL's completed spill descriptor. */
export using hash_aggregate_spill = HashAggSpill;

export {
    /** Describes one spilled grouping-set batch awaiting hash aggregation. */
    struct hash_aggregate_batch {
        /** Grouping-set index. */
        int set_index;
        /** Hash bits consumed by prior partitioning. */
        int used_bits;
        /** Logical tape containing this batch. */
        LogicalTape* input_tape;
        /** Number of tuples stored in the batch. */
        int64_t input_tuples;
        /** Estimated number of groups in the batch. */
        double input_card;
    };

    /** Accumulates aggregate and grouping-column references during plan walking. */
    struct find_columns_context {
        /** Whether traversal is inside an aggregate reference. */
        bool is_aggref;
        /** Columns referenced inside aggregate expressions. */
        Bitmapset* aggregated;
        /** Columns referenced outside aggregate expressions. */
        Bitmapset* unaggregated;
    };

    /** Holds the PostgreSQL aggregate executor owned by a vector custom scan. */
    struct vector_aggregate_state {
        /** PostgreSQL custom-scan prefix. */
        CustomScanState custom_scan_state;
        /** Aggregate executor copied from the wrapped plan. */
        AggState* aggregate_state;
    };

    /** Target groups per spill partition relative to the memory estimate. */
    inline constexpr double hash_aggregate_partition_factor = 1.50;
    /** Minimum spill partition count. */
    inline constexpr int hash_aggregate_min_partitions = 4;
    /** Maximum spill partition count. */
    inline constexpr int hash_aggregate_max_partitions = 1024;

    /** Logical-tape read buffer size. */
    inline constexpr int hash_aggregate_read_buffer_bytes = BLCKSZ;
    /** Logical-tape write buffer size. */
    inline constexpr int hash_aggregate_write_buffer_bytes = BLCKSZ;

    /** HyperLogLog precision used for spill cardinality. */
    inline constexpr int hash_aggregate_hll_bit_width = 5;

    /** Allocation-set chunk header included in entry estimates. */
    inline constexpr Size aggregate_chunk_header_bytes = 16;

    /** Executes one vector aggregate plan step. */
    TupleTableSlot* execute_aggregate_plan(PlanState* plan_state);
}
