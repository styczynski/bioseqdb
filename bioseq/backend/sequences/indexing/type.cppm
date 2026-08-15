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

/** @brief Identifies the persisted storage layout of a sequence index. */
enum class sequence_index_layout : std::uint8_t {
    /// @brief Stores multiple chunks under each sequence-local sub-key.
    fragment_chunks = 1,
    /// @brief Stores one inline chunk for each sequence.
    sequence_inline = 2,
    /// @brief Stores chunks shared across the whole table.
    table_chunks = 3,
};

/**
 * @brief Returns the persisted SQL spelling of an index layout.
 * @param layout Layout to name
 * @return Stable SQL spelling of the layout
 */
[[nodiscard]] constexpr std::string_view sequence_index_layout_name(
        sequence_index_layout layout) noexcept {
    switch (layout) {
        case sequence_index_layout::fragment_chunks: return "CHUNKED_PER_FRAGMENT";
        case sequence_index_layout::sequence_inline:  return "INLINE_PER_SEQUENCE";
        case sequence_index_layout::table_chunks:    return "CHUNKED_PER_TABLE";
    }
    return "unknown";
}

/** @brief Selects manual or insert-time index maintenance. */
enum class index_update_policy : std::uint8_t {
    /// @brief Maintains the index only when explicitly requested.
    manual = 0,
    /// @brief Maintains the index during sequence insertion.
    update_on_insert = 1,
};
/**
 * @brief Returns the persisted SQL spelling of an update policy.
 * @param policy Policy to name
 * @return Stable SQL spelling of the policy
 */
[[nodiscard]] constexpr std::string_view index_update_policy_name(
        index_update_policy policy) noexcept {
    return policy == index_update_policy::update_on_insert
        ? "UPDATE_ON_INSERT" : "UPDATE_MANUALLY";
}

/**
 * @brief Selects the implementation used to sort one minimizer-pair run.
 *
 * The explicit ordinals cross the PostgreSQL background-worker metadata
 * boundary. Keep them stable and keep this policy in the PostgreSQL-free
 * indexing layer so core sort code never imports generated GUC storage.
 */
