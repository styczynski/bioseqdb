/*-------------------------------------------------------------------------
 *
 * table/bioseq_scan_state.cppm
 *
 * BioseqScanState is the state object passed between the lifecycle hooks
 * of the bioseq custom scan node (BeginCustomScan, ExecCustomScan,
 * EndCustomScan, ReScanCustomScan, ExplainCustomScan, the parallel DSM
 * hooks). PostgreSQL passes around an opaque CustomScanState* and expects
 * the extension to cast it back to its concrete state type; we exploit
 * the fact that CustomScanState is the first member of the layout to
 * make those casts safe and uniform.
 *
 * BioseqIndexScanState plays the same role for the bioseq IndexScan
 * custom node. Both wrappers expose helper methods so call sites no
 * longer have to spell out C-style casts every time they need to move
 * between the bioseq state and the underlying CustomScanState/Node.
 *
 * Layout invariant: every state struct keeps CustomScanState as its
 * first member and is verified to be standard-layout, so the
 * reinterpret-cast across the C ABI continues to round-trip cleanly.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <type_traits>

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_scan_state;

/*
 * Vectorization-related per-scan state. Kept as a nested struct so that
 * callers can clearly tell vectorization metadata apart from the rest of
 * BioseqScanState.
 */
export struct BioseqScanVectorizationState
{
    bool vectorizationEnabled;
    bool vectorizationAggregate;
    TupleTableSlot* scanVectorSlot;
    TupleTableSlot* resultVectorSlot;
    uint32 vectorPendingRowNumber;
    uint32 vectorRowIndex;
    List* vectorizedQualList;
    List* constructedVectorizedQualList;
    List* attrNeededList;
};

/*
 * BioseqScanState is the per-execution state for a bioseq columnar custom
 * scan. CustomScanState must be the first field for the cast helpers
 * (from_node, from_custom_scan_state, as_node) to be valid.
 */
export struct BioseqScanState
{
    CustomScanState custom_scanstate;
    Bitmapset* attrNeeded;

    ExprContext* css_RuntimeContext;
    List* qual;

    ParallelColumnarScan parallelColumnarScan;

    BioseqScanVectorizationState vectorization;

    Snapshot snapshot;
    bool snapshotRegisteredByUs;

    /*
     * Reinterpret a CustomScanState* received from PostgreSQL back into
     * a BioseqScanState*. Safe because custom_scanstate is the first
     * field; static_assert on standard-layout below guarantees the cast
     * stays valid.
     */
    [[nodiscard]] static BioseqScanState* from_custom_scan_state(CustomScanState* state) {
        return reinterpret_cast<BioseqScanState*>(state);
    }

    /* Same as above for callers that hold a generic Node*. */
    [[nodiscard]] static BioseqScanState* from_node(Node* node) {
        return reinterpret_cast<BioseqScanState*>(node);
    }

    /*
     * Allocate a fresh BioseqScanState via PostgreSQL's newNode() helper,
     * tagged as T_CustomScanState so the executor recognises it.
     */
    [[nodiscard]] static BioseqScanState* allocate() {
        return reinterpret_cast<BioseqScanState*>(newNode(sizeof(BioseqScanState), T_CustomScanState));
    }

    /* Reverse direction: hand a Node* back to PostgreSQL. */
    [[nodiscard]] Node* as_node() {
        return reinterpret_cast<Node*>(&custom_scanstate);
    }

    /* Hand back the embedded CustomScanState* (non-cast accessor). */
    [[nodiscard]] CustomScanState* as_custom_scan_state() {
        return &custom_scanstate;
    }

    /* Convenience accessor: the nested ScanState used by the executor. */
    [[nodiscard]] ScanState* scan_state() {
        return &custom_scanstate.ss;
    }

    /* Convenience accessor: the PlanState shared by the executor. */
    [[nodiscard]] PlanState* plan_state() {
        return &custom_scanstate.ss.ps;
    }

    /* Convenience accessor: the relation being scanned. */
    [[nodiscard]] Relation current_relation() {
        return custom_scanstate.ss.ss_currentRelation;
    }
};

/*
 * BioseqIndexScanState is the per-execution state for the bioseq
 * IndexScan wrapper. As with BioseqScanState, the first field must be a
 * CustomScanState so reinterpret-casts from PostgreSQL round-trip.
 */
export struct BioseqIndexScanState
{
    CustomScanState css;
    IndexScanState* indexscan_state;

    [[nodiscard]] static BioseqIndexScanState* from_custom_scan_state(CustomScanState* state) {
        return reinterpret_cast<BioseqIndexScanState*>(state);
    }

    [[nodiscard]] static BioseqIndexScanState* from_node(Node* node) {
        return reinterpret_cast<BioseqIndexScanState*>(node);
    }

    [[nodiscard]] static BioseqIndexScanState* from_plan_state(PlanState* plan_state) {
        return reinterpret_cast<BioseqIndexScanState*>(plan_state);
    }

    [[nodiscard]] static BioseqIndexScanState* allocate() {
        return reinterpret_cast<BioseqIndexScanState*>(newNode(sizeof(BioseqIndexScanState), T_CustomScanState));
    }

    [[nodiscard]] Node* as_node() {
        return reinterpret_cast<Node*>(&css);
    }

    [[nodiscard]] CustomScanState* as_custom_scan_state() {
        return &css;
    }

    [[nodiscard]] PlanState* plan_state() {
        return &css.ss.ps;
    }
};

/*
 * Layout invariants. Both wrappers are reinterpret_cast'd back and forth
 * across the PostgreSQL C ABI, so they must stay standard-layout. If a
 * future field promotes either struct to non-standard-layout the C
 * boundary would silently corrupt memory; static_assert catches that at
 * compile time.
 */
static_assert(std::is_standard_layout_v<BioseqScanVectorizationState>,
              "BioseqScanVectorizationState must remain standard-layout (memcpy / reinterpret_cast safety)");
static_assert(std::is_standard_layout_v<BioseqScanState>,
              "BioseqScanState must remain standard-layout (cast to/from CustomScanState* must be valid)");
static_assert(std::is_standard_layout_v<BioseqIndexScanState>,
              "BioseqIndexScanState must remain standard-layout (cast to/from CustomScanState* must be valid)");
static_assert(offsetof(BioseqScanState, custom_scanstate) == 0,
              "BioseqScanState::custom_scanstate must be the first field");
static_assert(offsetof(BioseqIndexScanState, css) == 0,
              "BioseqIndexScanState::css must be the first field");
