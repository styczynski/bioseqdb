/**
 * @file index_chunked_storage.cppm
 * @brief DOC0279 §6 — the postgres-agnostic chunked-storage concepts.
 *
 * Expressed with the `is_..._v<T, type>` bool-variable-template-then-concept
 * pattern so different `SequenceIndexType`s require different surfaces. NO_POSTGRES.
 */
module;
#include "common.h"          // DEBUG_ASSERT
#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// Generator polynomial for persisted CRC-64/ECMA-182 checksums.
inline constexpr std::uint64_t kCrc64Ecma182Polynomial = 0x42F0E1EBA9EA3693ULL;

} // namespace bioseq::indexing

namespace bioseq::indexing::crc64_ecma182_detail {

[[nodiscard]] consteval std::array<std::uint64_t, 256> make_table() noexcept {
    std::array<std::uint64_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        std::uint64_t value = static_cast<std::uint64_t>(index) << 56u;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            value = (value & (std::uint64_t{1} << 63u)) != 0
                ? (value << 1u) ^ kCrc64Ecma182Polynomial
                : value << 1u;
        }
        table[index] = value;
    }
    return table;
}

inline constexpr auto table = make_table();

} // namespace bioseq::indexing::crc64_ecma182_detail

export namespace bioseq::indexing {

/// Incremental CRC-64/ECMA-182 state for streaming persisted bytes once.
struct Crc64Ecma182 {
    std::uint64_t value = 0;

    /// Add one byte to the checksum state.
    constexpr void update_byte(std::uint8_t byte) noexcept {
        const std::uint8_t table_index =
            static_cast<std::uint8_t>((value >> 56u) ^ byte);
        value = crc64_ecma182_detail::table[table_index] ^ (value << 8u);
    }

    /// Add an unsigned integer in fixed little-endian byte order.
    template <class UInt>
    requires (std::is_unsigned_v<UInt>)
    constexpr void update_little_endian(UInt input) noexcept {
        for (std::size_t byte = 0; byte < sizeof(UInt); ++byte) {
            update_byte(static_cast<std::uint8_t>(input & UInt{0xff}));
            input >>= 8u;
        }
    }

    /// Add an arbitrary byte span to the checksum state.
    constexpr void update(std::span<const std::byte> bytes) noexcept {
        for (std::byte byte : bytes) {
            update_byte(std::to_integer<std::uint8_t>(byte));
        }
    }

    /// Add the raw bytes of a string view to the checksum state.
    constexpr void update(std::string_view bytes) noexcept {
        for (char byte : bytes) {
            update_byte(static_cast<std::uint8_t>(byte));
        }
    }

