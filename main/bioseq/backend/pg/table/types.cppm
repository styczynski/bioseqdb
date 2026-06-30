/*-------------------------------------------------------------------------
 *
 * table/types.cppm
 *
 * Plain-old-data state objects shared between the bioseq table-access
 * method, the columnar custom scan, and the columnar index scan. None of
 * these structs own any non-trivial members; they exist purely as state
 * blobs passed across the PostgreSQL C ABI.
 *
 * Note: BioseqScanState (the CustomScanState wrapper for the bioseq
 * custom scan) and BioseqIndexScanState live in bioseq_scan_state.cppm
 * so their helper methods can be defined alongside the layout
 * invariants. Re-export them here for the convenience of callers that
 * import bioseq_table_types.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <math.h>
#include <type_traits>

export module bioseq.table.types;

export import bioseq.table.scan_state;
import bioseq.vector.types;
import bioseq.storage.metadata;   // DOC0322 §9.2 — BioseqColumnReadState, ParallelBioseqColumnScan, StripeMetadata (was types.h)
import bioseq.pg_utils.lists;
import bioseq.pg_utils.bitmapset;

/*
 * BioseqColumnScanDescData is the scan descriptor handed back to the executor
 * from beginscan(); subsequent getnextslot()/rescan()/endscan() calls
 * receive it via the TableScanDesc base pointer.
 */
export struct BioseqColumnScanDescData
{
    TableScanDescData cs_base;
    BioseqColumnReadState* cs_readState;

    /*
     * cs_readState is initialised lazily on the first getnextslot() call;
     * the scanContext / attr_needed / scanQual fields below are captured
     * during beginscan() so the lazy init has the inputs it needs.
     */
    MemoryContext scanContext;
    PBitmapset attr_needed;
    PList<Node> scanQual;

    ParallelBioseqColumnScan parallelBioseqColumnScan;

    bool returnVectorizedTuple;

    /*
     * DOC0260 Phase A — deferred-hydration switch and per-scan relation
     * cache. seq_cache is a `bioseq::storage::seq_caches*` allocated
     * separately in scanContext via placement-new; the void* erasure keeps
     * this struct standard-layout (the seq_caches embeds a non-standard-layout
     * cache container).
     */
    bool defer_seq_hydration;
    void* seq_cache;
};

static_assert(std::is_standard_layout_v<BioseqColumnScanDescData>,
              "BioseqColumnScanDescData must stay standard-layout (round-trips through TableScanDesc).");

export using BioseqColumnScanDesc = BioseqColumnScanDescData*;

/*
 * IndexFetchBioseqColumnData is the per-fetch state for index_fetch_begin /
 * index_fetch_reset / index_fetch_end / index_fetch_tuple. Lazy-init of
 * cs_readState mirrors BioseqColumnScanDescData.
 */
export struct IndexFetchBioseqColumnData
{
    IndexFetchTableData cs_base;
    BioseqColumnReadState* cs_readState;
    PBitmapset attr_needed;
    PList<StripeMetadata> stripeMetadataList;
    bool is_select_query;

    /*
     * scanContext is captured in bioseq_index_fetch_begin so the lazy
     * cs_readState allocation in bioseq_index_fetch_tuple lands in a
     * sub-context of the begin call rather than the per-tuple context.
     */
    MemoryContext scanContext;

    /*
     * DOC0260 Phase A — see BioseqColumnScanDescData above.
     */
    bool defer_seq_hydration;
    void* seq_cache;
};

static_assert(std::is_standard_layout_v<IndexFetchBioseqColumnData>,
              "IndexFetchBioseqColumnData must stay standard-layout (round-trips through IndexFetchTableData).");

export using PathPredicate = bool (*)(Path* path);
