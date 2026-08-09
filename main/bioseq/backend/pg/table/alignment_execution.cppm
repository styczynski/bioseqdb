/*-------------------------------------------------------------------------
 *
 * table/alignment_execution.cppm
 *
 * PostgreSQL-only statement-attestation values shared by the executor-hook
 * lifecycle owner and custom-scan alignment-node producers.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

export module bioseq.table.alignment_execution;

import bioseq.pipeline.stats;

/*
 * DOC0371 §5.2 — PostgreSQL-only statement-attestation values shared by
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
