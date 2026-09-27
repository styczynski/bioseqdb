/**
 * @file
 * @brief Defines the borrowed output view returned by the scheduler dispatch buffer.
 */
module;
#include <cstdint>
struct TupleTableSlot;

export module bioseq.postgres.scheduler.dispatch_buffer.output_contract;

import bioseq.pipeline.minimap_port.match_result_contract;

export namespace bioseq::scheduler {

/**
 * @brief Borrows one buffered scan row and its current alignment match.
 *
 * The buffer owns both pointers and keeps them valid until its next state
 * transition.
 */
struct dispatch_next_output {
    TupleTableSlot* scan_slot_copy = nullptr;
    std::int64_t sequence_rowno = -1;
    std::uint64_t query_ordinal = 0;
    const bioseq::simd::match_result* match = nullptr;
};

} // namespace bioseq::scheduler
