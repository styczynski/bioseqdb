/**
 * @file index_chunked_storage.cppm
 * @brief DOC0279 §6 — the postgres-agnostic chunked-storage concepts.
 *
 * Expressed with the `is_..._v<T, type>` bool-variable-template-then-concept
 * pattern so different `SequenceIndexType`s require different surfaces. NO_POSTGRES.
 */
module;
#include "common.h"          // DEBUG_ASSERT
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
export module bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// A chunk writer for `write_to_each<T>` / `write_to<T>`. Invoked once per chunk
/// slot with a `T`-typed, MAXALIGN(>=8)-aligned, zero-initialised scratch buffer of
/// `capacity_units` **T-units** (== `chunk_bytes / sizeof(T)`; the storage guarantees
/// `chunk_bytes % sizeof(T) == 0`). The writer fills `buf[0..n)` and returns the index
/// of the NEXT chunk to visit (`>= chunk_no`; `== chunk_no` => the loop advances by 1).
template <class F, class T>
concept index_chunk_writer_fn = requires (F&& fn, std::size_t chunk_no, T* buf, std::size_t capacity_units) {
    { fn(chunk_no, buf, capacity_units) } -> std::same_as<std::size_t>;
};

struct index_chunk_range_probe {
    void operator()(index_sub_key_t, const char*, std::size_t) const noexcept {}
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