enum class pair_run_sort_implementation : std::int32_t {
    /// @brief Uses the standard-library comparison sorter.
    std_sort = 0,
    /// @brief Uses Highway VQSort for packed 128-bit keys.
    simd_u128_vqsort = 1,
    /// @brief Uses one SIMD vector stream.
    simd_u128_vec_stream_1 = 2,
    /// @brief Uses two SIMD vector streams.
    simd_u128_vec_stream_2 = 3,
    /// @brief Uses four SIMD vector streams.
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

/**
 * @brief Checks whether an integer names a supported pair-run sorter.
 * @param ordinal Integer received across the configuration boundary
 * @return `true` when the ordinal maps to a sorter
 */
[[nodiscard]] constexpr bool pair_run_sort_implementation_ordinal_valid(
        std::int32_t ordinal) noexcept {
    return ordinal >= static_cast<std::int32_t>(
               pair_run_sort_implementation::std_sort)
        && ordinal <= static_cast<std::int32_t>(
               pair_run_sort_implementation::simd_u128_vec_stream_4);
}

/**
 * @brief Selects the persisted-index requirement for one target read.
 *
 * This is target-independent and travels unchanged from the SQL options
 * decoder to the typed processor. It is not a GUC-derived execution hint.
 */
enum class index_read_requirement : std::uint8_t {
    /// @brief Allows either a compatible persisted index or fallback routing.
    allow = 0,
    /// @brief Requires the persisted index to be absent.
    require_absent = 1,
    /// @brief Requires a compatible persisted index.
    require_compatible_persisted = 2,
};
static_assert(static_cast<std::uint8_t>(index_read_requirement::allow) == 0);
static_assert(static_cast<std::uint8_t>(index_read_requirement::require_absent) == 1);
static_assert(static_cast<std::uint8_t>(
                  index_read_requirement::require_compatible_persisted) == 2);

/**
 * @brief Returns the option spelling of a persisted-index requirement.
 * @param requirement Requirement to name
 * @return Stable option spelling of the requirement
 */
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

/**
 * @brief Checks whether an integer names a persisted-index requirement.
 * @param ordinal Integer received across the options boundary
 * @return `true` when the ordinal maps to a requirement
 */
[[nodiscard]] constexpr bool index_read_requirement_ordinal_valid(
        std::int32_t ordinal) noexcept {
    return ordinal >= static_cast<std::int32_t>(index_read_requirement::allow)
        && ordinal <= static_cast<std::int32_t>(
            index_read_requirement::require_compatible_persisted);
}

/// @brief Persisted name of the per-fragment minimizer index.
constexpr std::string_view k_minimizers_per_fragment_index_name = "minimizers_per_fragment";
/// @brief Persisted name of the per-sequence minimizer index.
constexpr std::string_view k_minimizers_per_sequence_index_name = "minimizers_per_sequence";
/// @brief Persisted name of the symbol-count index.
constexpr std::string_view k_symbol_counts_index_name = "symbol_counts";

/** @brief Lists minimizer indexes in persistence preference order. */
constexpr std::array<std::string_view, 2> k_minimizer_index_names = {
    k_minimizers_per_fragment_index_name,
    k_minimizers_per_sequence_index_name,
};
/**
 * @brief Checks whether a name identifies a registered minimizer index.
 * @param name Index name to inspect
 * @return `true` for a supported minimizer index name
 */
[[nodiscard]] constexpr bool is_minimizer_index_name(
        std::string_view name) noexcept {
    for (std::string_view candidate : k_minimizer_index_names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

/// @brief Names the default minimizer index used by persisted reads.
constexpr std::string_view k_default_active_read_index = k_minimizers_per_fragment_index_name;

/// @brief Lists indexes that default to insert-time maintenance.
constexpr std::array<std::string_view, 2> k_default_update_on_insert_indexes = {
    k_minimizers_per_fragment_index_name,
    k_symbol_counts_index_name,
};
/**
 * @brief Checks whether an index defaults to insert-time maintenance.
 * @param name Index name to inspect
 * @return `true` when fresh tables update the index on insert
 */
[[nodiscard]] constexpr bool is_default_update_on_insert_index(
        std::string_view name) noexcept {
    for (std::string_view candidate : k_default_update_on_insert_indexes) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Identifies a persisted fragment-chunk row within one index.
 *
 * The signed representation matches the SQL BIGINT column and reserves the
 * highest values for metadata rows.
 */
using index_sub_key_t = std::int64_t;
/// @brief Starts the reserved namespace for temporary external-sort runs.
constexpr index_sub_key_t k_temporary_run_sub_key_base = index_sub_key_t{1} << 62;
/// @brief Starts the reserved namespace for position-extent rows.
constexpr index_sub_key_t k_position_extent_sub_key_base = index_sub_key_t{1} << 61;
/// @brief Identifies the hash-major generation-head row.
constexpr index_sub_key_t k_hash_major_generation_head_sub_key =
    std::numeric_limits<index_sub_key_t>::max() - index_sub_key_t{3};
/// @brief Identifies the optional authenticated hash-major dense-heap seal.
constexpr index_sub_key_t k_hash_major_dense_seal_sub_key =
    std::numeric_limits<index_sub_key_t>::max() - index_sub_key_t{2};
/// @brief Identifies the grouped-SoA generation-head row.
constexpr index_sub_key_t k_generation_head_sub_key =
    std::numeric_limits<index_sub_key_t>::max() - index_sub_key_t{1};
/// @brief Identifies the persisted directory or completeness manifest row.
constexpr index_sub_key_t k_directory_sub_key = std::numeric_limits<index_sub_key_t>::max();
static_assert(k_position_extent_sub_key_base > 0);
static_assert(k_position_extent_sub_key_base + k_position_extent_sub_key_base ==
              k_temporary_run_sub_key_base);
static_assert(k_hash_major_generation_head_sub_key > k_temporary_run_sub_key_base);
static_assert(k_hash_major_generation_head_sub_key
    < k_hash_major_dense_seal_sub_key);
static_assert(k_hash_major_dense_seal_sub_key < k_generation_head_sub_key);
static_assert(k_generation_head_sub_key > k_temporary_run_sub_key_base);
static_assert(k_generation_head_sub_key < k_directory_sub_key);
static_assert(k_directory_sub_key > k_temporary_run_sub_key_base);

/**
 * @brief Largest payload persisted in one index tuple.
 *
 * The limit leaves space for the varlena header and heap-tuple overhead below
 * PostgreSQL's allocation and BYTEA limits.
 */
constexpr std::size_t k_max_index_chunk_size_bytes =
    (std::size_t{1} << 30) - (std::size_t{64} << 20);
static_assert(k_max_index_chunk_size_bytes % sizeof(std::int64_t) == 0,
              "index chunk must be int64-block sized");
// The binding limit includes the varlena header. This PostgreSQL-free module
// uses a local mirror because `VARHDRSZ` is unavailable here. The PostgreSQL
// storage boundary separately asserts that both widths match.
/// @brief Mirrors the PostgreSQL varlena header width for allocation checks.
constexpr std::size_t k_varlena_header_bytes = 4;
/// @brief Mirrors PostgreSQL's maximum allocation size.
constexpr std::size_t k_postgres_max_allocation_bytes = 0x3FFFFFFF;
static_assert(k_max_index_chunk_size_bytes + k_varlena_header_bytes <
                  k_postgres_max_allocation_bytes,
              "index chunk + bytea varlena header must stay under Postgres MaxAllocSize");

/**
 * @brief Checks whether a layout admits insert-time maintenance.
 * @param layout Persisted index layout
 * @return `true` for per-sequence layouts
 */
[[nodiscard]] constexpr bool index_layout_allows_update_on_insert(
        sequence_index_layout layout) noexcept {
    return layout != sequence_index_layout::table_chunks;
}

} // namespace bioseq::indexing
