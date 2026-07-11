/**
 * @file sequence_index_type.cppm
 * @brief DOC0279 §5 — the SequenceIndexType enum, chunk-size constants, and the
 *        per-(table,index) update policy. Replaces the legacy MetadataMode
 *        (DOC0109) PER_ROW/PER_TABLE model.
 *
 * NO_POSTGRES module: it names no Postgres symbol. The MaxAllocSize / VARHDRSZ
 * bounds are mirrored here as local constants and re-checked against the real
 * macros in the pg-side `index_chunked_storage_pg.cppm` (§5).
 */
module;
#include "common.h"          // DEBUG_ASSERT
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
export module bioseq.indexing.type;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// The three persisted storage layouts. The value is the on-SQL "mode" string
/// that replaces the legacy MetadataMode (DOC0109) PER_ROW/PER_TABLE.
enum class SequenceIndexType : std::uint8_t {
    CHUNKED_PER_FRAGMENT = 1,   ///< one chunked, length-bucketed, per-sequence store; sub_key axis chosen by the index
    INLINE_PER_SEQUENCE  = 2,   ///< exactly one chunk (< MAX_INDEX_CHUNK_SIZE_BYTES) per sequence; length-bucketed
    CHUNKED_PER_TABLE    = 3,   ///< one chunked store for the whole table; shard-only (no length bucketing)
};

[[nodiscard]] constexpr std::string_view sequence_index_type_name(SequenceIndexType t) noexcept {
    switch (t) {
        case SequenceIndexType::CHUNKED_PER_FRAGMENT: return "CHUNKED_PER_FRAGMENT";
        case SequenceIndexType::INLINE_PER_SEQUENCE:  return "INLINE_PER_SEQUENCE";
        case SequenceIndexType::CHUNKED_PER_TABLE:    return "CHUNKED_PER_TABLE";
    }
    return "unknown";
}

/// Per-(table,index) update policy. Replaces MetadataMode.insert_support + the
/// global GUC gate. Fresh tables default cheap insertion indexes to UPDATE_ON_INSERT
/// and keep heavyweight indexes UPDATE_MANUALLY.
enum class IndexUpdatePolicy : std::uint8_t {
    UPDATE_MANUALLY  = 0,   ///< only bioseq.update_index() (re)builds it
    UPDATE_ON_INSERT = 1,   ///< also (re)built per inserted sequence when bioseq.update_index_on_insert = on
};
[[nodiscard]] constexpr std::string_view index_update_policy_name(IndexUpdatePolicy p) noexcept {
    return p == IndexUpdatePolicy::UPDATE_ON_INSERT ? "UPDATE_ON_INSERT" : "UPDATE_MANUALLY";
}

/// Per-call contract for reading a persisted target minimizer index. This is
/// target-independent and travels unchanged from the SQL options decoder to
/// the typed processor; it is not a GUC-derived execution hint.
enum class IndexReadRequirement : std::uint8_t {
    allow = 0,
    require_absent = 1,
    require_compatible_persisted = 2,
};
static_assert(static_cast<std::uint8_t>(IndexReadRequirement::allow) == 0);
static_assert(static_cast<std::uint8_t>(IndexReadRequirement::require_absent) == 1);
static_assert(static_cast<std::uint8_t>(
                  IndexReadRequirement::require_compatible_persisted) == 2);

