/**
 * @file type.cppm
 * @brief Persisted index layouts, sub-key namespaces, and update policy.
 *
 * This module has no PostgreSQL dependency. Allocation and varlena limits are
 * mirrored here and checked again by the PostgreSQL storage boundary.
 */
module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
export module bioseq.indexing.type;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// The three persisted storage layouts. Numeric values remain stable in SQL.
enum class sequence_index_layout : std::uint8_t {
    fragment_chunks = 1,
    sequence_inline = 2,
    table_chunks = 3,
};

/// Return the persisted SQL spelling for an index layout.
[[nodiscard]] constexpr std::string_view sequence_index_layout_name(
        sequence_index_layout layout) noexcept {
    switch (layout) {
        case sequence_index_layout::fragment_chunks: return "CHUNKED_PER_FRAGMENT";
        case sequence_index_layout::sequence_inline:  return "INLINE_PER_SEQUENCE";
        case sequence_index_layout::table_chunks:    return "CHUNKED_PER_TABLE";
    }
    return "unknown";
}

/// Per-table, per-index update policy.
enum class index_update_policy : std::uint8_t {
    manual = 0,
    update_on_insert = 1,
};
[[nodiscard]] constexpr std::string_view index_update_policy_name(
        index_update_policy policy) noexcept {
    return policy == index_update_policy::update_on_insert
        ? "UPDATE_ON_INSERT" : "UPDATE_MANUALLY";
}

/// Implementation used to sort one in-memory minimizer-pair run.
///
/// The explicit ordinals cross the PostgreSQL background-worker metadata
/// boundary. Keep them stable and keep this policy in the PostgreSQL-free
/// indexing layer so core sort code never imports generated GUC storage.
enum class pair_run_sort_implementation : std::int32_t {
    std_sort = 0,
    simd_u128_vqsort = 1,
    simd_u128_vec_stream_1 = 2,
    simd_u128_vec_stream_2 = 3,
    simd_u128_vec_stream_4 = 4,
};
static_assert(static_cast<std::int32_t>(
                  pair_run_sort_implementation::std_sort) == 0);
static_assert(static_cast<std::int32_t>(
                  pair_run_sort_implementation::simd_u128_vqsort) == 1);
static_assert(static_cast<std::int32_t>(
                  pair_run_sort_implementation::simd_u128_vec_stream_1) == 2);
static_assert(static_cast<std::int32_t>(
                  pair_run_sort_implementation::simd_u128_vec_stream_2) == 3);
static_assert(static_cast<std::int32_t>(
                  pair_run_sort_implementation::simd_u128_vec_stream_4) == 4);

[[nodiscard]] constexpr bool pair_run_sort_implementation_ordinal_valid(
        std::int32_t ordinal) noexcept {
    return ordinal >= static_cast<std::int32_t>(
               pair_run_sort_implementation::std_sort)
        && ordinal <= static_cast<std::int32_t>(
               pair_run_sort_implementation::simd_u128_vec_stream_4);
}

/// Per-call contract for reading a persisted target minimizer index. This is
/// target-independent and travels unchanged from the SQL options decoder to
/// the typed processor; it is not a GUC-derived execution hint.
enum class index_read_requirement : std::uint8_t {
    allow = 0,
    require_absent = 1,
    require_compatible_persisted = 2,
};
static_assert(static_cast<std::uint8_t>(index_read_requirement::allow) == 0);
static_assert(static_cast<std::uint8_t>(index_read_requirement::require_absent) == 1);
static_assert(static_cast<std::uint8_t>(
                  index_read_requirement::require_compatible_persisted) == 2);