    /// Return the current checksum without mutating the state.
    [[nodiscard]] constexpr std::uint64_t checksum() const noexcept {
        return value;
    }
};

/// Compute CRC-64/ECMA-182 for one byte span.
[[nodiscard]] constexpr std::uint64_t crc64_ecma182(
        std::span<const std::byte> bytes) noexcept {
    Crc64Ecma182 crc;
    crc.update(bytes);
    return crc.checksum();
}

/// Compute CRC-64/ECMA-182 for the raw bytes of one string view.
[[nodiscard]] constexpr std::uint64_t crc64_ecma182(
        std::string_view bytes) noexcept {
    Crc64Ecma182 crc;
    crc.update(bytes);
    return crc.checksum();
}

/// Checksum one payload row over its fixed LE address/size prefix and raw blob.
[[nodiscard]] constexpr std::uint64_t crc64_index_chunk_row(
        std::uint64_t generation_id,
        std::int32_t position_segment,
        index_sub_key_t sub_key,
        std::uint32_t chunk_no,
        std::uint32_t chunk_bytes,
        std::span<const std::byte> blob) noexcept {
    Crc64Ecma182 crc;
    crc.update_little_endian(generation_id);
    crc.update_little_endian(std::bit_cast<std::uint32_t>(position_segment));
    crc.update_little_endian(std::bit_cast<std::uint64_t>(sub_key));
    crc.update_little_endian(chunk_no);
    crc.update_little_endian(chunk_bytes);
    crc.update(blob);
    return crc.checksum();
}

static_assert(crc64_ecma182("123456789") == 0x6C40DF5F0B497347ULL,
              "CRC-64/ECMA-182 check value must stay pinned");

/// Pure planner used by the PostgreSQL revision-3 writer before it invokes
/// CatalogTuplesMultiInsertWithInfo. `flush_before` prevents the next tuple
/// from crossing either bounded dimension; `flush_after` prevents a full
/// batch from being retained while the producer continues. One tuple larger
/// than the byte ceiling is rejected rather than silently degrading to an
/// unbounded or one-row-at-a-time path.
struct Revision3TupleBatchDecision {
    bool valid = false;
    bool flush_before = false;
    bool flush_after = false;
};

[[nodiscard]] constexpr Revision3TupleBatchDecision
plan_revision3_tuple_batch_append(
        std::uint64_t pending_rows,
        std::uint64_t pending_bytes,
        std::uint64_t tuple_bytes,
        std::uint64_t maximum_rows,
        std::uint64_t maximum_bytes) noexcept {
    if (tuple_bytes == 0 || maximum_rows == 0 || maximum_bytes == 0
        || tuple_bytes > maximum_bytes
        || pending_rows > maximum_rows || pending_bytes > maximum_bytes
        || (pending_rows == 0) != (pending_bytes == 0)) {
        return {};
    }
    const bool flush_before = pending_rows != 0
        && (pending_rows == maximum_rows
            || tuple_bytes > maximum_bytes - pending_bytes);
    const std::uint64_t base_rows = flush_before ? 0 : pending_rows;
    const std::uint64_t base_bytes = flush_before ? 0 : pending_bytes;
    const std::uint64_t rows_after = base_rows + 1u;
    const std::uint64_t bytes_after = base_bytes + tuple_bytes;
    return Revision3TupleBatchDecision{
        .valid = rows_after <= maximum_rows
            && bytes_after <= maximum_bytes,
        .flush_before = flush_before,
        .flush_after = rows_after == maximum_rows
            || bytes_after == maximum_bytes,
    };
}

/// Publication succeeds only when PostgreSQL RETURNING yields the complete
/// proven fixed wire byte-for-byte. Decoded-field subsets are insufficient:
/// compatibility, manifest and content checksum words are all authoritative.
[[nodiscard]] constexpr bool revision3_fixed_wire_returned_exactly(
        std::span<const std::byte> proven,
        std::span<const std::byte> returned) noexcept {
    return proven.size() == returned.size()
        && std::ranges::equal(proven, returned);
}

/// A chunk writer for `write_to_each<T>` / `write_to<T>`. Invoked once per chunk
/// slot with a `T`-typed, MAXALIGN(>=8)-aligned, zero-initialised scratch buffer of
/// `capacity_units` **T-units** (== `chunk_bytes / sizeof(T)`; the storage guarantees
/// `chunk_bytes % sizeof(T) == 0`). The writer fills `buf[0..n)` and returns the index
/// of the NEXT chunk to visit (`>= chunk_no`; `== chunk_no` => the loop advances by 1).
template <class F, class T>
concept index_chunk_writer_fn = requires (F&& fn, std::size_t chunk_no, T* buf, std::size_t capacity_units) {
    { fn(chunk_no, buf, capacity_units) } -> std::same_as<std::size_t>;
};

/// Whether a qualified read should populate reusable residency or bypass it.
enum class IndexChunkCachePolicy : std::uint8_t {
    populate_residency,
    bypass_one_shot,
};

/// Half-open sub-key range qualified by immutable generation and position segment.
struct IndexSubKeyRange {
    std::uint64_t generation_id = 0;
    std::int32_t position_segment = 0;
    index_sub_key_t begin = 0;
    index_sub_key_t end = 0;
};

/// Exact contiguous chunk extent expected for one qualified sub-key.
struct IndexSubKeyRequest {
    std::uint64_t generation_id = 0;
    std::int32_t position_segment = 0;
    index_sub_key_t sub_key = 0;
    std::uint32_t first_chunk_no = 0;
    std::uint32_t expected_chunk_count = 0;
    std::uint64_t expected_total_bytes = 0;
    std::uint64_t expected_checksum = 0;
};

/// Fully qualified address supplied with each validated chunk callback.
struct IndexChunkAddress {
    std::uint64_t generation_id = 0;
    std::int32_t position_segment = 0;
    index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;

