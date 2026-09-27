/**
 * @file
 * @brief Describe one row and match returned by the dispatch buffer.
 */
module;
#include <cstdint>
struct TupleTableSlot;

export module bioseq.postgres.scheduler.dispatch_buffer.output_contract;

import bioseq.pipeline.minimap_port.match_result_contract;

export namespace bioseq::scheduler {

/**
 * @brief A buffered input row and its current alignment match.
 *
 * The buffer owns the slot and match. Consume this view before clearing
 * the batch or replacing its input or results.
 */
struct dispatch_next_output {
    TupleTableSlot* scan_slot_copy = nullptr;
    std::int64_t sequence_rowno = -1;
    std::uint64_t query_ordinal = 0;
    const bioseq::simd::match_result* match = nullptr;
};

} // namespace bioseq::scheduler