[[nodiscard]] constexpr std::string_view index_read_requirement_name(
        index_read_requirement requirement) noexcept {
    switch (requirement) {
        case index_read_requirement::allow: return "allow";
        case index_read_requirement::require_absent: return "require_absent";
        case index_read_requirement::require_compatible_persisted:
            return "require_compatible_persisted";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool index_read_requirement_ordinal_valid(
        std::int32_t ordinal) noexcept {
    return ordinal >= static_cast<std::int32_t>(index_read_requirement::allow)
        && ordinal <= static_cast<std::int32_t>(
            index_read_requirement::require_compatible_persisted);
}

/// SQL-facing index names. Keep these in the low-level type module so registry,
/// index implementations, scheduler, and SQL glue all share the same spelling
/// without introducing module cycles.
constexpr std::string_view k_minimizers_per_fragment_index_name = "minimizers_per_fragment";
constexpr std::string_view k_minimizers_per_sequence_index_name = "minimizers_per_sequence";
constexpr std::string_view k_symbol_counts_index_name = "symbol_counts";

/// Supported minimizer index names in persistence preference order. When more
/// than one minimizer index is enabled for insert updates, deferred persistence
/// chooses the first matching entry.
constexpr std::array<std::string_view, 2> k_minimizer_index_names = {
    k_minimizers_per_fragment_index_name,
    k_minimizers_per_sequence_index_name,
};
[[nodiscard]] constexpr bool is_minimizer_index_name(
        std::string_view name) noexcept {
    for (std::string_view d : k_minimizer_index_names) if (d == name) return true;
    return false;
}

/// The read path defaults to the per-fragment minimizer index. This is separate
/// from write policy: `symbol_counts` is also updated on insert by default, but it
/// is not a minimizer lookup structure.
constexpr std::string_view k_default_active_read_index = k_minimizers_per_fragment_index_name;

/// Cheap per-sequence indexes that default to insert-time updates.
constexpr std::array<std::string_view, 2> k_default_update_on_insert_indexes = {
    k_minimizers_per_fragment_index_name,
    k_symbol_counts_index_name,
};
[[nodiscard]] constexpr bool is_default_update_on_insert_index(
        std::string_view name) noexcept {
    for (std::string_view d : k_default_update_on_insert_indexes) if (d == name) return true;
    return false;
}

/// Persisted sub-key identity for fragment-chunk storage. This is signed
/// because the SQL column is BIGINT and the high reserved sentinels use int64_t.
using index_sub_key_t = std::int64_t;
constexpr index_sub_key_t k_temporary_run_sub_key_base     = index_sub_key_t{1} << 62;
/// Positions occupy a disjoint physical sub-key namespace. The
/// directory stores the logical/control key below this base; the position row
/// key is derived as k_position_extent_sub_key_base + logical_key.
constexpr index_sub_key_t k_position_extent_sub_key_base = index_sub_key_t{1} << 61;
/// Hash-major generation head sub-key.
constexpr index_sub_key_t k_hash_major_generation_head_sub_key =
    std::numeric_limits<index_sub_key_t>::max() - index_sub_key_t{3};
/// Optional authenticated hash-major dense-heap seal sub-key.
constexpr index_sub_key_t k_hash_major_dense_seal_sub_key =
    std::numeric_limits<index_sub_key_t>::max() - index_sub_key_t{2};
/// Grouped-SoA generation-head sub-key.
constexpr index_sub_key_t k_generation_head_sub_key =
    std::numeric_limits<index_sub_key_t>::max() - index_sub_key_t{1};
constexpr index_sub_key_t k_directory_sub_key = std::numeric_limits<index_sub_key_t>::max();
static_assert(k_position_extent_sub_key_base > 0);
static_assert(k_position_extent_sub_key_base + k_position_extent_sub_key_base == k_temporary_run_sub_key_base);
static_assert(k_hash_major_generation_head_sub_key > k_temporary_run_sub_key_base);
static_assert(k_hash_major_generation_head_sub_key
    < k_hash_major_dense_seal_sub_key);
static_assert(k_hash_major_dense_seal_sub_key < k_generation_head_sub_key);
static_assert(k_generation_head_sub_key > k_temporary_run_sub_key_base);
static_assert(k_generation_head_sub_key < k_directory_sub_key);
static_assert(k_directory_sub_key > k_temporary_run_sub_key_base);

/// Largest blob we persist as ONE BYTEA index tuple. Bounded well under Postgres
/// MaxAllocSize (0x3FFFFFFF ~ 1 GiB-1) and the 1 GB BYTEA field cap, with ~64 MiB
/// headroom for the varlena header + heap-tuple overhead. The byte count is a
/// multiple of int64_t; actual pointer alignment is provided by the palloc/int64
/// scratch buffer used by the pg storage implementation.
constexpr std::size_t k_max_index_chunk_size_bytes = (std::size_t{1} << 30) - (std::size_t{64} << 20); // 1 GiB - 64 MiB
static_assert(k_max_index_chunk_size_bytes % sizeof(std::int64_t) == 0,
              "index chunk must be int64-block sized");
// The binding limit is the in-memory varlena `palloc(VARHDRSZ + payload)` (§8.5), not the bare payload.
// This is a NO_POSTGRES module where `VARHDRSZ` is undefined, so assert against a local mirror constant.
constexpr std::size_t k_varlena_header_bytes = 4;   // == VARHDRSZ; pinned here for the nopg module. The pg-side
    // `index_chunked_storage_pg.cppm` (where VARHDRSZ is in scope) MUST carry `static_assert(k_varlena_header_bytes == VARHDRSZ)`.
constexpr std::size_t k_postgres_max_allocation_bytes = 0x3FFFFFFF;  // == Postgres MaxAllocSize; mirrored here (macro undefined in NO_POSTGRES).
static_assert(k_max_index_chunk_size_bytes + k_varlena_header_bytes < k_postgres_max_allocation_bytes,
              "index chunk + bytea varlena header must stay under Postgres MaxAllocSize");

/// Return whether an index layout admits insert-time maintenance.
[[nodiscard]] constexpr bool index_layout_allows_update_on_insert(
        sequence_index_layout layout) noexcept {
    return layout != sequence_index_layout::table_chunks;
}

}  // namespace bioseq::indexing
