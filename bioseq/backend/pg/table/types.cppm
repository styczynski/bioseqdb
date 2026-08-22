/**
 * @file types.cppm
 * @brief Standard-layout state shared by Bioseq table and index scans.
 *
 * These records cross PostgreSQL callback boundaries through pointers to their
 * first PostgreSQL-owned base member. They therefore contain no non-trivial
 * owners and retain compile-time layout checks beside their definitions.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstddef>
#include <type_traits>

export module bioseq.postgres.table.types;

export import bioseq.postgres.table.scan_state;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;

/// @brief Private CustomScan marker for vectorized aggregate execution.
export inline constexpr int k_vectorized_aggregate_private_marker = 1;

/**
 * @brief Standard-layout descriptor returned from PostgreSQL table scan begin callbacks.
 *
 * Read state is created lazily on the first row request. Inputs required for
 * that initialization are retained in the scan memory context.
 */
export struct column_scan_descriptor {
    /// @brief PostgreSQL table-scan base that must remain the first member.
    TableScanDescData scan_base;
    /// @brief Lazily initialized column-store reader state.
    column_read_state* read_state;
    /// @brief Memory context owning all scan-lifetime allocations.
    MemoryContext scan_context;
    /// @brief Zero-based bitmap of physical attributes to materialize.
    bioseq::postgres::support::postgres_bitmapset attributes_needed;
    /// @brief Qualifiers used to initialize or rescan the column reader.
    bioseq::postgres::support::postgres_list<Node> scan_qualifiers;
    /// @brief PostgreSQL parallel-scan coordination record.
    parallel_column_scan_handle parallel_scan;
    /// @brief Whether the caller expects vectorized output slots.
    bool returns_vectorized_tuple;
    /// @brief Whether sequence locator stubs remain deferred for bulk hydration.
    bool defers_sequence_hydration;
    /// @brief Opaque pointer to scan-owned `bioseq::storage::seq_caches`.
    void* sequence_cache;
};

static_assert(std::is_standard_layout_v<column_scan_descriptor>,
              "column_scan_descriptor must remain standard-layout across TableScanDesc callbacks");
static_assert(offsetof(column_scan_descriptor, scan_base) == 0,
              "column_scan_descriptor::scan_base must remain the first member");

/// @brief PostgreSQL table-scan handle reinterpreted as the Bioseq descriptor.
export using column_scan_handle = column_scan_descriptor*;

/**
 * @brief Standard-layout state retained between PostgreSQL index-fetch callbacks.
 *
 * Reader and sequence-cache allocation is deferred until the first tuple fetch,
 * using the memory context captured by the begin callback.
 */
export struct index_fetch_state {
    /// @brief PostgreSQL index-fetch base that must remain the first member.
    IndexFetchTableData fetch_base;
    /// @brief Lazily initialized column-store reader state.
    column_read_state* read_state;
    /// @brief Zero-based bitmap of physical attributes to materialize.
    bioseq::postgres::support::postgres_bitmapset attributes_needed;
    /// @brief Stripe metadata ordered for row-number lookup.
    bioseq::postgres::support::postgres_list<stripe_metadata> stripe_metadata;
    /// @brief Whether the fetch belongs to a SELECT execution path.
    bool is_select_query;
    /// @brief Memory context owning all fetch-lifetime allocations.
    MemoryContext scan_context;
    /// @brief Whether sequence locator stubs remain deferred for bulk hydration.
    bool defers_sequence_hydration;
    /// @brief Opaque pointer to fetch-owned `bioseq::storage::seq_caches`.
    void* sequence_cache;
};

static_assert(std::is_standard_layout_v<index_fetch_state>,
              "index_fetch_state must remain standard-layout across IndexFetchTableData callbacks");
static_assert(offsetof(index_fetch_state, fetch_base) == 0,
              "index_fetch_state::fetch_base must remain the first member");

/// @brief Predicate used to classify PostgreSQL planner paths.
export using path_predicate = bool (*)(Path* path);
