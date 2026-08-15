/**
 * @file
 * @brief PostgreSQL executor state shared by alignment hooks and scan nodes.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

export module bioseq.postgres.table.alignment_execution;

import bioseq.pipeline.stats;

export namespace bioseq::postgres::table::alignment_execution {

/** Sentinel used until a scan node has joined its statement collector. */
inline constexpr std::size_t k_unregistered_node_index = (std::numeric_limits<std::size_t>::max)();

/** Per-scan-node statistics accumulated across rescans. */
struct alignment_node_stats {
    std::uint32_t scan_node_id = 0;
    bool has_batches = false;
    bool is_current_pass_fully_drained = false;
    bool has_incomplete_pass = false;
    bioseq::pipeline_stats::pipeline_execution_stats pipeline{};
};

/** Mutable statement-level collector owned by the executor hook frame. */
struct alignment_execution_collector {
    alignment_execution_collector* root_collector = nullptr;
    std::uint32_t nested_alignment_executor_count = 0;
    bool is_lifecycle_counter_saturated = false;
    std::vector<alignment_node_stats> alignment_nodes{};
    bioseq::pipeline_stats::persisted_gpu_operation_accumulator persisted_gpu_operations{};
};

/**
 * @brief Pointer-free value published after a statement finishes.
 *
 * Executor and query-context addresses must never escape into this snapshot.
 */
struct alignment_execution_snapshot {
    std::int64_t execution_id = 0;
    std::int32_t backend_pid = 0;
    Oid database_oid = InvalidOid;
    std::int32_t scan_node_count = 0;
    std::int32_t nested_alignment_executor_count = 0;
    bool is_completed = false;
    bool is_partial = false;
    bool is_fully_drained = false;
    bioseq::pipeline_stats::pipeline_execution_stats pipeline{};
    bioseq::pipeline_stats::persisted_gpu_operation_accumulator persisted_gpu_operations{};
};

/**
 * @brief Frame-owned indirection retained by alignment scan nodes.
 *
 * Nodes retain a vector index instead of an element pointer because nested
 * executor registration may reallocate the collector vector. Retiring this
 * binding makes escaped portals fail closed after their root statement exits.
 */
struct alignment_execution_binding {
    /** Stable association between a plan node ID and collector index. */
    struct node_registration {
        std::uint32_t scan_node_id = 0;
        std::size_t node_index = k_unregistered_node_index;
    };

    alignment_execution_collector* root_collector = nullptr;
    bool* nested_executor_invoked = nullptr;
    bool is_retired = false;
    std::vector<node_registration> node_registrations{};
};

/** Non-owning registration retained by one alignment custom-scan node. */
struct alignment_node_registration_handle {
    alignment_execution_binding* binding = nullptr;
    std::uint32_t scan_node_id = 0;
    std::size_t node_index = k_unregistered_node_index;

    /** @return Whether the handle belongs to an executor frame. */
    [[nodiscard]] bool is_bound() const noexcept {
        return binding != nullptr;
    }

    /** @return Whether the handle currently names a live collector entry. */
    [[nodiscard]] bool is_registered() const noexcept {
        return is_bound() && !binding->is_retired && binding->root_collector != nullptr &&
               node_index != k_unregistered_node_index;
    }
};

/** Callback supplied by the executor-hook owner to bind an `EState`. */
using alignment_node_binder = alignment_node_registration_handle (*)(EState*, std::uint32_t);

inline alignment_node_binder alignment_node_binder_callback = nullptr;

/**
 * @brief Install the executor-hook-owned node binder.
 * @param binder Non-null callback. Reinstalling a different callback is an error.
 */
inline void install_alignment_node_binder(alignment_node_binder binder) {
    if (binder == nullptr) {
        ereport(
            ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR), errmsg("bioseq alignment execution binder is null")));
    }
    if (alignment_node_binder_callback != nullptr && alignment_node_binder_callback != binder) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq alignment execution binder is already installed")));
    }
    alignment_node_binder_callback = binder;
}

/** Bind a plan node to the collector associated with `executor_state`. */
[[nodiscard]] inline alignment_node_registration_handle
bind_alignment_node(EState* executor_state, std::uint32_t scan_node_id) {
    if (alignment_node_binder_callback == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq alignment execution hooks are not installed")));
    }
    return alignment_node_binder_callback(executor_state, scan_node_id);
}

