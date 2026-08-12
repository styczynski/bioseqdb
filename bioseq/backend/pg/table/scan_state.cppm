/**
 * @file scan_state.cppm
 * @brief Standard-layout executor state for Bioseq custom scans.
 *
 * PostgreSQL owns the embedded `CustomScanState` and passes its address through
 * C callbacks. Each Bioseq wrapper keeps that object first, remains
 * standard-layout, and centralizes every boundary cast.
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

/// @brief Vectorization-specific state retained by one columnar custom scan.
export struct scan_vectorization_state {
    /// @brief Whether vectorized execution is active.
    bool is_enabled;
    /// @brief Whether the vectorized plan computes an aggregate.
    bool is_aggregate;
    /// @brief Input vector slot owned by the scan.
    TupleTableSlot* scan_vector_slot;
    /// @brief Output vector slot owned by the scan.
    TupleTableSlot* result_vector_slot;
    /// @brief First row not yet emitted from the current vector.
    std::uint32_t pending_row_number;
    /// @brief Current row position within the vector.
    std::uint32_t row_index;
    /// @brief Planned vectorized qualifiers.
    bioseq::postgres::support::postgres_list<Node> qualifiers;
    /// @brief Executor-owned rewritten vectorized qualifiers.
    bioseq::postgres::support::postgres_list<Node> constructed_qualifiers;
    /// @brief One-based attribute numbers required by vectorized execution.
    bioseq::postgres::support::postgres_integer_list attributes_needed;
};

/**
 * @brief Per-execution state for the Bioseq columnar custom scan.
 *
 * Non-standard-layout runtime owners remain behind `runtime_state`; the
 * concrete runtime is created and destroyed by the custom-scan owner.
 */
export struct scan_state {
    /// @brief PostgreSQL custom-scan base; must remain the first member.
    CustomScanState custom_scan_state;
    /// @brief Zero-based bitmap of physical attributes to materialize.
    bioseq::postgres::support::postgres_bitmapset attributes_needed;
    /// @brief Expression context used for runtime qualifier evaluation.
    ExprContext* runtime_expression_context;
    /// @brief Storage qualifiers retained by the scan.
    bioseq::postgres::support::postgres_list<Node> qualifiers;
    /// @brief PostgreSQL parallel-scan coordination record.
    parallel_column_scan_handle parallel_scan;
    /// @brief Vectorized execution state.
    scan_vectorization_state vectorization;
    /// @brief Snapshot used by the column reader.
    Snapshot snapshot;
    /// @brief Whether this scan must unregister `snapshot` during cleanup.
    bool owns_snapshot_registration;
    /// @brief Opaque pointer to the custom-scan runtime owner.
    void* runtime_state;

    /// @brief Reinterprets PostgreSQL's callback pointer as Bioseq scan state.
    /// @param state PostgreSQL custom-scan state pointer.
    /// @return The enclosing Bioseq state.
    [[nodiscard]] static scan_state* from_custom_scan_state(
        CustomScanState* state) noexcept {
        return reinterpret_cast<scan_state*>(state);
    }

    /// @brief Reinterprets a generic PostgreSQL node as Bioseq scan state.
    /// @param node Node whose first object is the embedded custom-scan state.
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

/// @brief Per-execution state for the Bioseq IndexScan wrapper node.
export struct index_scan_state {
    /// @brief PostgreSQL custom-scan base; must remain the first member.
    CustomScanState custom_scan_state;
    /// @brief Executor state of the wrapped PostgreSQL IndexScan.
    IndexScanState* index_scan_executor_state;

    /// @brief Reinterprets PostgreSQL's callback pointer as index-scan state.
    [[nodiscard]] static index_scan_state* from_custom_scan_state(
        CustomScanState* state) noexcept {
        return reinterpret_cast<index_scan_state*>(state);
    }

    /// @brief Reinterprets a generic PostgreSQL node as index-scan state.
    [[nodiscard]] static index_scan_state* from_node(Node* node) noexcept {
        return reinterpret_cast<index_scan_state*>(node);
    }

    /// @brief Reinterprets PostgreSQL plan state as the enclosing index-scan state.
    [[nodiscard]] static index_scan_state* from_plan_state(
        PlanState* state) noexcept {
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
            bioseq::postgres::support::postgres_list<Plan>(
                custom_scan_plan()->custom_plans).get_first());
    }
};

static_assert(
    std::is_standard_layout_v<scan_vectorization_state>,
    "scan_vectorization_state must remain standard-layout");
static_assert(
    std::is_standard_layout_v<scan_state>,
    "scan_state must remain standard-layout across CustomScanState callbacks");
static_assert(
    std::is_standard_layout_v<index_scan_state>,
    "index_scan_state must remain standard-layout across CustomScanState callbacks");
static_assert(
    offsetof(scan_state, custom_scan_state) == 0,
    "scan_state::custom_scan_state must remain the first member");
static_assert(
    offsetof(index_scan_state, custom_scan_state) == 0,
    "index_scan_state::custom_scan_state must remain the first member");
