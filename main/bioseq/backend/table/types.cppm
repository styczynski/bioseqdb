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

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_table_types;

export import bioseq_scan_state;
import pg_utils.lists;
import pg_utils.bitmapset;

/*
 * ColumnarScanDescData is the scan descriptor handed back to the executor
 * from beginscan(); subsequent getnextslot()/rescan()/endscan() calls
 * receive it via the TableScanDesc base pointer.
 */
export struct ColumnarScanDescData
{
    TableScanDescData cs_base;
    ColumnarReadState* cs_readState;

    /*
     * cs_readState is initialised lazily on the first getnextslot() call;
     * the scanContext / attr_needed / scanQual fields below are captured
     * during beginscan() so the lazy init has the inputs it needs.
     */
    MemoryContext scanContext;
    PBitmapset attr_needed;
    PList<Node> scanQual;

    ParallelColumnarScan parallelColumnarScan;

    bool returnVectorizedTuple;
};

static_assert(std::is_standard_layout_v<ColumnarScanDescData>,
              "ColumnarScanDescData must stay standard-layout (round-trips through TableScanDesc).");

export using ColumnarScanDesc = ColumnarScanDescData*;

/*
 * IndexFetchColumnarData is the per-fetch state for index_fetch_begin /
 * index_fetch_reset / index_fetch_end / index_fetch_tuple. Lazy-init of
 * cs_readState mirrors ColumnarScanDescData.
 */
export struct IndexFetchColumnarData
{
    IndexFetchTableData cs_base;
    ColumnarReadState* cs_readState;
    PBitmapset attr_needed;
    PList<StripeMetadata> stripeMetadataList;
    bool is_select_query;

    /*
     * scanContext is captured in bioseq_index_fetch_begin so the lazy
     * cs_readState allocation in bioseq_index_fetch_tuple lands in a
     * sub-context of the begin call rather than the per-tuple context.
     */
    MemoryContext scanContext;
};

static_assert(std::is_standard_layout_v<IndexFetchColumnarData>,
              "IndexFetchColumnarData must stay standard-layout (round-trips through IndexFetchTableData).");

export using PathPredicate = bool (*)(Path* path);
