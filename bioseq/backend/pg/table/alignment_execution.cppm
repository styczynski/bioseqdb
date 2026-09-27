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
import bioseq.pipeline.minimap_parameters_contract;

export namespace bioseq::postgres::table::alignment_execution {

/** Index used before a scan node has registered its statistics. */
inline constexpr std::size_t k_unregistered_node_index = (std::numeric_limits<std::size_t>::max)();

/** Per-scan-node statistics accumulated across rescans. */
struct alignment_node_stats {
    std::uint32_t scan_node_id = 0;
    bool has_batches = false;
    bool is_current_pass_fully_drained = false;
    bool has_incomplete_pass = false;
    bioseq::pipeline_stats::pipeline_execution_stats pipeline{};
};

/** Alignment statistics for one statement, owned by its executor hook. */
struct alignment_execution_collector {
    alignment_execution_collector* root_collector = nullptr;
    std::uint32_t nested_alignment_executor_count = 0;
    bool is_lifecycle_counter_saturated = false;
    std::vector<alignment_node_stats> alignment_nodes{};
    bioseq::pipeline_stats::persisted_gpu_operation_accumulator persisted_gpu_operations{};
    bioseq::simd::downstream_active_wall_tracker downstream_active_wall{};
};

/**
 * @brief Alignment statistics saved after a statement finishes.
 *
 * Store values only. Executor pointers and query-context memory expire during
 * cleanup and must not appear in this snapshot.
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Benchmark-only timing records are stored separately from GPU statistics.
    bioseq::simd::benchmark_e2e_interval_origin_buffer
        benchmark_e2e_interval_origins{};
#endif
};

/**
 * @brief Connect scan nodes to their statement's statistics collector.
 *
 * Vector indices stay valid when nested executors add entries and move the
 * vector's storage. Retire this binding before freeing the collector so a
 * cursor that outlives its root statement reports an error.
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

/** Borrowed connection from an alignment scan node to its statistics. */
struct alignment_node_registration_handle {
    alignment_execution_binding* binding = nullptr;
    std::uint32_t scan_node_id = 0;
    std::size_t node_index = k_unregistered_node_index;

    /** @return Whether the handle belongs to an executor frame. */
    [[nodiscard]] bool is_bound() const noexcept {
        return binding != nullptr;
    }

    /** @return Whether the binding is active and has a registered index. */
    [[nodiscard]] bool is_registered() const noexcept {
        return is_bound() && !binding->is_retired && binding->root_collector != nullptr &&
               node_index != k_unregistered_node_index;
    }
};

/** Executor-hook callback that connects a scan node to an EState. */
using alignment_node_binder = alignment_node_registration_handle (*)(EState*, std::uint32_t);

inline alignment_node_binder alignment_node_binder_callback = nullptr;

/**
 * @brief Install the callback that connects scan nodes to executor statistics.
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

/** Register a scan node on first use, or reuse its existing entry. */
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
 * @brief Borrow the statement's accumulator for persisted GPU operations.
 * @return Pointer valid until executor cleanup.
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

/** Borrow the statement timer that combines overlapping downstream work intervals. */
[[nodiscard]] inline bioseq::simd::downstream_active_wall_tracker*
borrow_downstream_active_wall_tracker(alignment_node_registration_handle& handle) {
    if (!handle.is_bound() || handle.binding->is_retired ||
        handle.binding->root_collector == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq downstream active-wall tracker is unavailable")));
    }
    return &handle.binding->root_collector->downstream_active_wall;
}

/** Add a completed batch's statistics. The caller must submit each batch once. */
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

/** Mark the current scan pass complete after all input and buffered output is consumed. */
inline void mark_alignment_pass_fully_drained(alignment_node_registration_handle& handle) {
    ensure_alignment_node_registered(handle);
    if (handle.is_registered()) {
        get_registered_alignment_node(handle).is_current_pass_fully_drained = true;
    }
}

/** Record a rescan and remember whether the abandoned pass was incomplete. */
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