/** Return the live statistics entry named by `handle`. */
[[nodiscard]] inline alignment_node_stats&
get_registered_alignment_node(alignment_node_registration_handle& handle) {
    if (!handle.is_registered() ||
        handle.node_index >= handle.binding->root_collector->alignment_nodes.size()) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq alignment node registration is invalid")));
    }
    return handle.binding->root_collector->alignment_nodes[handle.node_index];
}

/** Lazily register a node when its custom scan first executes. */
inline void ensure_alignment_node_registered(alignment_node_registration_handle& handle) {
    if (!handle.is_bound()) {
        return;
    }
    if (handle.binding->is_retired || handle.binding->root_collector == nullptr) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq nested alignment executor outlived its root statement"),
             errdetail("the escaped portal was retired before its root collector was released")));
    }
    if (handle.is_registered()) {
        return;
    }

    for (const alignment_execution_binding::node_registration& registration :
         handle.binding->node_registrations) {
        if (registration.scan_node_id != handle.scan_node_id) {
            continue;
        }
        if (registration.node_index >= handle.binding->root_collector->alignment_nodes.size() ||
            handle.binding->root_collector->alignment_nodes[registration.node_index].scan_node_id !=
                handle.scan_node_id) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq alignment node binding registry is invalid")));
        }
        handle.node_index = registration.node_index;
        return;
    }

    if (handle.binding->nested_executor_invoked != nullptr &&
        !*handle.binding->nested_executor_invoked) {
        auto& root = *handle.binding->root_collector;
        if (root.nested_alignment_executor_count >=
            bioseq::pipeline_stats::k_sql_integer_value_max) {
            root.is_lifecycle_counter_saturated = true;
        } else {
            ++root.nested_alignment_executor_count;
        }
        *handle.binding->nested_executor_invoked = true;
    }

    auto& root = *handle.binding->root_collector;
    handle.node_index = root.alignment_nodes.size();
    root.alignment_nodes.push_back(alignment_node_stats{
        .scan_node_id = handle.scan_node_id,
    });
    handle.binding->node_registrations.push_back(alignment_execution_binding::node_registration{
        .scan_node_id = handle.scan_node_id,
        .node_index = handle.node_index,
    });
}

/**
 * @brief Borrow the statement accumulator through a live node binding.
 * @return Non-owning accumulator pointer valid only until executor cleanup.
 */
[[nodiscard]] inline bioseq::pipeline_stats::persisted_gpu_operation_accumulator*
borrow_persisted_gpu_operation_accumulator(alignment_node_registration_handle& handle) {
    if (!handle.is_bound() || handle.binding->is_retired ||
        handle.binding->root_collector == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq persisted GPU operation accumulator is unavailable")));
    }
    return &handle.binding->root_collector->persisted_gpu_operations;
}

/** Merge one successfully completed, non-empty dispatch batch exactly once. */
inline void merge_alignment_batch(alignment_node_registration_handle& handle,
                                  const bioseq::pipeline_stats::pipeline_execution_stats& batch) {
    ensure_alignment_node_registered(handle);
    if (!handle.is_registered()) {
        return;
    }
    alignment_node_stats& node = get_registered_alignment_node(handle);
    node.has_batches = true;
    node.pipeline.merge(batch);
}

/** Record EOF after the source and every buffered output cursor are empty. */
inline void mark_alignment_pass_fully_drained(alignment_node_registration_handle& handle) {
    ensure_alignment_node_registered(handle);
    if (handle.is_registered()) {
        get_registered_alignment_node(handle).is_current_pass_fully_drained = true;
    }
}

/** Record a rescan and retain whether the abandoned pass was incomplete. */
inline void record_alignment_rescan(alignment_node_registration_handle& handle) {
    if (!handle.is_registered()) {
        return;
    }
    alignment_node_stats& node = get_registered_alignment_node(handle);
    if (!node.is_current_pass_fully_drained) {
        node.has_incomplete_pass = true;
    }
    node.is_current_pass_fully_drained = false;
}

} // namespace bioseq::postgres::table::alignment_execution
