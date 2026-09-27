/**
 * @file scan_state.cppm
 * Executor state passed between Bioseq custom scans and PostgreSQL callbacks.
 *
 * CustomScanState must be the first member so PostgreSQL's pointer also points
 * to the enclosing Bioseq state. Layout checks below enforce this requirement.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

export module bioseq.postgres.table.scan_state;

export import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;

/// @brief Column batches and filters used by one custom scan.
export struct scan_vectorization_state {
    /// @brief Whether vectorized execution is active.
    bool is_enabled;
    /// @brief Whether the vectorized plan computes an aggregate.
    bool is_aggregate;
    /// @brief Input vector slot owned by the scan.
    TupleTableSlot* scan_vector_slot;
    /// @brief Output vector slot owned by the scan.
    TupleTableSlot* result_vector_slot;
    /// @brief Rows left to visit in the current batch, including filtered rows.
    std::uint32_t pending_row_number;
    /// @brief Index of the next row to visit in the current batch.
    std::uint32_t row_index;
    /// @brief Filters selected by the planner for evaluation on column batches.
    bioseq::postgres::support::postgres_list<Node> qualifiers;
    /// @brief Filters compiled against the input vector slot for this execution.
    bioseq::postgres::support::postgres_list<Node> constructed_qualifiers;
    /// @brief Zero-based column indexes needed when extracting rows from a batch.
    bioseq::postgres::support::postgres_integer_list attributes_needed;
};

/**
 * @brief State for one execution of a Bioseq column scan.
 * The C++ runtime is stored separately so this object keeps the layout required
 * by PostgreSQL callbacks. The scan owns and cleans up that runtime.
 */
export struct scan_state {
    /// @brief PostgreSQL custom-scan base that must remain the first member.
    CustomScanState custom_scan_state;
    /// @brief Zero-based indexes of columns the scan must read.
    bioseq::postgres::support::postgres_bitmapset attributes_needed;
    /// @brief Context for evaluating filter parameters across rows.
    ExprContext* runtime_expression_context;
    /// @brief Filters passed to storage to skip chunk groups.
    bioseq::postgres::support::postgres_list<Node> qualifiers;
    /// @brief Shared stripe position used by parallel scan workers.
    parallel_column_scan_handle parallel_scan;
    /// @brief Vectorized execution state.
    scan_vectorization_state vectorization;
    /// @brief Snapshot used by the column reader.
    Snapshot snapshot;
    /// @brief Whether this scan must unregister `snapshot` during cleanup.
    bool owns_snapshot_registration;
    /// @brief Separately allocated C++ runtime owned by this scan.
    void* runtime_state;

    /// @brief Recover the Bioseq state from its embedded PostgreSQL state.
    /// @param state CustomScanState belonging to a Bioseq column scan.
    /// @return The enclosing Bioseq state.
    [[nodiscard]] static scan_state* from_custom_scan_state(CustomScanState* state) noexcept {
        return reinterpret_cast<scan_state*>(state);
    }

    /// @brief Recover the Bioseq state from a PostgreSQL node pointer.
    /// @param node Node belonging to a Bioseq column scan.
    /// @return The enclosing Bioseq state.
    [[nodiscard]] static scan_state* from_node(Node* node) noexcept {
        return reinterpret_cast<scan_state*>(node);
    }

    /// @brief Allocates zeroed scan state in PostgreSQL's active memory context.
    /// @return A state tagged as `T_CustomScanState`.
    [[nodiscard]] static scan_state* allocate() {
        auto* state = ::new (palloc0(sizeof(scan_state))) scan_state{};
        reinterpret_cast<Node*>(&state->custom_scan_state)->type = T_CustomScanState;
        return state;
    }

    /// @brief Returns the embedded state through PostgreSQL's generic node type.
    [[nodiscard]] Node* as_node() noexcept {
        return reinterpret_cast<Node*>(&custom_scan_state);
    }

    /// @brief Returns the embedded PostgreSQL custom-scan state.
    [[nodiscard]] CustomScanState* as_custom_scan_state() noexcept {
        return &custom_scan_state;
    }