    [[nodiscard]] bool operator==(const IndexChunkAddress&) const = default;
};

/// Per-call proof counters returned by the generalized qualified bulk reader.
struct IndexChunkReadStats {
    std::uint64_t relation_opens = 0;
    std::uint64_t index_scans = 0;
    std::uint64_t requested_ranges = 0;
    std::uint64_t heap_rows_read = 0;
    std::uint64_t toast_values_read = 0;
    std::uint64_t detoast_bytes = 0;
    std::uint64_t explicit_copy_bytes = 0;
    std::uint64_t wanted_subkeys = 0;
    std::uint64_t read_subkeys = 0;
    std::uint64_t wanted_extents = 0;
    std::uint64_t read_extents = 0;
    std::uint64_t wanted_chunks = 0;
    std::uint64_t skipped_rows = 0;
    std::uint64_t skip_budget_rows = 0;
    std::uint64_t repeated_chunk_reads = 0;
    std::uint64_t delivered_chunks = 0;
    std::uint64_t delivered_bytes = 0;
};

/// Callback contract for one transient, validated qualified chunk payload.
template <class F>
concept index_chunk_reader_fn = requires (
        F&& sink,
        const IndexChunkAddress& address,
        const char* payload,
        std::size_t payload_bytes) {
    { sink(address, payload, payload_bytes) } -> std::same_as<void>;
};

struct index_chunk_range_probe {
    void operator()(index_sub_key_t, const char*, std::size_t) const noexcept {}
};

/// Compile-time sink used to verify the qualified callback signature.
struct index_qualified_chunk_range_probe {
    void operator()(const IndexChunkAddress&, const char*, std::size_t) const noexcept {}
};

struct index_sub_key_probe {
    void operator()(index_sub_key_t) const noexcept {}
};

template <class T>
struct index_chunk_writer_probe {
    std::size_t operator()(std::size_t, T*, std::size_t) const noexcept {
        return std::size_t{0};
    }
};

struct index_merge_batch_probe {
    void operator()(std::span<const std::uint64_t>) const noexcept {}
};

struct index_merge_runs_probe {
    template <class Positions>
    void operator()(std::uint64_t, Positions&& positions) const {
        (void)positions.total_count();
        positions.for_each_batch(index_merge_batch_probe{});
    }
};

struct index_sequence_loader_probe {
    template <class LazySequence>
    void operator()(LazySequence&, std::size_t, std::size_t) const noexcept {}
};

// primary: nothing satisfies it until specialised per SequenceIndexType
template <class T, SequenceIndexType index_type>
constexpr inline bool is_index_chunked_storage_for_v = false;

// ---- CHUNKED_PER_FRAGMENT (per-sequence, 2-D sub_key x chunk) ----
template <class T>
constexpr inline bool is_index_chunked_storage_for_v<T, SequenceIndexType::CHUNKED_PER_FRAGMENT> =
    requires (T& s, const T& cs, index_sub_key_t sub_key, std::size_t chunk_no, std::size_t bytes) {
        { cs.number_of_sub_keys()                 } -> std::convertible_to<std::size_t>;
        { cs.number_of_chunks(sub_key)            } -> std::convertible_to<std::size_t>;
        { cs.chunk_byte_size(sub_key, chunk_no)   } -> std::convertible_to<std::size_t>;
        { s.template read_chunk<std::int64_t>(sub_key, chunk_no) } -> std::same_as<const std::int64_t*>;
        { s.template read_chunk<char>(sub_key, chunk_no)         } -> std::same_as<const char*>;
        { s.resize(sub_key, bytes)                } -> std::same_as<void>;
        { s.resize_chunks(sub_key, std::span<const std::size_t>{}) } -> std::same_as<void>;
        { s.prefetch_chunk(sub_key, chunk_no)     } -> std::same_as<void>;
        // The legacy overload remains the storage-concept requirement until the
        // PostgreSQL implementation and callers cut over atomically. The POC fake
        // additionally exposes the qualified overload without introducing
        // a competing selected-reader concept.
        { s.read_chunks_in_range(sub_key, sub_key, index_chunk_range_probe{}) } -> std::same_as<void>;
        { cs.for_each_sub_key(index_sub_key_probe{}) } -> std::same_as<void>;
        { s.template write_to_each<std::int64_t>(
              sub_key, index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.template write_to_each<char>(
              sub_key, index_chunk_writer_probe<char>{}) }
            -> std::same_as<void>;
        { s.clear_entity()                        } -> std::same_as<void>;
        { s.clear(std::string_view{})             } -> std::same_as<void>;
        // EXTERNAL-SORT SPILL (the §10.4 minimizers_per_sequence build).
        { s.spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned(std::vector<std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned_admitted(
              std::vector<std::pair<std::uint64_t,std::uint64_t>>{},
              std::uint64_t{},
              bioseq::parallelism::MemoryPermit{}) } -> std::convertible_to<std::size_t>;
        { s.merge_runs(index_merge_runs_probe{}) } -> std::same_as<void>;
        { s.release_resident_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs()                        } -> std::same_as<void>;
        { s.run_buffer_bytes()                    } -> std::convertible_to<std::size_t>;
    };

// ---- INLINE_PER_SEQUENCE (exactly one chunk) ----
template <class T>
constexpr inline bool is_index_chunked_storage_for_v<T, SequenceIndexType::INLINE_PER_SEQUENCE> =
    requires (T& s, const T& cs, std::size_t bytes) {
        { cs.number_of_chunks()                   } -> std::convertible_to<std::size_t>;   // must == 1 (runtime-asserted)
        { cs.chunk_byte_size()                    } -> std::convertible_to<std::size_t>;
        { s.template read_chunk<std::int64_t>()   } -> std::same_as<const std::int64_t*>;
        { s.resize(bytes)                         } -> std::same_as<void>;
        { s.template write_to_each<std::int64_t>(
              index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.prefetch_chunk()                      } -> std::same_as<void>;
        { s.clear_entity()                        } -> std::same_as<void>;
        { s.clear(std::string_view{})             } -> std::same_as<void>;
    };

// ---- CHUNKED_PER_TABLE (flat chunk space + write_to(chunk_no) + for_each_sequence) ----
template <class T>
constexpr inline bool is_index_chunked_storage_for_v<T, SequenceIndexType::CHUNKED_PER_TABLE> =
    requires (T& s, const T& cs, std::size_t chunk_no, std::size_t bytes) {
        { cs.number_of_chunks()                   } -> std::convertible_to<std::size_t>;
        { cs.chunk_byte_size(chunk_no)            } -> std::convertible_to<std::size_t>;
        { s.template read_chunk<std::int64_t>(chunk_no) } -> std::same_as<const std::int64_t*>;
        { s.resize(bytes)                         } -> std::same_as<void>;
        { s.prefetch_chunk(chunk_no)              } -> std::same_as<void>;
        { s.template write_to_each<std::int64_t>(
              index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.template write_to<std::int64_t>(
              chunk_no, index_chunk_writer_probe<std::int64_t>{}) }
            -> std::same_as<void>;
        { s.for_each_sequence(index_sequence_loader_probe{}) }
            -> std::same_as<void>;
        { s.clear(std::string_view{})             } -> std::same_as<void>;
    };

template <class T, SequenceIndexType index_type>
concept is_index_chunked_storage_for = is_index_chunked_storage_for_v<T, index_type>;

template <class T>
concept is_index_chunked_storage =
       is_index_chunked_storage_for<T, SequenceIndexType::CHUNKED_PER_FRAGMENT>
    || is_index_chunked_storage_for<T, SequenceIndexType::INLINE_PER_SEQUENCE>
    || is_index_chunked_storage_for<T, SequenceIndexType::CHUNKED_PER_TABLE>;

/// The external-sort spill sub-concept (§6.3). Only minimizers_per_sequence
/// requires it. `IndexStorageFragmentPg` satisfies both this and the per-fragment
/// concept; it is kept as a named sub-concept for documentation + the index's own
/// `requires` clause.
template <class T>
concept is_external_sort_spill_storage =
    requires (T& s) {
        { s.spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned(std::vector<std::pair<std::uint64_t,std::uint64_t>>{}) } -> std::convertible_to<std::size_t>;
        { s.spill_sorted_run_owned_admitted(
              std::vector<std::pair<std::uint64_t,std::uint64_t>>{},
              std::uint64_t{},
              bioseq::parallelism::MemoryPermit{}) } -> std::convertible_to<std::size_t>;
        { s.merge_runs(index_merge_runs_probe{}) } -> std::same_as<void>;
        { s.release_resident_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs_noexcept() } noexcept -> std::same_as<void>;
        { s.release_runs()     } -> std::same_as<void>;
        { s.run_buffer_bytes() } -> std::convertible_to<std::size_t>;
    };

}  // namespace bioseq::indexing
