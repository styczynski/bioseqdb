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

#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

export module bioseq.table.scan_state;

import bioseq.vector.types;
import bioseq.storage.metadata;   // DOC0322 §9.2 — ParallelBioseqColumnScan (was types.h)
import bioseq.pg_utils.lists;
import bioseq.pg_utils.bitmapset;
import bioseq.pipeline.stats;

/*
 * DOC0371 \u00a75.2 \u2014 PostgreSQL-only statement-attestation values shared by
 * mod.cppm (executor lifecycle owner) and custom_scan.cppm (alignment-node
 * producer).  The canonical biological proof remains the target-independent
 * PipelineExecutionStats; this envelope adds only executor/node lifecycle.
 */
export namespace bioseq::pg::alignment_execution {

inline constexpr std::size_t unregistered_node_index =
    (std::numeric_limits<std::size_t>::max)();

struct AlignmentNodeStats {
    std::uint32_t scan_node_id = 0;
    bool saw_batch = false;
    bool current_pass_fully_drained = false;
    bool incomplete_pass_seen = false;
    bioseq::pipeline_stats::PipelineExecutionStats pipeline{};
};

struct AlignmentExecutionCollector {
    AlignmentExecutionCollector* root = nullptr;
    std::uint32_t nested_alignment_executor_count = 0;
    bool lifecycle_counter_saturated = false;
    std::vector<AlignmentNodeStats> alignment_nodes{};
    bioseq::pipeline_stats::PersistedGpuOperationEvidenceLedger
        persisted_gpu_operation_evidence{};
};

/*
 * Immutable value copied into the backend-local published slot at the sole
 * ExecutorFinish publication point.  No QueryDesc/EState/collector pointer is
 * allowed to escape into the SQL-facing snapshot.
 */
struct AlignmentExecutionSnapshot {
    std::int64_t execution_id = 0;
    std::int32_t backend_pid = 0;
    Oid database_oid = InvalidOid;
    std::int32_t scan_node_count = 0;
    std::int32_t nested_alignment_executor_count = 0;
    bool completed = false;
    bool partial = false;
    bool fully_drained = false;
    bioseq::pipeline_stats::PipelineExecutionStats pipeline{};
    bioseq::pipeline_stats::PersistedGpuOperationEvidenceLedger
        persisted_gpu_operation_evidence{};
};

/*
 * A node retains only the collector plus a stable vector index.  In
 * particular it never retains AlignmentNodeStats*, because another nested
 * node may grow the collector vector and invalidate all element addresses.
 * nested_alignment_invoked belongs to the executor frame in mod.cppm and
 * remains alive until the query-context reset callback runs.
 */
/*
 * Frame-owned indirection retained by every node handle.  A genuinely nested
 * portal may outlive the ExecutorRun that created it; if its root finishes
 * first, mod.cppm retires this binding before the root collector can disappear.
 * A later ExecCustomScan then raises instead of dereferencing escaped state.
 */
struct AlignmentCollectorBinding {
    struct NodeRegistration {
        std::uint32_t scan_node_id = 0;
        std::size_t node_index = unregistered_node_index;
    };

    AlignmentExecutionCollector* root = nullptr;
    bool* nested_alignment_invoked = nullptr;
    bool retired = false;
    std::vector<NodeRegistration> node_registrations{};
};

struct AlignmentNodeRegistrationHandle {
    AlignmentCollectorBinding* binding = nullptr;
    std::uint32_t scan_node_id = 0;
    std::size_t node_index = unregistered_node_index;

    [[nodiscard]] bool bound() const noexcept {
        return binding != nullptr;
    }

    [[nodiscard]] bool registered() const noexcept {
        return bound() && !binding->retired && binding->root != nullptr
            && node_index != unregistered_node_index;
    }
};

using AlignmentNodeBindCallback = AlignmentNodeRegistrationHandle (*)(
    EState*, std::uint32_t);

inline AlignmentNodeBindCallback alignment_node_bind_callback = nullptr;

/* Install the mod.cppm-owned EState/frame lookup without introducing a cycle. */
inline void install_alignment_node_bind_callback(
        AlignmentNodeBindCallback callback) {
    if (callback == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq alignment execution binder is null")));
    }
    if (alignment_node_bind_callback != nullptr
        && alignment_node_bind_callback != callback) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq alignment execution binder is already installed")));
    }
    alignment_node_bind_callback = callback;
}