    /// @brief Returns the embedded PostgreSQL scan executor state.
    [[nodiscard]] ScanState* executor_scan_state() noexcept {
        return &custom_scan_state.ss;
    }

    /// @brief Returns the embedded PostgreSQL plan executor state.
    [[nodiscard]] PlanState* executor_plan_state() noexcept {
        return &custom_scan_state.ss.ps;
    }

    /// @brief Returns the relation currently scanned by PostgreSQL.
    [[nodiscard]] Relation current_relation() noexcept {
        return custom_scan_state.ss.ss_currentRelation;
    }

    /// @brief Returns the CustomScan plan that created this state.
    [[nodiscard]] CustomScan* custom_scan_plan() noexcept {
        return reinterpret_cast<CustomScan*>(custom_scan_state.ss.ps.plan);
    }
};

/// @brief State for a Bioseq custom scan that wraps a PostgreSQL IndexScan.
export struct index_scan_state {
    /// @brief PostgreSQL custom-scan base that must remain the first member.
    CustomScanState custom_scan_state;
    /// @brief Executor state of the wrapped PostgreSQL IndexScan.
    IndexScanState* index_scan_executor_state;

    /// @brief Recover the Bioseq index wrapper from its embedded CustomScanState.
    [[nodiscard]] static index_scan_state* from_custom_scan_state(CustomScanState* state) noexcept {
        return reinterpret_cast<index_scan_state*>(state);
    }

    /// @brief Recover the Bioseq index wrapper from its PostgreSQL node pointer.
    [[nodiscard]] static index_scan_state* from_node(Node* node) noexcept {
        return reinterpret_cast<index_scan_state*>(node);
    }

    /// @brief Recover the Bioseq index wrapper from its embedded plan state.
    [[nodiscard]] static index_scan_state* from_plan_state(PlanState* state) noexcept {
        return reinterpret_cast<index_scan_state*>(state);
    }

    /// @brief Allocates zeroed index-scan state in the active memory context.
    [[nodiscard]] static index_scan_state* allocate() {
        auto* state = ::new (palloc0(sizeof(index_scan_state))) index_scan_state{};
        reinterpret_cast<Node*>(&state->custom_scan_state)->type = T_CustomScanState;
        return state;
    }

    /// @brief Returns the embedded state through PostgreSQL's generic node type.
    [[nodiscard]] Node* as_node() noexcept {
        return reinterpret_cast<Node*>(&custom_scan_state);
    }

    /// @brief Returns the embedded PostgreSQL custom-scan state.
    [[nodiscard]] CustomScanState* as_custom_scan_state() noexcept {
        return &custom_scan_state;
    }

    /// @brief Returns the embedded PostgreSQL plan executor state.
    [[nodiscard]] PlanState* executor_plan_state() noexcept {
        return &custom_scan_state.ss.ps;
    }

    /// @brief Returns the CustomScan plan wrapping the inner IndexScan.
    [[nodiscard]] CustomScan* custom_scan_plan() noexcept {
        return reinterpret_cast<CustomScan*>(custom_scan_state.ss.ps.plan);
    }

    /// @brief Returns the wrapped PostgreSQL IndexScan plan node.
    [[nodiscard]] IndexScan* inner_index_scan_plan() noexcept {
        return reinterpret_cast<IndexScan*>(
            bioseq::postgres::support::postgres_list<Plan>(custom_scan_plan()->custom_plans)
                .get_first());
    }
};

static_assert(std::is_standard_layout_v<scan_vectorization_state>,
              "scan_vectorization_state must remain standard-layout");
static_assert(std::is_standard_layout_v<scan_state>,
              "scan_state must remain standard-layout across CustomScanState callbacks");
static_assert(std::is_standard_layout_v<index_scan_state>,
              "index_scan_state must remain standard-layout across CustomScanState callbacks");
static_assert(offsetof(scan_state, custom_scan_state) == 0,
              "scan_state::custom_scan_state must remain the first member");
static_assert(offsetof(index_scan_state, custom_scan_state) == 0,
              "index_scan_state::custom_scan_state must remain the first member");