[[nodiscard]] constexpr std::string_view index_read_requirement_name(
        IndexReadRequirement requirement) noexcept {
    switch (requirement) {
        case IndexReadRequirement::allow: return "allow";
        case IndexReadRequirement::require_absent: return "require_absent";
        case IndexReadRequirement::require_compatible_persisted:
            return "require_compatible_persisted";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool index_read_requirement_ordinal_valid(
        std::int32_t ordinal) noexcept {
    return ordinal >= static_cast<std::int32_t>(IndexReadRequirement::allow)
        && ordinal <= static_cast<std::int32_t>(
            IndexReadRequirement::require_compatible_persisted);
}

/// SQL-facing index names. Keep these in the low-level type module so registry,
/// index implementations, scheduler, and SQL glue all share the same spelling
/// without introducing module cycles.
constexpr std::string_view MINIMIZERS_PER_FRAGMENT_INDEX_NAME = "minimizers_per_fragment";
constexpr std::string_view MINIMIZERS_PER_SEQUENCE_INDEX_NAME = "minimizers_per_sequence";
constexpr std::string_view SYMBOL_COUNTS_INDEX_NAME = "symbol_counts";

/// Supported minimizer index names in persistence preference order. When more
/// than one minimizer index is enabled for UPDATE_ON_INSERT, deferred persistence
/// chooses the first matching entry.
constexpr std::array<std::string_view, 2> MINIMIZER_INDICES = {
    MINIMIZERS_PER_FRAGMENT_INDEX_NAME,
    MINIMIZERS_PER_SEQUENCE_INDEX_NAME,
};
[[nodiscard]] constexpr bool minimizer_index_contains(std::string_view name) noexcept {
    for (std::string_view d : MINIMIZER_INDICES) if (d == name) return true;
    return false;
}

/// The read path defaults to the per-fragment minimizer index. This is separate
/// from write policy: `symbol_counts` is also UPDATE_ON_INSERT by default, but it
/// is not a minimizer lookup structure.
constexpr std::string_view DEFAULT_ACTIVE_READ_INDEX = MINIMIZERS_PER_FRAGMENT_INDEX_NAME;

/// Cheap per-sequence indexes that default to UPDATE_ON_INSERT on a fresh table.
/// `symbol_counts` intentionally keeps the legacy "counter sidecar is populated
/// on insert" behaviour, but with the real tally from §12.5 instead of the old
/// `49` stub.
constexpr std::array<std::string_view, 2> DEFAULT_UPDATE_ON_INSERT_INDEXES = {
    MINIMIZERS_PER_FRAGMENT_INDEX_NAME,
    SYMBOL_COUNTS_INDEX_NAME,
};
[[nodiscard]] constexpr bool default_update_on_insert_contains(std::string_view name) noexcept {
    for (std::string_view d : DEFAULT_UPDATE_ON_INSERT_INDEXES) if (d == name) return true;
    return false;
}

/// Persisted sub-key identity for CHUNKED_PER_FRAGMENT storage. This is signed
/// because the SQL column is BIGINT and the high reserved sentinels use int64_t.
using index_sub_key_t = std::int64_t;
constexpr index_sub_key_t TEMP_RUN_BASE     = index_sub_key_t{1} << 62;
/// Revision-3 generation-head row, directly below the legacy directory sentinel.
constexpr index_sub_key_t GENERATION_HEAD_SUB_KEY =
    std::numeric_limits<index_sub_key_t>::max() - index_sub_key_t{1};
constexpr index_sub_key_t DIRECTORY_SUB_KEY = std::numeric_limits<index_sub_key_t>::max();
static_assert(GENERATION_HEAD_SUB_KEY > TEMP_RUN_BASE);
static_assert(GENERATION_HEAD_SUB_KEY < DIRECTORY_SUB_KEY);
static_assert(DIRECTORY_SUB_KEY > TEMP_RUN_BASE);

/// Largest blob we persist as ONE BYTEA index tuple. Bounded well under Postgres
/// MaxAllocSize (0x3FFFFFFF ~ 1 GiB-1) and the 1 GB BYTEA field cap, with ~64 MiB
/// headroom for the varlena header + heap-tuple overhead. The byte count is a
/// multiple of int64_t; actual pointer alignment is provided by the palloc/int64
/// scratch buffer used by the pg storage implementation.
constexpr std::size_t MAX_INDEX_CHUNK_SIZE_BYTES = (std::size_t{1} << 30) - (std::size_t{64} << 20); // 1 GiB - 64 MiB
static_assert(MAX_INDEX_CHUNK_SIZE_BYTES % sizeof(std::int64_t) == 0,
              "index chunk must be int64-block sized");
// The binding limit is the in-memory varlena `palloc(VARHDRSZ + payload)` (§8.5), not the bare payload.
// This is a NO_POSTGRES module where `VARHDRSZ` is undefined, so assert against a local mirror constant.
constexpr std::size_t kVarlenaHeaderBytes = 4;   // == VARHDRSZ; pinned here for the nopg module. The pg-side
    // `index_chunked_storage_pg.cppm` (where VARHDRSZ is in scope) MUST carry `static_assert(kVarlenaHeaderBytes == VARHDRSZ)`.
constexpr std::size_t kPgMaxAllocSize = 0x3FFFFFFF;  // == Postgres MaxAllocSize; mirrored here (macro undefined in NO_POSTGRES).
static_assert(MAX_INDEX_CHUNK_SIZE_BYTES + kVarlenaHeaderBytes < kPgMaxAllocSize,
              "index chunk + bytea varlena header must stay under Postgres MaxAllocSize");

/// Hard invariant DOC0278 §metadata: a table-wide (CHUNKED_PER_TABLE) index may
/// NEVER be UPDATE_ON_INSERT (one insert would rebuild a possibly-huge index).
[[nodiscard]] constexpr bool index_type_allows_update_on_insert(SequenceIndexType t) noexcept {
    return t != SequenceIndexType::CHUNKED_PER_TABLE;
}

}  // namespace bioseq::indexing