[[nodiscard]] inline AlignmentNodeRegistrationHandle bind_alignment_node(
        EState* estate,
        std::uint32_t scan_node_id) {
    if (alignment_node_bind_callback == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq alignment execution hooks are not installed")));
    }
    return alignment_node_bind_callback(estate, scan_node_id);
}

[[nodiscard]] inline AlignmentNodeStats& registered_node(
        AlignmentNodeRegistrationHandle& handle) {
    if (!handle.registered()
        || handle.node_index >= handle.binding->root->alignment_nodes.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq alignment node registration is invalid")));
    }
    return handle.binding->root->alignment_nodes[handle.node_index];
}

/* Lazily register on first ExecCustomScan, not at executor initialization. */
inline void ensure_alignment_node_registered(
        AlignmentNodeRegistrationHandle& handle) {
    if (!handle.bound()) {
        return;
    }
    if (handle.binding->retired || handle.binding->root == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq nested alignment executor outlived its root statement"),
             errdetail("the escaped portal was retired fail closed before its root collector was released")));
    }
    if (handle.registered()) {
        return;
    }
    for (const AlignmentCollectorBinding::NodeRegistration& registration :
         handle.binding->node_registrations) {
        if (registration.scan_node_id != handle.scan_node_id) {
            continue;
        }
        if (registration.node_index
                >= handle.binding->root->alignment_nodes.size()
            || handle.binding->root->alignment_nodes[
                   registration.node_index].scan_node_id
                != handle.scan_node_id) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq alignment node binding registry is invalid")));
        }
        handle.node_index = registration.node_index;
        return;
    }
    if (handle.binding->nested_alignment_invoked != nullptr
        && !*handle.binding->nested_alignment_invoked) {
        if (handle.binding->root->nested_alignment_executor_count
            >= bioseq::pipeline_stats::sql_integer_value_max) {
            handle.binding->root->lifecycle_counter_saturated = true;
        } else {
            ++handle.binding->root->nested_alignment_executor_count;
        }
        *handle.binding->nested_alignment_invoked = true;
    }
    handle.node_index = handle.binding->root->alignment_nodes.size();
    handle.binding->root->alignment_nodes.push_back(AlignmentNodeStats{
        .scan_node_id = handle.scan_node_id,
    });
    handle.binding->node_registrations.push_back(
        AlignmentCollectorBinding::NodeRegistration{
            .scan_node_id = handle.scan_node_id,
            .node_index = handle.node_index,
        });
}

/// Borrow the statement-root ledger through the same non-owning binding used
/// for node stats. The scheduler may never retain it beyond executor cleanup.
[[nodiscard]] inline bioseq::pipeline_stats::
PersistedGpuOperationEvidenceLedger*
borrow_persisted_gpu_operation_evidence_sink(
        AlignmentNodeRegistrationHandle& handle) {
    if (!handle.bound() || handle.binding->retired
        || handle.binding->root == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq persisted GPU evidence sink is unavailable")));
    }
    return &handle.binding->root->persisted_gpu_operation_evidence;
}

/* Merge one successfully completed, non-empty dispatch buffer exactly once. */
inline void merge_alignment_batch(
        AlignmentNodeRegistrationHandle& handle,
        const bioseq::pipeline_stats::PipelineExecutionStats& batch) {
    ensure_alignment_node_registered(handle);
    if (!handle.registered()) {
        return;
    }
    AlignmentNodeStats& node = registered_node(handle);
    node.saw_batch = true;
    node.pipeline.merge(batch);
}

/* Mark EOF only after the source and every buffered output cursor are empty. */
inline void mark_alignment_pass_fully_drained(
        AlignmentNodeRegistrationHandle& handle) {
    ensure_alignment_node_registered(handle);
    if (handle.registered()) {
        registered_node(handle).current_pass_fully_drained = true;
    }
}

/* Preserve cumulative proof while making an abandoned pass permanently partial. */
inline void mark_alignment_rescan(
        AlignmentNodeRegistrationHandle& handle) {
    if (!handle.registered()) {
        return;
    }
    AlignmentNodeStats& node = registered_node(handle);
    if (!node.current_pass_fully_drained) {
        node.incomplete_pass_seen = true;
    }
    node.current_pass_fully_drained = false;
}

} // namespace bioseq::pg::alignment_execution

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
