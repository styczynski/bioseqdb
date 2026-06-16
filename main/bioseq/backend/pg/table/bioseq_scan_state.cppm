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

#include <new>
#include <type_traits>

export module bioseq.table.scan_state;

import bioseq.vector.types;
import bioseq.storage.metadata;   // DOC0322 §9.2 — ParallelBioseqColumnScan (was types.h)
import bioseq.pg_utils.lists;
import bioseq.pg_utils.bitmapset;

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
    uint32_t vectorPendingRowNumber;
    uint32_t vectorRowIndex;
    PList<Node> vectorizedQualList;
    PList<Node> constructedVectorizedQualList;
    PIntList attrNeededList;
};

/*
 * BioseqScanState is the per-execution state for a bioseq columnar custom
 * scan. CustomScanState must be the first field for the cast helpers
 * (from_node, from_custom_scan_state, as_node) to be valid.
 */
export struct BioseqScanState
{
    CustomScanState custom_scanstate;
    PBitmapset attrNeeded;

    ExprContext* css_RuntimeContext;
    PList<Node> qual;

    ParallelBioseqColumnScan parallelBioseqColumnScan;

    BioseqScanVectorizationState vectorization;

    Snapshot snapshot;
    bool snapshotRegisteredByUs;

    /*
     * Per-scan runtime, owned by BioseqScanState. Stored as a `void*`
     * opaque pointer so the std::vector / std::map / TupleDispatchBuffer
     * fields it really points at don't drag non-standard-layout state
     * into this struct (BioseqScanState must remain standard-layout for
     * the reinterpret_cast across the Postgres C ABI to round-trip). The
     * concrete type is `BioseqScanRuntime`, defined inside
     * columnar_customscan.cppm; that file is the only one that
     * dereferences the pointer. Allocated in
     * BioseqColumnScan_CreateCustomScanState and released in
     * BioseqColumnScan_EndCustomScan.
     *
     * Owning this state per-scan-state (rather than as a file static)
     * is what lets a single backend run two BioseqScan nodes
     * concurrently — a precondition for working CROSS / LEFT joins.
     */
    void* runtime;

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
        auto* state = ::new (palloc0(sizeof(BioseqScanState))) BioseqScanState{};
        reinterpret_cast<Node*>(&state->custom_scanstate)->type = T_CustomScanState;
        return state;
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

    /*
     * Returns the CustomScan plan node that this state was built from.
     * Centralises the `reinterpret_cast<CustomScan*>(ss.ps.plan)` cast
     * that call sites would otherwise duplicate.
     */
    [[nodiscard]] CustomScan* custom_scan_plan() {
        return reinterpret_cast<CustomScan*>(custom_scanstate.ss.ps.plan);
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
        auto* state = ::new (palloc0(sizeof(BioseqIndexScanState))) BioseqIndexScanState{};
        reinterpret_cast<Node*>(&state->css)->type = T_CustomScanState;
        return state;
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

    /*
     * Returns the CustomScan plan node that wraps the inner IndexScan
     * so call sites don't have to spell out the reinterpret_cast.
     */
    [[nodiscard]] CustomScan* custom_scan_plan() {
        return reinterpret_cast<CustomScan*>(css.ss.ps.plan);
    }

    /*
     * Returns the inner IndexScan plan node that we wrapped. The plan
     * is stored as the first (and only) entry of the outer CustomScan's
     * custom_plans list, so this centralises the PList indexing +
     * reinterpret_cast sequence that call sites would otherwise repeat.
     */
    [[nodiscard]] IndexScan* inner_index_scan_plan() {
        return reinterpret_cast<IndexScan*>(
            PList<Plan>(custom_scan_plan()->custom_plans).first());
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
