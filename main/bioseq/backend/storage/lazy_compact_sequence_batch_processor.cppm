/*-------------------------------------------------------------------------
 *
 * storage/lazy_compact_sequence_batch_processor.cppm
 *
 * `lazy_compact_sequence_batch_processor<R, Scanner>` — the algorithm-side
 * hook the `BioseqBatchSortScan` custom scan calls to order a batch of
 * sequences (see backend/pg/table/batch_sort_scan.cppm). It batches a span
 * of `lazy_compact_sequence<R, Scanner>` and returns the output ordering
 * (a permutation) for those sequences.
 *
 * SCOPE — the real ordering algorithm is intentionally out of scope. The
 * value of this file is the *interface* plus a verifiable placeholder:
 *
 *   - A `lazy_compact_sequence` can be larger than RAM: the FRAGMENTED
 *     scanner (`sequence_scanner_pg`) streams fixed-size fragments on demand
 *     with aggressive eviction, so RSS is bounded by the fragment buffer, not
 *     by sequence length (DOC0263 R12). A real comparator would therefore
 *     stream both operands letter-by-letter through the Scanner — exactly the
 *     `iterable<BP>()` pattern `tally_symbol_counts` uses (symbol_counts_index
 *     .cppm). The processor is templated on `Scanner`, so swapping
 *     `sequence_scanner_none` (in-RAM) for `sequence_scanner_pg` (out-of-core)
 *     is mechanical — no change to the interface or the custom scan.
 *
 *   - The placeholder `lazy_compact_sequence_batch_sort` returns the batch in
 *     REVERSE order instead of sorting. This makes the planner + executor
 *     wiring verifiable end-to-end via SQL: the emitted row order becomes the
 *     exact reverse of the scan order, an unambiguous signal that our node ran
 *     (a core Sort would emit ascending/descending order, never reverse-of-
 *     scan order).
 *
 * NO_POSTGRES — this module references no Postgres internals; it lives in the
 * shared nopg module library and depends only on the lazy-sequence/scanner
 * abstractions (so the processor is unit-testable without a running server).
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cstddef>
#include <ranges>
#include <span>
#include <vector>

export module bioseq.storage.batch_processor;

import bioseq.storage.lazy;     // lazy_compact_sequence
import bioseq.storage.scanner;  // is_valid_sequence_scanner, sequence_scanner_none
import bioseq.sequences;        // supports_compact_sequence_range, compact_sequence_range

export namespace bioseq::storage {

/*
 * Ordering parameters mirrored from the SQL `ORDER BY` clause. The reverse
 * placeholder ignores these (it always reverses); a real comparator-based
 * sort would honour them. Kept as a small value type so the custom scan can
 * forward the planner's sort direction without coupling to Postgres types.
 */
struct batch_sort_spec {
    bool descending  = false;   //< ORDER BY ... DESC
    bool nulls_first = false;   //< NULLS FIRST
};

/*
 * Batches lazy sequences and decides their output order.
 *
 *   R       : the compact-sequence range brand (any alphabet — generic per
 *             the bioseq guidelines; never hardcoded to dna4).
 *   Scanner : the lazy back-end. `sequence_scanner_none` keeps the bytes in
 *             RAM (zero-overhead pointer wrapper); `sequence_scanner_pg`
 *             streams fragments from columnar storage for larger-than-RAM
 *             sequences. The processor is identical for both.
 *   Batch   : a sized, random-access view of the lazy wrappers (a std::span).
 */
template<
    supports_compact_sequence_range R,
    is_valid_sequence_scanner Scanner,
    typename Batch = std::span<lazy_compact_sequence<R, Scanner>>
>
    requires (std::ranges::sized_range<Batch> &&
              std::ranges::random_access_range<Batch>)
struct lazy_compact_sequence_batch_processor {
    using seq_t   = std::ranges::range_value_t<Batch>;
    using batch_t = Batch;

    /*
     * Return a permutation `order` of [0, batch.size()): the executor emits
     * batch element `order[i]` as its i-th output row.
     *
     * PLACEHOLDER — returns the reverse permutation (`batch.reverse()`
     * expressed as indices) instead of a real ordering, so the end-to-end
     * custom-scan path is provable via SQL without implementing the
     * (out-of-scope) sequence comparison.
     *
     * Returning a permutation rather than reordering the span in place is
     * deliberate on two counts: (1) the executor must reorder whole tuples —
     * every projected column travels with its sequence, not just the seq
     * Datum; (2) the FRAGMENTED `lazy_compact_sequence` specialisation is
     * non-movable (it owns its Scanner + fragment cache), so a sort that
     * swaps wrappers could not compile for it anyway. A permutation works
     * uniformly for both specialisations.
     */
    [[nodiscard]] static std::vector<std::size_t>
    lazy_compact_sequence_batch_sort(const Batch& batch,
                                     [[maybe_unused]] const batch_sort_spec& spec = {}) {
        const std::size_t batch_size = std::ranges::size(batch);
        std::vector<std::size_t> order(batch_size);
        for (std::size_t i = 0; i < batch_size; ++i) {
            order[i] = batch_size - 1 - i;   //< reverse: last scan row emitted first
        }
        return order;
    }
};

}  // namespace bioseq::storage
