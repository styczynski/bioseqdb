/**
 * @file types.cppm
 * State shared by Bioseq table scans and index row lookups.
 *
 * Each descriptor begins with the PostgreSQL state passed to callbacks, so
 * both pointers have the same, address. Layout checks below enforc this.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.postgres.table.types;

export import bioseq.postgres.table.scan_state;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;

/// @brief Private CustomScan marker for vectorized aggregate execution.
export inline constexpr int k_vectorized_aggregate_private_marker = 1;

/**
 * @brief State returned when PostgreSQL starts a table scan.
 * The column reader is created on the first row request. The descriptor keeps
 * its column list and filters in the scan memory context until then.
 */
export struct column_scan_descriptor {
    /// @brief PostgreSQL table-scan base that must remain the first member.
    TableScanDescData scan_base;
    /// @brief Column reader created on the first row request.
    column_read_state* read_state;
    /// @brief Memory  context for allocations kept until the scan ends.
    MemoryContext scan_context;
    /// @brief Zero-based indexes of columns the scan must read.
    bioseq::postgres::support::postgres_bitmapset attributes_needed;
    /// @brief Filters passed to the column reader when starting or rescanning.
    bioseq::postgres::support::postgres_list<Node> scan_qualifiers;
    /// @brief Shared stripe position used by parallel scan workers.
    parallel_column_scan_handle parallel_scan;
    /// @brief Whether the caller expects vectorized output slots.
    bool returns_vectorized_tuple;
    /// @brief Keep sequence locators for the caller to load when needed.
    bool defers_sequence_hydration;
    /// @brief Sequence-read caches allocated for this scan.
    void* sequence_cache;
};

static_assert(std::is_standard_layout_v<column_scan_descriptor>,
              "column_scan_descriptor must remain standard-layout across TableScanDesc callbacks");
static_assert(offsetof(column_scan_descriptor, scan_base) == 0,
              "column_scan_descriptor::scan_base must remain the first member");

/// @brief Pointer to a Bioseq table-scan descriptor.
export using column_scan_handle = column_scan_descriptor*;

// PostgreSQL 20 passes index work through IndexScanDesc. Keep the relation
// and flags needed by the Bioseq reader here. PostgreSQL 17-19 use their
// IndexFetchTableData base directly.
#if PG_VERSION_NUM >= 200000
export struct column_index_fetch_base {
    Relation rel;
    std::uint32_t flags;
};
#else
export using column_index_fetch_base = IndexFetchTableData;
#endif

/**
 * @brief State kept between PostgreSQL index row lookups.
 * The begin callback allocates sequence caches. The column reader  is created
 * when the first row is fetched.
 */
export struct index_fetch_state {
    /// @brief Index-fetch base that must remain the first member.
    column_index_fetch_base fetch_base;
    /// @brief Column reader created on the first row lookup
    column_read_state* read_state;
    /// @brief Zero-based indexes of columns the lookup must read.
    bioseq::postgres::support::postgres_bitmapset attributes_needed;
    /// @brief Stripe metadata ordered for row-number lookup.
    bioseq::postgres::support::postgres_list<stripe_metadata> stripe_metadata;
    /// @brief Whether the fetch belongs to a SELECT execution path.
    bool is_select_query;
    /// @brief Memory context for allocations kept until index fetching ends.
    MemoryContext scan_context;
    /// @brief Keep sequence locators for the caller to loar when needed.
    bool defers_sequence_hydration;
    /// @brief Sequence-read caches allocated for these index lookups.
    void* sequence_cache;
#if PG_VERSION_NUM >= 200000
    /// @brief Table-row slot used to check visibility for index-only scans.
    TupleTableSlot* visibility_slot;
#endif
};

static_assert(std::is_standard_layout_v<index_fetch_state>,
              "index_fetch_state must remain standard-layout across index-fetch callbacks");
static_assert(offsetof(index_fetch_state, fetch_base) == 0,
              "index_fetch_state::fetch_base must remain the first member");

/// @brief Predicate used to classify PostgreSQL planner paths.
export using path_predicate = bool (*)(Path* path);
