/**
 * @file minimizer_hashmap_index.cppm
 * @brief DOC0279 §9 — the relocated `TargetIndex` (now
 *        `MinimizerHashmapFragmentIndex`, storage-free, the single pipeline
 *        IndexT) + the storage-bound `MinimizersPerFragmentIndex<S>` builder.
 *
 * NO_POSTGRES. Corruption guards call `index_corruption_panic` (bioseq_log<DBG>+abort),
 * never elog/bioseq_log_pg(no-ops here). Storage-side validation goes through
 * `storage.validate_minimizer_*`/`storage.index_error` (duck-typed on S).
 */
module;
#include "common.h"                       // DEBUG_ASSERT, bioseq_log<DBG>*
#include <hwy/highway.h>                   // ONLY for the HWY_NAMESPACE macro
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <utility>
export module bioseq.indexing.minimizer_hashmap;
import bioseq.common.mlogging;                                     // bioseq_log_progress<DBG>(enable_logging)
import bioseq.common.sequan_compat;                                // nucleotide alphabet dispatch for the non-HWY ExactSimd bridge contract
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.sequences;                             // sequence_custom_alphabet, compact_sequence_range for bounded Paper windows
import bioseq.sequences.kmers;                       // collect_minimizers_view, sketch_collector, MinimapSketchData, MinimizerSketchBackend
import bioseq.sequences.kmers.vulkan;                // DOC0345 Patch F — collect_sorted_minimizer_pairs_vulkan_segment_or_throw
import bioseq.sequences.kmers.hashers.minimap_int;   // minimap_int_hasher
import bioseq.storage.lazy;                          // lazy_compact_sequence, make_lazy_in_memory
import bioseq.storage.size;                          // sequence_size_t
import bioseq.common.radix_sort;                  // bioseq::sorting::radix_sort
import bioseq.instr;                                 // instrumentation region scoping
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

namespace bioseq::indexing::detail {
struct MinimizerHashmapFragmentIndexTable;
}

export namespace bioseq::indexing {
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
using bioseq::simd::HWY_NAMESPACE::minimap_int_hasher;
using bioseq::simd::HWY_NAMESPACE::collect_minimizers_view;
using bioseq::simd::HWY_NAMESPACE::sketch_collector;
using bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;   // DOC0345 Patch F
using bioseq::storage::sequence_size_t;
using bioseq::storage::lazy_compact_sequence;

// Non-Highway ABI between indexing interfaces and the existing hashmap
// implementation unit. No indexing module imports two_stacks: the .cpp owns
// that import and maps its checked status/proof onto these plain ABI records.
enum class ExactSimdIndexStatus : std::uint8_t {
    ok = 0,
    invalid_window_size,
    invalid_kmer_size,
    invalid_static_configuration,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    not_initialized,
    already_finalized,
    invalid_state,
    invalid_range,
    non_contiguous_range,
    arithmetic_overflow,
    collector_overflow,
};

[[nodiscard]] constexpr std::string_view exact_simd_index_status_name(
        ExactSimdIndexStatus status) noexcept {
    switch (status) {
        case ExactSimdIndexStatus::ok: return "ok";
        case ExactSimdIndexStatus::invalid_window_size:
            return "invalid_window_size";
        case ExactSimdIndexStatus::invalid_kmer_size:
            return "invalid_kmer_size";
        case ExactSimdIndexStatus::invalid_static_configuration:
            return "invalid_static_configuration";
        case ExactSimdIndexStatus::unsupported_alphabet:
            return "unsupported_alphabet";
        case ExactSimdIndexStatus::unsupported_storage:
            return "unsupported_storage";
        case ExactSimdIndexStatus::unsupported_representation:
            return "unsupported_representation";
        case ExactSimdIndexStatus::unsupported_vector_shape:
            return "unsupported_vector_shape";
        case ExactSimdIndexStatus::not_initialized:
            return "not_initialized";
        case ExactSimdIndexStatus::already_finalized:
            return "already_finalized";
        case ExactSimdIndexStatus::invalid_state:
            return "invalid_state";
        case ExactSimdIndexStatus::invalid_range:
            return "invalid_range";
        case ExactSimdIndexStatus::non_contiguous_range:
            return "non_contiguous_range";
        case ExactSimdIndexStatus::arithmetic_overflow:
            return "arithmetic_overflow";
        case ExactSimdIndexStatus::collector_overflow:
            return "collector_overflow";
    }
    return "unknown";
}

class ExactSimdIndexRuntimeError final : public std::runtime_error {
public:
    ExactSimdIndexRuntimeError(
            ExactSimdIndexStatus status,
            std::string_view operation)
        : std::runtime_error(
              std::string{operation}
              + ": ExactSimd minimizer execution rejected: "
              + std::string{exact_simd_index_status_name(status)}),
          status_(status) {}

    [[nodiscard]] ExactSimdIndexStatus status() const noexcept {
        return status_;
    }

private:
    ExactSimdIndexStatus status_;
};

struct ExactSimdIndexPackedFragment {
    const void* data = nullptr;
    std::size_t storage_units = 0;
};

enum class ExactSimdIndexAlphabet : std::uint8_t {
    dna4 = 0,
    dna5 = 1,
};

using ExactSimdIndexReadFragmentFn =
    ExactSimdIndexPackedFragment (*)(void*, std::size_t);
using ExactSimdIndexReleaseFragmentFn = void (*)(void*, std::size_t) noexcept;
using ExactSimdIndexAppendRawFn =
    bool (*)(void*, const std::uint64_t*, std::size_t);

struct ExactSimdIndexBridgeRequest {
    void* sequence_context = nullptr;
    ExactSimdIndexReadFragmentFn read_fragment = nullptr;
    ExactSimdIndexReleaseFragmentFn release_fragment = nullptr;
    void* collector_context = nullptr;
    ExactSimdIndexAppendRawFn append_raw = nullptr;
    ExactSimdIndexAlphabet alphabet = ExactSimdIndexAlphabet::dna4;
    std::size_t storage_unit_bytes = 0;
    std::size_t letters_per_unit = 0;
    std::size_t total_units = 0;
    std::size_t total_bp = 0;
    std::size_t fragment_units = 0;
    std::size_t fragment_count = 0;
    std::size_t lo_unit = 0;
    std::size_t hi_unit = 0;
    std::size_t window_size = 0;
    std::size_t kmer_size = 0;
    std::size_t emitted_position_base = 0;
};

struct ExactSimdIndexExecutionProof {
    std::uint64_t bp_consumed = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t vector_batches = 0;
    std::uint64_t masked_tail_lanes = 0;
    std::uint64_t packed_decode_batches = 0;
    std::uint64_t packed_decode_lanes = 0;
    std::uint64_t ambiguous_symbols = 0;
    std::uint64_t ambiguous_reset_batches = 0;
    std::uint64_t scalar_backend_branches = 0;
    std::uint64_t scalar_tail_symbols = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_event_records = 0;
    std::uint64_t scalar_sort_records = 0;
    std::uint64_t scalar_dedup_records = 0;
    std::uint64_t fail_closed_rejections = 0;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const ExactSimdIndexExecutionProof&) const noexcept = default;

    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    void combine(const ExactSimdIndexExecutionProof& other) noexcept {
        saturated = saturated || other.saturated;
        add(bp_consumed, other.bp_consumed, saturated);
        add(minimizers_emitted, other.minimizers_emitted, saturated);
        add(vector_batches, other.vector_batches, saturated);
        add(masked_tail_lanes, other.masked_tail_lanes, saturated);
        add(packed_decode_batches, other.packed_decode_batches, saturated);
        add(packed_decode_lanes, other.packed_decode_lanes, saturated);
        add(ambiguous_symbols, other.ambiguous_symbols, saturated);
        add(ambiguous_reset_batches, other.ambiguous_reset_batches, saturated);
        add(scalar_backend_branches, other.scalar_backend_branches, saturated);
        add(scalar_tail_symbols, other.scalar_tail_symbols, saturated);
        add(scalar_hash_symbols, other.scalar_hash_symbols, saturated);
        add(scalar_window_transitions, other.scalar_window_transitions, saturated);
        add(scalar_event_records, other.scalar_event_records, saturated);
        add(scalar_sort_records, other.scalar_sort_records, saturated);
        add(scalar_dedup_records, other.scalar_dedup_records, saturated);
        add(fail_closed_rejections, other.fail_closed_rejections, saturated);
    }

    [[nodiscard]] bool accepted(
            bool require_nonempty_vector_work = false) const noexcept {
        const bool exact = !saturated
            && fail_closed_rejections == 0
            && scalar_backend_branches == 0
            && scalar_tail_symbols == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_event_records == 0
            && scalar_sort_records == 0
            && scalar_dedup_records == 0
            && packed_decode_lanes == bp_consumed;
        return exact && (!require_nonempty_vector_work
            || (bp_consumed > 0 && vector_batches > 0
                && packed_decode_batches > 0));
    }
};

struct ExactSimdIndexBridgeResult {
    ExactSimdIndexStatus status = ExactSimdIndexStatus::ok;
    ExactSimdIndexExecutionProof proof{};
};

[[nodiscard]] ExactSimdIndexBridgeResult
collect_exact_simd_index_erased(const ExactSimdIndexBridgeRequest& request);

template<class Collector, class Sequence>
[[nodiscard]] inline ExactSimdIndexExecutionProof
collect_exact_simd_index_range_or_throw(
        Collector& collector,
        Sequence& sequence,
        std::size_t lo_unit,
        std::size_t hi_unit,
        std::size_t runtime_w,
        std::size_t runtime_k,
        std::size_t emitted_position_base,
        std::string_view operation) {
    using sequence_type = std::remove_cvref_t<Sequence>;
    using traits = typename sequence_type::traits;
    using storage_unit = typename traits::storage_unit;
    static_assert(std::same_as<typename MinimapSketchData::storage_t, std::uint64_t>);

    if constexpr (!std::same_as<typename traits::alphabet_type, seq::dna4>
        && !std::same_as<typename traits::alphabet_type, seq::dna5>) {
        throw ExactSimdIndexRuntimeError{
            ExactSimdIndexStatus::unsupported_alphabet, operation};
    } else if constexpr (!std::is_integral_v<storage_unit>
        || !(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
             || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8)
        || traits::bits_per_char == 0
        || traits::bits_per_char > 8
        || traits::letters_per_unit
            != sizeof(storage_unit) * 8 / traits::bits_per_char) {
        throw ExactSimdIndexRuntimeError{
            ExactSimdIndexStatus::unsupported_storage, operation};
    } else {
        struct SequenceContext {
            sequence_type* sequence = nullptr;
        } sequence_context{&sequence};

        constexpr bool has_scanner = requires { typename sequence_type::scanner_type; };
        constexpr bool fragmented = [] {
            if constexpr (requires { typename sequence_type::scanner_type; }) {
                return sequence_type::scanner_type::supports_fragments();
            } else {
                return false;
            }
        }();
        const std::size_t total_units = static_cast<std::size_t>(sequence.size());
        const std::size_t fragment_units = [&]() {
            if constexpr (fragmented) {
                return sequence_type::scanner_type::fragment_storage_units;
            } else {
                return std::max<std::size_t>(total_units, 1);
            }
        }();
        const std::size_t fragment_count = [&]() {
            if constexpr (fragmented) {
                return static_cast<std::size_t>(sequence.number_of_fragments());
            } else {
                return total_units == 0 ? 0u : 1u;
            }
        }();

        const auto read_fragment = +[](void* opaque, std::size_t fragment_no) {
            auto& seq_ref = *static_cast<SequenceContext*>(opaque)->sequence;
            if constexpr (fragmented) {
                const auto packed = seq_ref.scanner().read_fragment(fragment_no);
                return ExactSimdIndexPackedFragment{
                    .data = packed.data(),
                    .storage_units = packed.size(),
                };
            } else {
                const auto& resident = [&]() -> const auto& {
                    if constexpr (has_scanner) return seq_ref.underlying();
                    else return seq_ref;
                }();
                const std::size_t units = static_cast<std::size_t>(resident.size());
                return ExactSimdIndexPackedFragment{
                    .data = units == 0
                        ? nullptr
                        : resident.read_cptr(resident.cbegin()),
                    .storage_units = units,
                };
            }
        };
        const auto release_fragment = +[](void* opaque, std::size_t fragment_no) noexcept {
            if constexpr (fragmented) {
                auto& seq_ref = *static_cast<SequenceContext*>(opaque)->sequence;
                seq_ref.scanner().release_fragment(fragment_no);
            } else {
                (void)opaque;
                (void)fragment_no;
            }
        };
        const auto append_raw = +[](void* opaque,
                                    const std::uint64_t* raw,
                                    std::size_t count) {
            return static_cast<Collector*>(opaque)->append_raw_storage(raw, count);
        };

        const ExactSimdIndexBridgeResult result = collect_exact_simd_index_erased(
            ExactSimdIndexBridgeRequest{
                .sequence_context = &sequence_context,
                .read_fragment = read_fragment,
                .release_fragment = release_fragment,
                .collector_context = &collector,
                .append_raw = append_raw,
                .alphabet = std::same_as<
                        typename traits::alphabet_type, seq::dna4>
                    ? ExactSimdIndexAlphabet::dna4
                    : ExactSimdIndexAlphabet::dna5,
                .storage_unit_bytes = sizeof(storage_unit),
                .letters_per_unit = traits::letters_per_unit,
                .total_units = total_units,
                .total_bp = static_cast<std::size_t>(sequence.bp_size()),
                .fragment_units = fragment_units,
                .fragment_count = fragment_count,
                .lo_unit = lo_unit,
                .hi_unit = hi_unit,
                .window_size = runtime_w,
                .kmer_size = runtime_k,
                .emitted_position_base = emitted_position_base,
            });
        if (result.status != ExactSimdIndexStatus::ok) {
            throw ExactSimdIndexRuntimeError{result.status, operation};
        }
        return result.proof;
    }
}

// Plain ABI mirror of the packed Paper-ntHash checked kernel. The implementation
// unit imports the Highway owner and maps its native status/proof without
// allowing the indexing interface to instantiate or reimplement the algorithm.
enum class PackedPaperNthashIndexStatus : std::uint8_t {
    ok = 0,
    invalid_configuration,
    input_too_short,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    invalid_range,
    input_too_large,
    invalid_state,
};

[[nodiscard]] constexpr std::string_view
packed_paper_nthash_index_status_name(
        PackedPaperNthashIndexStatus status) noexcept {
    switch (status) {
        case PackedPaperNthashIndexStatus::ok: return "ok";
        case PackedPaperNthashIndexStatus::invalid_configuration:
            return "invalid_configuration";
        case PackedPaperNthashIndexStatus::input_too_short:
            return "input_too_short";
        case PackedPaperNthashIndexStatus::unsupported_alphabet:
            return "unsupported_alphabet";
        case PackedPaperNthashIndexStatus::unsupported_storage:
            return "unsupported_storage";
        case PackedPaperNthashIndexStatus::unsupported_representation:
            return "unsupported_representation";
        case PackedPaperNthashIndexStatus::unsupported_vector_shape:
            return "unsupported_vector_shape";
        case PackedPaperNthashIndexStatus::invalid_range:
            return "invalid_range";
        case PackedPaperNthashIndexStatus::input_too_large:
            return "input_too_large";
        case PackedPaperNthashIndexStatus::invalid_state:
            return "invalid_state";
    }
    return "unknown";
}

enum class PackedPaperNthashIndexAlphabet : std::uint8_t {
    dna4 = 0,
    dna5 = 1,
    unsupported = 2,
};

inline constexpr std::uint32_t kPackedPaperNthashKernelSchemeId =
    0x504E5431u;
inline constexpr std::size_t kPackedPaperNthashMaximumWindow = 64u;

/** Full non-Highway copy of the real checked kernel's execution evidence. */
struct PackedPaperNthashIndexExecutionProof {
    std::uint32_t scheme_id = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t window_size = 0;
    std::uint32_t nthash_rotation = 0;
    std::uint32_t source_storage_unit_bytes = 0;
    std::uint64_t input_symbols = 0;
    std::uint64_t input_storage_units = 0;
    std::uint64_t segments_combined = 0;
    std::uint64_t logical_streams = 0;
    std::uint64_t physical_vector_lanes = 0;
    std::uint64_t vector_registers_per_step = 0;
    std::uint64_t packed_decode_batches = 0;
    std::uint64_t packed_decode_lane_visits = 0;
    std::uint64_t hash_initialization_batches = 0;
    std::uint64_t rolling_hash_batches = 0;
    std::uint64_t hash_table_lookup_batches = 0;
    std::uint64_t canonical_sum_batches = 0;
    std::uint64_t window_update_batches = 0;
    std::uint64_t suffix_rebuild_batches = 0;
    std::uint64_t emission_decision_batches = 0;
    std::uint64_t emission_mask_store_batches = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t tied_endpoint_records_emitted = 0;
    std::uint64_t stream_boundary_records_buffered = 0;
    std::uint64_t stream_boundary_records_committed = 0;
    std::uint64_t stream_boundary_duplicates_suppressed = 0;
    std::uint64_t scalar_collector_appends = 0;
    std::uint64_t partial_chunk_callback_batches = 0;
    std::uint64_t masked_tail_lane_visits = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_minimum_decisions = 0;
    std::uint64_t fallback_calls = 0;
    bool positions_are_segment_local = false;
    bool aggregation_mismatch = false;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const PackedPaperNthashIndexExecutionProof&) const noexcept = default;

    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    /** Combine independently checked bounded windows without losing evidence. */
    void combine(
            const PackedPaperNthashIndexExecutionProof& other) noexcept {
        if (other == PackedPaperNthashIndexExecutionProof{}) return;
        if (*this == PackedPaperNthashIndexExecutionProof{}) {
            *this = other;
            return;
        }
        const bool same_identity = segments_combined > 0
            && other.segments_combined > 0
            && scheme_id == other.scheme_id
            && kmer_size == other.kmer_size
            && window_size == other.window_size
            && nthash_rotation == other.nthash_rotation
            && source_storage_unit_bytes
                == other.source_storage_unit_bytes
            && logical_streams == other.logical_streams
            && physical_vector_lanes == other.physical_vector_lanes
            && vector_registers_per_step
                == other.vector_registers_per_step
            && positions_are_segment_local
                == other.positions_are_segment_local;
        aggregation_mismatch = aggregation_mismatch
            || other.aggregation_mismatch || !same_identity;
        saturated = saturated || other.saturated;
        add(input_symbols, other.input_symbols, saturated);
        add(input_storage_units, other.input_storage_units, saturated);
        add(segments_combined, other.segments_combined, saturated);
        add(packed_decode_batches, other.packed_decode_batches, saturated);
        add(packed_decode_lane_visits,
            other.packed_decode_lane_visits, saturated);
        add(hash_initialization_batches,
            other.hash_initialization_batches, saturated);
        add(rolling_hash_batches, other.rolling_hash_batches, saturated);
        add(hash_table_lookup_batches,
            other.hash_table_lookup_batches, saturated);
        add(canonical_sum_batches, other.canonical_sum_batches, saturated);
        add(window_update_batches, other.window_update_batches, saturated);
        add(suffix_rebuild_batches, other.suffix_rebuild_batches, saturated);
        add(emission_decision_batches,
            other.emission_decision_batches, saturated);
        add(emission_mask_store_batches,
            other.emission_mask_store_batches, saturated);
        add(minimizers_emitted, other.minimizers_emitted, saturated);
        add(tied_endpoint_records_emitted,
            other.tied_endpoint_records_emitted, saturated);
        add(stream_boundary_records_buffered,
            other.stream_boundary_records_buffered, saturated);
        add(stream_boundary_records_committed,
            other.stream_boundary_records_committed, saturated);
        add(stream_boundary_duplicates_suppressed,
            other.stream_boundary_duplicates_suppressed, saturated);
        add(scalar_collector_appends,
            other.scalar_collector_appends, saturated);
        add(partial_chunk_callback_batches,
            other.partial_chunk_callback_batches, saturated);
        add(masked_tail_lane_visits,
            other.masked_tail_lane_visits, saturated);
        add(fail_closed_rejections,
            other.fail_closed_rejections, saturated);
        add(scalar_hash_symbols, other.scalar_hash_symbols, saturated);
        add(scalar_window_transitions,
            other.scalar_window_transitions, saturated);
        add(scalar_minimum_decisions,
            other.scalar_minimum_decisions, saturated);
        add(fallback_calls, other.fallback_calls, saturated);
    }

    [[nodiscard]] constexpr bool accepted(
            bool require_nonempty_vector_work = true) const noexcept {
        const bool vector_work_ok = !require_nonempty_vector_work
            || (packed_decode_batches > 0
                && packed_decode_lane_visits > 0
                && hash_initialization_batches > 0
                && hash_table_lookup_batches > 0
                && canonical_sum_batches > 0
                && window_update_batches > 0
                && emission_decision_batches > 0
                && emission_mask_store_batches > 0);
        return !saturated
            && !aggregation_mismatch
            && scheme_id == kPackedPaperNthashKernelSchemeId
            && kmer_size > 0
            && window_size > 0
            && window_size <= kPackedPaperNthashMaximumWindow
            && nthash_rotation == 7
            && source_storage_unit_bytes >= sizeof(std::uint32_t)
            && logical_streams == 8
            && (physical_vector_lanes == 4
                || physical_vector_lanes == 8)
            && vector_registers_per_step * physical_vector_lanes == 8
            && segments_combined > 0
            && positions_are_segment_local
            && vector_work_ok
            && canonical_sum_batches == window_update_batches
            && minimizers_emitted == scalar_collector_appends
            && stream_boundary_records_buffered
                == stream_boundary_records_committed
                    + stream_boundary_duplicates_suppressed
            && fail_closed_rejections == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_minimum_decisions == 0
            && fallback_calls == 0;
    }
};

struct PackedPaperNthashIndexBridgeRequest {
    void* sequence_context = nullptr;
    void* collector_context = nullptr;
    PackedPaperNthashIndexAlphabet alphabet =
        PackedPaperNthashIndexAlphabet::unsupported;
    std::size_t storage_unit_bytes = 0;
    std::size_t letters_per_unit = 0;
    std::size_t storage_units = 0;
    std::size_t local_bp_size = 0;
    std::size_t window_size = 0;
    std::size_t kmer_size = 0;
};

struct PackedPaperNthashIndexBridgeResult {
    PackedPaperNthashIndexStatus status = PackedPaperNthashIndexStatus::ok;
    PackedPaperNthashIndexExecutionProof proof{};
};

[[nodiscard]] constexpr PackedPaperNthashIndexBridgeResult
rejected_packed_paper_nthash_index_result(
        PackedPaperNthashIndexStatus status) noexcept {
    return {
        .status = status,
        .proof = PackedPaperNthashIndexExecutionProof{
            .fail_closed_rejections = 1,
        },
    };
}

[[nodiscard]] PackedPaperNthashIndexBridgeResult
collect_packed_paper_nthash_index_erased(
    const PackedPaperNthashIndexBridgeRequest& request);

/// Materialize exactly one bounded haloed window from a lazy packed sequence
/// and execute the checked packed-Paper bridge over that resident window.  MPF
/// and MPS persistence share this adapter so neither route can grow a second
/// implementation or silently substitute a scalar collector.
template <class R, class Scanner>
[[nodiscard]] PackedPaperNthashIndexBridgeResult
collect_bounded_packed_paper_nthash_index_lazy(
        lazy_compact_sequence<R, Scanner>& sequence,
        std::uint64_t lo_unit,
        std::uint64_t hi_unit,
        std::uint64_t local_bp_size,
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        sketch_collector& collector,
        std::uint64_t* materialized_window_bp_out = nullptr) {
    using traits = typename lazy_compact_sequence<R, Scanner>::traits;
    using storage_unit = typename traits::storage_unit;
    constexpr std::uint64_t lpu = traits::letters_per_unit;
    if (materialized_window_bp_out != nullptr) {
        *materialized_window_bp_out = 0;
    }
    if constexpr (!std::same_as<typename traits::alphabet_type, seq::dna4>) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_alphabet);
    } else if constexpr (!std::same_as<storage_unit, std::uint32_t>
        && !std::same_as<storage_unit, std::uint64_t>) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_storage);
    } else {
        if (lpu == 0 || lo_unit > hi_unit
            || hi_unit > static_cast<std::uint64_t>(sequence.size())) {
            return rejected_packed_paper_nthash_index_result(
                PackedPaperNthashIndexStatus::invalid_range);
        }
        if (local_bp_size == 0
            || local_bp_size - 1u
                > static_cast<std::uint64_t>(
                    MinimapSketchData::pos_field_mask)) {
            return rejected_packed_paper_nthash_index_result(
                PackedPaperNthashIndexStatus::unsupported_representation);
        }
        const std::uint64_t materialized_units_u64 = hi_unit - lo_unit;
        if (materialized_units_u64
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || materialized_units_u64
                > (std::numeric_limits<std::uint64_t>::max)() / lpu) {
            return rejected_packed_paper_nthash_index_result(
                PackedPaperNthashIndexStatus::input_too_large);
        }
        const std::uint64_t materialized_capacity_bp =
            materialized_units_u64 * lpu;
        if (materialized_capacity_bp < local_bp_size
            || local_bp_size
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || materialized_capacity_bp - local_bp_size
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return rejected_packed_paper_nthash_index_result(
                PackedPaperNthashIndexStatus::invalid_range);
        }

        const std::size_t materialized_units =
            static_cast<std::size_t>(materialized_units_u64);
        using packed_config = sequence_custom_alphabet<seq::dna4, storage_unit>;
        using packed_range = compact_sequence_range<packed_config>;
        typename packed_range::base_container packed(materialized_units);
        auto begin = sequence.cbegin();
        begin += static_cast<sequence_size_t>(lo_unit);
        const storage_unit* source = sequence.read_cptr(
            begin,
            std::span<storage_unit>{packed},
            materialized_units);
        if (materialized_units != 0 && source != packed.data()) {
            std::memcpy(
                packed.data(),
                source,
                materialized_units * sizeof(storage_unit));
        }
        const std::size_t padding = static_cast<std::size_t>(
            materialized_capacity_bp - local_bp_size);
        packed_range resident = packed_range::from_packed(
            std::move(packed), padding);
        if (materialized_window_bp_out != nullptr) {
            *materialized_window_bp_out = local_bp_size;
        }
        return collect_packed_paper_nthash_index_erased(
            PackedPaperNthashIndexBridgeRequest{
                .sequence_context = &resident,
                .collector_context = &collector,
                .alphabet = PackedPaperNthashIndexAlphabet::dna4,
                .storage_unit_bytes = sizeof(storage_unit),
                .letters_per_unit = lpu,
                .storage_units = materialized_units,
                .local_bp_size = static_cast<std::size_t>(local_bp_size),
                .window_size = window_size,
                .kmer_size = kmer_size,
            });
    }
}

// Revision-3 persisted minimizer codecs stay in this wire owner. Their shared
// CRC implementation is imported from the lower chunk-storage module so row,
// manifest, fake, and future PostgreSQL paths cannot drift.

/// Canonical minimap-style hash/tie/strand semantics encoded by revision 3.
inline constexpr std::uint32_t kMinimizerHashSemanticsVersion = 1u;

/// `(global_last_position << 1) | strand` position encoding semantics.
inline constexpr std::uint32_t kMinimizerPositionEncodingVersion = 1u;

/// Segment-local `(local_bp << 1) | strand` encoding used by format 4.
inline constexpr std::uint32_t kMinimizerSegmentLocalPositionEncodingVersion = 2u;

/// Fixed 16-byte `SerializedBucket` control layout consumed by CPU and GPU.
inline constexpr std::uint32_t kMinimizerControlLayoutAbiVersion = 1u;

/// Sorted-dense two-buffer GPU control/position ABI expected by revision 3.
inline constexpr std::uint32_t kMinimizerSortedDenseGpuAbiVersion = 1u;

/// Compact grouped-u32 payload ABI reserved by wire format 4. The format is
/// structurally decodable, but no production planner or reader selects it yet.
inline constexpr std::uint16_t kMinimizerGrouped32PayloadLayoutAbiVersion = 1u;
inline constexpr std::uint32_t kMinimizerGrouped32GpuAbiVersion = 2u;
inline constexpr std::size_t kMinimizerGrouped32ControlEncodedBytes = 12u;
inline constexpr std::size_t kMinimizerGrouped32PositionEncodedBytes = 4u;
inline constexpr std::uint32_t kMinimizerGrouped32MaxLocalBp = 0x7fffffffu;
inline constexpr std::uint32_t kLegacyMinimapK15MaxHash = 0x3fffffffu;

// Persisted minimizer scheme identities are frozen wire values.  The legacy
// minimap-v1 scheme deliberately remains the all-zero pair so every revision-3
// manifest produced before scheme identity was named preserves both its exact
// 128-byte image and compatibility checksum.
inline constexpr std::uint16_t kLegacyMinimapV1SchemeId = 0u;
inline constexpr std::uint16_t kLegacyMinimapV1SchemeVersion = 0u;
inline constexpr std::uint16_t kPaperNtHashU32R7SumV1SchemeId = 1u;
inline constexpr std::uint16_t kPaperNtHashU32R7SumV1SchemeVersion = 1u;
inline constexpr std::uint16_t kInvalidMinimizerSchemeId =
    (std::numeric_limits<std::uint16_t>::max)();
inline constexpr std::uint16_t kInvalidMinimizerSchemeVersion =
    (std::numeric_limits<std::uint16_t>::max)();

/// Frozen biological scheme identity shared by producers, writers and readers.
struct MinimizerSchemeIdentity {
    std::uint16_t id = kInvalidMinimizerSchemeId;
    std::uint16_t version = kInvalidMinimizerSchemeVersion;

    [[nodiscard]] bool operator==(const MinimizerSchemeIdentity&) const = default;
};

/// Storage-issued incarnation of one MPS canonical replay set.  The token
/// changes on every begin operation; the version changes whenever a caller can
/// alter the qualified logical replay (including rerouting or physical-run
/// injection). Internal compaction that preserves the exact canonical replay
/// is representation-only and leaves this binding stable. A zero token is
/// never authoritative.
struct MpsSegmentSpillStorageBinding {
    std::uint64_t generation_token = 0;
    std::uint64_t mutation_version = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation_token != 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const MpsSegmentSpillStorageBinding&) const noexcept = default;
};

inline constexpr MinimizerSchemeIdentity kLegacyMinimapV1SchemeIdentity{
    .id = kLegacyMinimapV1SchemeId,
    .version = kLegacyMinimapV1SchemeVersion,
};
inline constexpr MinimizerSchemeIdentity kPaperNtHashU32R7SumV1SchemeIdentity{
    .id = kPaperNtHashU32R7SumV1SchemeId,
    .version = kPaperNtHashU32R7SumV1SchemeVersion,
};

/// Return whether an id/version pair names one frozen revision-3 scheme.
[[nodiscard]] constexpr bool is_supported_minimizer_scheme_identity(
        std::uint16_t scheme_id,
        std::uint16_t scheme_version) noexcept {
    return (scheme_id == kLegacyMinimapV1SchemeId
            && scheme_version == kLegacyMinimapV1SchemeVersion)
        || (scheme_id == kPaperNtHashU32R7SumV1SchemeId
            && scheme_version == kPaperNtHashU32R7SumV1SchemeVersion);
}

/// Bind every currently routable sketch producer to its persisted semantics.
/// Adding a backend without extending this switch is a fail-closed operation.
[[nodiscard]] constexpr MinimizerSchemeIdentity
minimizer_scheme_identity_for_backend(
        MinimizerSketchBackend backend) noexcept {
    switch (backend) {
        case MinimizerSketchBackend::Scalar:
        case MinimizerSketchBackend::VulkanSegment:
        case MinimizerSketchBackend::ExactSimd:
            return kLegacyMinimapV1SchemeIdentity;
        case MinimizerSketchBackend::PaperNtHashSimd:
            return kPaperNtHashU32R7SumV1SchemeIdentity;
    }
    return {};
}

/// Uniform fixed-core segment geometry plus disjoint control/position extent
/// namespaces encoded in revision-3 manifests. Version 1 used one physical
/// sub-key with adjacent chunk intervals and is rejected by current readers.
inline constexpr std::uint16_t kMinimizerSegmentGeometryVersion = 2u;

/// Preferred immutable core span; overflow retries halve this power of two.
inline constexpr std::uint64_t kMinimizerPreferredSegmentSpanBp = 8u * 1024u * 1024u;

/// Deterministic hash-partition payload target used by the initial encoder POC.
inline constexpr std::uint64_t kMinimizerPartitionTargetBytes = 256u * 1024u;

/// Page-fitting payload bytes per seqidx row (record-aligned for both arrays).
inline constexpr std::uint32_t kMinimizerExtentPayloadBytes = 6u * 1024u;

/// Return whether a non-negative unsigned key fits a PostgreSQL BIGINT key.
[[nodiscard]] constexpr bool sql_bigint_can_represent(
        std::uint64_t value) noexcept {
    // Key axes are non-negative numeric values. CRC words persisted through a
    // signed SQL column use an explicit bit-preserving conversion instead.
    return value <= static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());
}

/// Return whether a non-negative unsigned key fits a PostgreSQL INTEGER key.
[[nodiscard]] constexpr bool sql_integer_can_represent(
        std::uint64_t value) noexcept {
    return value <= static_cast<std::uint64_t>(
        (std::numeric_limits<std::int32_t>::max)());
}

/// Fail-closed error classes reported by revision-3 fixed-wire codecs and geometry.
enum class MinimizerGenerationWireError : std::uint8_t {
    encoded_size,
    magic,
    version,
    declared_size,
    reserved_nonzero,
    generation_id,
    segment_geometry_version,
    segment_span,
    segment_count,
    directory_chunk_count,
    arithmetic_overflow,
    segment_ordinal,
    bp_range,
    directory_size,
    directory_order,
    partition_id,
    position_segment,
    hash_range,
    sub_key,
    chunk_range,
    element_count,
    byte_count,
    checksum,
    identity_mismatch,
    compatibility_mismatch,
    control_order,
    control_range,
    position_range,
    admission,
    duplicate_event,
    replay_mismatch,
    row_order,
    row_count,
};

/// Frozen persisted wire/profile selector encoded in every fixed metadata row.
enum class MinimizerGenerationWireFormat : std::uint16_t {
    invalid = 0u,
    sorted_dense_global64 = 3u,
    grouped_hash32_segment_local32 = 4u,
};

/// Return whether a persisted format selector has a canonical local profile.
[[nodiscard]] constexpr bool is_supported_minimizer_generation_wire_format(
        MinimizerGenerationWireFormat wire_format) noexcept {
    return wire_format
            == MinimizerGenerationWireFormat::sorted_dense_global64
        || wire_format
            == MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32;
}

/// Canonical physical payload contract derived only from format, scheme and K.
/// Callers cannot supply widths or ABI identities independently.
struct MinimizerGenerationPayloadProfile {
    MinimizerGenerationWireFormat wire_format =
        MinimizerGenerationWireFormat::invalid;
    MinimizerSchemeIdentity minimizer_scheme{};
    std::uint16_t kmer_size = 0;
    std::uint16_t control_encoded_byte_width = 0;
    std::uint16_t position_encoded_byte_width = 0;
    std::uint16_t payload_layout_abi_version = 0;
    std::uint32_t gpu_abi_version = 0;
    std::uint64_t max_hash = 0;
    bool segment_local_positions = false;

    [[nodiscard]] bool operator==(
        const MinimizerGenerationPayloadProfile&) const = default;
};

/// Derive the only admitted payload profile for a persisted identity.
[[nodiscard]] constexpr std::expected<
    MinimizerGenerationPayloadProfile,
    MinimizerGenerationWireError>
minimizer_generation_payload_profile(
        MinimizerGenerationWireFormat wire_format,
        MinimizerSchemeIdentity minimizer_scheme,
        std::uint16_t kmer_size) noexcept {
    if (!is_supported_minimizer_scheme_identity(
            minimizer_scheme.id, minimizer_scheme.version)
        || kmer_size == 0) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    if (wire_format
            == MinimizerGenerationWireFormat::sorted_dense_global64) {
        return MinimizerGenerationPayloadProfile{
            .wire_format = wire_format,
            .minimizer_scheme = minimizer_scheme,
            .kmer_size = kmer_size,
            .control_encoded_byte_width = 16u,
            .position_encoded_byte_width = 8u,
            .payload_layout_abi_version =
                static_cast<std::uint16_t>(
                    kMinimizerControlLayoutAbiVersion),
            .gpu_abi_version = kMinimizerSortedDenseGpuAbiVersion,
            .max_hash = (std::numeric_limits<std::uint64_t>::max)(),
            .segment_local_positions = false,
        };
    }
    if (wire_format
            == MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32
        && kmer_size == 15u) {
        return MinimizerGenerationPayloadProfile{
            .wire_format = wire_format,
            .minimizer_scheme = minimizer_scheme,
            .kmer_size = kmer_size,
            .control_encoded_byte_width =
                static_cast<std::uint16_t>(
                    kMinimizerGrouped32ControlEncodedBytes),
            .position_encoded_byte_width =
                static_cast<std::uint16_t>(
                    kMinimizerGrouped32PositionEncodedBytes),
            .payload_layout_abi_version =
                kMinimizerGrouped32PayloadLayoutAbiVersion,
            .gpu_abi_version = kMinimizerGrouped32GpuAbiVersion,
            .max_hash = minimizer_scheme == kLegacyMinimapV1SchemeIdentity
                ? static_cast<std::uint64_t>(kLegacyMinimapK15MaxHash)
                : static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)()),
            .segment_local_positions = true,
        };
    }
    return std::unexpected(
        MinimizerGenerationWireError::compatibility_mismatch);
}

/// Reject hand-constructed profiles that disagree with canonical derivation.
[[nodiscard]] constexpr std::expected<void, MinimizerGenerationWireError>
validate_minimizer_generation_payload_profile(
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    const auto expected = minimizer_generation_payload_profile(
        profile.wire_format, profile.minimizer_scheme, profile.kmer_size);
    if (!expected || *expected != profile) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    return {};
}

/// Derive the physical position-extent key from one authoritative logical /
/// control key.  The checked boundary keeps both payload namespaces below the
/// legacy TEMP_RUN_BASE reservation without changing the directory wire.
[[nodiscard]] constexpr std::expected<
    index_sub_key_t,
    MinimizerGenerationWireError>
derive_minimizer_position_extent_sub_key(
        index_sub_key_t control_sub_key) noexcept {
    if (control_sub_key < 0 || control_sub_key >= POSITION_EXTENT_BASE) {
        return std::unexpected(MinimizerGenerationWireError::sub_key);
    }
    return POSITION_EXTENT_BASE + control_sub_key;
}

/// Project one physical extent key back into the authoritative logical /
/// control namespace without performing unchecked signed arithmetic.
[[nodiscard]] constexpr std::expected<
    index_sub_key_t,
    MinimizerGenerationWireError>
project_minimizer_extent_sub_key(
        index_sub_key_t physical_sub_key,
        bool position_extent) noexcept {
    if (position_extent) {
        if (physical_sub_key < POSITION_EXTENT_BASE
            || physical_sub_key >= TEMP_RUN_BASE) {
            return std::unexpected(MinimizerGenerationWireError::sub_key);
        }
        return physical_sub_key - POSITION_EXTENT_BASE;
    }
    if (physical_sub_key < 0
        || physical_sub_key >= POSITION_EXTENT_BASE) {
        return std::unexpected(MinimizerGenerationWireError::sub_key);
    }
    return physical_sub_key;
}

/// Fixed 64-byte generation publication head stored at GENERATION_HEAD_SUB_KEY.
struct MinimizerGenerationHead {
    static constexpr std::uint32_t magic = 0x42494F48u; // BIOH numeric tag
    static constexpr std::size_t encoded_bytes = 64u;

    MinimizerGenerationWireFormat wire_format =
        MinimizerGenerationWireFormat::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t compatibility_checksum = 0;
    std::uint64_t manifest_checksum = 0;
    std::uint64_t source_content_checksum = 0;

    [[nodiscard]] bool operator==(const MinimizerGenerationHead&) const = default;
};

/// Fixed 128-byte immutable generation manifest for persisted minimizer payloads.
struct MinimizerGenerationManifest {
    static constexpr std::uint32_t magic = 0x42494F47u; // BIOG numeric tag
    static constexpr std::size_t encoded_bytes = 128u;
    static constexpr std::uint16_t uniform_segment_geometry_version =
        kMinimizerSegmentGeometryVersion;

    MinimizerGenerationWireFormat wire_format =
        MinimizerGenerationWireFormat::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t target_length = 0;
    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t payload_checksum = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t directory_checksum = 0;
    std::uint32_t position_segment_count = 0;
    std::uint32_t extent_count = 0;
    std::uint32_t directory_chunk_count = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    std::uint32_t gpu_abi_version = 0;
    std::uint64_t position_segment_span_bp = 0;
    std::uint16_t segment_geometry_version = 0;
    std::uint16_t minimizer_scheme_id = kLegacyMinimapV1SchemeId;
    std::uint16_t minimizer_scheme_version = kLegacyMinimapV1SchemeVersion;

    [[nodiscard]] bool operator==(const MinimizerGenerationManifest&) const = default;
};

/// Caller-owned compatibility contract checked before consuming one generation.
struct ExpectedMinimizerCompatibility {
    MinimizerGenerationWireFormat wire_format =
        MinimizerGenerationWireFormat::invalid;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    std::uint32_t gpu_abi_version = kMinimizerSortedDenseGpuAbiVersion;
    std::uint16_t minimizer_scheme_id = kInvalidMinimizerSchemeId;
    std::uint16_t minimizer_scheme_version = kInvalidMinimizerSchemeVersion;

    [[nodiscard]] bool operator==(
        const ExpectedMinimizerCompatibility&) const = default;
};

/// Construct an independent persisted-read contract from the selected sketch
/// backend. No manifest field participates in this selection.
[[nodiscard]] constexpr std::expected<
    ExpectedMinimizerCompatibility,
    MinimizerGenerationWireError>
expected_minimizer_compatibility_for_backend(
        MinimizerGenerationWireFormat wire_format,
        MinimizerSketchBackend backend,
        std::uint16_t alphabet_code,
        std::uint16_t storage_unit_byte_width,
        std::uint16_t kmer_size,
        std::uint16_t window_size) noexcept {
    const MinimizerSchemeIdentity scheme =
        minimizer_scheme_identity_for_backend(backend);
    const auto profile = minimizer_generation_payload_profile(
        wire_format, scheme, kmer_size);
    if (!profile || storage_unit_byte_width == 0 || window_size == 0) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    return ExpectedMinimizerCompatibility{
        .wire_format = wire_format,
        .alphabet_code = alphabet_code,
        .storage_unit_byte_width = storage_unit_byte_width,
        .kmer_size = kmer_size,
        .window_size = window_size,
        .gpu_abi_version = profile->gpu_abi_version,
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_version = scheme.version,
    };
}

/// Logical revision-3 directory entry; persistence always uses the fixed codec.
/// `sub_key` is the logical/control key. Position rows use the checked derived
/// key POSITION_EXTENT_BASE + sub_key. Both extent-local chunk sequences start
/// at zero, while the 96-byte directory ABI remains unchanged.
struct MinimizerBucketDirEntry {
    static constexpr std::size_t encoded_bytes = 96u;

    std::uint32_t partition_id = 0;
    std::int32_t position_segment = 0;
    std::uint64_t lo_hash = 0;
    std::uint64_t hi_hash = 0;
    index_sub_key_t sub_key = 0;
    std::uint32_t control_first_chunk = 0;
    std::uint32_t control_chunk_count = 0;
    std::uint32_t position_first_chunk = 0;
    std::uint32_t position_chunk_count = 0;
    std::uint64_t control_element_count = 0;
    std::uint64_t position_element_count = 0;
    std::uint64_t control_encoded_bytes = 0;
    std::uint64_t position_encoded_bytes = 0;
    std::uint64_t control_checksum = 0;
    std::uint64_t position_checksum = 0;

    [[nodiscard]] bool operator==(const MinimizerBucketDirEntry&) const = default;
};

/// Half-open base-pair core owned by one persisted position-segment ordinal.
struct MinimizerPositionSegmentCore {
    std::uint64_t begin_bp = 0;
    std::uint64_t end_bp = 0;

    [[nodiscard]] bool operator==(const MinimizerPositionSegmentCore&) const = default;
};

/// Half-open ordinal interval covering a requested base-pair range.
struct MinimizerPositionSegmentOrdinalRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0; // half-open

    [[nodiscard]] bool operator==(
        const MinimizerPositionSegmentOrdinalRange&) const = default;
};

} // namespace bioseq::indexing

namespace bioseq::indexing::minimizer_generation_wire_detail {

template <class UInt>
requires (std::is_unsigned_v<UInt>)
constexpr void write_le(
        std::span<std::byte> output,
        std::size_t offset,
        UInt value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(UInt); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & UInt{0xff});
        value >>= 8u;
    }
}

template <class UInt>
requires (std::is_unsigned_v<UInt>)
[[nodiscard]] constexpr UInt read_le(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    UInt value = 0;
    for (std::size_t byte = 0; byte < sizeof(UInt); ++byte) {
        value |= static_cast<UInt>(
            std::to_integer<std::uint8_t>(input[offset + byte])) << (byte * 8u);
    }
    return value;
}

[[nodiscard]] constexpr bool all_zero(
        std::span<const std::byte> input,
        std::size_t begin,
        std::size_t end) noexcept {
    for (std::size_t offset = begin; offset < end; ++offset) {
        if (input[offset] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

// Standalone wire decoding validates only invariants available in the fixed
// manifest itself. Head/manifest identity plus payload/directory byte counts,
// checksums, extents, and compatibility relationships require the referenced
// objects and deliberately belong to generation publication/load validation.
[[nodiscard]] constexpr std::optional<MinimizerGenerationWireError>
validate_manifest_standalone(const MinimizerGenerationManifest& manifest) noexcept {
    const auto profile = minimizer_generation_payload_profile(
        manifest.wire_format,
        MinimizerSchemeIdentity{
            .id = manifest.minimizer_scheme_id,
            .version = manifest.minimizer_scheme_version,
        },
        manifest.kmer_size);
    if (!profile) {
        return MinimizerGenerationWireError::compatibility_mismatch;
    }
    if (manifest.segment_geometry_version
        != MinimizerGenerationManifest::uniform_segment_geometry_version) {
        return MinimizerGenerationWireError::segment_geometry_version;
    }
    if (!sql_integer_can_represent(manifest.position_segment_count)) {
        return MinimizerGenerationWireError::segment_count;
    }
    if (!sql_integer_can_represent(manifest.directory_chunk_count)) {
        return MinimizerGenerationWireError::directory_chunk_count;
    }
    if (manifest.target_length == 0) {
        if (manifest.position_segment_count != 0) {
            return MinimizerGenerationWireError::segment_count;
        }
        return std::nullopt;
    }
    if (manifest.position_segment_span_bp == 0) {
        return MinimizerGenerationWireError::segment_span;
    }
    const std::uint64_t expected_count =
        manifest.target_length / manifest.position_segment_span_bp
        + (manifest.target_length % manifest.position_segment_span_bp != 0 ? 1u : 0u);
    if (!sql_integer_can_represent(expected_count)
        || expected_count != manifest.position_segment_count) {
        return MinimizerGenerationWireError::segment_count;
    }
    return std::nullopt;
}

} // namespace minimizer_generation_wire_detail

export namespace bioseq::indexing {

/// Encode a validated generation head to its exact 64-byte little-endian image.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes>,
    MinimizerGenerationWireError>
encode_minimizer_generation_head(const MinimizerGenerationHead& head) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (!is_supported_minimizer_generation_wire_format(head.wire_format)) {
        return std::unexpected(MinimizerGenerationWireError::version);
    }
    if (head.generation_id == 0 || !sql_bigint_can_represent(head.generation_id)) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes> output{};
    std::span<std::byte> bytes{output};
    write_le<std::uint32_t>(bytes, 0, MinimizerGenerationHead::magic);
    write_le<std::uint16_t>(bytes, 4,
        static_cast<std::uint16_t>(head.wire_format));
    write_le<std::uint16_t>(bytes, 6,
        static_cast<std::uint16_t>(MinimizerGenerationHead::encoded_bytes));
    write_le<std::uint64_t>(bytes, 8, head.generation_id);
    write_le<std::uint64_t>(bytes, 16, head.source_identity);
    write_le<std::uint64_t>(bytes, 24, head.compatibility_checksum);
    write_le<std::uint64_t>(bytes, 32, head.manifest_checksum);
    write_le<std::uint64_t>(bytes, 40, head.source_content_checksum);
    return output;
}

/// Decode one exact 64-byte generation head, rejecting malformed or reserved data.
[[nodiscard]] constexpr std::expected<
    MinimizerGenerationHead,
    MinimizerGenerationWireError>
decode_minimizer_generation_head(std::span<const std::byte> input) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (input.size() != MinimizerGenerationHead::encoded_bytes) {
        return std::unexpected(MinimizerGenerationWireError::encoded_size);
    }
    if (read_le<std::uint32_t>(input, 0) != MinimizerGenerationHead::magic) {
        return std::unexpected(MinimizerGenerationWireError::magic);
    }
    const auto wire_format = static_cast<MinimizerGenerationWireFormat>(
        read_le<std::uint16_t>(input, 4));
    if (!is_supported_minimizer_generation_wire_format(wire_format)) {
        return std::unexpected(MinimizerGenerationWireError::version);
    }
    if (read_le<std::uint16_t>(input, 6) != MinimizerGenerationHead::encoded_bytes) {
        return std::unexpected(MinimizerGenerationWireError::declared_size);
    }
    if (!all_zero(input, 48, 64)) {
        return std::unexpected(MinimizerGenerationWireError::reserved_nonzero);
    }
    MinimizerGenerationHead head{
        .wire_format = wire_format,
        .generation_id = read_le<std::uint64_t>(input, 8),
        .source_identity = read_le<std::uint64_t>(input, 16),
        .compatibility_checksum = read_le<std::uint64_t>(input, 24),
        .manifest_checksum = read_le<std::uint64_t>(input, 32),
        .source_content_checksum = read_le<std::uint64_t>(input, 40),
    };
    if (head.generation_id == 0 || !sql_bigint_can_represent(head.generation_id)) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    return head;
}

/// Encode a standalone-valid manifest to its exact 128-byte little-endian image.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, MinimizerGenerationManifest::encoded_bytes>,
    MinimizerGenerationWireError>
encode_minimizer_generation_manifest(
        const MinimizerGenerationManifest& manifest) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (manifest.generation_id == 0
        || !sql_bigint_can_represent(manifest.generation_id)) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    std::array<std::byte, MinimizerGenerationManifest::encoded_bytes> output{};
    std::span<std::byte> bytes{output};
    write_le<std::uint32_t>(bytes, 0, MinimizerGenerationManifest::magic);
    write_le<std::uint16_t>(bytes, 4,
        static_cast<std::uint16_t>(manifest.wire_format));
    write_le<std::uint16_t>(bytes, 6,
        static_cast<std::uint16_t>(MinimizerGenerationManifest::encoded_bytes));
    write_le<std::uint64_t>(bytes, 8, manifest.generation_id);
    write_le<std::uint64_t>(bytes, 16, manifest.source_identity);
    write_le<std::uint64_t>(bytes, 24, manifest.source_content_checksum);
    write_le<std::uint64_t>(bytes, 32, manifest.target_length);
    write_le<std::uint64_t>(bytes, 40, manifest.control_count);
    write_le<std::uint64_t>(bytes, 48, manifest.position_count);
    write_le<std::uint64_t>(bytes, 56, manifest.payload_bytes);
    write_le<std::uint64_t>(bytes, 64, manifest.payload_checksum);
    write_le<std::uint64_t>(bytes, 72, manifest.directory_bytes);
    write_le<std::uint64_t>(bytes, 80, manifest.directory_checksum);
    write_le<std::uint32_t>(bytes, 88, manifest.position_segment_count);
    write_le<std::uint32_t>(bytes, 92, manifest.extent_count);
    write_le<std::uint32_t>(bytes, 96, manifest.directory_chunk_count);
    write_le<std::uint16_t>(bytes, 100, manifest.alphabet_code);
    write_le<std::uint16_t>(bytes, 102, manifest.storage_unit_byte_width);
    write_le<std::uint16_t>(bytes, 104, manifest.kmer_size);
    write_le<std::uint16_t>(bytes, 106, manifest.window_size);
    write_le<std::uint32_t>(bytes, 108, manifest.gpu_abi_version);
    write_le<std::uint64_t>(bytes, 112, manifest.position_segment_span_bp);
    write_le<std::uint16_t>(bytes, 120, manifest.segment_geometry_version);
    write_le<std::uint16_t>(bytes, 122, manifest.minimizer_scheme_id);
    write_le<std::uint16_t>(bytes, 124, manifest.minimizer_scheme_version);
    return output;
}

/// Decode one exact 128-byte manifest and validate intrinsic scalar invariants.
[[nodiscard]] constexpr std::expected<
    MinimizerGenerationManifest,
    MinimizerGenerationWireError>
decode_minimizer_generation_manifest(std::span<const std::byte> input) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (input.size() != MinimizerGenerationManifest::encoded_bytes) {
        return std::unexpected(MinimizerGenerationWireError::encoded_size);
    }
    if (read_le<std::uint32_t>(input, 0) != MinimizerGenerationManifest::magic) {
        return std::unexpected(MinimizerGenerationWireError::magic);
    }
    const auto wire_format = static_cast<MinimizerGenerationWireFormat>(
        read_le<std::uint16_t>(input, 4));
    if (!is_supported_minimizer_generation_wire_format(wire_format)) {
        return std::unexpected(MinimizerGenerationWireError::version);
    }
    if (read_le<std::uint16_t>(input, 6)
        != MinimizerGenerationManifest::encoded_bytes) {
        return std::unexpected(MinimizerGenerationWireError::declared_size);
    }
    if (!all_zero(input, 126, 128)) {
        return std::unexpected(MinimizerGenerationWireError::reserved_nonzero);
    }
    MinimizerGenerationManifest manifest{
        .wire_format = wire_format,
        .generation_id = read_le<std::uint64_t>(input, 8),
        .source_identity = read_le<std::uint64_t>(input, 16),
        .source_content_checksum = read_le<std::uint64_t>(input, 24),
        .target_length = read_le<std::uint64_t>(input, 32),
        .control_count = read_le<std::uint64_t>(input, 40),
        .position_count = read_le<std::uint64_t>(input, 48),
        .payload_bytes = read_le<std::uint64_t>(input, 56),
        .payload_checksum = read_le<std::uint64_t>(input, 64),
        .directory_bytes = read_le<std::uint64_t>(input, 72),
        .directory_checksum = read_le<std::uint64_t>(input, 80),
        .position_segment_count = read_le<std::uint32_t>(input, 88),
        .extent_count = read_le<std::uint32_t>(input, 92),
        .directory_chunk_count = read_le<std::uint32_t>(input, 96),
        .alphabet_code = read_le<std::uint16_t>(input, 100),
        .storage_unit_byte_width = read_le<std::uint16_t>(input, 102),
        .kmer_size = read_le<std::uint16_t>(input, 104),
        .window_size = read_le<std::uint16_t>(input, 106),
        .gpu_abi_version = read_le<std::uint32_t>(input, 108),
        .position_segment_span_bp = read_le<std::uint64_t>(input, 112),
        .segment_geometry_version = read_le<std::uint16_t>(input, 120),
        .minimizer_scheme_id = read_le<std::uint16_t>(input, 122),
        .minimizer_scheme_version = read_le<std::uint16_t>(input, 124),
    };
    if (manifest.generation_id == 0
        || !sql_bigint_can_represent(manifest.generation_id)) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    return manifest;
}

/// Validate invariants intrinsic to one decoded revision-3 directory entry.
[[nodiscard]] constexpr std::optional<MinimizerGenerationWireError>
validate_minimizer_bucket_dir_entry(
        const MinimizerBucketDirEntry& entry,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (!validate_minimizer_generation_payload_profile(profile)) {
        return MinimizerGenerationWireError::compatibility_mismatch;
    }
    if (entry.position_segment < 0) {
        return MinimizerGenerationWireError::position_segment;
    }
    if (entry.lo_hash > entry.hi_hash || entry.hi_hash > profile.max_hash) {
        return MinimizerGenerationWireError::hash_range;
    }
    if (!derive_minimizer_position_extent_sub_key(entry.sub_key)) {
        return MinimizerGenerationWireError::sub_key;
    }
    if (entry.control_element_count == 0
        || entry.position_element_count == 0
        || entry.control_element_count
            > (std::numeric_limits<std::uint32_t>::max)()
        || entry.position_element_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return MinimizerGenerationWireError::element_count;
    }
    const std::uint64_t control_width =
        profile.control_encoded_byte_width;
    const std::uint64_t position_width =
        profile.position_encoded_byte_width;
    if (entry.control_element_count
            > (std::numeric_limits<std::uint64_t>::max)() / control_width
        || entry.position_element_count
            > (std::numeric_limits<std::uint64_t>::max)() / position_width
        || entry.control_encoded_bytes
            != entry.control_element_count * control_width
        || entry.position_encoded_bytes
            != entry.position_element_count * position_width) {
        return MinimizerGenerationWireError::byte_count;
    }
    if (entry.control_first_chunk != 0 || entry.position_first_chunk != 0
        || entry.control_chunk_count == 0 || entry.position_chunk_count == 0) {
        return MinimizerGenerationWireError::chunk_range;
    }
    const std::uint64_t control_end =
        static_cast<std::uint64_t>(entry.control_first_chunk)
        + entry.control_chunk_count;
    const std::uint64_t position_end =
        static_cast<std::uint64_t>(entry.position_first_chunk)
        + entry.position_chunk_count;
    if (control_end > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)()) + 1u
        || position_end > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)()) + 1u) {
        return MinimizerGenerationWireError::chunk_range;
    }
    return std::nullopt;
}

/// Encode one logical directory entry to the exact 96-byte little-endian ABI.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, MinimizerBucketDirEntry::encoded_bytes>,
    MinimizerGenerationWireError>
encode_minimizer_bucket_dir_entry(
        const MinimizerBucketDirEntry& entry,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
        return std::unexpected(*error);
    }
    std::array<std::byte, MinimizerBucketDirEntry::encoded_bytes> output{};
    std::span<std::byte> bytes{output};
    write_le<std::uint32_t>(bytes, 0, entry.partition_id);
    write_le<std::uint32_t>(bytes, 4,
        std::bit_cast<std::uint32_t>(entry.position_segment));
    write_le<std::uint64_t>(bytes, 8, entry.lo_hash);
    write_le<std::uint64_t>(bytes, 16, entry.hi_hash);
    write_le<std::uint64_t>(bytes, 24,
        std::bit_cast<std::uint64_t>(entry.sub_key));
    write_le<std::uint32_t>(bytes, 32, entry.control_first_chunk);
    write_le<std::uint32_t>(bytes, 36, entry.control_chunk_count);
    write_le<std::uint32_t>(bytes, 40, entry.position_first_chunk);
    write_le<std::uint32_t>(bytes, 44, entry.position_chunk_count);
    write_le<std::uint64_t>(bytes, 48, entry.control_element_count);
    write_le<std::uint64_t>(bytes, 56, entry.position_element_count);
    write_le<std::uint64_t>(bytes, 64, entry.control_encoded_bytes);
    write_le<std::uint64_t>(bytes, 72, entry.position_encoded_bytes);
    write_le<std::uint64_t>(bytes, 80, entry.control_checksum);
    write_le<std::uint64_t>(bytes, 88, entry.position_checksum);
    return output;
}

/// Decode and intrinsically validate one exact 96-byte directory entry.
[[nodiscard]] constexpr std::expected<
    MinimizerBucketDirEntry,
    MinimizerGenerationWireError>
decode_minimizer_bucket_dir_entry(
        std::span<const std::byte> input,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (input.size() != MinimizerBucketDirEntry::encoded_bytes) {
        return std::unexpected(MinimizerGenerationWireError::encoded_size);
    }
    MinimizerBucketDirEntry entry{
        .partition_id = read_le<std::uint32_t>(input, 0),
        .position_segment = std::bit_cast<std::int32_t>(
            read_le<std::uint32_t>(input, 4)),
        .lo_hash = read_le<std::uint64_t>(input, 8),
        .hi_hash = read_le<std::uint64_t>(input, 16),
        .sub_key = std::bit_cast<index_sub_key_t>(
            read_le<std::uint64_t>(input, 24)),
        .control_first_chunk = read_le<std::uint32_t>(input, 32),
        .control_chunk_count = read_le<std::uint32_t>(input, 36),
        .position_first_chunk = read_le<std::uint32_t>(input, 40),
        .position_chunk_count = read_le<std::uint32_t>(input, 44),
        .control_element_count = read_le<std::uint64_t>(input, 48),
        .position_element_count = read_le<std::uint64_t>(input, 56),
        .control_encoded_bytes = read_le<std::uint64_t>(input, 64),
        .position_encoded_bytes = read_le<std::uint64_t>(input, 72),
        .control_checksum = read_le<std::uint64_t>(input, 80),
        .position_checksum = read_le<std::uint64_t>(input, 88),
    };
    if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
        return std::unexpected(*error);
    }
    return entry;
}

/// Validate stable ordinal assignment and sorted, non-overlapping hash ranges.
[[nodiscard]] inline std::optional<MinimizerGenerationWireError>
validate_minimizer_bucket_directory_entries(
        std::span<const MinimizerBucketDirEntry> entries,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (!validate_minimizer_generation_payload_profile(profile)) {
        return MinimizerGenerationWireError::compatibility_mismatch;
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const MinimizerBucketDirEntry& entry = entries[index];
        if (entry.partition_id != index) {
            return MinimizerGenerationWireError::partition_id;
        }
        if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
            return error;
        }
        if (index == 0) {
            continue;
        }
        const MinimizerBucketDirEntry& previous = entries[index - 1u];
        if (entry.position_segment < previous.position_segment
            || (entry.position_segment == previous.position_segment
                && entry.lo_hash <= previous.hi_hash)) {
            return MinimizerGenerationWireError::directory_order;
        }
    }
    return std::nullopt;
}

/// Encode the authoritative header-free sequence of fixed directory entries.
[[nodiscard]] inline std::expected<
    std::vector<std::byte>,
    MinimizerGenerationWireError>
encode_minimizer_bucket_directory(
        std::span<const MinimizerBucketDirEntry> entries,
        const MinimizerGenerationPayloadProfile& profile) {
    if (auto error = validate_minimizer_bucket_directory_entries(
            entries, profile)) {
        return std::unexpected(*error);
    }
    if (entries.size() > (std::numeric_limits<std::size_t>::max)()
            / MinimizerBucketDirEntry::encoded_bytes) {
        return std::unexpected(MinimizerGenerationWireError::arithmetic_overflow);
    }
    std::vector<std::byte> output(
        entries.size() * MinimizerBucketDirEntry::encoded_bytes);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto encoded = encode_minimizer_bucket_dir_entry(
            entries[index], profile);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        std::copy(encoded->begin(), encoded->end(),
            output.begin() + static_cast<std::ptrdiff_t>(
                index * MinimizerBucketDirEntry::encoded_bytes));
    }
    return output;
}

/// Owning decoded view of the authoritative fixed directory bytes.
class DirView {
public:
    DirView() = default;

    /// Copy and validate a complete header-free encoded directory.
    [[nodiscard]] static std::expected<DirView, MinimizerGenerationWireError>
    decode(
            std::span<const std::byte> bytes,
            const MinimizerGenerationPayloadProfile& profile) {
        if (!validate_minimizer_generation_payload_profile(profile)) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        if (bytes.size() % MinimizerBucketDirEntry::encoded_bytes != 0) {
            return std::unexpected(MinimizerGenerationWireError::directory_size);
        }
        DirView view;
        view.profile_ = profile;
        view.encoded_.assign(bytes.begin(), bytes.end());
        const std::size_t count =
            bytes.size() / MinimizerBucketDirEntry::encoded_bytes;
        view.entries_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const std::span<const std::byte> encoded_entry{
                view.encoded_.data()
                    + index * MinimizerBucketDirEntry::encoded_bytes,
                MinimizerBucketDirEntry::encoded_bytes};
            auto entry = decode_minimizer_bucket_dir_entry(
                encoded_entry, profile);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            view.entries_.push_back(*entry);
        }
        if (auto error = validate_minimizer_bucket_directory_entries(
                view.entries_, profile)) {
            return std::unexpected(*error);
        }
        return view;
    }

    /// Decode under an existing logical reservation. Both vectors reserve
    /// first, then their actual capacities are reconciled before any persisted
    /// byte or decoded entry is written. The caller's permit may include other
    /// simultaneously-live allocations; only the directory capacity delta is
    /// added here.
    [[nodiscard]] static std::expected<DirView, MinimizerGenerationWireError>
    decode_admitted(
            std::span<const std::byte> bytes,
            const MinimizerGenerationPayloadProfile& profile,
            bioseq::parallelism::MemoryPermit& permit) {
        if (!validate_minimizer_generation_payload_profile(profile)) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        if (bytes.size() % MinimizerBucketDirEntry::encoded_bytes != 0) {
            return std::unexpected(MinimizerGenerationWireError::directory_size);
        }
        const std::size_t count =
            bytes.size() / MinimizerBucketDirEntry::encoded_bytes;
        DirView view;
        view.profile_ = profile;
        view.encoded_.reserve(bytes.size());
        view.entries_.reserve(count);
        const std::uint64_t logical_bytes =
            bioseq::parallelism::saturating_add(
                bytes.size(),
                bioseq::parallelism::saturating_mul(
                    count, sizeof(MinimizerBucketDirEntry)));
        const std::uint64_t actual_bytes = view.owned_capacity_bytes();
        if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()
            || actual_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        if (actual_bytes > logical_bytes) {
            const std::uint64_t reconciled =
                bioseq::parallelism::saturating_add(
                    permit.bytes(), actual_bytes - logical_bytes);
            if (reconciled == (std::numeric_limits<std::uint64_t>::max)()
                || !permit.grow_to(reconciled)) {
                return std::unexpected(MinimizerGenerationWireError::admission);
            }
        }
        view.encoded_.assign(bytes.begin(), bytes.end());
        for (std::size_t index = 0; index < count; ++index) {
            const auto entry = decode_minimizer_bucket_dir_entry(
                std::span<const std::byte>{view.encoded_}.subspan(
                    index * MinimizerBucketDirEntry::encoded_bytes,
                    MinimizerBucketDirEntry::encoded_bytes),
                profile);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            view.entries_.push_back(*entry);
        }
        if (auto error = validate_minimizer_bucket_directory_entries(
                view.entries_, profile)) {
            return std::unexpected(*error);
        }
        return view;
    }

    /// Return the immutable encoded bytes suitable for direct GPU upload.
    [[nodiscard]] std::span<const std::byte> encoded_bytes() const noexcept {
        return encoded_;
    }

    /// Return decoded entries in their stable directory-ordinal order.
    [[nodiscard]] std::span<const MinimizerBucketDirEntry> entries() const noexcept {
        return entries_;
    }

    /// Return the canonical profile used to qualify every decoded entry.
    [[nodiscard]] const MinimizerGenerationPayloadProfile& profile() const noexcept {
        return profile_;
    }

    /// Heap bytes owned by this view. Readers use capacities, rather than
    /// logical sizes, when retaining a memory-admission permit.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        return bioseq::parallelism::saturating_add(
            static_cast<std::uint64_t>(encoded_.capacity()),
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(entries_.capacity()),
                sizeof(MinimizerBucketDirEntry)));
    }

    /// Resolve one stable directory ordinal.
    [[nodiscard]] const MinimizerBucketDirEntry* entry_by_ordinal(
            std::uint32_t ordinal) const noexcept {
        return ordinal < entries_.size() ? &entries_[ordinal] : nullptr;
    }

    /// Find the unique hash partition in one fixed persisted position core.
    [[nodiscard]] const MinimizerBucketDirEntry* find(
            std::int32_t position_segment,
            std::uint64_t hash) const noexcept {
        std::size_t lo = 0;
        std::size_t hi = entries_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2u;
            const MinimizerBucketDirEntry& entry = entries_[mid];
            if (entry.position_segment < position_segment
                || (entry.position_segment == position_segment
                    && entry.hi_hash < hash)) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (lo >= entries_.size()) {
            return nullptr;
        }
        const MinimizerBucketDirEntry& entry = entries_[lo];
        return entry.position_segment == position_segment
                && entry.lo_hash <= hash && hash <= entry.hi_hash
            ? &entry
            : nullptr;
    }

private:
    MinimizerGenerationPayloadProfile profile_{};
    std::vector<std::byte> encoded_;
    std::vector<MinimizerBucketDirEntry> entries_;
};

/// Derive the overflow-safe half-open core for one persisted segment ordinal.
[[nodiscard]] constexpr std::expected<
    MinimizerPositionSegmentCore,
    MinimizerGenerationWireError>
minimizer_position_segment_core(
        const MinimizerGenerationManifest& manifest,
        std::uint32_t ordinal) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    if (ordinal >= manifest.position_segment_count) {
        return std::unexpected(MinimizerGenerationWireError::segment_ordinal);
    }
    if (manifest.position_segment_span_bp != 0
        && ordinal > (std::numeric_limits<std::uint64_t>::max)()
            / manifest.position_segment_span_bp) {
        return std::unexpected(MinimizerGenerationWireError::arithmetic_overflow);
    }
    const std::uint64_t begin =
        static_cast<std::uint64_t>(ordinal) * manifest.position_segment_span_bp;
    if (begin >= manifest.target_length) {
        return std::unexpected(MinimizerGenerationWireError::segment_ordinal);
    }
    const std::uint64_t remaining = manifest.target_length - begin;
    const std::uint64_t width =
        std::min(manifest.position_segment_span_bp, remaining);
    return MinimizerPositionSegmentCore{begin, begin + width};
}

/// Map a half-open base-pair range to the covering persisted segment ordinals.
[[nodiscard]] constexpr std::expected<
    MinimizerPositionSegmentOrdinalRange,
    MinimizerGenerationWireError>
minimizer_position_segments_for_bp_range(
        const MinimizerGenerationManifest& manifest,
        std::uint64_t begin_bp,
        std::uint64_t end_bp) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    if (begin_bp > end_bp || end_bp > manifest.target_length) {
        return std::unexpected(MinimizerGenerationWireError::bp_range);
    }
    if (begin_bp == end_bp) {
        const std::uint32_t ordinal = begin_bp == manifest.target_length
            ? manifest.position_segment_count
            : static_cast<std::uint32_t>(
                begin_bp / manifest.position_segment_span_bp);
        return MinimizerPositionSegmentOrdinalRange{ordinal, ordinal};
    }
    const std::uint64_t first = begin_bp / manifest.position_segment_span_bp;
    const std::uint64_t last = (end_bp - 1u) / manifest.position_segment_span_bp;
    if (last >= manifest.position_segment_count
        || !sql_integer_can_represent(first)
        || !sql_integer_can_represent(last + 1u)) {
        return std::unexpected(MinimizerGenerationWireError::segment_ordinal);
    }
    return MinimizerPositionSegmentOrdinalRange{
        static_cast<std::uint32_t>(first),
        static_cast<std::uint32_t>(last + 1u)};
}

/// Compute the compatibility identity from every named semantic/ABI axis.
[[nodiscard]] constexpr std::uint64_t
minimizer_generation_compatibility_checksum(
        const MinimizerGenerationManifest& manifest) noexcept {
    const auto profile = minimizer_generation_payload_profile(
        manifest.wire_format,
        MinimizerSchemeIdentity{
            .id = manifest.minimizer_scheme_id,
            .version = manifest.minimizer_scheme_version,
        },
        manifest.kmer_size);
    Crc64Ecma182 crc;
    crc.update_little_endian(manifest.alphabet_code);
    crc.update_little_endian(manifest.storage_unit_byte_width);
    crc.update_little_endian(manifest.kmer_size);
    crc.update_little_endian(manifest.window_size);
    crc.update_little_endian(kMinimizerHashSemanticsVersion);
    crc.update_little_endian(
        profile && profile->segment_local_positions
            ? kMinimizerSegmentLocalPositionEncodingVersion
            : kMinimizerPositionEncodingVersion);
    crc.update_little_endian(
        profile
            ? static_cast<std::uint32_t>(
                profile->payload_layout_abi_version)
            : kMinimizerControlLayoutAbiVersion);
    crc.update_little_endian(manifest.segment_geometry_version);
    crc.update_little_endian(manifest.position_segment_span_bp);
    crc.update_little_endian(manifest.position_segment_count);
    crc.update_little_endian(manifest.gpu_abi_version);
    // Preserve the historical legacy checksum exactly.  Named non-legacy
    // schemes extend that checksum with their frozen wire identity so two
    // otherwise identical indexes cannot be accepted across scheme boundaries.
    if (manifest.minimizer_scheme_id != kLegacyMinimapV1SchemeId
        || manifest.minimizer_scheme_version
            != kLegacyMinimapV1SchemeVersion) {
        crc.update_little_endian(manifest.minimizer_scheme_id);
        crc.update_little_endian(manifest.minimizer_scheme_version);
    }
    // Revision 3 historically implied its format and must retain its exact
    // checksum. New formats bind the explicit selector and physical widths.
    if (manifest.wire_format
            != MinimizerGenerationWireFormat::sorted_dense_global64) {
        crc.update_little_endian(
            static_cast<std::uint16_t>(manifest.wire_format));
        if (profile) {
            crc.update_little_endian(profile->control_encoded_byte_width);
            crc.update_little_endian(profile->position_encoded_byte_width);
            crc.update_little_endian(profile->max_hash);
        }
    }
    return crc.checksum();
}

/// Reject a persisted generation that does not match the caller's biological
/// minimizer scheme or the fixed representation axes used to consume it.
[[nodiscard]] constexpr std::expected<void, MinimizerGenerationWireError>
validate_minimizer_generation_compatibility(
        const MinimizerGenerationManifest& manifest,
        const ExpectedMinimizerCompatibility& expected) noexcept {
    if (auto error = minimizer_generation_wire_detail::validate_manifest_standalone(
            manifest)) {
        return std::unexpected(*error);
    }
    const auto manifest_profile = minimizer_generation_payload_profile(
        manifest.wire_format,
        MinimizerSchemeIdentity{
            .id = manifest.minimizer_scheme_id,
            .version = manifest.minimizer_scheme_version,
        },
        manifest.kmer_size);
    const auto expected_profile = minimizer_generation_payload_profile(
        expected.wire_format,
        MinimizerSchemeIdentity{
            .id = expected.minimizer_scheme_id,
            .version = expected.minimizer_scheme_version,
        },
        expected.kmer_size);
    if (!manifest_profile || !expected_profile
        || *manifest_profile != *expected_profile
        || manifest.gpu_abi_version != manifest_profile->gpu_abi_version
        || expected.gpu_abi_version != expected_profile->gpu_abi_version
        || manifest.wire_format != expected.wire_format
        || manifest.alphabet_code != expected.alphabet_code
        || manifest.storage_unit_byte_width
            != expected.storage_unit_byte_width
        || manifest.kmer_size != expected.kmer_size
        || manifest.window_size != expected.window_size
        || manifest.gpu_abi_version != expected.gpu_abi_version
        || manifest.minimizer_scheme_id != expected.minimizer_scheme_id
        || manifest.minimizer_scheme_version
            != expected.minimizer_scheme_version) {
        return std::unexpected(MinimizerGenerationWireError::compatibility_mismatch);
    }
    return {};
}

/// Validate head/manifest/directory equations once all metadata objects exist.
[[nodiscard]] inline std::expected<void, MinimizerGenerationWireError>
validate_minimizer_generation_metadata(
        const MinimizerGenerationHead& head,
        const MinimizerGenerationManifest& manifest,
        const DirView& directory) noexcept {
    if (auto error = minimizer_generation_wire_detail::validate_manifest_standalone(
            manifest)) {
        return std::unexpected(*error);
    }
    const auto profile = minimizer_generation_payload_profile(
        manifest.wire_format,
        MinimizerSchemeIdentity{
            .id = manifest.minimizer_scheme_id,
            .version = manifest.minimizer_scheme_version,
        },
        manifest.kmer_size);
    if (!profile || directory.profile() != *profile) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    if (head.wire_format != manifest.wire_format) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    if (head.generation_id != manifest.generation_id
        || head.source_identity != manifest.source_identity
        || head.source_content_checksum != manifest.source_content_checksum) {
        return std::unexpected(MinimizerGenerationWireError::identity_mismatch);
    }
    const auto manifest_bytes = encode_minimizer_generation_manifest(manifest);
    if (!manifest_bytes
        || head.manifest_checksum != crc64_ecma182(
            std::span<const std::byte>{*manifest_bytes})) {
        return std::unexpected(MinimizerGenerationWireError::checksum);
    }
    if (manifest.gpu_abi_version != profile->gpu_abi_version
        || head.compatibility_checksum
            != minimizer_generation_compatibility_checksum(manifest)) {
        return std::unexpected(MinimizerGenerationWireError::compatibility_mismatch);
    }
    if (profile->segment_local_positions
        && manifest.target_length != 0
        && (manifest.position_segment_span_bp == 0
            || manifest.position_segment_span_bp
                > static_cast<std::uint64_t>(
                    kMinimizerGrouped32MaxLocalBp) + 1u)) {
        return std::unexpected(MinimizerGenerationWireError::position_range);
    }
    if (manifest.directory_bytes != directory.encoded_bytes().size()
        || manifest.directory_checksum
            != crc64_ecma182(directory.encoded_bytes())) {
        return std::unexpected(MinimizerGenerationWireError::checksum);
    }
    const std::uint64_t expected_directory_chunks =
        manifest.directory_bytes / kMinimizerExtentPayloadBytes
        + (manifest.directory_bytes % kMinimizerExtentPayloadBytes != 0 ? 1u : 0u);
    if (expected_directory_chunks != manifest.directory_chunk_count) {
        return std::unexpected(MinimizerGenerationWireError::chunk_range);
    }

    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    for (const MinimizerBucketDirEntry& entry : directory.entries()) {
        if (static_cast<std::uint64_t>(entry.position_segment)
            >= manifest.position_segment_count) {
            return std::unexpected(MinimizerGenerationWireError::position_segment);
        }
        const std::uint64_t expected_control_chunks =
            entry.control_encoded_bytes / kMinimizerExtentPayloadBytes
            + (entry.control_encoded_bytes % kMinimizerExtentPayloadBytes != 0
                ? 1u : 0u);
        const std::uint64_t expected_position_chunks =
            entry.position_encoded_bytes / kMinimizerExtentPayloadBytes
            + (entry.position_encoded_bytes % kMinimizerExtentPayloadBytes != 0
                ? 1u : 0u);
        if (entry.control_first_chunk != 0
            || entry.control_chunk_count != expected_control_chunks
            || entry.position_first_chunk != 0
            || entry.position_chunk_count != expected_position_chunks) {
            return std::unexpected(MinimizerGenerationWireError::chunk_range);
        }
        if (control_count > (std::numeric_limits<std::uint64_t>::max)()
                - entry.control_element_count
            || position_count > (std::numeric_limits<std::uint64_t>::max)()
                - entry.position_element_count
            || payload_bytes > (std::numeric_limits<std::uint64_t>::max)()
                - entry.control_encoded_bytes
            || payload_bytes + entry.control_encoded_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - entry.position_encoded_bytes) {
            return std::unexpected(MinimizerGenerationWireError::arithmetic_overflow);
        }
        control_count += entry.control_element_count;
        position_count += entry.position_element_count;
        payload_bytes += entry.control_encoded_bytes + entry.position_encoded_bytes;
    }
    if (directory.entries().size()
            > (std::numeric_limits<std::uint32_t>::max)() / 2u
        || manifest.extent_count != directory.entries().size() * 2u
        || manifest.control_count != control_count
        || manifest.position_count != position_count
        || manifest.payload_bytes != payload_bytes) {
        return std::unexpected(MinimizerGenerationWireError::element_count);
    }
    return {};
}

[[noreturn]] [[gnu::cold]] void index_corruption_panic(std::string_view msg);  // bioseq_log<DBG>+abort (NO_POSTGRES-safe)

// (1) the lookup structure — the old TargetIndex, renamed & relocated, storage-free
struct MinimizerHashmapFragmentIndex {
private:
    std::unique_ptr<detail::MinimizerHashmapFragmentIndexTable> table_;

    void clear_table();
    void reserve_table(std::size_t bucket_count);
    void emplace_table(std::uint64_t hash, std::uint64_t packed);

    template <class MinimizerT>
    void build_sorted_in_place(
            std::vector<MinimizerT>& sorted_minimizers,
            sequence_size_t target_length) {
        static const auto* _desc =
            bioseq::instr::register_region_static("target_index_build");
        auto _scope = bioseq::instr::region(_desc);
        target_len = target_length;
        clear_table();
        positions.clear();

        const std::size_t total = sorted_minimizers.size();
        if (total == 0) {
            return;
        }
        struct KmerKey {
            [[nodiscard, gnu::always_inline]]
            inline std::uint64_t operator()(const MinimizerT& m) const noexcept {
                return m.kmer();
            }
        };
        bioseq::sorting::radix_sort(
            sorted_minimizers.data(), sorted_minimizers.data() + total, KmerKey{});

        positions.resize(total);
        reserve_table(total / 2 + 16);
        std::size_t i = 0;
        while (i < total) {
            const std::uint64_t kmer = sorted_minimizers[i].kmer();
            std::size_t j = i;
            while (j < total && sorted_minimizers[j].kmer() == kmer) {
                positions[j] = sorted_minimizers[j].pos_strand_encoded();
                ++j;
            }
            emplace_table(kmer,
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32)
                | static_cast<std::uint64_t>(static_cast<std::uint32_t>(j - i)));
            i = j;
        }
    }

public:
    std::vector<std::uint64_t> positions;        // (last_pos<<1)|strand, full uint64
    sequence_size_t target_len = 0;

    MinimizerHashmapFragmentIndex();
    ~MinimizerHashmapFragmentIndex();
    MinimizerHashmapFragmentIndex(const MinimizerHashmapFragmentIndex& other);
    MinimizerHashmapFragmentIndex& operator=(const MinimizerHashmapFragmentIndex& other);
    MinimizerHashmapFragmentIndex(MinimizerHashmapFragmentIndex&& other) noexcept;
    MinimizerHashmapFragmentIndex& operator=(MinimizerHashmapFragmentIndex&& other) noexcept;

    static constexpr std::uint32_t kSerializeMagic   = 0xB1000001u;   // unchanged wire format
    static constexpr std::uint32_t kSerializeVersion = 2u;
    static constexpr std::string_view index_name     = MINIMIZERS_PER_FRAGMENT_INDEX_NAME;

    struct SerializedHeader {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint64_t target_len;       ///< v2: full 64-bit target length (corrected from the stale "int32" doc)
        std::uint32_t bucket_count;
        std::uint32_t reserved;         ///< pad; written as 0
        std::uint64_t total_positions;
    };
    struct SerializedBucket {
        std::uint64_t hash;
        std::uint32_t pos_offset;
        std::uint32_t pos_count;
    };
    static constexpr std::size_t kSerializeHeaderBytes = sizeof(SerializedHeader);
    static constexpr std::size_t kSerializeBucketBytes = sizeof(SerializedBucket);
    static_assert(kSerializeHeaderBytes == 32, "wire layout pinned (v2)");
    static_assert(kSerializeBucketBytes == 16, "wire layout pinned (v2)");
    static_assert(std::endian::native == std::endian::little, "wire format requires little-endian host");
    static_assert(sizeof(std::size_t) >= 8, "wire format requires 64-bit size_t");

    [[nodiscard]] std::size_t table_size() const noexcept;
    [[nodiscard]] bool table_empty() const noexcept;
    void for_each_table_entry_erased(
        void* ctx,
        void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const;

    template <class F>
    void for_each_table_entry(F&& emit) const {
        using Fn = std::remove_reference_t<F>;
        for_each_table_entry_erased(
            &emit,
            [](void* ctx, std::uint64_t hash, std::uint64_t packed) {
                (*static_cast<Fn*>(ctx))(hash, packed);
            });
    }

    template <class MinimizerT>
    void build(const std::vector<MinimizerT>& target_minimizers,
               sequence_size_t target_length) {
        // A call-local copy is intentional.  The old static thread_local
        // scratch retained an XXL target-sized allocation for the lifetime of
        // the worker thread, making unrelated later GPU stages pay its RSS.
        std::vector<MinimizerT> sorted_minimizers{
            target_minimizers.begin(), target_minimizers.end()};
        build_sorted_in_place(sorted_minimizers, target_length);
    }

    /// Build a transient lookup index by consuming caller-owned minimizers.
    /// Sorting happens in the caller's allocation and the vector is
    /// deterministically deallocated before this function returns.
    template <class MinimizerT>
    void build_consuming(
            std::vector<MinimizerT>&& target_minimizers,
            sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers, target_length);
        std::vector<MinimizerT>{}.swap(target_minimizers);
    }

    /// Deallocate lookup storage after the final seed consumer.  This is for
    /// an operation-owned transient index only; borrowed persisted indexes
    /// must never be passed to the post-seed lifetime coordinator.
    void release_storage() noexcept;

    /// NEW (§9.4): radix-bin over pre-extracted globalised (kmer, encoded_pos) pairs.
    /// `pairs` is MUTABLE and radix-sorted IN PLACE (caller-owned scratch; no copy).
    void build_from_pairs(std::span<std::pair<std::uint64_t,std::uint64_t>> pairs,
                          sequence_size_t target_length);

    [[nodiscard]] std::span<const std::uint64_t> lookup(std::uint64_t kmer_hash) const;

    void add_position_offset(sequence_size_t offset);

    [[nodiscard]] sequence_size_t target_length() const { return target_len; }

    [[nodiscard]] std::size_t serialize_binary_size() const;

    void serialize_to_binary(char* output) const;

    [[nodiscard]] static MinimizerHashmapFragmentIndex deserialize_from_binary(
        const char* input, std::size_t input_size);
};
static_assert(MinimizerLookupIndex<MinimizerHashmapFragmentIndex>,
              "MinimizerHashmapFragmentIndex must model MinimizerLookupIndex");
static_assert(PersistableMinimizerIndex<MinimizerHashmapFragmentIndex>,
              "MinimizerHashmapFragmentIndex must model PersistableMinimizerIndex");

/// Encode one sorted-dense control without persisting native struct layout.
[[nodiscard]] constexpr std::array<std::byte, 16>
encode_minimizer_serialized_bucket(
        const MinimizerHashmapFragmentIndex::SerializedBucket& bucket) noexcept {
    std::array<std::byte, 16> output{};
    std::span<std::byte> bytes{output};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(
        bytes, 0, bucket.hash);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        bytes, 8, bucket.pos_offset);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        bytes, 12, bucket.pos_count);
    return output;
}

/// Decode one exact sorted-dense control from fixed little-endian bytes.
[[nodiscard]] constexpr std::expected<
    MinimizerHashmapFragmentIndex::SerializedBucket,
    MinimizerGenerationWireError>
decode_minimizer_serialized_bucket(
        std::span<const std::byte> input) noexcept {
    if (input.size() != 16u) {
        return std::unexpected(MinimizerGenerationWireError::encoded_size);
    }
    return MinimizerHashmapFragmentIndex::SerializedBucket{
        .hash = minimizer_generation_wire_detail::read_le<std::uint64_t>(
            input, 0),
        .pos_offset = minimizer_generation_wire_detail::read_le<std::uint32_t>(
            input, 8),
        .pos_count = minimizer_generation_wire_detail::read_le<std::uint32_t>(
            input, 12),
    };
}

/// Logical representation of one 12-byte grouped-u32 control.
struct MinimizerGrouped32Control {
    std::uint32_t hash = 0;
    std::uint32_t position_offset = 0;
    std::uint32_t position_count = 0;

    [[nodiscard]] bool operator==(
        const MinimizerGrouped32Control&) const = default;
};

/// Reconstructed global position; keeping strand separate avoids overflowing
/// a `(global_bp << 1) | strand` intermediate for large 64-bit references.
struct MinimizerGrouped32GlobalPosition {
    std::uint64_t bp = 0;
    bool reverse_strand = false;

    [[nodiscard]] bool operator==(
        const MinimizerGrouped32GlobalPosition&) const = default;
};

/// Accept exactly the two pinned K=15 schemes whose hash widths are proven.
[[nodiscard]] constexpr std::expected<void, MinimizerGenerationWireError>
validate_minimizer_grouped32_compatibility(
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (!validate_minimizer_generation_payload_profile(profile)
        || profile.wire_format
            != MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    return {};
}

/// Narrow one scheme-qualified hash without truncation. Legacy minimap K=15
/// occupies at most 30 bits; the Paper ntHash scheme owns the full u32 range.
[[nodiscard]] constexpr std::expected<
    std::uint32_t,
    MinimizerGenerationWireError>
admit_minimizer_grouped32_hash(
        std::uint64_t hash,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (auto admitted = validate_minimizer_grouped32_compatibility(
            profile); !admitted) {
        return std::unexpected(admitted.error());
    }
    const std::uint64_t max_hash =
        profile.max_hash;
    if (hash > max_hash) {
        return std::unexpected(MinimizerGenerationWireError::hash_range);
    }
    return static_cast<std::uint32_t>(hash);
}

/// Construct one compact control from wide planner counts, rejecting every
/// narrowing or one-past-end overflow before a physical byte is produced.
[[nodiscard]] constexpr std::expected<
    MinimizerGrouped32Control,
    MinimizerGenerationWireError>
make_minimizer_grouped32_control(
        std::uint64_t hash,
        std::uint64_t position_offset,
        std::uint64_t position_count,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    const auto admitted_hash = admit_minimizer_grouped32_hash(
        hash, profile);
    if (!admitted_hash) {
        return std::unexpected(admitted_hash.error());
    }
    constexpr std::uint64_t max_u32 =
        (std::numeric_limits<std::uint32_t>::max)();
    if (position_count == 0 || position_offset > max_u32
        || position_count > max_u32
        || position_count > max_u32 - position_offset) {
        return std::unexpected(MinimizerGenerationWireError::control_range);
    }
    return MinimizerGrouped32Control{
        .hash = *admitted_hash,
        .position_offset = static_cast<std::uint32_t>(position_offset),
        .position_count = static_cast<std::uint32_t>(position_count),
    };
}

/// Encode one exact 12-byte `{u32 hash,u32 offset,u32 count}` LE control.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, kMinimizerGrouped32ControlEncodedBytes>,
    MinimizerGenerationWireError>
encode_minimizer_grouped32_control(
        const MinimizerGrouped32Control& control,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    const auto validated = make_minimizer_grouped32_control(
        control.hash,
        control.position_offset,
        control.position_count,
        profile);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    std::array<std::byte, kMinimizerGrouped32ControlEncodedBytes> output{};
    std::span<std::byte> bytes{output};
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        bytes, 0, control.hash);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        bytes, 4, control.position_offset);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        bytes, 8, control.position_count);
    return output;
}

/// Decode and qualify one compact control against its containing position
/// extent. A corrupt offset/count pair can never escape the supplied extent.
[[nodiscard]] constexpr std::expected<
    MinimizerGrouped32Control,
    MinimizerGenerationWireError>
decode_minimizer_grouped32_control(
        std::span<const std::byte> input,
        std::uint64_t position_element_count,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (input.size() != kMinimizerGrouped32ControlEncodedBytes) {
        return std::unexpected(MinimizerGenerationWireError::encoded_size);
    }
    if (auto admitted = validate_minimizer_grouped32_compatibility(
            profile); !admitted) {
        return std::unexpected(admitted.error());
    }
    constexpr std::uint64_t max_u32 =
        (std::numeric_limits<std::uint32_t>::max)();
    if (position_element_count > max_u32) {
        return std::unexpected(MinimizerGenerationWireError::control_range);
    }
    const MinimizerGrouped32Control control{
        .hash = minimizer_generation_wire_detail::read_le<std::uint32_t>(
            input, 0),
        .position_offset =
            minimizer_generation_wire_detail::read_le<std::uint32_t>(
                input, 4),
        .position_count =
            minimizer_generation_wire_detail::read_le<std::uint32_t>(
                input, 8),
    };
    if (auto admitted_hash = admit_minimizer_grouped32_hash(
            control.hash, profile); !admitted_hash) {
        return std::unexpected(admitted_hash.error());
    }
    if (control.position_count == 0
        || control.position_offset > position_element_count
        || control.position_count
            > position_element_count - control.position_offset) {
        return std::unexpected(MinimizerGenerationWireError::control_range);
    }
    return control;
}

/// Validate a half-open segment whose every local bp is representable in the
/// 31-bit local portion of the four-byte position word.
[[nodiscard]] constexpr std::expected<void, MinimizerGenerationWireError>
validate_minimizer_grouped32_segment(
        std::uint64_t segment_base_bp,
        std::uint64_t segment_end_bp) noexcept {
    if (segment_base_bp >= segment_end_bp
        || segment_end_bp - segment_base_bp
            > static_cast<std::uint64_t>(kMinimizerGrouped32MaxLocalBp) + 1u) {
        return std::unexpected(MinimizerGenerationWireError::position_range);
    }
    return {};
}

/// Convert one global position to the segment-local 31-bit bp + strand word.
[[nodiscard]] constexpr std::expected<
    std::uint32_t,
    MinimizerGenerationWireError>
localize_minimizer_grouped32_position(
        std::uint64_t global_bp,
        bool reverse_strand,
        std::uint64_t segment_base_bp,
        std::uint64_t segment_end_bp,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (auto admitted = validate_minimizer_grouped32_compatibility(
            profile); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (auto admitted_segment = validate_minimizer_grouped32_segment(
            segment_base_bp, segment_end_bp); !admitted_segment) {
        return std::unexpected(admitted_segment.error());
    }
    if (global_bp < segment_base_bp || global_bp >= segment_end_bp) {
        return std::unexpected(MinimizerGenerationWireError::position_range);
    }
    const std::uint64_t local_bp = global_bp - segment_base_bp;
    if (local_bp > kMinimizerGrouped32MaxLocalBp) {
        return std::unexpected(MinimizerGenerationWireError::position_range);
    }
    return static_cast<std::uint32_t>(
        (local_bp << 1u) | static_cast<std::uint64_t>(reverse_strand));
}

/// Encode one already-localized position as exactly four little-endian bytes.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, kMinimizerGrouped32PositionEncodedBytes>,
    MinimizerGenerationWireError>
encode_minimizer_grouped32_position(
        std::uint32_t encoded_local_position,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (auto admitted = validate_minimizer_grouped32_compatibility(
            profile); !admitted) {
        return std::unexpected(admitted.error());
    }
    std::array<std::byte, kMinimizerGrouped32PositionEncodedBytes> output{};
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        std::span<std::byte>{output}, 0, encoded_local_position);
    return output;
}

/// Globalize one already-native segment-local position word. Strand remains a
/// separate field, so references near UINT64_MAX never construct a shifted
/// global position and no byte re-encoding is involved.
[[nodiscard]] constexpr std::expected<
    MinimizerGrouped32GlobalPosition,
    MinimizerGenerationWireError>
globalize_minimizer_grouped32_local_position(
        std::uint32_t encoded_local_position,
        std::uint64_t segment_base_bp,
        std::uint64_t segment_end_bp) noexcept {
    if (auto admitted_segment = validate_minimizer_grouped32_segment(
            segment_base_bp, segment_end_bp); !admitted_segment) {
        return std::unexpected(admitted_segment.error());
    }
    const std::uint64_t local_bp = encoded_local_position >> 1u;
    if (local_bp
        > (std::numeric_limits<std::uint64_t>::max)() - segment_base_bp) {
        return std::unexpected(
            MinimizerGenerationWireError::arithmetic_overflow);
    }
    const std::uint64_t global_bp = segment_base_bp + local_bp;
    if (global_bp >= segment_end_bp) {
        return std::unexpected(MinimizerGenerationWireError::position_range);
    }
    return MinimizerGrouped32GlobalPosition{
        .bp = global_bp,
        .reverse_strand = (encoded_local_position & 1u) != 0u,
    };
}

/// Decode and reconstruct one segment-local word with checked u64 addition and
/// an independent half-open segment bound. This supports references above 4G.
[[nodiscard]] constexpr std::expected<
    MinimizerGrouped32GlobalPosition,
    MinimizerGenerationWireError>
decode_minimizer_grouped32_position(
        std::span<const std::byte> input,
        std::uint64_t segment_base_bp,
        std::uint64_t segment_end_bp,
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    if (input.size() != kMinimizerGrouped32PositionEncodedBytes) {
        return std::unexpected(MinimizerGenerationWireError::encoded_size);
    }
    if (auto admitted = validate_minimizer_grouped32_compatibility(
            profile); !admitted) {
        return std::unexpected(admitted.error());
    }
    const std::uint32_t encoded_local_position =
        minimizer_generation_wire_detail::read_le<std::uint32_t>(input, 0);
    return globalize_minimizer_grouped32_local_position(
        encoded_local_position, segment_base_bp, segment_end_bp);
}

/// Diagnostic query result from the grouped32 reader. Positions use the normal
/// minimizer-index ABI `(global_bp << 1) | strand`. An owned absent hash has
/// count zero and complete positions; a globally pruned hash has a nonzero
/// count, incomplete positions, and an empty span.
struct PreparedGrouped32QueryLookup {
    std::uint64_t global_occurrence_count = 0;
    bool positions_complete = true;
    std::span<const std::uint64_t> positions{};
};

/// Per-reader proof emitted by the production grouped32 validation operation.
/// Counters are reader-owned, never global, and therefore preserve isolation
/// between concurrent cold-index loads.
struct PreparedGrouped32QueryReaderSimdRouteEvidence {
    std::uint64_t attempted_spans = 0;
    std::uint64_t accepted_spans = 0;
    std::uint64_t rejected_spans = 0;
    std::uint64_t scalar_required_spans = 0;
    std::uint64_t vectorized_words = 0;
    std::uint64_t selected_hwy_target = 0;
    std::uint8_t selected_vector_stream_width = 0;
    bool saturated = false;
};

/// Inactive two-phase query reader for the grouped-u32/segment-local32 wire.
/// It owns every byte and identity field used after `begin`: callers first
/// deliver the internally-selected control extents, then only the authorized
/// position extents which survived the target-wide occurrence policy.
class PreparedGrouped32QueryReader {
private:
    enum class Phase : std::uint8_t {
        controls,
        positions,
        ready,
        failed,
    };

    struct ExtentLoad {
        std::uint32_t directory_entry_ordinal = 0;
        std::uint32_t next_chunk_no = 0;
        Crc64Ecma182 crc{};
        std::unique_ptr<std::byte[]> encoded;
        std::size_t encoded_size = 0;
        std::size_t encoded_capacity = 0;
    };

    struct QueryRecord {
        std::uint32_t hash = 0;
        std::uint64_t global_occurrence_count = 0;
        std::uint64_t positions_begin = 0;
        std::uint64_t position_count = 0;
    };

    static constexpr std::size_t no_query =
        (std::numeric_limits<std::size_t>::max)();

    enum class NonScatterSimdValidation : std::uint8_t {
        scalar_required,
        accepted,
        rejected,
    };

    struct NonScatterSimdValidationResult {
        NonScatterSimdValidation validation =
            NonScatterSimdValidation::scalar_required;
        std::uint64_t vectorized_words = 0;
        std::uint64_t selected_hwy_target = 0;
        std::uint8_t selected_vector_stream_width = 0;
    };

    /// Implemented in the existing module implementation unit so Highway and
    /// vector_stream inline definitions never leak through this exported class
    /// definition into importers such as the PostgreSQL module aggregate.
    [[nodiscard]] static NonScatterSimdValidationResult
    validate_non_scatter_local32_w2(
        std::span<const std::byte> encoded_bytes,
        const MinimizerGrouped32Control& control,
        const MinimizerPositionSegmentCore& core) noexcept;

    // The permit is declared first and therefore released after all owned
    // containers. Phase transitions shrink it only after transient release.
    bioseq::parallelism::MemoryPermit permit_;
    DirView directory_;
    MinimizerGenerationHead head_{};
    MinimizerGenerationManifest manifest_{};
    ExpectedMinimizerCompatibility expected_{};
    MinimizerGenerationPayloadProfile profile_{};
    MinimizerSketchBackend backend_ = MinimizerSketchBackend::Scalar;
    std::vector<std::uint32_t> query_hashes_;
    std::vector<std::uint32_t> control_ordinals_;
    std::vector<ExtentLoad> control_extents_;
    std::vector<MinimizerGrouped32Control> decoded_controls_;
    std::vector<std::uint64_t> decoded_control_begins_;
    std::vector<std::size_t> decoded_control_query_indices_;
    std::vector<QueryRecord> query_records_;
    std::vector<std::uint32_t> retained_position_ordinals_;
    std::vector<ExtentLoad> position_extents_;
    std::unique_ptr<std::uint64_t[]> positions_;
    std::size_t position_count_ = 0;
    std::vector<std::uint64_t> position_write_cursors_;
    std::size_t extent_cursor_ = 0;
    std::uint64_t max_occ_ = 0;
    PreparedGrouped32QueryReaderSimdRouteEvidence simd_route_evidence_{};
    MinimizerGenerationWireError failure_ =
        MinimizerGenerationWireError::chunk_range;
    Phase phase_ = Phase::controls;

    PreparedGrouped32QueryReader() = default;

    template <class T>
    [[nodiscard]] std::expected<T, MinimizerGenerationWireError> poison(
            MinimizerGenerationWireError error) noexcept {
        if (phase_ != Phase::failed) {
            phase_ = Phase::failed;
            failure_ = error;
        }
        return std::unexpected(failure_);
    }

    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        using bioseq::parallelism::saturating_add;
        using bioseq::parallelism::saturating_mul;
        std::uint64_t bytes = directory_.owned_capacity_bytes();
        const auto add_vector = [&bytes](std::size_t capacity,
                                         std::size_t width) noexcept {
            bytes = saturating_add(
                bytes, saturating_mul(
                    static_cast<std::uint64_t>(capacity), width));
        };
        add_vector(query_hashes_.capacity(), sizeof(std::uint32_t));
        add_vector(control_ordinals_.capacity(), sizeof(std::uint32_t));
        add_vector(control_extents_.capacity(), sizeof(ExtentLoad));
        for (const ExtentLoad& extent : control_extents_) {
            add_vector(extent.encoded_capacity, sizeof(std::byte));
        }
        add_vector(decoded_controls_.capacity(),
                   sizeof(MinimizerGrouped32Control));
        add_vector(decoded_control_begins_.capacity(), sizeof(std::uint64_t));
        add_vector(decoded_control_query_indices_.capacity(),
                   sizeof(std::size_t));
        add_vector(query_records_.capacity(), sizeof(QueryRecord));
        add_vector(retained_position_ordinals_.capacity(),
                   sizeof(std::uint32_t));
        add_vector(position_extents_.capacity(), sizeof(ExtentLoad));
        for (const ExtentLoad& extent : position_extents_) {
            add_vector(extent.encoded_capacity, sizeof(std::byte));
        }
        add_vector(position_count_, sizeof(std::uint64_t));
        add_vector(position_write_cursors_.capacity(), sizeof(std::uint64_t));
        return bytes;
    }

    [[nodiscard]] bool admit_owned_capacity() noexcept {
        const std::uint64_t bytes = owned_capacity_bytes();
        return bytes != (std::numeric_limits<std::uint64_t>::max)()
            && permit_.grow_to(bytes);
    }

    [[nodiscard]] static bool query_intersects(
            const std::vector<std::uint32_t>& queries,
            const MinimizerBucketDirEntry& entry) noexcept {
        const auto query = std::lower_bound(
            queries.begin(), queries.end(),
            static_cast<std::uint32_t>(entry.lo_hash));
        return query != queries.end()
            && static_cast<std::uint64_t>(*query) <= entry.hi_hash;
    }

    [[nodiscard]] const MinimizerBucketDirEntry* current_entry(
            bool positions) const noexcept {
        const auto& extents = positions ? position_extents_ : control_extents_;
        return extent_cursor_ < extents.size()
            ? directory_.entry_by_ordinal(
                extents[extent_cursor_].directory_entry_ordinal)
            : nullptr;
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    accept_extent_chunk(
            bool positions,
            const IndexChunkAddress& address,
            std::span<const std::byte> payload) noexcept {
        const Phase wanted = positions ? Phase::positions : Phase::controls;
        if (phase_ == Phase::failed) {
            return std::unexpected(failure_);
        }
        auto& extents = positions ? position_extents_ : control_extents_;
        if (phase_ != wanted || extent_cursor_ >= extents.size()) {
            return poison<void>(MinimizerGenerationWireError::chunk_range);
        }
        ExtentLoad& load = extents[extent_cursor_];
        const MinimizerBucketDirEntry* entry = current_entry(positions);
        if (entry == nullptr) {
            return poison<void>(MinimizerGenerationWireError::partition_id);
        }
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry->sub_key);
        if (!position_sub_key) {
            return poison<void>(position_sub_key.error());
        }
        const index_sub_key_t wanted_sub_key = positions
            ? *position_sub_key : entry->sub_key;
        const std::uint32_t wanted_chunks = positions
            ? entry->position_chunk_count : entry->control_chunk_count;
        const std::uint64_t wanted_bytes = positions
            ? entry->position_encoded_bytes : entry->control_encoded_bytes;
        if (address.generation_id != head_.generation_id
            || address.position_segment != entry->position_segment
            || address.sub_key != wanted_sub_key
            || address.chunk_no != load.next_chunk_no
            || load.next_chunk_no >= wanted_chunks) {
            return poison<void>(MinimizerGenerationWireError::chunk_range);
        }
        const std::uint64_t byte_offset =
            static_cast<std::uint64_t>(load.next_chunk_no)
            * kMinimizerExtentPayloadBytes;
        if (byte_offset >= wanted_bytes) {
            return poison<void>(MinimizerGenerationWireError::chunk_range);
        }
        const std::uint64_t expected_bytes = std::min<std::uint64_t>(
            kMinimizerExtentPayloadBytes, wanted_bytes - byte_offset);
        if (payload.size() != expected_bytes
            || byte_offset != load.encoded_size
            || byte_offset > load.encoded_capacity
            || payload.size() > load.encoded_capacity - byte_offset
            || load.encoded == nullptr) {
            return poison<void>(MinimizerGenerationWireError::byte_count);
        }
        load.crc.update(payload);
        std::memcpy(
            load.encoded.get() + static_cast<std::size_t>(byte_offset),
            payload.data(), payload.size());
        load.encoded_size += payload.size();
        ++load.next_chunk_no;
        if (load.next_chunk_no == wanted_chunks) {
            const std::uint64_t expected_checksum = positions
                ? entry->position_checksum : entry->control_checksum;
            if (load.crc.checksum() != expected_checksum) {
                return poison<void>(MinimizerGenerationWireError::checksum);
            }
            ++extent_cursor_;
        }
        return {};
    }

public:
    PreparedGrouped32QueryReader(
            PreparedGrouped32QueryReader&& other) noexcept
        : permit_(std::move(other.permit_)),
          directory_(std::move(other.directory_)),
          head_(other.head_),
          manifest_(other.manifest_),
          expected_(other.expected_),
          profile_(other.profile_),
          backend_(other.backend_),
          query_hashes_(std::move(other.query_hashes_)),
          control_ordinals_(std::move(other.control_ordinals_)),
          control_extents_(std::move(other.control_extents_)),
          decoded_controls_(std::move(other.decoded_controls_)),
          decoded_control_begins_(
              std::move(other.decoded_control_begins_)),
          decoded_control_query_indices_(
              std::move(other.decoded_control_query_indices_)),
          query_records_(std::move(other.query_records_)),
          retained_position_ordinals_(
              std::move(other.retained_position_ordinals_)),
          position_extents_(std::move(other.position_extents_)),
          positions_(std::move(other.positions_)),
          position_count_(other.position_count_),
          position_write_cursors_(
              std::move(other.position_write_cursors_)),
          extent_cursor_(other.extent_cursor_),
          max_occ_(other.max_occ_),
          simd_route_evidence_(other.simd_route_evidence_),
          failure_(other.failure_),
          phase_(other.phase_) {
        other.head_ = {};
        other.manifest_ = {};
        other.expected_ = {};
        other.profile_ = {};
        other.backend_ = MinimizerSketchBackend::Scalar;
        other.position_count_ = 0;
        other.extent_cursor_ = 0;
        other.max_occ_ = 0;
        other.simd_route_evidence_ = {};
        other.failure_ = MinimizerGenerationWireError::chunk_range;
        other.phase_ = Phase::failed;
    }
    PreparedGrouped32QueryReader& operator=(
        PreparedGrouped32QueryReader&&) = delete;
    PreparedGrouped32QueryReader(const PreparedGrouped32QueryReader&) = delete;
    PreparedGrouped32QueryReader& operator=(
        const PreparedGrouped32QueryReader&) = delete;

    /// Validate and own one complete persisted identity and exact query union.
    /// No caller-supplied partition ordinal participates in read authority.
    [[nodiscard]] static std::expected<
        PreparedGrouped32QueryReader,
        MinimizerGenerationWireError>
    begin(
            bioseq::parallelism::MemoryAdmissionController& admission,
            const MinimizerGenerationHead& head,
            const MinimizerGenerationManifest& manifest,
            const ExpectedMinimizerCompatibility& expected,
            MinimizerSketchBackend backend,
            std::span<const std::byte> encoded_directory,
            std::span<const std::uint64_t> query_hashes,
            std::uint64_t max_occ) {
        // Final positions use the ordinary minimizer-index ABI
        // `(global_bp << 1) | strand`.  PostgreSQL source identities are
        // restricted to non-negative BIGINT lengths, which also guarantees
        // that every valid bp (< target_length) can be shifted without u64
        // overflow. Reject a wider manifest before acquiring any ownership.
        if (!sql_bigint_can_represent(manifest.target_length)) {
            return std::unexpected(MinimizerGenerationWireError::bp_range);
        }
        if (auto compatible = validate_minimizer_generation_compatibility(
                manifest, expected); !compatible) {
            return std::unexpected(compatible.error());
        }
        const MinimizerSchemeIdentity backend_scheme =
            minimizer_scheme_identity_for_backend(backend);
        if (backend_scheme.id != expected.minimizer_scheme_id
            || backend_scheme.version != expected.minimizer_scheme_version) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        const auto profile = minimizer_generation_payload_profile(
            expected.wire_format, backend_scheme, expected.kmer_size);
        if (!profile || !validate_minimizer_grouped32_compatibility(*profile)) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        if (encoded_directory.size()
                % MinimizerBucketDirEntry::encoded_bytes != 0
            || query_hashes.size()
                > (std::numeric_limits<std::size_t>::max)()
                    / sizeof(std::uint32_t)) {
            return std::unexpected(
                MinimizerGenerationWireError::directory_size);
        }
        for (const std::uint64_t hash : query_hashes) {
            if (!admit_minimizer_grouped32_hash(hash, *profile)) {
                return std::unexpected(MinimizerGenerationWireError::hash_range);
            }
        }

        const std::size_t entry_count = encoded_directory.size()
            / MinimizerBucketDirEntry::encoded_bytes;
        const std::uint64_t logical_directory_bytes =
            bioseq::parallelism::saturating_add(
                encoded_directory.size(),
                bioseq::parallelism::saturating_mul(
                    entry_count, sizeof(MinimizerBucketDirEntry)));
        const std::uint64_t base_bytes =
            bioseq::parallelism::saturating_add(
                logical_directory_bytes,
                bioseq::parallelism::saturating_mul(
                    query_hashes.size(), sizeof(std::uint32_t)));
        if (base_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        auto permit = admission.try_reserve(base_bytes);
        if (!permit) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }
        auto directory = DirView::decode_admitted(
            encoded_directory, *profile, *permit);
        if (!directory) {
            return std::unexpected(directory.error());
        }
        if (auto metadata = validate_minimizer_generation_metadata(
                head, manifest, *directory); !metadata) {
            return std::unexpected(metadata.error());
        }

        PreparedGrouped32QueryReader reader;
        reader.permit_ = std::move(*permit);
        reader.directory_ = std::move(*directory);
        reader.head_ = head;
        reader.manifest_ = manifest;
        reader.expected_ = expected;
        reader.profile_ = *profile;
        reader.backend_ = backend;
        reader.max_occ_ = max_occ;

        // The raw caller sequence is a transient sort/dedup buffer. Reserve and
        // reconcile its actual capacity before touching it, then replace it
        // with exact canonical ownership so duplicate-heavy input cannot retain
        // phantom admission throughout cold I/O.
        reader.query_hashes_.reserve(query_hashes.size());
        if (!reader.admit_owned_capacity()) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }
        for (const std::uint64_t hash : query_hashes) {
            reader.query_hashes_.push_back(static_cast<std::uint32_t>(hash));
        }
        std::ranges::sort(reader.query_hashes_);
        reader.query_hashes_.erase(
            std::unique(reader.query_hashes_.begin(),
                        reader.query_hashes_.end()),
            reader.query_hashes_.end());
        {
            std::vector<std::uint32_t> canonical;
            std::uint64_t canonical_peak =
                bioseq::parallelism::saturating_add(
                    reader.owned_capacity_bytes(),
                    bioseq::parallelism::saturating_mul(
                        reader.query_hashes_.size(),
                        sizeof(std::uint32_t)));
            if (canonical_peak
                    == (std::numeric_limits<std::uint64_t>::max)()
                || !reader.permit_.grow_to(canonical_peak)) {
                return std::unexpected(
                    MinimizerGenerationWireError::admission);
            }
            canonical.reserve(reader.query_hashes_.size());
            canonical_peak = bioseq::parallelism::saturating_add(
                reader.owned_capacity_bytes(),
                bioseq::parallelism::saturating_mul(
                    canonical.capacity(), sizeof(std::uint32_t)));
            if (canonical_peak
                    == (std::numeric_limits<std::uint64_t>::max)()
                || !reader.permit_.grow_to(canonical_peak)) {
                return std::unexpected(
                    MinimizerGenerationWireError::admission);
            }
            canonical.assign(
                reader.query_hashes_.begin(), reader.query_hashes_.end());
            reader.query_hashes_.swap(canonical);
        }
        reader.permit_.shrink_to(reader.owned_capacity_bytes());

        std::size_t selected_extent_count = 0;
        std::uint64_t total_controls = 0;
        for (const MinimizerBucketDirEntry& entry
                : reader.directory_.entries()) {
            if (!query_intersects(reader.query_hashes_, entry)) {
                continue;
            }
            if (total_controls
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.control_element_count) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            total_controls += entry.control_element_count;
            ++selected_extent_count;
        }
        if (total_controls > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }

        std::uint64_t projected = reader.owned_capacity_bytes();
        const auto add_projected = [&projected](
                std::uint64_t count, std::uint64_t width) noexcept {
            projected = bioseq::parallelism::saturating_add(
                projected,
                bioseq::parallelism::saturating_mul(count, width));
        };
        add_projected(
            reader.query_hashes_.size(), sizeof(QueryRecord));
        add_projected(selected_extent_count, sizeof(std::uint32_t));
        add_projected(selected_extent_count, sizeof(ExtentLoad));
        add_projected(total_controls, sizeof(MinimizerGrouped32Control));
        add_projected(total_controls, sizeof(std::size_t));
        add_projected(selected_extent_count + 1u, sizeof(std::uint64_t));
        add_projected(selected_extent_count, sizeof(std::uint32_t));
        add_projected(selected_extent_count, sizeof(ExtentLoad));
        add_projected(reader.query_hashes_.size(), sizeof(std::uint64_t));
        if (projected == (std::numeric_limits<std::uint64_t>::max)()
            || !reader.permit_.grow_to(projected)) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }

        reader.query_records_.reserve(reader.query_hashes_.size());
        reader.control_ordinals_.reserve(selected_extent_count);
        reader.control_extents_.reserve(selected_extent_count);
        reader.decoded_controls_.reserve(
            static_cast<std::size_t>(total_controls));
        reader.decoded_control_begins_.reserve(selected_extent_count + 1u);
        reader.decoded_control_query_indices_.reserve(
            static_cast<std::size_t>(total_controls));
        reader.retained_position_ordinals_.reserve(selected_extent_count);
        reader.position_extents_.reserve(selected_extent_count);
        reader.position_write_cursors_.reserve(reader.query_hashes_.size());
        if (!reader.admit_owned_capacity()) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }

        for (const std::uint32_t hash : reader.query_hashes_) {
            reader.query_records_.push_back(QueryRecord{.hash = hash});
        }
        std::uint64_t control_payload_bytes = 0;
        for (const MinimizerBucketDirEntry& entry
                : reader.directory_.entries()) {
            if (!query_intersects(reader.query_hashes_, entry)) {
                continue;
            }
            reader.control_ordinals_.push_back(entry.partition_id);
            control_payload_bytes =
                bioseq::parallelism::saturating_add(
                    control_payload_bytes,
                    entry.control_encoded_bytes);
        }
        const std::uint64_t payload_peak =
            bioseq::parallelism::saturating_add(
                reader.owned_capacity_bytes(), control_payload_bytes);
        if (control_payload_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || payload_peak == (std::numeric_limits<std::uint64_t>::max)()
            || !reader.permit_.grow_to(payload_peak)) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }
        for (const std::uint32_t ordinal : reader.control_ordinals_) {
            const MinimizerBucketDirEntry& entry =
                *reader.directory_.entry_by_ordinal(ordinal);
            const auto encoded_bytes = static_cast<std::size_t>(
                entry.control_encoded_bytes);
            ExtentLoad load{
                .directory_entry_ordinal = ordinal,
                .encoded = std::make_unique_for_overwrite<std::byte[]>(
                    encoded_bytes),
                .encoded_capacity = encoded_bytes,
            };
            reader.control_extents_.push_back(std::move(load));
        }
        const std::uint64_t owned_bytes = reader.owned_capacity_bytes();
        if (owned_bytes == (std::numeric_limits<std::uint64_t>::max)()
            || owned_bytes > reader.permit_.bytes()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        reader.permit_.shrink_to(owned_bytes);
        return reader;
    }

    /// Exact internally-derived control extent ordinals in callback order.
    [[nodiscard]] std::span<const std::uint32_t> control_ordinals()
            const noexcept {
        return phase_ == Phase::failed
            ? std::span<const std::uint32_t>{}
            : std::span<const std::uint32_t>{control_ordinals_};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    accept_control_chunk(
            const IndexChunkAddress& address,
            std::span<const std::byte> payload) noexcept {
        return accept_extent_chunk(false, address, payload);
    }

    /// Decode all controls, merge target-wide counts, and authorize the exact
    /// subset of position extents needed by globally retained query hashes.
    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    finish_controls();
    /// Exact internally-authorized position extent ordinals in callback order.
    [[nodiscard]] std::span<const std::uint32_t>
    retained_position_ordinals() const noexcept {
        return phase_ == Phase::positions || phase_ == Phase::ready
            ? std::span<const std::uint32_t>{retained_position_ordinals_}
            : std::span<const std::uint32_t>{};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    accept_position_chunk(
            const IndexChunkAddress& address,
            std::span<const std::byte> payload) noexcept {
        return accept_extent_chunk(true, address, payload);
    }

    /// Validate every word in every authorized full position extent, scatter
    /// only globally retained query slices, then publish immutable lookups.
    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    finish_load();
    [[nodiscard]] std::expected<
        PreparedGrouped32QueryLookup,
        MinimizerGenerationWireError>
    lookup(std::uint64_t hash) noexcept {
        if (phase_ == Phase::failed) {
            return std::unexpected(failure_);
        }
        if (phase_ != Phase::ready) {
            return poison<PreparedGrouped32QueryLookup>(
                MinimizerGenerationWireError::chunk_range);
        }
        const auto admitted = admit_minimizer_grouped32_hash(hash, profile_);
        if (!admitted) {
            return poison<PreparedGrouped32QueryLookup>(admitted.error());
        }
        const auto found = std::lower_bound(
            query_records_.begin(), query_records_.end(), *admitted,
            [](const QueryRecord& record, std::uint32_t wanted) noexcept {
                return record.hash < wanted;
            });
        if (found == query_records_.end() || found->hash != *admitted) {
            return poison<PreparedGrouped32QueryLookup>(
                MinimizerGenerationWireError::hash_range);
        }
        const bool complete = found->global_occurrence_count == 0
            || found->global_occurrence_count <= max_occ_;
        const auto position_begin = static_cast<std::size_t>(
            found->positions_begin);
        const auto position_count = static_cast<std::size_t>(
            found->position_count);
        return PreparedGrouped32QueryLookup{
            .global_occurrence_count = found->global_occurrence_count,
            .positions_complete = complete,
            .positions = position_count == 0
                ? std::span<const std::uint64_t>{}
                : std::span<const std::uint64_t>{
                    positions_.get() + position_begin, position_count},
        };
    }

    [[nodiscard]] sequence_size_t target_length() const noexcept {
        return manifest_.target_length;
    }

    /// Direct seed-consumer surface. Only hashes owned by the immutable query
    /// union may be observed; confusing an unprepared hash with an authenticated
    /// absent hash would silently hide a planner/reader mismatch, so it throws.
    [[nodiscard]] MinimizerLookupSpans lookup_spans(
            std::uint64_t hash) const {
        if (phase_ != Phase::ready) {
            throw PersistedMinimizerLookupError(
                "grouped32 persisted lookup is not ready");
        }
        const auto admitted = admit_minimizer_grouped32_hash(hash, profile_);
        if (!admitted) {
            throw PersistedMinimizerLookupError(
                "grouped32 persisted lookup hash is outside its profile");
        }
        const auto found = std::lower_bound(
            query_records_.begin(), query_records_.end(), *admitted,
            [](const QueryRecord& record, std::uint32_t wanted) noexcept {
                return record.hash < wanted;
            });
        if (found == query_records_.end() || found->hash != *admitted) {
            throw PersistedMinimizerLookupError(
                "grouped32 persisted lookup hash was not prepared");
        }
        if (found->global_occurrence_count == 0) {
            return MinimizerLookupSpans::contiguous(
                std::span<const std::uint64_t>{});
        }
        if (found->global_occurrence_count > max_occ_) {
            return MinimizerLookupSpans::positions_unavailable(
                found->global_occurrence_count);
        }
        if (positions_ == nullptr
            || found->position_count != found->global_occurrence_count
            || found->positions_begin > position_count_
            || found->position_count
                > static_cast<std::uint64_t>(position_count_)
                    - found->positions_begin) {
            throw PersistedMinimizerLookupError(
                "grouped32 persisted lookup retained span is inconsistent");
        }
        return MinimizerLookupSpans::contiguous(
            std::span<const std::uint64_t>{
                positions_.get()
                    + static_cast<std::size_t>(found->positions_begin),
                static_cast<std::size_t>(found->position_count)});
    }

    /// This qualifies persisted sketch semantics only. It is deliberately not
    /// accelerator-execution attestation; host decode is part of this reader.
    [[nodiscard]] MinimizerSketchBackend qualified_sketch_backend()
            const noexcept {
        return backend_;
    }

    /// Operation-originated proof of the route selected by `finish_load()`.
    [[nodiscard]] const PreparedGrouped32QueryReaderSimdRouteEvidence&
    simd_route_evidence() const noexcept {
        return simd_route_evidence_;
    }

    [[nodiscard]] bool ready() const noexcept {
        return phase_ == Phase::ready;
    }

    [[nodiscard]] bool failed() const noexcept {
        return phase_ == Phase::failed;
    }
};

inline std::expected<void, MinimizerGenerationWireError>
PreparedGrouped32QueryReader::finish_controls() {
    if (phase_ == Phase::failed) {
        return std::unexpected(failure_);
    }
    if (phase_ != Phase::controls
        || extent_cursor_ != control_extents_.size()) {
        return poison<void>(MinimizerGenerationWireError::chunk_range);
    }

    decoded_control_begins_.push_back(0u);
    for (const ExtentLoad& load : control_extents_) {
        const MinimizerBucketDirEntry* entry =
            directory_.entry_by_ordinal(load.directory_entry_ordinal);
        if (entry == nullptr
            || load.next_chunk_no != entry->control_chunk_count
            || load.encoded_size != entry->control_encoded_bytes
            || load.encoded_capacity != entry->control_encoded_bytes
            || load.encoded == nullptr) {
            return poison<void>(MinimizerGenerationWireError::chunk_range);
        }
        std::optional<std::uint32_t> previous_hash;
        std::uint64_t expected_position_offset = 0;
        for (std::uint64_t control_index = 0;
             control_index < entry->control_element_count;
             ++control_index) {
            const std::uint64_t byte_offset = control_index
                * kMinimizerGrouped32ControlEncodedBytes;
            if (byte_offset > load.encoded_size
                || kMinimizerGrouped32ControlEncodedBytes
                    > load.encoded_size - byte_offset) {
                return poison<void>(
                    MinimizerGenerationWireError::byte_count);
            }
            const auto control = decode_minimizer_grouped32_control(
                std::span<const std::byte>{
                    load.encoded.get()
                        + static_cast<std::size_t>(byte_offset),
                    kMinimizerGrouped32ControlEncodedBytes},
                entry->position_element_count,
                profile_);
            if (!control) {
                return poison<void>(control.error());
            }
            if ((previous_hash && control->hash <= *previous_hash)
                || control->hash < entry->lo_hash
                || control->hash > entry->hi_hash
                || control->position_offset != expected_position_offset) {
                return poison<void>(
                    MinimizerGenerationWireError::control_range);
            }
            previous_hash = control->hash;
            expected_position_offset =
                static_cast<std::uint64_t>(control->position_offset)
                + control->position_count;
            decoded_controls_.push_back(*control);
            const auto query = std::lower_bound(
                query_hashes_.begin(), query_hashes_.end(), control->hash);
            const std::size_t query_index =
                query != query_hashes_.end() && *query == control->hash
                ? static_cast<std::size_t>(query - query_hashes_.begin())
                : no_query;
            decoded_control_query_indices_.push_back(query_index);
            if (query_index != no_query) {
                QueryRecord& record = query_records_[query_index];
                if (record.global_occurrence_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - control->position_count) {
                    return poison<void>(
                        MinimizerGenerationWireError::arithmetic_overflow);
                }
                record.global_occurrence_count += control->position_count;
            }
        }
        if (!previous_hash
            || *previous_hash != entry->hi_hash
            || decoded_controls_[static_cast<std::size_t>(
                    decoded_control_begins_.back())].hash != entry->lo_hash
            || expected_position_offset != entry->position_element_count) {
            return poison<void>(
                MinimizerGenerationWireError::control_range);
        }
        decoded_control_begins_.push_back(decoded_controls_.size());
    }
    if (decoded_control_begins_.size() != control_ordinals_.size() + 1u
        || decoded_control_query_indices_.size()
            != decoded_controls_.size()) {
        return poison<void>(MinimizerGenerationWireError::element_count);
    }

    // Every selected partition whose range can contain a query has now
    // contributed directly to that query's checked u64 global count. This is
    // linear in decoded controls and avoids an all-control k-way heap.
    std::vector<ExtentLoad>{}.swap(control_extents_);
    permit_.shrink_to(owned_capacity_bytes());

    std::uint64_t total_retained_positions = 0;
    for (QueryRecord& record : query_records_) {
        record.positions_begin = total_retained_positions;
        record.position_count = record.global_occurrence_count != 0
                && record.global_occurrence_count <= max_occ_
            ? record.global_occurrence_count : 0u;
        if (total_retained_positions
                > (std::numeric_limits<std::uint64_t>::max)()
                    - record.position_count) {
            return poison<void>(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        total_retained_positions += record.position_count;
    }
    if (total_retained_positions
        > (std::numeric_limits<std::size_t>::max)()) {
        return poison<void>(
            MinimizerGenerationWireError::arithmetic_overflow);
    }

    for (std::size_t extent = 0;
         extent < control_ordinals_.size(); ++extent) {
        bool contains_retained_query = false;
        for (std::uint64_t index = decoded_control_begins_[extent];
             index < decoded_control_begins_[extent + 1u]; ++index) {
            const std::size_t query = decoded_control_query_indices_[
                static_cast<std::size_t>(index)];
            if (query != no_query
                && query_records_[query].position_count != 0) {
                contains_retained_query = true;
                break;
            }
        }
        if (contains_retained_query) {
            retained_position_ordinals_.push_back(
                control_ordinals_[extent]);
        }
    }

    std::uint64_t projected = owned_capacity_bytes();
    projected = bioseq::parallelism::saturating_add(
        projected,
        bioseq::parallelism::saturating_mul(
            total_retained_positions,
            sizeof(std::uint64_t)));
    for (const std::uint32_t ordinal : retained_position_ordinals_) {
        const MinimizerBucketDirEntry* entry =
            directory_.entry_by_ordinal(ordinal);
        if (entry == nullptr
            || entry->position_encoded_bytes
                > (std::numeric_limits<std::size_t>::max)()) {
            return poison<void>(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        projected = bioseq::parallelism::saturating_add(
            projected, entry->position_encoded_bytes);
    }
    if (projected == (std::numeric_limits<std::uint64_t>::max)()
        || !permit_.grow_to(projected)) {
        return poison<void>(MinimizerGenerationWireError::admission);
    }

    position_count_ = static_cast<std::size_t>(total_retained_positions);
    if (position_count_ != 0) {
        positions_ = std::make_unique_for_overwrite<std::uint64_t[]>(
            position_count_);
    }
    position_write_cursors_.clear();
    for (const QueryRecord& record : query_records_) {
        position_write_cursors_.push_back(record.positions_begin);
    }
    for (const std::uint32_t ordinal : retained_position_ordinals_) {
        const MinimizerBucketDirEntry& entry =
            *directory_.entry_by_ordinal(ordinal);
        const auto encoded_bytes = static_cast<std::size_t>(
            entry.position_encoded_bytes);
        ExtentLoad load{
            .directory_entry_ordinal = ordinal,
            .encoded = std::make_unique_for_overwrite<std::byte[]>(
                encoded_bytes),
            .encoded_capacity = encoded_bytes,
        };
        position_extents_.push_back(std::move(load));
    }
    const std::uint64_t owned_bytes = owned_capacity_bytes();
    if (owned_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || owned_bytes > permit_.bytes()) {
        return poison<void>(
            MinimizerGenerationWireError::arithmetic_overflow);
    }
    permit_.shrink_to(owned_bytes);

    extent_cursor_ = 0;
    phase_ = Phase::positions;
    return {};
}

inline std::expected<void, MinimizerGenerationWireError>
PreparedGrouped32QueryReader::finish_load() {
    if (phase_ == Phase::failed) {
        return std::unexpected(failure_);
    }
    if (phase_ != Phase::positions
        || extent_cursor_ != position_extents_.size()) {
        return poison<void>(MinimizerGenerationWireError::chunk_range);
    }

    for (const ExtentLoad& load : position_extents_) {
        const MinimizerBucketDirEntry* entry =
            directory_.entry_by_ordinal(load.directory_entry_ordinal);
        if (entry == nullptr
            || load.next_chunk_no != entry->position_chunk_count
            || load.encoded_size != entry->position_encoded_bytes
            || load.encoded_capacity != entry->position_encoded_bytes
            || load.encoded == nullptr) {
            return poison<void>(MinimizerGenerationWireError::chunk_range);
        }
        const auto ordinal = std::lower_bound(
            control_ordinals_.begin(), control_ordinals_.end(),
            load.directory_entry_ordinal);
        if (ordinal == control_ordinals_.end()
            || *ordinal != load.directory_entry_ordinal) {
            return poison<void>(MinimizerGenerationWireError::partition_id);
        }
        const std::size_t extent = static_cast<std::size_t>(
            ordinal - control_ordinals_.begin());
        const auto core = minimizer_position_segment_core(
            manifest_, static_cast<std::uint32_t>(entry->position_segment));
        if (!core) {
            return poison<void>(core.error());
        }

        for (std::uint64_t control_index =
                decoded_control_begins_[extent];
             control_index < decoded_control_begins_[extent + 1u];
             ++control_index) {
            const MinimizerGrouped32Control& control = decoded_controls_[
                static_cast<std::size_t>(control_index)];
            const std::size_t query = decoded_control_query_indices_[
                static_cast<std::size_t>(control_index)];
            const bool scatter = query != no_query
                && query_records_[query].position_count != 0;
            if (!scatter) {
                const NonScatterSimdValidationResult validation =
                    validate_non_scatter_local32_w2(
                        std::span<const std::byte>{
                            load.encoded.get(), load.encoded_size},
                        control,
                        *core);
                const auto add_evidence = [this](
                        std::uint64_t& destination,
                        std::uint64_t increment) noexcept {
                    if (increment
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - destination) {
                        destination =
                            (std::numeric_limits<std::uint64_t>::max)();
                        simd_route_evidence_.saturated = true;
                    } else {
                        destination += increment;
                    }
                };
                if (simd_route_evidence_.selected_hwy_target == 0u) {
                    simd_route_evidence_.selected_hwy_target =
                        validation.selected_hwy_target;
                } else if (simd_route_evidence_.selected_hwy_target
                        != validation.selected_hwy_target) {
                    simd_route_evidence_.saturated = true;
                }
                if (validation.selected_vector_stream_width == 0u) {
                    add_evidence(
                        simd_route_evidence_.scalar_required_spans, 1u);
                } else {
                    if (simd_route_evidence_.selected_vector_stream_width
                            == 0u) {
                        simd_route_evidence_.selected_vector_stream_width =
                            validation.selected_vector_stream_width;
                    } else if (
                        simd_route_evidence_.selected_vector_stream_width
                            != validation.selected_vector_stream_width) {
                        simd_route_evidence_.saturated = true;
                    }
                    add_evidence(simd_route_evidence_.attempted_spans, 1u);
                    add_evidence(
                        simd_route_evidence_.vectorized_words,
                        validation.vectorized_words);
                    if (validation.validation
                            == NonScatterSimdValidation::accepted) {
                        add_evidence(
                            simd_route_evidence_.accepted_spans, 1u);
                    } else {
                        add_evidence(
                            simd_route_evidence_.rejected_spans, 1u);
                    }
                }
                if (validation.validation
                        == NonScatterSimdValidation::accepted) {
                    continue;
                }
                // Both `rejected` and `scalar_required` deliberately replay
                // the unchanged loop below. On rejected SIMD masks this is
                // the sole source of the exact first wire error; on short or
                // unsupported shapes it remains the ordinary scalar route.
            }
            std::optional<std::uint32_t> previous_position;
            for (std::uint64_t position_index = 0;
                 position_index < control.position_count;
                 ++position_index) {
                const std::uint64_t element =
                    static_cast<std::uint64_t>(control.position_offset)
                    + position_index;
                const std::uint64_t byte_offset = element
                    * kMinimizerGrouped32PositionEncodedBytes;
                if (byte_offset > load.encoded_size
                    || kMinimizerGrouped32PositionEncodedBytes
                        > load.encoded_size - byte_offset) {
                    return poison<void>(
                        MinimizerGenerationWireError::byte_count);
                }
                const std::uint32_t encoded =
                    minimizer_generation_wire_detail::read_le<std::uint32_t>(
                        std::span<const std::byte>{
                            load.encoded.get()
                                + static_cast<std::size_t>(byte_offset),
                            kMinimizerGrouped32PositionEncodedBytes},
                        0);
                if (previous_position && encoded <= *previous_position) {
                    return poison<void>(
                        MinimizerGenerationWireError::position_range);
                }
                previous_position = encoded;
                const auto global =
                    globalize_minimizer_grouped32_local_position(
                        encoded, core->begin_bp, core->end_bp);
                if (!global) {
                    return poison<void>(global.error());
                }
                if (scatter) {
                    const QueryRecord& record = query_records_[query];
                    std::uint64_t& destination =
                        position_write_cursors_[query];
                    if (destination < record.positions_begin
                        || destination
                            >= record.positions_begin
                                + record.position_count
                        || destination >= position_count_) {
                        return poison<void>(
                            MinimizerGenerationWireError::element_count);
                    }
                    positions_[static_cast<std::size_t>(destination)] =
                        (global->bp << 1u)
                        | static_cast<std::uint64_t>(
                            global->reverse_strand);
                    ++destination;
                }
            }
        }
    }

    for (std::size_t query = 0; query < query_records_.size(); ++query) {
        const QueryRecord& record = query_records_[query];
        if (position_write_cursors_[query]
            != record.positions_begin + record.position_count) {
            return poison<void>(MinimizerGenerationWireError::element_count);
        }
        for (std::uint64_t index = 1;
             index < record.position_count; ++index) {
            const std::uint64_t previous = positions_[static_cast<std::size_t>(
                record.positions_begin + index - 1u)];
            const std::uint64_t current = positions_[static_cast<std::size_t>(
                record.positions_begin + index)];
            if (current <= previous) {
                return poison<void>(
                    MinimizerGenerationWireError::position_range);
            }
        }
    }

    std::vector<ExtentLoad>{}.swap(position_extents_);
    std::vector<MinimizerGrouped32Control>{}.swap(decoded_controls_);
    std::vector<std::uint64_t>{}.swap(decoded_control_begins_);
    std::vector<std::size_t>{}.swap(decoded_control_query_indices_);
    std::vector<std::uint64_t>{}.swap(position_write_cursors_);
    const std::uint64_t retained_bytes = owned_capacity_bytes();
    if (retained_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return poison<void>(
            MinimizerGenerationWireError::arithmetic_overflow);
    }
    permit_.shrink_to(retained_bytes);
    phase_ = Phase::ready;
    return {};
}

static_assert(!std::is_move_assignable_v<PreparedGrouped32QueryReader>);
static_assert(SegmentedMinimizerLookupIndex<PreparedGrouped32QueryReader>);
static_assert(MinimizerLookupIndex<PreparedGrouped32QueryReader>);

/// Admitted read-only lookup filled directly by qualified range-read callbacks.
class PreparedSerializedSegment {
private:
    friend class PreparedSerializedGeneration;

    // The permit is first so it is destroyed last, after every owned byte.
    bioseq::parallelism::MemoryPermit permit_;

    struct LoadedOffsets {
        std::uint32_t directory_entry_ordinal = 0;
        std::uint64_t controls_begin = 0;
        std::uint64_t positions_begin = 0;
        Crc64Ecma182 control_crc{};
        Crc64Ecma182 position_crc{};
    };

    DirView directory_;
    std::vector<LoadedOffsets> loaded_partitions_;
    std::vector<MinimizerHashmapFragmentIndex::SerializedBucket> controls_;
    std::vector<std::uint64_t> positions_;
    std::uint64_t generation_id_ = 0;
    std::uint64_t manifest_checksum_ = 0;
    sequence_size_t target_length_ = 0;
    std::uint32_t position_segment_count_ = 0;
    std::int32_t position_segment_ = 0;
    std::uint64_t core_begin_bp_ = 0;
    std::uint64_t core_end_bp_ = 0;
    std::size_t load_partition_cursor_ = 0;
    std::uint32_t load_chunk_cursor_ = 0;
    bool loading_position_extents_ = false;
    bool ready_ = false;

    PreparedSerializedSegment(
            bioseq::parallelism::MemoryPermit permit,
            DirView directory,
            std::vector<LoadedOffsets> loaded_partitions,
            std::vector<MinimizerHashmapFragmentIndex::SerializedBucket> controls,
            std::vector<std::uint64_t> positions,
            std::uint64_t generation_id,
            std::uint64_t manifest_checksum,
            sequence_size_t target_length,
            std::uint32_t position_segment_count,
            std::int32_t position_segment,
            std::uint64_t core_begin_bp,
            std::uint64_t core_end_bp) noexcept
        : permit_(std::move(permit)),
          directory_(std::move(directory)),
          loaded_partitions_(std::move(loaded_partitions)),
          controls_(std::move(controls)),
          positions_(std::move(positions)),
          generation_id_(generation_id),
          manifest_checksum_(manifest_checksum),
          target_length_(target_length),
          position_segment_count_(position_segment_count),
          position_segment_(position_segment),
          core_begin_bp_(core_begin_bp),
          core_end_bp_(core_end_bp) {}

public:
    PreparedSerializedSegment(PreparedSerializedSegment&&) noexcept = default;
    PreparedSerializedSegment& operator=(PreparedSerializedSegment&&) = delete;
    PreparedSerializedSegment(const PreparedSerializedSegment&) = delete;
    PreparedSerializedSegment& operator=(const PreparedSerializedSegment&) = delete;

    /// Compute exact final allocation bytes from raw authoritative directory bytes.
    [[nodiscard]] static std::expected<std::uint64_t, MinimizerGenerationWireError>
    required_permit_bytes(
            std::span<const std::byte> encoded_directory,
            std::int32_t position_segment,
            std::span<const std::uint32_t> selected_ordinals,
            const MinimizerGenerationPayloadProfile& profile) noexcept {
        if (!validate_minimizer_generation_payload_profile(profile)
            || profile.wire_format
                != MinimizerGenerationWireFormat::sorted_dense_global64) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        if (encoded_directory.size() % MinimizerBucketDirEntry::encoded_bytes != 0
            || position_segment < 0) {
            return std::unexpected(MinimizerGenerationWireError::directory_size);
        }
        for (std::size_t index = 1; index < selected_ordinals.size(); ++index) {
            if (selected_ordinals[index] <= selected_ordinals[index - 1u]) {
                return std::unexpected(MinimizerGenerationWireError::directory_order);
            }
        }
        const std::size_t entry_count =
            encoded_directory.size() / MinimizerBucketDirEntry::encoded_bytes;
        std::uint64_t required = bioseq::parallelism::saturating_add(
            encoded_directory.size(),
            bioseq::parallelism::saturating_mul(
                entry_count, sizeof(MinimizerBucketDirEntry)));
        required = bioseq::parallelism::saturating_add(
            required,
            bioseq::parallelism::saturating_mul(
                selected_ordinals.size(), sizeof(LoadedOffsets)));
        std::size_t selected_index = 0;
        for (std::size_t ordinal = 0; ordinal < entry_count; ++ordinal) {
            const auto entry = decode_minimizer_bucket_dir_entry(
                encoded_directory.subspan(
                    ordinal * MinimizerBucketDirEntry::encoded_bytes,
                    MinimizerBucketDirEntry::encoded_bytes),
                profile);
            if (!entry || entry->partition_id != ordinal) {
                return std::unexpected(entry
                    ? MinimizerGenerationWireError::partition_id
                    : entry.error());
            }
            if (selected_index < selected_ordinals.size()
                && selected_ordinals[selected_index] == ordinal) {
                if (entry->position_segment != position_segment) {
                    return std::unexpected(
                        MinimizerGenerationWireError::position_segment);
                }
                required = bioseq::parallelism::saturating_add(
                    required, entry->control_encoded_bytes);
                required = bioseq::parallelism::saturating_add(
                    required, entry->position_encoded_bytes);
                ++selected_index;
            }
        }
        if (selected_index != selected_ordinals.size()
            || required == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(MinimizerGenerationWireError::arithmetic_overflow);
        }
        return required;
    }

    /// Acquire admission, allocate exact final arrays, then await range callbacks.
    [[nodiscard]] static std::expected<
        PreparedSerializedSegment,
        MinimizerGenerationWireError>
    begin_load(
            bioseq::parallelism::MemoryAdmissionController& admission,
            const MinimizerGenerationHead& head,
            const MinimizerGenerationManifest& manifest,
            const ExpectedMinimizerCompatibility& expected,
            std::span<const std::byte> encoded_directory,
            std::int32_t position_segment,
            std::span<const std::uint32_t> selected_ordinals) {
        if (auto compatibility = validate_minimizer_generation_compatibility(
                manifest, expected); !compatibility) {
            return std::unexpected(compatibility.error());
        }
        const auto profile = minimizer_generation_payload_profile(
            expected.wire_format,
            MinimizerSchemeIdentity{
                .id = expected.minimizer_scheme_id,
                .version = expected.minimizer_scheme_version,
            },
            expected.kmer_size);
        if (!profile
            || profile->wire_format
                != MinimizerGenerationWireFormat::sorted_dense_global64) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        const auto required = required_permit_bytes(
            encoded_directory, position_segment, selected_ordinals, *profile);
        if (!required) {
            return std::unexpected(required.error());
        }
        auto permit = admission.try_reserve(*required);
        if (!permit) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }
        auto directory = DirView::decode(encoded_directory, *profile);
        if (!directory) {
            return std::unexpected(directory.error());
        }
        if (auto metadata = validate_minimizer_generation_metadata(
                head, manifest, *directory); !metadata) {
            return std::unexpected(metadata.error());
        }
        if (static_cast<std::uint64_t>(position_segment)
            >= manifest.position_segment_count) {
            return std::unexpected(MinimizerGenerationWireError::position_segment);
        }
        const auto core = minimizer_position_segment_core(
            manifest, static_cast<std::uint32_t>(position_segment));
        if (!core) {
            return std::unexpected(core.error());
        }

        std::uint64_t total_controls = 0;
        std::uint64_t total_positions = 0;
        std::vector<LoadedOffsets> loaded;
        loaded.reserve(selected_ordinals.size());
        for (std::uint32_t ordinal : selected_ordinals) {
            const MinimizerBucketDirEntry* entry =
                directory->entry_by_ordinal(ordinal);
            if (entry == nullptr || entry->position_segment != position_segment) {
                return std::unexpected(MinimizerGenerationWireError::position_segment);
            }
            loaded.push_back(LoadedOffsets{
                .directory_entry_ordinal = ordinal,
                .controls_begin = total_controls,
                .positions_begin = total_positions,
            });
            total_controls = bioseq::parallelism::saturating_add(
                total_controls, entry->control_element_count);
            total_positions = bioseq::parallelism::saturating_add(
                total_positions, entry->position_element_count);
        }
        if (total_controls == (std::numeric_limits<std::uint64_t>::max)()
            || total_positions == (std::numeric_limits<std::uint64_t>::max)()
            || total_controls > (std::numeric_limits<std::size_t>::max)()
            || total_positions > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(MinimizerGenerationWireError::arithmetic_overflow);
        }
        std::vector<MinimizerHashmapFragmentIndex::SerializedBucket> controls(
            static_cast<std::size_t>(total_controls));
        std::vector<std::uint64_t> positions(
            static_cast<std::size_t>(total_positions));
        return PreparedSerializedSegment{
            std::move(*permit),
            std::move(*directory),
            std::move(loaded),
            std::move(controls),
            std::move(positions),
            manifest.generation_id,
            head.manifest_checksum,
            static_cast<sequence_size_t>(manifest.target_length),
            manifest.position_segment_count,
            position_segment,
            core->begin_bp,
            core->end_bp};
    }

    /// Copy one validated qualified callback payload directly into final storage.
    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    accept_chunk(
            const IndexChunkAddress& address,
            const char* payload,
            std::size_t payload_bytes) noexcept {
        if (ready_ || load_partition_cursor_ >= loaded_partitions_.size()) {
            return std::unexpected(MinimizerGenerationWireError::chunk_range);
        }
        LoadedOffsets& loaded = loaded_partitions_[load_partition_cursor_];
        const MinimizerBucketDirEntry& entry =
            *directory_.entry_by_ordinal(loaded.directory_entry_ordinal);
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key) {
            return std::unexpected(position_sub_key.error());
        }
        const index_sub_key_t expected_sub_key = loading_position_extents_
            ? *position_sub_key : entry.sub_key;
        const std::uint32_t extent_chunk_count = loading_position_extents_
            ? entry.position_chunk_count : entry.control_chunk_count;
        if (address.generation_id != generation_id_
            || address.position_segment != position_segment_
            || address.sub_key != expected_sub_key
            || address.chunk_no != load_chunk_cursor_
            || load_chunk_cursor_ >= extent_chunk_count) {
            return std::unexpected(MinimizerGenerationWireError::chunk_range);
        }

        const std::uint32_t extent_chunk = load_chunk_cursor_;
        const std::uint64_t extent_bytes = loading_position_extents_
            ? entry.position_encoded_bytes : entry.control_encoded_bytes;
        const std::uint64_t byte_offset =
            static_cast<std::uint64_t>(extent_chunk)
            * kMinimizerExtentPayloadBytes;
        const std::uint64_t expected_bytes = std::min<std::uint64_t>(
            kMinimizerExtentPayloadBytes, extent_bytes - byte_offset);
        if (payload == nullptr || payload_bytes != expected_bytes) {
            return std::unexpected(MinimizerGenerationWireError::byte_count);
        }

        std::byte* destination = nullptr;
        if (!loading_position_extents_) {
            destination = reinterpret_cast<std::byte*>(controls_.data())
                + loaded.controls_begin * 16u + byte_offset;
            loaded.control_crc.update(std::as_bytes(
                std::span<const char>{payload, payload_bytes}));
        } else {
            destination = reinterpret_cast<std::byte*>(positions_.data())
                + loaded.positions_begin * 8u + byte_offset;
            loaded.position_crc.update(std::as_bytes(
                std::span<const char>{payload, payload_bytes}));
        }
        std::memcpy(destination, payload, payload_bytes);
        ++load_chunk_cursor_;
        if (load_chunk_cursor_ == extent_chunk_count) {
            const bool checksum_matches = loading_position_extents_
                ? loaded.position_crc.checksum() == entry.position_checksum
                : loaded.control_crc.checksum() == entry.control_checksum;
            if (!checksum_matches) {
                return std::unexpected(MinimizerGenerationWireError::checksum);
            }
            ++load_partition_cursor_;
            load_chunk_cursor_ = 0;
            if (load_partition_cursor_ == loaded_partitions_.size()
                && !loading_position_extents_) {
                loading_position_extents_ = true;
                load_partition_cursor_ = 0;
            }
        }
        return {};
    }

    /// Validate final controls/positions after the last callback and publish lookup.
    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    finish_load() noexcept {
        const bool empty_selection = loaded_partitions_.empty();
        if (ready_ || load_chunk_cursor_ != 0
            || (!empty_selection
                && (!loading_position_extents_
                    || load_partition_cursor_ != loaded_partitions_.size()))) {
            return std::unexpected(MinimizerGenerationWireError::chunk_range);
        }
        for (const LoadedOffsets& loaded : loaded_partitions_) {
            const MinimizerBucketDirEntry& entry =
                *directory_.entry_by_ordinal(loaded.directory_entry_ordinal);
            std::uint64_t expected_position_offset = 0;
            std::optional<std::uint64_t> previous_hash;
            for (std::uint64_t index = 0;
                 index < entry.control_element_count;
                 ++index) {
                const auto& control = controls_[static_cast<std::size_t>(
                    loaded.controls_begin + index)];
                if (control.pos_count == 0
                    || (previous_hash && control.hash <= *previous_hash)
                    || control.hash < entry.lo_hash || control.hash > entry.hi_hash
                    || control.pos_offset != expected_position_offset
                    || static_cast<std::uint64_t>(control.pos_offset)
                            + control.pos_count
                        > entry.position_element_count) {
                    return std::unexpected(MinimizerGenerationWireError::control_range);
                }
                previous_hash = control.hash;
                expected_position_offset =
                    static_cast<std::uint64_t>(control.pos_offset)
                    + control.pos_count;
                const auto begin = positions_.begin()
                    + static_cast<std::ptrdiff_t>(
                        loaded.positions_begin + control.pos_offset);
                const auto end = begin + static_cast<std::ptrdiff_t>(control.pos_count);
                if (!std::is_sorted(begin, end)) {
                    return std::unexpected(MinimizerGenerationWireError::position_range);
                }
                for (auto position = begin; position != end; ++position) {
                    const std::uint64_t bp = *position >> 1u;
                    if (bp < core_begin_bp_ || bp >= core_end_bp_) {
                        return std::unexpected(
                            MinimizerGenerationWireError::position_range);
                    }
                }
            }
            const auto first_control = controls_.begin()
                + static_cast<std::ptrdiff_t>(loaded.controls_begin);
            if (!previous_hash || first_control->hash != entry.lo_hash
                || *previous_hash != entry.hi_hash
                || expected_position_offset != entry.position_element_count) {
                return std::unexpected(MinimizerGenerationWireError::control_range);
            }
        }
        ready_ = true;
        return {};
    }

    /// Return the full target length carried by the immutable generation.
    [[nodiscard]] sequence_size_t target_length() const noexcept {
        return target_length_;
    }

    /// Return the one persisted core ordinal represented by this object.
    [[nodiscard]] std::int32_t position_segment() const noexcept {
        return position_segment_;
    }

    /// Probe authoritative controls without pair materialization or rebasing.
    [[nodiscard]] std::span<const std::uint64_t> lookup(
            std::uint64_t hash) const noexcept {
        if (!ready_) {
            return {};
        }
        const MinimizerBucketDirEntry* entry =
            directory_.find(position_segment_, hash);
        if (entry == nullptr) {
            return {};
        }
        const auto loaded = std::lower_bound(
            loaded_partitions_.begin(), loaded_partitions_.end(),
            entry->partition_id,
            [](const LoadedOffsets& value, std::uint32_t ordinal) noexcept {
                return value.directory_entry_ordinal < ordinal;
            });
        if (loaded == loaded_partitions_.end()
            || loaded->directory_entry_ordinal != entry->partition_id) {
            return {};
        }
        const auto controls_begin = controls_.begin()
            + static_cast<std::ptrdiff_t>(loaded->controls_begin);
        const auto controls_end = controls_begin
            + static_cast<std::ptrdiff_t>(entry->control_element_count);
        const auto control = std::lower_bound(
            controls_begin, controls_end, hash,
            [](const MinimizerHashmapFragmentIndex::SerializedBucket& value,
               std::uint64_t wanted) noexcept {
                return value.hash < wanted;
            });
        if (control == controls_end || control->hash != hash) {
            return {};
        }
        const std::uint64_t begin = loaded->positions_begin + control->pos_offset;
        return std::span<const std::uint64_t>{
            positions_.data() + static_cast<std::ptrdiff_t>(begin),
            control->pos_count};
    }
};

static_assert(MinimizerLookupIndex<PreparedSerializedSegment>);
static_assert(!PersistableMinimizerIndex<PreparedSerializedSegment>);
static_assert(!std::is_move_assignable_v<PreparedSerializedSegment>);

/// Authoritative target-wide occurrence policy for one sorted minimizer hash.
/// A pruned record preserves its global count but deliberately has no positions.
struct PreparedSerializedHashRecord {
    std::uint64_t hash = 0;
    std::uint64_t global_occurrence_count = 0;
    bool positions_complete = true;
};

/// Construction evidence for the bounded two-pass k-way control merge.
struct PreparedSerializedAssemblyProof {
    std::uint64_t total_loaded_controls = 0;
    std::uint64_t hash_record_count = 0;
    std::uint64_t merged_control_visits = 0;
    std::uint64_t hash_record_visits = 0;
    std::uint64_t heap_build_items = 0;
    std::uint64_t heap_reinsertions = 0;
    std::uint64_t emitted_spans = 0;
    std::uint32_t segment_count = 0;
    std::uint32_t merge_pass_count = 0;

    /// Prove construction was bounded by two linear control-stream passes,
    /// rather than by a hash-record x segment probe cross product.
    [[nodiscard]] constexpr bool linear_merge_bound_holds() const noexcept {
        constexpr std::uint64_t max =
            (std::numeric_limits<std::uint64_t>::max)();
        if (merge_pass_count != 2u || segment_count == 0
            || total_loaded_controls > max / merge_pass_count
            || hash_record_count > max / merge_pass_count
            || segment_count > max / merge_pass_count) {
            return false;
        }
        return merged_control_visits
                <= total_loaded_controls * merge_pass_count
            && hash_record_visits
                == hash_record_count * merge_pass_count
            && heap_build_items
                <= static_cast<std::uint64_t>(segment_count)
                    * merge_pass_count
            && heap_reinsertions <= merged_control_visits
            && emitted_spans <= total_loaded_controls;
    }
};

/// Immutable target-wide view assembled from every finished position segment.
/// The flat span directory owns no position payload: its spans borrow from the
/// segment objects owned by this generation, so move assignment is forbidden.
class PreparedSerializedGeneration {
private:
    struct HashDirectoryEntry {
        std::uint64_t hash = 0;
        std::uint64_t global_occurrence_count = 0;
        std::size_t span_begin = 0;
        std::size_t span_count = 0;
        bool positions_complete = true;
    };

    struct MergeCursor {
        std::size_t segment_ordinal = 0;
        std::size_t loaded_partition_ordinal = 0;
        std::size_t control_ordinal = 0;
        std::size_t control_end = 0;
    };

    struct MergeCursorLater {
        const std::vector<PreparedSerializedSegment>* segments = nullptr;

        [[nodiscard]] bool operator()(
                const MergeCursor& lhs,
                const MergeCursor& rhs) const noexcept {
            const std::uint64_t lhs_hash = (*segments)[lhs.segment_ordinal]
                .controls_[lhs.control_ordinal].hash;
            const std::uint64_t rhs_hash = (*segments)[rhs.segment_ordinal]
                .controls_[rhs.control_ordinal].hash;
            return lhs_hash > rhs_hash
                || (lhs_hash == rhs_hash
                    && lhs.segment_ordinal > rhs.segment_ordinal);
        }
    };

    // The permit is first so it is destroyed after all admitted allocations.
    bioseq::parallelism::MemoryPermit permit_;
    std::vector<PreparedSerializedSegment> segments_;
    std::vector<MinimizerPositionSpan> spans_;
    std::vector<HashDirectoryEntry> directory_;
    sequence_size_t target_length_ = 0;
    PreparedSerializedAssemblyProof assembly_proof_{};

    PreparedSerializedGeneration(
            bioseq::parallelism::MemoryPermit permit,
            std::vector<PreparedSerializedSegment> segments,
            std::vector<MinimizerPositionSpan> spans,
            std::vector<HashDirectoryEntry> directory,
            sequence_size_t target_length,
            PreparedSerializedAssemblyProof assembly_proof) noexcept
        : permit_(std::move(permit)),
          segments_(std::move(segments)),
          spans_(std::move(spans)),
          directory_(std::move(directory)),
          target_length_(target_length),
          assembly_proof_(assembly_proof) {}

    [[nodiscard]] static std::uint64_t cursor_hash(
            const std::vector<PreparedSerializedSegment>& segments,
            const MergeCursor& cursor) noexcept {
        return segments[cursor.segment_ordinal]
            .controls_[cursor.control_ordinal].hash;
    }

    [[nodiscard]] static std::span<const std::uint64_t> cursor_positions(
            const std::vector<PreparedSerializedSegment>& segments,
            const MergeCursor& cursor) noexcept {
        const PreparedSerializedSegment& segment =
            segments[cursor.segment_ordinal];
        const PreparedSerializedSegment::LoadedOffsets& loaded =
            segment.loaded_partitions_[cursor.loaded_partition_ordinal];
        const auto& control = segment.controls_[cursor.control_ordinal];
        const std::uint64_t begin = loaded.positions_begin + control.pos_offset;
        return std::span<const std::uint64_t>{
            segment.positions_.data() + static_cast<std::ptrdiff_t>(begin),
            control.pos_count};
    }

    static void advance_merge_front(
            const std::vector<PreparedSerializedSegment>& segments,
            std::vector<MergeCursor>& heap,
            const MergeCursorLater& later,
            PreparedSerializedAssemblyProof& proof) noexcept {
        std::pop_heap(heap.begin(), heap.end(), later);
        MergeCursor cursor = heap.back();
        heap.pop_back();
        ++proof.merged_control_visits;
        ++cursor.control_ordinal;
        if (cursor.control_ordinal == cursor.control_end) {
            const PreparedSerializedSegment& segment =
                segments[cursor.segment_ordinal];
            ++cursor.loaded_partition_ordinal;
            if (cursor.loaded_partition_ordinal
                == segment.loaded_partitions_.size()) {
                return;
            }
            const PreparedSerializedSegment::LoadedOffsets& loaded =
                segment.loaded_partitions_[cursor.loaded_partition_ordinal];
            const MinimizerBucketDirEntry& entry = *segment.directory_
                .entry_by_ordinal(loaded.directory_entry_ordinal);
            cursor.control_ordinal = static_cast<std::size_t>(
                loaded.controls_begin);
            cursor.control_end = static_cast<std::size_t>(
                loaded.controls_begin + entry.control_element_count);
        }
        heap.push_back(cursor);
        std::push_heap(heap.begin(), heap.end(), later);
        ++proof.heap_reinsertions;
    }

    [[nodiscard]] static std::expected<
        std::size_t,
        MinimizerGenerationWireError>
    merge_hash_records(
            const std::vector<PreparedSerializedSegment>& segments,
            std::span<const PreparedSerializedHashRecord> hash_records,
            std::vector<MergeCursor>& heap,
            std::vector<MinimizerPositionSpan>* spans,
            std::vector<HashDirectoryEntry>* directory,
            PreparedSerializedAssemblyProof& proof) {
        const bool emit = spans != nullptr && directory != nullptr;
        if ((spans == nullptr) != (directory == nullptr)) {
            return std::unexpected(MinimizerGenerationWireError::control_range);
        }
        heap.clear();
        for (std::size_t segment_ordinal = 0;
             segment_ordinal < segments.size();
             ++segment_ordinal) {
            const PreparedSerializedSegment& segment =
                segments[segment_ordinal];
            if (segment.controls_.empty()) {
                if (!segment.loaded_partitions_.empty()) {
                    return std::unexpected(
                        MinimizerGenerationWireError::control_range);
                }
                continue;
            }
            if (segment.loaded_partitions_.empty()) {
                return std::unexpected(
                    MinimizerGenerationWireError::control_range);
            }
            const PreparedSerializedSegment::LoadedOffsets& loaded =
                segment.loaded_partitions_.front();
            const MinimizerBucketDirEntry* entry = segment.directory_
                .entry_by_ordinal(loaded.directory_entry_ordinal);
            if (entry == nullptr || loaded.controls_begin >= segment.controls_.size()
                || entry->control_element_count
                    > segment.controls_.size() - loaded.controls_begin) {
                return std::unexpected(
                    MinimizerGenerationWireError::control_range);
            }
            heap.push_back(MergeCursor{
                .segment_ordinal = segment_ordinal,
                .loaded_partition_ordinal = 0,
                .control_ordinal = static_cast<std::size_t>(
                    loaded.controls_begin),
                .control_end = static_cast<std::size_t>(
                    loaded.controls_begin + entry->control_element_count),
            });
        }
        const MergeCursorLater later{.segments = &segments};
        std::make_heap(heap.begin(), heap.end(), later);
        ++proof.merge_pass_count;
        proof.heap_build_items += heap.size();

        std::size_t matched_span_count = 0;
        for (const PreparedSerializedHashRecord& record : hash_records) {
            ++proof.hash_record_visits;
            while (!heap.empty()
                   && cursor_hash(segments, heap.front()) < record.hash) {
                advance_merge_front(segments, heap, later, proof);
            }
            const std::size_t span_begin = emit
                ? spans->size() : matched_span_count;
            std::uint64_t observed_count = 0;
            while (!heap.empty()
                   && cursor_hash(segments, heap.front()) == record.hash) {
                const MergeCursor cursor = heap.front();
                if (record.positions_complete) {
                    const std::span<const std::uint64_t> positions =
                        cursor_positions(segments, cursor);
                    if (positions.empty()
                        || observed_count
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - positions.size()
                        || matched_span_count
                            == (std::numeric_limits<std::size_t>::max)()) {
                        return std::unexpected(
                            MinimizerGenerationWireError::arithmetic_overflow);
                    }
                    observed_count += positions.size();
                    ++matched_span_count;
                    if (emit) {
                        spans->push_back(MinimizerPositionSpan{positions});
                    }
                }
                advance_merge_front(segments, heap, later, proof);
            }
            if (record.positions_complete
                && observed_count != record.global_occurrence_count) {
                return std::unexpected(
                    MinimizerGenerationWireError::element_count);
            }
            if (emit) {
                directory->push_back(HashDirectoryEntry{
                    .hash = record.hash,
                    .global_occurrence_count = record.global_occurrence_count,
                    .span_begin = span_begin,
                    .span_count = spans->size() - span_begin,
                    .positions_complete = record.positions_complete,
                });
            }
        }
        return matched_span_count;
    }

public:
    PreparedSerializedGeneration(PreparedSerializedGeneration&&) noexcept =
        default;
    PreparedSerializedGeneration& operator=(PreparedSerializedGeneration&&) =
        delete;
    PreparedSerializedGeneration(const PreparedSerializedGeneration&) = delete;
    PreparedSerializedGeneration& operator=(
        const PreparedSerializedGeneration&) = delete;

    /// Own all segment payloads and build one allocation-free lookup directory.
    /// Segments must be complete, ordinal-ordered, and cover exactly one shared
    /// persisted generation. Hash records must be strictly hash-ordered.
    [[nodiscard]] static std::expected<
        PreparedSerializedGeneration,
        MinimizerGenerationWireError>
    assemble(
            bioseq::parallelism::MemoryAdmissionController& admission,
            std::vector<PreparedSerializedSegment> segments,
            std::span<const PreparedSerializedHashRecord> hash_records) {
        if (segments.empty()) {
            return std::unexpected(MinimizerGenerationWireError::segment_count);
        }
        const PreparedSerializedSegment& first = segments.front();
        const sequence_size_t target_length = first.target_length_;
        if (first.position_segment_count_ == 0
            || segments.size() != first.position_segment_count_) {
            return std::unexpected(MinimizerGenerationWireError::segment_count);
        }
        std::uint64_t previous_core_end = 0;
        for (std::size_t ordinal = 0; ordinal < segments.size(); ++ordinal) {
            const PreparedSerializedSegment& segment = segments[ordinal];
            if (!segment.ready_) {
                return std::unexpected(MinimizerGenerationWireError::chunk_range);
            }
            if (segment.generation_id_ != first.generation_id_
                || segment.manifest_checksum_ != first.manifest_checksum_
                || segment.target_length_ != first.target_length_
                || segment.position_segment_count_
                    != first.position_segment_count_) {
                return std::unexpected(
                    MinimizerGenerationWireError::identity_mismatch);
            }
            if (segment.position_segment_ < 0
                || static_cast<std::size_t>(segment.position_segment_)
                    != ordinal) {
                return std::unexpected(
                    MinimizerGenerationWireError::segment_ordinal);
            }
            if (segment.core_begin_bp_ != previous_core_end
                || segment.core_end_bp_ <= segment.core_begin_bp_) {
                return std::unexpected(MinimizerGenerationWireError::bp_range);
            }
            previous_core_end = segment.core_end_bp_;
        }
        if (previous_core_end != first.target_length_) {
            return std::unexpected(MinimizerGenerationWireError::bp_range);
        }

        std::optional<std::uint64_t> previous_hash;
        for (const PreparedSerializedHashRecord& record : hash_records) {
            if (record.global_occurrence_count == 0
                || (previous_hash && record.hash <= *previous_hash)) {
                return std::unexpected(
                    MinimizerGenerationWireError::control_order);
            }
            previous_hash = record.hash;
        }

        std::uint64_t total_loaded_controls = 0;
        for (const PreparedSerializedSegment& segment : segments) {
            if (total_loaded_controls
                > (std::numeric_limits<std::uint64_t>::max)()
                    - segment.controls_.size()) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            total_loaded_controls += segment.controls_.size();
        }
        constexpr std::uint64_t merge_passes = 2;
        if (total_loaded_controls
                > (std::numeric_limits<std::uint64_t>::max)() / merge_passes
            || hash_records.size()
                > (std::numeric_limits<std::uint64_t>::max)() / merge_passes) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        PreparedSerializedAssemblyProof proof{
            .total_loaded_controls = total_loaded_controls,
            .hash_record_count = hash_records.size(),
            .segment_count = static_cast<std::uint32_t>(segments.size()),
        };

        const std::uint64_t segment_bytes =
            bioseq::parallelism::saturating_mul(
                segments.capacity(), sizeof(PreparedSerializedSegment));
        const std::uint64_t requested_directory_bytes =
            bioseq::parallelism::saturating_mul(
                hash_records.size(), sizeof(HashDirectoryEntry));
        const std::uint64_t requested_heap_bytes =
            bioseq::parallelism::saturating_mul(
                segments.size(), sizeof(MergeCursor));
        std::uint64_t required_bytes = bioseq::parallelism::saturating_add(
            segment_bytes, requested_directory_bytes);
        required_bytes = bioseq::parallelism::saturating_add(
            required_bytes, requested_heap_bytes);
        if (required_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        auto permit = admission.try_reserve(required_bytes);
        if (!permit) {
            return std::unexpected(MinimizerGenerationWireError::admission);
        }

        std::vector<MinimizerPositionSpan> spans;
        std::vector<HashDirectoryEntry> directory;
        std::size_t required_span_count = 0;
        {
            std::vector<MergeCursor> heap;
            heap.reserve(segments.size());
            const std::uint64_t actual_heap_bytes =
                bioseq::parallelism::saturating_mul(
                    heap.capacity(), sizeof(MergeCursor));
            std::uint64_t live_bytes = bioseq::parallelism::saturating_add(
                segment_bytes, requested_directory_bytes);
            live_bytes = bioseq::parallelism::saturating_add(
                live_bytes, actual_heap_bytes);
            if (live_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            if (!permit->grow_to(live_bytes)) {
                return std::unexpected(MinimizerGenerationWireError::admission);
            }

            const auto counted = merge_hash_records(
                segments, hash_records, heap, nullptr, nullptr, proof);
            if (!counted) {
                return std::unexpected(counted.error());
            }
            required_span_count = *counted;
            const std::uint64_t requested_span_bytes =
                bioseq::parallelism::saturating_mul(
                    required_span_count, sizeof(MinimizerPositionSpan));
            live_bytes = bioseq::parallelism::saturating_add(
                live_bytes, requested_span_bytes);
            if (live_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            if (!permit->grow_to(live_bytes)) {
                return std::unexpected(MinimizerGenerationWireError::admission);
            }

            spans.reserve(required_span_count);
            directory.reserve(hash_records.size());
            live_bytes = bioseq::parallelism::saturating_add(
                segment_bytes,
                bioseq::parallelism::saturating_mul(
                    directory.capacity(), sizeof(HashDirectoryEntry)));
            live_bytes = bioseq::parallelism::saturating_add(
                live_bytes,
                bioseq::parallelism::saturating_mul(
                    spans.capacity(), sizeof(MinimizerPositionSpan)));
            live_bytes = bioseq::parallelism::saturating_add(
                live_bytes, actual_heap_bytes);
            if (live_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            if (!permit->grow_to(live_bytes)) {
                return std::unexpected(MinimizerGenerationWireError::admission);
            }

            const auto emitted = merge_hash_records(
                segments, hash_records, heap, &spans, &directory, proof);
            if (!emitted) {
                return std::unexpected(emitted.error());
            }
            if (*emitted != required_span_count
                || spans.size() != required_span_count
                || directory.size() != hash_records.size()) {
                return std::unexpected(
                    MinimizerGenerationWireError::element_count);
            }
            proof.emitted_spans = spans.size();
            if (!proof.linear_merge_bound_holds()) {
                return std::unexpected(
                    MinimizerGenerationWireError::control_range);
            }
        }

        std::uint64_t owned_bytes = bioseq::parallelism::saturating_add(
            segment_bytes,
            bioseq::parallelism::saturating_mul(
                directory.capacity(), sizeof(HashDirectoryEntry)));
        owned_bytes = bioseq::parallelism::saturating_add(
            owned_bytes,
            bioseq::parallelism::saturating_mul(
                spans.capacity(), sizeof(MinimizerPositionSpan)));
        if (owned_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        permit->shrink_to(owned_bytes);
        return PreparedSerializedGeneration{
            std::move(*permit),
            std::move(segments),
            std::move(spans),
            std::move(directory),
            target_length,
            proof};
    }

    [[nodiscard]] sequence_size_t target_length() const noexcept {
        return target_length_;
    }

    [[nodiscard]] const PreparedSerializedAssemblyProof& assembly_proof()
            const noexcept {
        return assembly_proof_;
    }

    /// Binary-search the flat immutable directory without allocating.
    [[nodiscard]] MinimizerLookupSpans lookup_spans(
            std::uint64_t hash) const noexcept {
        const auto entry = std::lower_bound(
            directory_.begin(), directory_.end(), hash,
            [](const HashDirectoryEntry& value, std::uint64_t wanted) noexcept {
                return value.hash < wanted;
            });
        if (entry == directory_.end() || entry->hash != hash) {
            return MinimizerLookupSpans::segmented(
                std::span<const MinimizerPositionSpan>{}, 0);
        }
        if (!entry->positions_complete) {
            return MinimizerLookupSpans::positions_unavailable(
                entry->global_occurrence_count);
        }
        return MinimizerLookupSpans::segmented(
            std::span<const MinimizerPositionSpan>{spans_}.subspan(
                entry->span_begin, entry->span_count),
            entry->global_occurrence_count);
    }
};

static_assert(SegmentedMinimizerLookupIndex<PreparedSerializedGeneration>);
static_assert(MinimizerLookupIndex<PreparedSerializedGeneration>);
static_assert(!PersistableMinimizerIndex<PreparedSerializedGeneration>);
static_assert(!std::is_move_assignable_v<PreparedSerializedGeneration>);

/// Fixed-width limits of revision-3 controls and PostgreSQL key axes.
struct MinimizerGenerationFixedWidthLimits {
    std::uint64_t max_segment_control_count =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_segment_position_count =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_hash_position_count =
        (std::numeric_limits<std::uint32_t>::max)();
};

/// Immutable compatibility and source identity supplied to both producers.
struct MinimizerGenerationEncodeOptions {
    MinimizerGenerationWireFormat wire_format =
        MinimizerGenerationWireFormat::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t target_length = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    // Deliberately invalid: every writer must name the producer's scheme.
    std::uint16_t minimizer_scheme_id = kInvalidMinimizerSchemeId;
    std::uint16_t minimizer_scheme_version = kInvalidMinimizerSchemeVersion;
};

/// One canonical event supplied in `(segment,hash,encoded_position)` order.
struct CanonicalMinimizerEvent {
    std::uint64_t hash = 0;
    std::uint64_t encoded_position = 0;

    [[nodiscard]] bool operator==(const CanonicalMinimizerEvent&) const = default;
};

/// Type-erased callback used by independently replayable MPF/MPS producers.
struct CanonicalMinimizerEventReceiver {
    void* context = nullptr;
    std::expected<void, MinimizerGenerationWireError> (*emit_fn)(
        void*, const CanonicalMinimizerEvent&) = nullptr;

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    operator()(const CanonicalMinimizerEvent& event) const {
        if (context == nullptr || emit_fn == nullptr) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        return emit_fn(context, event);
    }
};

/// A producer that can replay a bounded/external sorted stream for each pass.
template <class Producer>
concept MinimizerGenerationEventProducer = requires(
        Producer& producer,
        std::uint64_t position_segment_span_bp,
        CanonicalMinimizerEventReceiver receiver) {
    { producer.minimizer_scheme_identity() }
        -> std::same_as<MinimizerSchemeIdentity>;
    { producer.scan_sorted_events(position_segment_span_bp, receiver) }
        -> std::same_as<std::expected<void, MinimizerGenerationWireError>>;
};

/// Callback-scoped qualified row emitted directly to PostgreSQL/fake storage.
struct MinimizerGenerationRowView {
    IndexChunkAddress address{};
    std::span<const std::byte> bytes;
    std::uint64_t row_checksum = 0;
};

template <class Sink>
concept MinimizerGenerationRowSink = requires(
        Sink& sink,
        const MinimizerGenerationRowView& row) {
    { sink.write(row) }
        -> std::same_as<std::expected<void, MinimizerGenerationWireError>>;
};

struct MinimizerGenerationRowSinkRef {
    void* context = nullptr;
    std::expected<void, MinimizerGenerationWireError> (*write_fn)(
        void*, const MinimizerGenerationRowView&) = nullptr;

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    write(const MinimizerGenerationRowView& row) const {
        if (context == nullptr || write_fn == nullptr) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        return write_fn(context, row);
    }
};

/// One bounded fixed metadata row (head or manifest chunk zero).
struct EncodedMinimizerFixedRow {
    IndexChunkAddress address{};
    std::array<std::byte, MinimizerGenerationManifest::encoded_bytes> storage{};
    std::uint32_t byte_count = 0;
    std::uint64_t row_checksum = 0;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>{storage.data(), byte_count};
    }
};

/// Bounded metadata result; biological payload rows are never retained here.
struct EncodedMinimizerGeneration {
    MinimizerGenerationHead head{};
    MinimizerGenerationManifest manifest{};
    /// Publication is deliberately separate: this row is never sent to the
    /// generation sink by `encode_minimizer_generation`.
    EncodedMinimizerFixedRow unpublished_head_row{};
    EncodedMinimizerFixedRow manifest_row{};
    DirView directory{};
};

/// Immutable identity of the exact encoded generation whose execution proof
/// may be admitted to READY publication.  Route evidence alone is deliberately
/// insufficient: two generations can execute the same backend over equal-sized
/// sources while naming different source epochs or publication rows.
struct MinimizerGenerationPublicationBinding {
    MinimizerGenerationWireFormat wire_format =
        MinimizerGenerationWireFormat::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;
    std::uint64_t manifest_checksum = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return wire_format != MinimizerGenerationWireFormat::invalid
            && generation_id != 0 && source_identity != 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const MinimizerGenerationPublicationBinding&) const noexcept = default;
};

[[nodiscard]] constexpr MinimizerGenerationPublicationBinding
minimizer_generation_publication_binding(
        const EncodedMinimizerGeneration& generation) noexcept {
    return MinimizerGenerationPublicationBinding{
        .wire_format = generation.manifest.wire_format,
        .generation_id = generation.manifest.generation_id,
        .source_identity = generation.manifest.source_identity,
        .source_content_checksum =
            generation.manifest.source_content_checksum,
        .compatibility_checksum =
            minimizer_generation_compatibility_checksum(generation.manifest),
        .manifest_checksum = generation.head.manifest_checksum,
    };
}

struct MinimizerGenerationPlan {
    MinimizerGenerationPayloadProfile profile{};
    std::uint64_t position_segment_span_bp = 0;
    std::uint32_t position_segment_count = 0;
    std::uint64_t canonical_event_count = 0;
    std::uint64_t canonical_event_checksum = 0;
    std::vector<MinimizerBucketDirEntry> entries;
};

namespace minimizer_generation_stream_detail {

inline void update_canonical_event_checksum(
        Crc64Ecma182& checksum,
        const CanonicalMinimizerEvent& event) noexcept {
    std::array<std::byte, 16> encoded{};
    std::span<std::byte> bytes{encoded};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(
        bytes, 0, event.hash);
    minimizer_generation_wire_detail::write_le<std::uint64_t>(
        bytes, 8, event.encoded_position);
    checksum.update(encoded);
}

template <MinimizerGenerationWireFormat WireFormat>
[[nodiscard]] constexpr bool payload_profile_matches_specialization(
        const MinimizerGenerationPayloadProfile& profile) noexcept {
    static_assert(
        WireFormat == MinimizerGenerationWireFormat::sorted_dense_global64
        || WireFormat == MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32);
    return profile.wire_format == WireFormat
        && validate_minimizer_generation_payload_profile(profile).has_value();
}

[[nodiscard]] constexpr std::expected<
    std::uint64_t,
    MinimizerGenerationWireError>
checked_payload_byte_count(
        std::uint64_t element_count,
        std::uint64_t encoded_byte_width) noexcept {
    if (encoded_byte_width == 0
        || element_count > (std::numeric_limits<std::uint64_t>::max)()
            / encoded_byte_width) {
        return std::unexpected(
            MinimizerGenerationWireError::arithmetic_overflow);
    }
    return element_count * encoded_byte_width;
}

[[nodiscard]] constexpr std::uint64_t payload_chunk_count(
        std::uint64_t encoded_bytes) noexcept {
    return encoded_bytes / kMinimizerExtentPayloadBytes
        + (encoded_bytes % kMinimizerExtentPayloadBytes != 0 ? 1u : 0u);
}

template <MinimizerGenerationWireFormat WireFormat>
struct PlanPass {
    std::uint64_t span = 0;
    std::uint64_t target_length = 0;
    MinimizerGenerationPayloadProfile profile{};
    MinimizerGenerationFixedWidthLimits limits{};
    std::vector<MinimizerBucketDirEntry> entries;
    bool retry_with_smaller_span = false;
    bool has_event = false;
    std::uint64_t previous_segment = 0;
    CanonicalMinimizerEvent previous_event{};
    std::uint64_t current_segment = 0;
    std::uint64_t current_hash = 0;
    std::uint64_t group_positions = 0;
    std::uint64_t segment_controls = 0;
    std::uint64_t segment_positions = 0;
    std::uint64_t partition_controls = 0;
    std::uint64_t partition_positions = 0;
    std::uint64_t partition_lo_hash = 0;
    std::uint64_t partition_hi_hash = 0;
    std::uint64_t canonical_event_count = 0;
    Crc64Ecma182 canonical_event_checksum{};

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    flush_partition() {
        if (partition_controls == 0) {
            return {};
        }
        constexpr std::uint64_t max_u32 =
            (std::numeric_limits<std::uint32_t>::max)();
        if (entries.size() >= max_u32 / 2u
            || partition_controls > (std::numeric_limits<std::uint32_t>::max)()
            || partition_positions > (std::numeric_limits<std::uint32_t>::max)()
            || !sql_integer_can_represent(current_segment)) {
            return std::unexpected(MinimizerGenerationWireError::element_count);
        }
        const auto control_bytes = checked_payload_byte_count(
            partition_controls, profile.control_encoded_byte_width);
        const auto position_bytes = checked_payload_byte_count(
            partition_positions, profile.position_encoded_byte_width);
        if (!control_bytes || !position_bytes) {
            return std::unexpected(MinimizerGenerationWireError::arithmetic_overflow);
        }
        const std::uint64_t control_chunks = payload_chunk_count(*control_bytes);
        const std::uint64_t position_chunks = payload_chunk_count(*position_bytes);
        if (control_chunks
                > (std::numeric_limits<std::uint64_t>::max)() - position_chunks
            || !sql_integer_can_represent(control_chunks + position_chunks)) {
            return std::unexpected(MinimizerGenerationWireError::chunk_range);
        }
        const std::uint32_t partition_id =
            static_cast<std::uint32_t>(entries.size());
        entries.push_back(MinimizerBucketDirEntry{
            .partition_id = partition_id,
            .position_segment = static_cast<std::int32_t>(current_segment),
            .lo_hash = partition_lo_hash,
            .hi_hash = partition_hi_hash,
            .sub_key = static_cast<index_sub_key_t>(partition_id),
            .control_first_chunk = 0,
            .control_chunk_count = static_cast<std::uint32_t>(control_chunks),
            .position_first_chunk = 0,
            .position_chunk_count = static_cast<std::uint32_t>(position_chunks),
            .control_element_count = partition_controls,
            .position_element_count = partition_positions,
            .control_encoded_bytes = *control_bytes,
            .position_encoded_bytes = *position_bytes,
        });
        partition_controls = 0;
        partition_positions = 0;
        partition_lo_hash = 0;
        partition_hi_hash = 0;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    finish_group() {
        if (group_positions == 0) {
            return {};
        }
        constexpr std::uint64_t max_u32 =
            (std::numeric_limits<std::uint32_t>::max)();
        if (group_positions > limits.max_hash_position_count
            || group_positions > max_u32) {
            retry_with_smaller_span = true;
            return std::unexpected(MinimizerGenerationWireError::element_count);
        }
        const auto group_position_bytes = checked_payload_byte_count(
            group_positions, profile.position_encoded_byte_width);
        const auto partition_control_bytes = checked_payload_byte_count(
            partition_controls, profile.control_encoded_byte_width);
        const auto partition_position_bytes = checked_payload_byte_count(
            partition_positions, profile.position_encoded_byte_width);
        if (!group_position_bytes || !partition_control_bytes
            || !partition_position_bytes
            || *group_position_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - profile.control_encoded_byte_width
            || *partition_control_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - *partition_position_bytes) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        const std::uint64_t group_bytes =
            profile.control_encoded_byte_width + *group_position_bytes;
        const std::uint64_t partition_bytes =
            *partition_control_bytes + *partition_position_bytes;
        if (partition_controls != 0
            && (partition_bytes > kMinimizerPartitionTargetBytes
                || group_bytes > kMinimizerPartitionTargetBytes - partition_bytes)) {
            if (auto flushed = flush_partition(); !flushed) {
                return flushed;
            }
        }
        if (partition_controls == 0) {
            partition_lo_hash = current_hash;
        }
        partition_hi_hash = current_hash;
        if (partition_controls == max_u32
            || partition_positions > max_u32 - group_positions) {
            return std::unexpected(MinimizerGenerationWireError::element_count);
        }
        ++partition_controls;
        partition_positions += group_positions;
        group_positions = 0;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    accept(const CanonicalMinimizerEvent& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        if (span == 0 || position >= target_length
            || event.hash > profile.max_hash) {
            return std::unexpected(event.hash > profile.max_hash
                ? MinimizerGenerationWireError::hash_range
                : MinimizerGenerationWireError::position_range);
        }
        if constexpr (WireFormat
                == MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32) {
            if (span
                > static_cast<std::uint64_t>(
                    kMinimizerGrouped32MaxLocalBp) + 1u) {
                return std::unexpected(
                    MinimizerGenerationWireError::position_range);
            }
        }
        const std::uint64_t segment = position / span;
        if (!sql_integer_can_represent(segment)) {
            return std::unexpected(MinimizerGenerationWireError::position_range);
        }
        if (has_event) {
            const bool before = segment < previous_segment
                || (segment == previous_segment
                    && (event.hash < previous_event.hash
                        || (event.hash == previous_event.hash
                            && event.encoded_position
                                < previous_event.encoded_position)));
            if (before) {
                return std::unexpected(MinimizerGenerationWireError::row_order);
            }
            if (segment == previous_segment && event == previous_event) {
                return std::unexpected(MinimizerGenerationWireError::duplicate_event);
            }
            if (segment != current_segment || event.hash != current_hash) {
                if (auto finished = finish_group(); !finished) {
                    return finished;
                }
                if (segment != current_segment) {
                    if (auto flushed = flush_partition(); !flushed) {
                        return flushed;
                    }
                    current_segment = segment;
                    segment_controls = 0;
                    segment_positions = 0;
                }
                current_hash = event.hash;
                if (segment_controls
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                    return std::unexpected(
                        MinimizerGenerationWireError::arithmetic_overflow);
                }
                ++segment_controls;
            }
        } else {
            has_event = true;
            current_segment = segment;
            current_hash = event.hash;
            segment_controls = 1;
        }
        if (canonical_event_count
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        ++canonical_event_count;
        update_canonical_event_checksum(canonical_event_checksum, event);
        if (segment_positions
                == (std::numeric_limits<std::uint64_t>::max)()
            || group_positions
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        ++segment_positions;
        ++group_positions;
        if (segment_controls > limits.max_segment_control_count
            || segment_positions > limits.max_segment_position_count
            || segment_controls
                > (std::numeric_limits<std::uint32_t>::max)()
            || segment_positions
                > (std::numeric_limits<std::uint32_t>::max)()
            || group_positions > limits.max_hash_position_count
            || group_positions
                > (std::numeric_limits<std::uint32_t>::max)()) {
            retry_with_smaller_span = true;
            return std::unexpected(MinimizerGenerationWireError::element_count);
        }
        previous_segment = segment;
        previous_event = event;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError> finish() {
        if (auto group = finish_group(); !group) {
            return group;
        }
        return flush_partition();
    }
};

template <MinimizerGenerationWireFormat WireFormat>
[[nodiscard]] inline std::expected<void, MinimizerGenerationWireError>
plan_receiver(void* context, const CanonicalMinimizerEvent& event) {
    return static_cast<PlanPass<WireFormat>*>(context)->accept(event);
}

template <MinimizerGenerationWireFormat WireFormat>
struct EmitPass {
    struct EncodedElement {
        std::array<std::byte, 16> storage{};
        std::uint16_t byte_count = 0;

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
            return {storage.data(), byte_count};
        }
    };

    MinimizerGenerationEncodeOptions options{};
    MinimizerGenerationPayloadProfile profile{};
    std::uint64_t span = 0;
    std::vector<MinimizerBucketDirEntry>* entries = nullptr;
    MinimizerGenerationRowSinkRef sink{};
    Crc64Ecma182 payload_row_checksums{};
    std::size_t entry_index = 0;
    std::uint64_t entry_positions_seen = 0;
    std::uint64_t current_hash = 0;
    std::uint64_t current_hash_count = 0;
    bool has_hash = false;
    bool has_previous = false;
    CanonicalMinimizerEvent previous_event{};
    std::uint64_t previous_segment = 0;
    std::vector<std::byte> control_bytes;
    std::vector<std::byte> position_bytes;
    std::uint64_t buffered_control_count = 0;
    std::uint64_t buffered_position_count = 0;
    std::uint64_t first_buffered_hash = 0;
    std::uint64_t last_buffered_hash = 0;
    bool has_buffered_control = false;
    bool normal_buffers_prepared = false;
    std::uint64_t cached_core_segment =
        (std::numeric_limits<std::uint64_t>::max)();
    MinimizerPositionSegmentCore cached_core{};
    std::array<std::byte, kMinimizerExtentPayloadBytes> heavy_page{};
    std::size_t heavy_page_bytes = 0;
    std::uint32_t heavy_position_chunk = 0;
    Crc64Ecma182 heavy_position_crc{};
    bool heavy_control_emitted = false;
    std::uint64_t canonical_event_count = 0;
    Crc64Ecma182 canonical_event_checksum{};

    static_assert(kMinimizerExtentPayloadBytes % 16u == 0u);
    static_assert(kMinimizerExtentPayloadBytes
        % kMinimizerGrouped32ControlEncodedBytes == 0u);
    static_assert(kMinimizerExtentPayloadBytes % 8u == 0u);
    static_assert(kMinimizerExtentPayloadBytes
        % kMinimizerGrouped32PositionEncodedBytes == 0u);

    [[nodiscard]] std::expected<
        MinimizerPositionSegmentCore,
        MinimizerGenerationWireError>
    position_core(std::uint64_t segment) noexcept {
        if (cached_core_segment == segment) {
            return cached_core;
        }
        if (span == 0 || segment
                > (std::numeric_limits<std::uint64_t>::max)() / span) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        const std::uint64_t begin = segment * span;
        if (begin >= options.target_length) {
            return std::unexpected(
                MinimizerGenerationWireError::position_range);
        }
        const std::uint64_t width = std::min(
            span, options.target_length - begin);
        const MinimizerPositionSegmentCore core{
            .begin_bp = begin,
            .end_bp = begin + width,
        };
        if constexpr (WireFormat
                == MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32) {
            if (auto admitted = validate_minimizer_grouped32_segment(
                    core.begin_bp, core.end_bp); !admitted) {
                return std::unexpected(admitted.error());
            }
        }
        cached_core_segment = segment;
        cached_core = core;
        return core;
    }

    [[nodiscard]] std::expected<EncodedElement, MinimizerGenerationWireError>
    encode_control(
            std::uint64_t hash,
            std::uint64_t position_offset,
            std::uint64_t position_count) const noexcept {
        EncodedElement output{};
        if constexpr (WireFormat
                == MinimizerGenerationWireFormat::sorted_dense_global64) {
            constexpr std::uint64_t max_u32 =
                (std::numeric_limits<std::uint32_t>::max)();
            if (position_count == 0 || position_offset > max_u32
                || position_count > max_u32
                || position_count > max_u32 - position_offset) {
                return std::unexpected(
                    MinimizerGenerationWireError::control_range);
            }
            const auto encoded = encode_minimizer_serialized_bucket(
                MinimizerHashmapFragmentIndex::SerializedBucket{
                    .hash = hash,
                    .pos_offset = static_cast<std::uint32_t>(position_offset),
                    .pos_count = static_cast<std::uint32_t>(position_count),
                });
            std::copy(encoded.begin(), encoded.end(), output.storage.begin());
            output.byte_count = static_cast<std::uint16_t>(encoded.size());
        } else {
            const auto control = make_minimizer_grouped32_control(
                hash, position_offset, position_count, profile);
            if (!control) {
                return std::unexpected(control.error());
            }
            const auto encoded = encode_minimizer_grouped32_control(
                *control, profile);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            std::copy(encoded->begin(), encoded->end(), output.storage.begin());
            output.byte_count = static_cast<std::uint16_t>(encoded->size());
        }
        if (output.byte_count != profile.control_encoded_byte_width) {
            return std::unexpected(MinimizerGenerationWireError::byte_count);
        }
        return output;
    }

    [[nodiscard]] std::expected<EncodedElement, MinimizerGenerationWireError>
    encode_position(
            const CanonicalMinimizerEvent& event,
            std::uint64_t segment) noexcept {
        EncodedElement output{};
        if constexpr (WireFormat
                == MinimizerGenerationWireFormat::sorted_dense_global64) {
            minimizer_generation_wire_detail::write_le<std::uint64_t>(
                std::span<std::byte>{output.storage},
                0,
                event.encoded_position);
            output.byte_count = sizeof(std::uint64_t);
        } else {
            const auto core = position_core(segment);
            if (!core) {
                return std::unexpected(core.error());
            }
            const auto localized = localize_minimizer_grouped32_position(
                event.encoded_position >> 1u,
                (event.encoded_position & 1u) != 0u,
                core->begin_bp,
                core->end_bp,
                profile);
            if (!localized) {
                return std::unexpected(localized.error());
            }
            const auto encoded = encode_minimizer_grouped32_position(
                *localized, profile);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            std::copy(encoded->begin(), encoded->end(), output.storage.begin());
            output.byte_count = static_cast<std::uint16_t>(encoded->size());
        }
        if (output.byte_count != profile.position_encoded_byte_width) {
            return std::unexpected(MinimizerGenerationWireError::byte_count);
        }
        return output;
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    emit_payload_row(
            const MinimizerBucketDirEntry& entry,
            bool position_extent,
            std::uint32_t chunk_no,
            std::span<const std::byte> bytes) {
        if (bytes.empty() || bytes.size() > kMinimizerExtentPayloadBytes) {
            return std::unexpected(MinimizerGenerationWireError::byte_count);
        }
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (position_extent && !position_sub_key) {
            return std::unexpected(position_sub_key.error());
        }
        const IndexChunkAddress address{
            .generation_id = options.generation_id,
            .position_segment = entry.position_segment,
            .sub_key = position_extent ? *position_sub_key : entry.sub_key,
            .chunk_no = chunk_no,
        };
        const std::uint64_t checksum = crc64_index_chunk_row(
            address.generation_id,
            address.position_segment,
            address.sub_key,
            address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()),
            bytes);
        if (auto written = sink.write(MinimizerGenerationRowView{
                .address = address,
                .bytes = bytes,
                .row_checksum = checksum,
            }); !written) {
            return written;
        }
        payload_row_checksums.update_little_endian(checksum);
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    emit_extent(
            const MinimizerBucketDirEntry& entry,
            bool position_extent,
            std::uint32_t first_chunk,
            std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        std::uint32_t chunk = first_chunk;
        while (offset < bytes.size()) {
            const std::size_t count = std::min<std::size_t>(
                kMinimizerExtentPayloadBytes, bytes.size() - offset);
            if (auto emitted = emit_payload_row(
                    entry, position_extent, chunk,
                    bytes.subspan(offset, count)); !emitted) {
                return emitted;
            }
            offset += count;
            ++chunk;
        }
        return {};
    }

    [[nodiscard]] bool current_is_heavy() const noexcept {
        if (entry_index >= entries->size()) {
            return false;
        }
        const MinimizerBucketDirEntry& entry = (*entries)[entry_index];
        return entry.control_element_count == 1
            && entry.control_encoded_bytes + entry.position_encoded_bytes
                > kMinimizerPartitionTargetBytes;
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    start_heavy_entry() {
        MinimizerBucketDirEntry& entry = (*entries)[entry_index];
        if (entry.control_element_count != 1
            || entry.lo_hash != entry.hi_hash
            || entry.position_element_count == 0
            || entry.position_element_count
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(MinimizerGenerationWireError::element_count);
        }
        const auto control = encode_control(
            entry.lo_hash, 0, entry.position_element_count);
        if (!control) {
            return std::unexpected(control.error());
        }
        entry.control_checksum = crc64_ecma182(
            control->bytes());
        heavy_control_emitted = true;
        return emit_extent(
            entry, false, entry.control_first_chunk, control->bytes());
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    flush_heavy_page() {
        if (heavy_page_bytes == 0) {
            return {};
        }
        MinimizerBucketDirEntry& entry = (*entries)[entry_index];
        const auto bytes = std::span<const std::byte>{
            heavy_page.data(), heavy_page_bytes};
        if (auto emitted = emit_payload_row(
                entry,
                true,
                entry.position_first_chunk + heavy_position_chunk,
                bytes); !emitted) {
            return emitted;
        }
        ++heavy_position_chunk;
        heavy_page_bytes = 0;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    append_heavy_position(
            const CanonicalMinimizerEvent& event,
            std::uint64_t segment) {
        const auto encoded = encode_position(event, segment);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        if (heavy_page_bytes > heavy_page.size() - encoded->bytes().size()) {
            return std::unexpected(MinimizerGenerationWireError::byte_count);
        }
        std::copy(encoded->bytes().begin(), encoded->bytes().end(),
            heavy_page.begin() + static_cast<std::ptrdiff_t>(heavy_page_bytes));
        heavy_position_crc.update(encoded->bytes());
        heavy_page_bytes += encoded->bytes().size();
        if (heavy_page_bytes == heavy_page.size()) {
            return flush_heavy_page();
        }
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    prepare_normal_buffers() {
        if (normal_buffers_prepared) {
            return {};
        }
        if (entry_index >= entries->size()) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        const MinimizerBucketDirEntry& entry = (*entries)[entry_index];
        if (entry.control_encoded_bytes
                > (std::numeric_limits<std::size_t>::max)()
            || entry.position_encoded_bytes
                > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        control_bytes.reserve(
            static_cast<std::size_t>(entry.control_encoded_bytes));
        position_bytes.reserve(
            static_cast<std::size_t>(entry.position_encoded_bytes));
        normal_buffers_prepared = true;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    finish_normal_hash() {
        if (!has_hash) {
            return {};
        }
        if (current_hash_count == 0
            || current_hash_count > buffered_position_count) {
            return std::unexpected(MinimizerGenerationWireError::control_range);
        }
        const auto encoded = encode_control(
            current_hash,
            buffered_position_count - current_hash_count,
            current_hash_count);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        control_bytes.insert(
            control_bytes.end(), encoded->bytes().begin(), encoded->bytes().end());
        if (!has_buffered_control) {
            first_buffered_hash = current_hash;
            has_buffered_control = true;
        }
        last_buffered_hash = current_hash;
        ++buffered_control_count;
        has_hash = false;
        current_hash_count = 0;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    append_normal_position(
            const CanonicalMinimizerEvent& event,
            std::uint64_t segment) {
        if (auto prepared = prepare_normal_buffers(); !prepared) {
            return prepared;
        }
        const auto encoded = encode_position(event, segment);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const MinimizerBucketDirEntry& entry = (*entries)[entry_index];
        if (position_bytes.size() > entry.position_encoded_bytes
            || encoded->bytes().size()
                > entry.position_encoded_bytes - position_bytes.size()) {
            return std::unexpected(MinimizerGenerationWireError::byte_count);
        }
        position_bytes.insert(
            position_bytes.end(), encoded->bytes().begin(), encoded->bytes().end());
        ++buffered_position_count;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    finalize_entry() {
        if (entry_index >= entries->size()) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        MinimizerBucketDirEntry& entry = (*entries)[entry_index];
        if (entry_positions_seen != entry.position_element_count) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        if (current_is_heavy()) {
            if (!heavy_control_emitted) {
                return std::unexpected(MinimizerGenerationWireError::row_count);
            }
            if (auto flushed = flush_heavy_page(); !flushed) {
                return flushed;
            }
            if (heavy_position_chunk != entry.position_chunk_count) {
                return std::unexpected(MinimizerGenerationWireError::row_count);
            }
            entry.position_checksum = heavy_position_crc.checksum();
        } else {
            if (auto finished = finish_normal_hash(); !finished) {
                return finished;
            }
            if (buffered_control_count != entry.control_element_count
                || buffered_position_count != entry.position_element_count
                || control_bytes.size() != entry.control_encoded_bytes
                || position_bytes.size() != entry.position_encoded_bytes
                || !has_buffered_control
                || first_buffered_hash != entry.lo_hash
                || last_buffered_hash != entry.hi_hash) {
                return std::unexpected(MinimizerGenerationWireError::control_range);
            }
            entry.control_checksum = crc64_ecma182(control_bytes);
            entry.position_checksum = crc64_ecma182(position_bytes);
            if (auto emitted = emit_extent(
                    entry, false, entry.control_first_chunk,
                    control_bytes); !emitted) {
                return emitted;
            }
            if (auto emitted = emit_extent(
                    entry, true, entry.position_first_chunk,
                    position_bytes); !emitted) {
                return emitted;
            }
        }
        ++entry_index;
        entry_positions_seen = 0;
        current_hash = 0;
        current_hash_count = 0;
        has_hash = false;
        control_bytes.clear();
        position_bytes.clear();
        buffered_control_count = 0;
        buffered_position_count = 0;
        first_buffered_hash = 0;
        last_buffered_hash = 0;
        has_buffered_control = false;
        normal_buffers_prepared = false;
        heavy_page_bytes = 0;
        heavy_position_chunk = 0;
        heavy_position_crc = {};
        heavy_control_emitted = false;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    accept(const CanonicalMinimizerEvent& event) {
        if (entry_index >= entries->size()) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        const std::uint64_t position = event.encoded_position >> 1u;
        if (span == 0 || position >= options.target_length
            || event.hash > profile.max_hash) {
            return std::unexpected(event.hash > profile.max_hash
                ? MinimizerGenerationWireError::hash_range
                : MinimizerGenerationWireError::position_range);
        }
        if constexpr (WireFormat
                == MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32) {
            if (span
                > static_cast<std::uint64_t>(
                    kMinimizerGrouped32MaxLocalBp) + 1u) {
                return std::unexpected(
                    MinimizerGenerationWireError::position_range);
            }
        }
        const std::uint64_t segment = position / span;
        if (!sql_integer_can_represent(segment)) {
            return std::unexpected(MinimizerGenerationWireError::position_range);
        }
        if (has_previous) {
            const bool before = segment < previous_segment
                || (segment == previous_segment
                    && (event.hash < previous_event.hash
                        || (event.hash == previous_event.hash
                            && event.encoded_position
                                < previous_event.encoded_position)));
            if (before) {
                return std::unexpected(MinimizerGenerationWireError::row_order);
            }
            if (segment == previous_segment && event == previous_event) {
                return std::unexpected(MinimizerGenerationWireError::duplicate_event);
            }
        }
        if (canonical_event_count
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        ++canonical_event_count;
        update_canonical_event_checksum(canonical_event_checksum, event);
        if (entry_positions_seen
            == (*entries)[entry_index].position_element_count) {
            if (auto finalized = finalize_entry(); !finalized) {
                return finalized;
            }
            if (entry_index >= entries->size()) {
                return std::unexpected(MinimizerGenerationWireError::row_count);
            }
        }
        MinimizerBucketDirEntry& entry = (*entries)[entry_index];
        if (segment != static_cast<std::uint64_t>(entry.position_segment)
            || event.hash < entry.lo_hash || event.hash > entry.hi_hash) {
            return std::unexpected(MinimizerGenerationWireError::row_order);
        }

        if (current_is_heavy()) {
            if (event.hash != entry.lo_hash) {
                return std::unexpected(MinimizerGenerationWireError::hash_range);
            }
            if (!heavy_control_emitted) {
                if (auto started = start_heavy_entry(); !started) {
                    return started;
                }
            }
            if (auto appended = append_heavy_position(
                    event, segment); !appended) {
                return appended;
            }
        } else {
            if (!has_hash || event.hash != current_hash) {
                if (auto finished = finish_normal_hash(); !finished) {
                    return finished;
                }
                current_hash = event.hash;
                has_hash = true;
            }
            if (auto appended = append_normal_position(
                    event, segment); !appended) {
                return appended;
            }
            if (current_hash_count
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            ++current_hash_count;
        }
        if (entry_positions_seen
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                MinimizerGenerationWireError::arithmetic_overflow);
        }
        ++entry_positions_seen;
        has_previous = true;
        previous_segment = segment;
        previous_event = event;
        return {};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError> finish() {
        if (entry_index < entries->size()) {
            if (auto finalized = finalize_entry(); !finalized) {
                return finalized;
            }
        }
        if (entry_index != entries->size()) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        return {};
    }
};

template <MinimizerGenerationWireFormat WireFormat>
[[nodiscard]] inline std::expected<void, MinimizerGenerationWireError>
emit_receiver(void* context, const CanonicalMinimizerEvent& event) {
    return static_cast<EmitPass<WireFormat>*>(context)->accept(event);
}

template <
    MinimizerGenerationWireFormat WireFormat,
    MinimizerGenerationEventProducer Producer>
[[nodiscard]] inline std::expected<
    MinimizerGenerationPlan,
    MinimizerGenerationWireError>
plan_minimizer_generation_with_profile(
        Producer& producer,
        const MinimizerGenerationEncodeOptions& options,
        const MinimizerGenerationPayloadProfile& profile,
        const MinimizerGenerationFixedWidthLimits& limits) {
    if (!payload_profile_matches_specialization<WireFormat>(profile)) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    if (options.target_length == 0) {
        PlanPass<WireFormat> empty_pass{
            .span = 1,
            .target_length = 0,
            .profile = profile,
            .limits = limits,
        };
        const auto scanned = producer.scan_sorted_events(
            1,
            CanonicalMinimizerEventReceiver{
                .context = &empty_pass,
                .emit_fn = plan_receiver<WireFormat>,
            });
        if (!scanned) {
            return std::unexpected(scanned.error());
        }
        if (empty_pass.has_event) {
            return std::unexpected(MinimizerGenerationWireError::position_range);
        }
        return MinimizerGenerationPlan{.profile = profile};
    }

    std::uint64_t span = kMinimizerPreferredSegmentSpanBp;
    if constexpr (WireFormat
            == MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32) {
        span = std::min<std::uint64_t>(
            span,
            static_cast<std::uint64_t>(kMinimizerGrouped32MaxLocalBp) + 1u);
    }
    for (;;) {
        const std::uint64_t segment_count = options.target_length / span
            + (options.target_length % span != 0 ? 1u : 0u);
        if (!sql_integer_can_represent(segment_count)) {
            return std::unexpected(MinimizerGenerationWireError::segment_count);
        }
        PlanPass<WireFormat> pass{
            .span = span,
            .target_length = options.target_length,
            .profile = profile,
            .limits = limits,
        };
        auto scanned = producer.scan_sorted_events(
            span,
            CanonicalMinimizerEventReceiver{
                .context = &pass,
                .emit_fn = plan_receiver<WireFormat>,
            });
        if (scanned) {
            scanned = pass.finish();
        }
        if (scanned) {
            return MinimizerGenerationPlan{
                .profile = profile,
                .position_segment_span_bp = span,
                .position_segment_count =
                    static_cast<std::uint32_t>(segment_count),
                .canonical_event_count = pass.canonical_event_count,
                .canonical_event_checksum =
                    pass.canonical_event_checksum.checksum(),
                .entries = std::move(pass.entries),
            };
        }
        if (!pass.retry_with_smaller_span || span == 1u) {
            return std::unexpected(scanned.error());
        }
        span /= 2u;
    }
}

template <
    MinimizerGenerationWireFormat WireFormat,
    MinimizerGenerationEventProducer Producer>
[[nodiscard]] inline std::expected<
    std::uint64_t,
    MinimizerGenerationWireError>
emit_minimizer_generation_with_profile(
        Producer& producer,
        const MinimizerGenerationEncodeOptions& options,
        const MinimizerGenerationPayloadProfile& profile,
        MinimizerGenerationPlan& plan,
        const MinimizerGenerationRowSinkRef& sink) {
    if (!payload_profile_matches_specialization<WireFormat>(profile)
        || plan.profile != profile) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    const MinimizerSchemeIdentity before_replay =
        producer.minimizer_scheme_identity();
    if (before_replay != profile.minimizer_scheme) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    EmitPass<WireFormat> emitter{
        .options = options,
        .profile = profile,
        .span = plan.position_segment_span_bp,
        .entries = &plan.entries,
        .sink = sink,
    };
    auto emitted = producer.scan_sorted_events(
        plan.position_segment_span_bp == 0
            ? 1u : plan.position_segment_span_bp,
        CanonicalMinimizerEventReceiver{
            .context = &emitter,
            .emit_fn = emit_receiver<WireFormat>,
        });
    if (!emitted) {
        return std::unexpected(emitted.error());
    }
    const bool replay_equal =
        emitter.canonical_event_count == plan.canonical_event_count
        && emitter.canonical_event_checksum.checksum()
            == plan.canonical_event_checksum;
    if (!replay_equal) {
        return std::unexpected(MinimizerGenerationWireError::replay_mismatch);
    }
    if (producer.minimizer_scheme_identity() != profile.minimizer_scheme) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    emitted = emitter.finish();
    if (!emitted) {
        return std::unexpected(emitted.error());
    }
    return emitter.payload_row_checksums.checksum();
}

[[nodiscard]] inline EncodedMinimizerFixedRow make_fixed_row(
        const IndexChunkAddress& address,
        std::span<const std::byte> bytes) {
    EncodedMinimizerFixedRow row{
        .address = address,
        .byte_count = static_cast<std::uint32_t>(bytes.size()),
    };
    std::copy(bytes.begin(), bytes.end(), row.storage.begin());
    row.row_checksum = crc64_index_chunk_row(
        address.generation_id,
        address.position_segment,
        address.sub_key,
        address.chunk_no,
        row.byte_count,
        row.bytes());
    return row;
}

} // namespace minimizer_generation_stream_detail

/// Deterministically plan fixed geometry without retaining biological events.
template <MinimizerGenerationEventProducer Producer>
[[nodiscard]] inline std::expected<
    MinimizerGenerationPlan,
    MinimizerGenerationWireError>
plan_minimizer_generation(
        Producer& producer,
        const MinimizerGenerationEncodeOptions& options,
        const MinimizerGenerationFixedWidthLimits& limits = {}) {
    const MinimizerSchemeIdentity producer_scheme =
        producer.minimizer_scheme_identity();
    const auto profile = minimizer_generation_payload_profile(
        options.wire_format, producer_scheme, options.kmer_size);
    if (!profile
        || producer_scheme.id != options.minimizer_scheme_id
        || producer_scheme.version != options.minimizer_scheme_version) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    if (options.target_length != 0
        && (limits.max_segment_control_count == 0
            || limits.max_segment_position_count == 0
            || limits.max_hash_position_count == 0)) {
        return std::unexpected(MinimizerGenerationWireError::element_count);
    }
    switch (profile->wire_format) {
        case MinimizerGenerationWireFormat::sorted_dense_global64:
            return minimizer_generation_stream_detail::
                plan_minimizer_generation_with_profile<
                    MinimizerGenerationWireFormat::sorted_dense_global64>(
                        producer, options, *profile, limits);
        case MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32:
            return minimizer_generation_stream_detail::
                plan_minimizer_generation_with_profile<
                    MinimizerGenerationWireFormat::
                        grouped_hash32_segment_local32>(
                            producer, options, *profile, limits);
        case MinimizerGenerationWireFormat::invalid:
            break;
    }
    return std::unexpected(
        MinimizerGenerationWireError::compatibility_mismatch);
}

/// Stream one immutable qualified generation into a callback-scoped row sink.
template <MinimizerGenerationEventProducer Producer,
          MinimizerGenerationRowSink Sink>
[[nodiscard]] inline std::expected<
    EncodedMinimizerGeneration,
    MinimizerGenerationWireError>
encode_minimizer_generation(
        Producer& producer,
        Sink& sink,
        const MinimizerGenerationEncodeOptions& options,
        const MinimizerGenerationFixedWidthLimits& limits = {}) {
    if (options.generation_id == 0
        || !sql_bigint_can_represent(options.generation_id)) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    const auto profile = minimizer_generation_payload_profile(
        options.wire_format,
        MinimizerSchemeIdentity{
            .id = options.minimizer_scheme_id,
            .version = options.minimizer_scheme_version,
        },
        options.kmer_size);
    if (!profile
        || options.storage_unit_byte_width == 0
        || options.kmer_size == 0 || options.window_size == 0
        || options.target_length > (std::uint64_t{1} << 63u)
        || !is_supported_minimizer_scheme_identity(
            options.minimizer_scheme_id,
            options.minimizer_scheme_version)) {
        return std::unexpected(MinimizerGenerationWireError::compatibility_mismatch);
    }
    static_assert(kMinimizerExtentPayloadBytes
        % MinimizerBucketDirEntry::encoded_bytes == 0);
    auto plan = plan_minimizer_generation(producer, options, limits);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (plan->profile != *profile) {
        return std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    }
    const MinimizerGenerationRowSinkRef sink_ref{
        .context = &sink,
        .write_fn = [](void* context, const MinimizerGenerationRowView& row) {
            return static_cast<Sink*>(context)->write(row);
        },
    };
    std::expected<std::uint64_t, MinimizerGenerationWireError>
        payload_checksum = std::unexpected(
            MinimizerGenerationWireError::compatibility_mismatch);
    switch (plan->profile.wire_format) {
        case MinimizerGenerationWireFormat::sorted_dense_global64:
            payload_checksum = minimizer_generation_stream_detail::
                emit_minimizer_generation_with_profile<
                    MinimizerGenerationWireFormat::sorted_dense_global64>(
                        producer, options, plan->profile, *plan, sink_ref);
            break;
        case MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32:
            payload_checksum = minimizer_generation_stream_detail::
                emit_minimizer_generation_with_profile<
                    MinimizerGenerationWireFormat::
                        grouped_hash32_segment_local32>(
                            producer, options, plan->profile, *plan, sink_ref);
            break;
        case MinimizerGenerationWireFormat::invalid:
            break;
    }
    if (!payload_checksum) {
        return std::unexpected(payload_checksum.error());
    }

    auto directory_bytes = encode_minimizer_bucket_directory(
        plan->entries, plan->profile);
    if (!directory_bytes) {
        return std::unexpected(directory_bytes.error());
    }
    auto directory = DirView::decode(*directory_bytes, plan->profile);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    const std::uint64_t directory_chunk_count = directory_bytes->size()
            / kMinimizerExtentPayloadBytes
        + (directory_bytes->size() % kMinimizerExtentPayloadBytes != 0 ? 1u : 0u);
    if (!sql_integer_can_represent(directory_chunk_count)) {
        return std::unexpected(MinimizerGenerationWireError::chunk_range);
    }

    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    for (const MinimizerBucketDirEntry& entry : plan->entries) {
        control_count = bioseq::parallelism::saturating_add(
            control_count, entry.control_element_count);
        position_count = bioseq::parallelism::saturating_add(
            position_count, entry.position_element_count);
        payload_bytes = bioseq::parallelism::saturating_add(
            payload_bytes,
            bioseq::parallelism::saturating_add(
                entry.control_encoded_bytes, entry.position_encoded_bytes));
    }
    if (plan->entries.size()
            > (std::numeric_limits<std::uint32_t>::max)() / 2u
        || control_count == (std::numeric_limits<std::uint64_t>::max)()
        || position_count == (std::numeric_limits<std::uint64_t>::max)()
        || payload_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(MinimizerGenerationWireError::arithmetic_overflow);
    }
    MinimizerGenerationManifest manifest{
        .wire_format = options.wire_format,
        .generation_id = options.generation_id,
        .source_identity = options.source_identity,
        .source_content_checksum = options.source_content_checksum,
        .target_length = options.target_length,
        .control_count = control_count,
        .position_count = position_count,
        .payload_bytes = payload_bytes,
        .payload_checksum = *payload_checksum,
        .directory_bytes = directory_bytes->size(),
        .directory_checksum = crc64_ecma182(*directory_bytes),
        .position_segment_count = plan->position_segment_count,
        .extent_count = static_cast<std::uint32_t>(plan->entries.size() * 2u),
        .directory_chunk_count = static_cast<std::uint32_t>(directory_chunk_count),
        .alphabet_code = options.alphabet_code,
        .storage_unit_byte_width = options.storage_unit_byte_width,
        .kmer_size = options.kmer_size,
        .window_size = options.window_size,
        .gpu_abi_version = plan->profile.gpu_abi_version,
        .position_segment_span_bp = plan->position_segment_span_bp,
        .segment_geometry_version = kMinimizerSegmentGeometryVersion,
        .minimizer_scheme_id = options.minimizer_scheme_id,
        .minimizer_scheme_version = options.minimizer_scheme_version,
    };
    const auto manifest_bytes = encode_minimizer_generation_manifest(manifest);
    if (!manifest_bytes) {
        return std::unexpected(manifest_bytes.error());
    }
    MinimizerGenerationHead head{
        .wire_format = options.wire_format,
        .generation_id = options.generation_id,
        .source_identity = options.source_identity,
        .compatibility_checksum =
            minimizer_generation_compatibility_checksum(manifest),
        .manifest_checksum = crc64_ecma182(
            std::span<const std::byte>{*manifest_bytes}),
        .source_content_checksum = options.source_content_checksum,
    };
    const auto head_bytes = encode_minimizer_generation_head(head);
    if (!head_bytes) {
        return std::unexpected(head_bytes.error());
    }

    const EncodedMinimizerFixedRow manifest_row =
        minimizer_generation_stream_detail::make_fixed_row(
            IndexChunkAddress{
                .generation_id = options.generation_id,
                .position_segment = -1,
                .sub_key = DIRECTORY_SUB_KEY,
                .chunk_no = 0,
            },
            *manifest_bytes);
    const EncodedMinimizerFixedRow unpublished_head_row =
        minimizer_generation_stream_detail::make_fixed_row(
            IndexChunkAddress{
                .generation_id = 0,
                .position_segment = -1,
                .sub_key = GENERATION_HEAD_SUB_KEY,
                .chunk_no = 0,
            },
            *head_bytes);
    if (auto metadata = validate_minimizer_generation_metadata(
            head, manifest, *directory); !metadata) {
        return std::unexpected(metadata.error());
    }

    // Publication boundary: payload was written by EmitPass above; only the
    // directory and READY manifest are appended here. The active head is
    // returned to the caller and must be published only after a storage
    // reread validates this complete generation.
    for (std::size_t offset = 0, chunk = 1;
         offset < directory_bytes->size();
         offset += kMinimizerExtentPayloadBytes, ++chunk) {
        const std::size_t count = std::min<std::size_t>(
            kMinimizerExtentPayloadBytes, directory_bytes->size() - offset);
        const auto bytes = std::span<const std::byte>{*directory_bytes}
            .subspan(offset, count);
        const IndexChunkAddress address{
            .generation_id = options.generation_id,
            .position_segment = -1,
            .sub_key = DIRECTORY_SUB_KEY,
            .chunk_no = static_cast<std::uint32_t>(chunk),
        };
        const std::uint64_t checksum = crc64_index_chunk_row(
            address.generation_id, address.position_segment,
            address.sub_key, address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()), bytes);
        if (auto written = sink.write(MinimizerGenerationRowView{
                .address = address, .bytes = bytes, .row_checksum = checksum});
            !written) {
            return std::unexpected(written.error());
        }
    }
    if (auto written = sink.write(MinimizerGenerationRowView{
            .address = manifest_row.address,
            .bytes = manifest_row.bytes(),
            .row_checksum = manifest_row.row_checksum}); !written) {
        return std::unexpected(written.error());
    }
    return EncodedMinimizerGeneration{
        .head = head,
        .manifest = manifest,
        .unpublished_head_row = unpublished_head_row,
        .manifest_row = manifest_row,
        .directory = std::move(*directory),
    };
}

/// Streaming complete-generation validator for ordered qualified rows.
class MinimizerGenerationRowValidator {
private:
    struct ValidatedTag {};

    EncodedMinimizerGeneration generation_{};
    std::size_t entry_index_ = 0;
    std::uint32_t payload_chunk_ = 0;
    std::uint32_t directory_chunk_ = 1;
    Crc64Ecma182 extent_crc_{};
    Crc64Ecma182 payload_row_checksums_{};
    bool manifest_seen_ = false;
    bool ready_validated_ = false;

    explicit MinimizerGenerationRowValidator(
            ValidatedTag,
            const EncodedMinimizerGeneration& generation)
        : generation_(generation) {}

    [[nodiscard]] static std::expected<void, MinimizerGenerationWireError>
    validate_fixed_row(
            const EncodedMinimizerFixedRow& row,
            const IndexChunkAddress& expected_address,
            std::span<const std::byte> expected_bytes) noexcept {
        if (row.address != expected_address
            || row.byte_count != expected_bytes.size()
            || row.byte_count > row.storage.size()
            || !std::ranges::equal(row.bytes(), expected_bytes)) {
            return std::unexpected(MinimizerGenerationWireError::row_order);
        }
        const std::uint64_t expected_checksum = crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            row.byte_count,
            row.bytes());
        if (row.row_checksum != expected_checksum) {
            return std::unexpected(MinimizerGenerationWireError::checksum);
        }
        return {};
    }

public:
    /// Validate every metadata equation and both fixed rows before any caller
    /// can enter row/subspan arithmetic.
    [[nodiscard]] static std::expected<
        MinimizerGenerationRowValidator,
        MinimizerGenerationWireError>
    create(const EncodedMinimizerGeneration& generation) {
        if (auto metadata = validate_minimizer_generation_metadata(
                generation.head,
                generation.manifest,
                generation.directory); !metadata) {
            return std::unexpected(metadata.error());
        }
        const auto manifest_bytes =
            encode_minimizer_generation_manifest(generation.manifest);
        const auto head_bytes = encode_minimizer_generation_head(generation.head);
        if (!manifest_bytes || !head_bytes) {
            return std::unexpected(!manifest_bytes
                ? manifest_bytes.error() : head_bytes.error());
        }
        if (auto fixed = validate_fixed_row(
                generation.manifest_row,
                IndexChunkAddress{
                    .generation_id = generation.manifest.generation_id,
                    .position_segment = -1,
                    .sub_key = DIRECTORY_SUB_KEY,
                    .chunk_no = 0,
                },
                *manifest_bytes); !fixed) {
            return std::unexpected(fixed.error());
        }
        if (auto fixed = validate_fixed_row(
                generation.unpublished_head_row,
                IndexChunkAddress{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = GENERATION_HEAD_SUB_KEY,
                    .chunk_no = 0,
                },
                *head_bytes); !fixed) {
            return std::unexpected(fixed.error());
        }
        for (const MinimizerBucketDirEntry& entry
             : generation.directory.entries()) {
            if (entry.control_chunk_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                        - entry.position_chunk_count) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
        }
        return MinimizerGenerationRowValidator{ValidatedTag{}, generation};
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    accept(const MinimizerGenerationRowView& row) noexcept {
        if (ready_validated_ || manifest_seen_) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        if (entry_index_ < generation_.directory.entries().size()) {
            const MinimizerBucketDirEntry& entry =
                generation_.directory.entries()[entry_index_];
            if (entry.control_chunk_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                        - entry.position_chunk_count) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            const std::uint32_t total_chunks =
                entry.control_chunk_count + entry.position_chunk_count;
            if (payload_chunk_ >= total_chunks) {
                return std::unexpected(MinimizerGenerationWireError::row_count);
            }
            const bool control = payload_chunk_ < entry.control_chunk_count;
            const auto position_sub_key =
                derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) {
                return std::unexpected(position_sub_key.error());
            }
            const std::uint32_t extent_chunk = control
                ? payload_chunk_
                : payload_chunk_ - entry.control_chunk_count;
            const std::uint64_t extent_bytes = control
                ? entry.control_encoded_bytes : entry.position_encoded_bytes;
            if (extent_chunk
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / kMinimizerExtentPayloadBytes) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            const std::uint64_t offset =
                static_cast<std::uint64_t>(extent_chunk)
                * kMinimizerExtentPayloadBytes;
            if (offset >= extent_bytes) {
                return std::unexpected(MinimizerGenerationWireError::byte_count);
            }
            const std::uint64_t expected_bytes = std::min<std::uint64_t>(
                kMinimizerExtentPayloadBytes, extent_bytes - offset);
            if (row.address.generation_id != generation_.manifest.generation_id
                || row.address.position_segment != entry.position_segment
                || row.address.sub_key != (control
                    ? entry.sub_key : *position_sub_key)
                || row.address.chunk_no != extent_chunk
                || row.bytes.size() != expected_bytes) {
                return std::unexpected(MinimizerGenerationWireError::row_order);
            }
            const std::uint64_t expected_checksum = crc64_index_chunk_row(
                row.address.generation_id, row.address.position_segment,
                row.address.sub_key, row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()), row.bytes);
            if (row.row_checksum != expected_checksum) {
                return std::unexpected(MinimizerGenerationWireError::checksum);
            }
            extent_crc_.update(row.bytes);
            payload_row_checksums_.update_little_endian(row.row_checksum);
            ++payload_chunk_;
            if (payload_chunk_ == entry.control_chunk_count) {
                if (extent_crc_.checksum() != entry.control_checksum) {
                    return std::unexpected(MinimizerGenerationWireError::checksum);
                }
                extent_crc_ = {};
            }
            if (payload_chunk_ == total_chunks) {
                if (extent_crc_.checksum() != entry.position_checksum) {
                    return std::unexpected(MinimizerGenerationWireError::checksum);
                }
                extent_crc_ = {};
                payload_chunk_ = 0;
                ++entry_index_;
            }
            return {};
        }
        if (directory_chunk_ <= generation_.manifest.directory_chunk_count) {
            const std::uint64_t chunk_index =
                static_cast<std::uint64_t>(directory_chunk_ - 1u);
            if (chunk_index
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / kMinimizerExtentPayloadBytes) {
                return std::unexpected(
                    MinimizerGenerationWireError::arithmetic_overflow);
            }
            const std::uint64_t offset64 =
                chunk_index * kMinimizerExtentPayloadBytes;
            if (offset64 >= generation_.directory.encoded_bytes().size()
                || offset64
                    > (std::numeric_limits<std::size_t>::max)()) {
                return std::unexpected(MinimizerGenerationWireError::directory_size);
            }
            const std::size_t offset = static_cast<std::size_t>(offset64);
            const std::size_t count = std::min<std::size_t>(
                kMinimizerExtentPayloadBytes,
                generation_.directory.encoded_bytes().size() - offset);
            const auto expected_bytes = generation_.directory.encoded_bytes()
                .subspan(offset, count);
            if (row.address != IndexChunkAddress{
                    .generation_id = generation_.manifest.generation_id,
                    .position_segment = -1,
                    .sub_key = DIRECTORY_SUB_KEY,
                    .chunk_no = directory_chunk_,
                }
                || !std::ranges::equal(row.bytes, expected_bytes)) {
                return std::unexpected(MinimizerGenerationWireError::row_order);
            }
            const std::uint64_t checksum = crc64_index_chunk_row(
                row.address.generation_id, row.address.position_segment,
                row.address.sub_key, row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()), row.bytes);
            if (row.row_checksum != checksum) {
                return std::unexpected(MinimizerGenerationWireError::checksum);
            }
            ++directory_chunk_;
            return {};
        }
        if (!manifest_seen_) {
            if (row.address != generation_.manifest_row.address
                || row.row_checksum != generation_.manifest_row.row_checksum
                || !std::ranges::equal(row.bytes,
                    generation_.manifest_row.bytes())) {
                return std::unexpected(MinimizerGenerationWireError::row_order);
            }
            manifest_seen_ = true;
            return {};
        }
        return std::unexpected(MinimizerGenerationWireError::row_count);
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    finish_ready() noexcept {
        if (ready_validated_) {
            return {};
        }
        if (entry_index_ != generation_.directory.entries().size()
            || payload_chunk_ != 0
            || directory_chunk_
                != generation_.manifest.directory_chunk_count + 1u
            || !manifest_seen_) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        if (payload_row_checksums_.checksum()
            != generation_.manifest.payload_checksum) {
            return std::unexpected(MinimizerGenerationWireError::checksum);
        }
        ready_validated_ = true;
        return {};
    }

    /// Validate the separately published active-head row only after READY was
    /// completely reread and accepted.
    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    validate_publication_head(
            const MinimizerGenerationRowView& row) const noexcept {
        if (!ready_validated_) {
            return std::unexpected(MinimizerGenerationWireError::row_count);
        }
        const EncodedMinimizerFixedRow& expected =
            generation_.unpublished_head_row;
        if (row.address != expected.address
            || row.row_checksum != expected.row_checksum
            || !std::ranges::equal(row.bytes, expected.bytes())) {
            return std::unexpected(MinimizerGenerationWireError::row_order);
        }
        return {};
    }
};

inline void index_corruption_panic(std::string_view msg) {
    bioseq_log_progress<ERR>("[bioseq index corruption] {}", msg);
    std::abort();
}

/// Caller-owned per-row/per-query assembly scratch (§9.1). Reused across
/// segments within one query, destroyed with its IndexReadContext.
struct MinimizerAssemblyScratch {
    std::vector<std::pair<std::uint64_t,std::uint64_t>> pairs;
    std::vector<std::uint64_t> query_kmers;
    std::vector<std::uint64_t> bucket_positions;
    std::vector<bioseq::indexing::index_sub_key_t> wanted_sub_keys;
    void clear_segment() noexcept { pairs.clear(); }
    void clear_query() noexcept { pairs.clear(); query_kmers.clear(); bucket_positions.clear(); wanted_sub_keys.clear(); }
};

// ---- shared wire walkers (§9.4) ------------------------------------------

/// Walk one chunk's wire blob (existing v2 layout); emit(kmer, encoded_pos).
template <class F>   // F: (uint64 kmer, uint64 encoded_pos) -> void
void wire_for_each_entry(const char* blob, std::size_t blob_bytes, F&& emit) {
    using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
    using BucketRecord = MinimizerHashmapFragmentIndex::SerializedBucket;
    if (blob_bytes < sizeof(Header)) index_corruption_panic("index chunk: truncated header");
    Header header{}; std::memcpy(&header, blob, sizeof(Header));
    if (header.magic != MinimizerHashmapFragmentIndex::kSerializeMagic ||
        header.version != MinimizerHashmapFragmentIndex::kSerializeVersion) index_corruption_panic("index chunk: magic/version");
    const std::size_t expect = sizeof(Header) + sizeof(BucketRecord)*header.bucket_count + sizeof(std::uint64_t)*header.total_positions;
    if (blob_bytes != expect) index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(Header);
    const char* pos  = recs + sizeof(BucketRecord) * header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < header.bucket_count; ++bucket_no) {
        BucketRecord bucket{}; std::memcpy(&bucket, recs + sizeof(BucketRecord) * bucket_no, sizeof(BucketRecord));
        if ((std::uint64_t)bucket.pos_offset + bucket.pos_count > header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        for (std::uint32_t pos_no = 0; pos_no < bucket.pos_count; ++pos_no) {
            std::uint64_t encoded{};
            std::memcpy(&encoded, pos + sizeof(std::uint64_t) * (bucket.pos_offset + pos_no), sizeof(encoded));
            emit(bucket.hash, encoded);
        }
    }
}

/// PER-KMER sibling of wire_for_each_entry: cb(kmer, pos_count, gpos).
template <class F>   // F: (uint64 kmer, uint32 pos_count, const uint64* gpos) -> void
void wire_for_each_bucket(const char* blob, std::size_t blob_bytes,
                          std::vector<std::uint64_t>& pos_scratch, F&& cb) {
    using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
    using BucketRecord = MinimizerHashmapFragmentIndex::SerializedBucket;
    if (blob_bytes < sizeof(Header)) index_corruption_panic("index chunk: truncated header");
    Header header{}; std::memcpy(&header, blob, sizeof(Header));
    if (header.magic != MinimizerHashmapFragmentIndex::kSerializeMagic ||
        header.version != MinimizerHashmapFragmentIndex::kSerializeVersion) index_corruption_panic("index chunk: magic/version");
    const std::size_t expect = sizeof(Header) + sizeof(BucketRecord)*header.bucket_count + sizeof(std::uint64_t)*header.total_positions;
    if (blob_bytes != expect) index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(Header);
    const char* pos  = recs + sizeof(BucketRecord) * header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < header.bucket_count; ++bucket_no) {
        BucketRecord bucket{}; std::memcpy(&bucket, recs + sizeof(BucketRecord) * bucket_no, sizeof(BucketRecord));
        if ((std::uint64_t)bucket.pos_offset + bucket.pos_count > header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        pos_scratch.resize(bucket.pos_count);
        if (bucket.pos_count > 0)
            std::memcpy(pos_scratch.data(), pos + sizeof(std::uint64_t) * bucket.pos_offset, sizeof(std::uint64_t) * bucket.pos_count);
        cb(bucket.hash, bucket.pos_count, pos_scratch.data());
    }
}

// DOC0345 Patch F — GPU batched mpf helpers. A GPU window covers as many whole
// fragments as fit both the run buffer and the 28-bit position field (with halo).
[[nodiscard]] inline std::size_t gpu_mpf_fragments_per_batch(
    std::size_t nfrag,
    std::size_t frag_bp,
    std::size_t halo_bp,
    std::size_t run_buffer_bytes) noexcept
{
    constexpr std::size_t max_bp =
        static_cast<std::size_t>(MinimapSketchData::pos_field_mask);
    if (nfrag == 0u || frag_bp == 0u) {
        return 1u;
    }
    constexpr std::size_t gpu_segment_local_size = 128u;
    constexpr std::size_t gpu_segment_max_workgroups = 65535u;
    constexpr std::size_t gpu_segment_bytes_per_bp = 96u;
    const std::size_t by_memory = std::max<std::size_t>(
        1u,
        run_buffer_bytes
            / std::max<std::size_t>(
                1u,
                frag_bp * gpu_segment_bytes_per_bp));
    const std::size_t by_pos =
        halo_bp >= max_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_bp - halo_bp) / frag_bp);
    const std::size_t max_dispatch_bp = gpu_segment_local_size * gpu_segment_max_workgroups;
    const std::size_t by_dispatch =
        halo_bp >= max_dispatch_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_dispatch_bp - halo_bp) / frag_bp);
    return std::min({nfrag, by_memory, by_pos, by_dispatch});
}

// Serialize already (kmer, encoded_pos)-sorted pairs (from the GPU sorted-pair
// seam: fused GPU minimizer emit + host pair sort) into the existing mpf
// per-fragment blob format (header + bucket directory + positions). CPU-only
// persistence -- no minimizer collection or sort HERE.
inline void serialize_sorted_pairs_to_fragment_blob(
    std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted_pairs,
    sequence_size_t target_len,
    std::vector<char>& out_blob)
{
    using Header = MinimizerHashmapFragmentIndex::SerializedHeader;
    using Bucket = MinimizerHashmapFragmentIndex::SerializedBucket;

    std::vector<Bucket> buckets;
    std::vector<std::uint64_t> positions;
    buckets.reserve(sorted_pairs.size() / 2u + 1u);
    positions.reserve(sorted_pairs.size());

    std::size_t index = 0;
    while (index < sorted_pairs.size()) {
        const std::uint64_t kmer = sorted_pairs[index].first;
        const std::uint32_t offset =
            static_cast<std::uint32_t>(positions.size());
        std::uint32_t count = 0;
        while (index < sorted_pairs.size() && sorted_pairs[index].first == kmer) {
            positions.push_back(sorted_pairs[index].second);
            ++count;
            ++index;
            if (count == UINT32_MAX) {
                index_corruption_panic(
                    "minimizers_per_fragment GPU blob: kmer count exceeds u32");
            }
        }
        buckets.push_back(Bucket{kmer, offset, count});
    }

    const Header header{
        MinimizerHashmapFragmentIndex::kSerializeMagic,
        MinimizerHashmapFragmentIndex::kSerializeVersion,
        static_cast<std::uint64_t>(target_len),
        static_cast<std::uint32_t>(buckets.size()),
        0u,
        static_cast<std::uint64_t>(positions.size()),
    };
    const std::size_t bytes =
        sizeof(Header)
        + buckets.size() * sizeof(Bucket)
        + positions.size() * sizeof(std::uint64_t);
    out_blob.resize(bytes);
    char* cursor = out_blob.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    if (!buckets.empty()) {
        std::memcpy(cursor, buckets.data(), buckets.size() * sizeof(Bucket));
        cursor += buckets.size() * sizeof(Bucket);
    }
    if (!positions.empty()) {
        std::memcpy(
            cursor,
            positions.data(),
            positions.size() * sizeof(std::uint64_t));
    }
}

/// Operation-originated route evidence for one complete MPF generation encode.
/// The common encoder replays its producer at least once for planning and once
/// for emission; every biological core records the branch it actually entered.
/// Selected SIMD/GPU routes additionally carry their native checked proof, so a
/// caller cannot accept a backend merely because its enum was requested.
class MpfGenerationExecutionEvidence {
public:
    MinimizerSketchBackend selected_backend = MinimizerSketchBackend::Scalar;
    MinimizerSchemeIdentity minimizer_scheme{};
    std::uint64_t target_length = 0;
    std::uint16_t window_size = 0;
    std::uint16_t kmer_size = 0;
    std::uint64_t producer_scan_count = 0;
    std::uint64_t geometry_retry_count = 0;
    std::uint64_t short_core_runs = 0;
    std::uint64_t scalar_core_runs = 0;
    std::uint64_t exact_simd_core_runs = 0;
    std::uint64_t paper_nthash_core_runs = 0;
    std::uint64_t vulkan_core_runs = 0;
    ExactSimdIndexExecutionProof exact_simd_proof{};
    bool exact_simd_proof_present = false;
    PackedPaperNthashIndexStatus packed_paper_status =
        PackedPaperNthashIndexStatus::invalid_state;
    PackedPaperNthashIndexExecutionProof packed_paper_proof{};
    bool packed_paper_proof_present = false;
    bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof vulkan_proof{};
    bool backend_bound = false;
    bool saturated = false;

    [[nodiscard]] bool accepted() const noexcept {
        // The encoder performs one successful planning scan and one replay
        // scan. A fixed-width geometry retry contributes exactly one additional
        // planning scan, so this equation admits retries without weakening the
        // ordinary exact-two-pass contract.
        if (!backend_bound || saturated
            || geometry_retry_count
                > (std::numeric_limits<std::uint64_t>::max)() - 2u
            || producer_scan_count != geometry_retry_count + 2u
            || target_length == 0 || window_size == 0 || kmer_size == 0
            || minimizer_scheme
                != minimizer_scheme_identity_for_backend(selected_backend)) {
            return false;
        }
        const std::uint64_t minimum_work_bases =
            static_cast<std::uint64_t>(window_size) + kmer_size - 1u;
        const bool require_backend_work = target_length >= minimum_work_bases;
        const bool no_exact = exact_simd_core_runs == 0
            && !exact_simd_proof_present;
        const bool no_paper = paper_nthash_core_runs == 0
            && !packed_paper_proof_present;
        const bool no_vulkan = vulkan_core_runs == 0
            && vulkan_proof.completed_roundtrips == 0
            && vulkan_proof.downloaded_minimizers == 0
            && vulkan_proof.gpu_window_splits == 0;

        switch (selected_backend) {
            case MinimizerSketchBackend::Scalar:
                return exact_simd_core_runs == 0
                    && paper_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_exact && no_paper && no_vulkan
                    && (!require_backend_work || scalar_core_runs != 0);
            case MinimizerSketchBackend::ExactSimd:
                return scalar_core_runs == 0
                    && paper_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_paper && no_vulkan
                    && (exact_simd_core_runs == 0
                        ? (!require_backend_work
                            && !exact_simd_proof_present)
                        : (exact_simd_proof_present
                            && exact_simd_proof.accepted(
                                require_backend_work)));
            case MinimizerSketchBackend::PaperNtHashSimd:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_exact && no_vulkan
                    && (paper_nthash_core_runs == 0
                        ? (!require_backend_work
                            && !packed_paper_proof_present)
                        : (packed_paper_proof_present
                            && packed_paper_status
                                == PackedPaperNthashIndexStatus::ok
                            && packed_paper_proof.accepted(
                                require_backend_work)));
            case MinimizerSketchBackend::VulkanSegment:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && paper_nthash_core_runs == 0
                    && no_exact && no_paper
                    && (vulkan_core_runs == 0
                        ? (!require_backend_work
                            && vulkan_proof.accepted(false))
                        : vulkan_proof.accepted(require_backend_work));
        }
        return false;
    }

    [[nodiscard]] bool publication_accepted(
            const MinimizerGenerationPublicationBinding& expected)
            const noexcept {
        return accepted() && publication_binding_present_
            && expected.valid() && publication_binding_ == expected;
    }

private:
    template <class>
    friend struct MinimizersPerFragmentIndex;

    MinimizerGenerationPublicationBinding publication_binding_{};
    bool publication_binding_present_ = false;

    void bind_publication_(
            const EncodedMinimizerGeneration& generation) noexcept {
        publication_binding_ =
            minimizer_generation_publication_binding(generation);
        publication_binding_present_ = publication_binding_.valid();
    }
};

/// Concrete MPF revision-3 producer. Each replay rescans source fragments with
/// the canonical `w+k-2` halo, retains at most one persisted core's fragment
/// runs, and k-way merges those bounded runs into `(segment,hash,position)`
/// order. It never owns a target-wide event array.
template <class R, class Scanner>
class MpfFragmentGenerationReplayProducer {
private:
    using sequence_type = lazy_compact_sequence<R, Scanner>;
    using traits = typename sequence_type::traits;

    sequence_type* sequence_ = nullptr;
    MinimizerSketchBackend backend_ = MinimizerSketchBackend::Scalar;
    std::uint16_t window_size_ = 10;
    std::uint16_t kmer_size_ = 15;
    std::uint64_t scan_count_ = 0;
    std::uint64_t geometry_retry_count_ = 0;
    std::uint64_t last_scan_span_ = 0;
    bool has_last_scan_span_ = false;
    std::uint64_t peak_buffered_events_ = 0;
    std::uint64_t peak_buffered_core_bp_ = 0;
    std::uint64_t peak_active_fragment_runs_ = 0;
    std::uint64_t peak_paper_materialized_window_bp_ = 0;
    ExactSimdIndexExecutionProof exact_simd_proof_{};
    bool exact_simd_proof_present_ = false;
    PackedPaperNthashIndexStatus packed_paper_status_ =
        PackedPaperNthashIndexStatus::invalid_state;
    PackedPaperNthashIndexExecutionProof packed_paper_proof_{};
    bool packed_paper_proof_present_ = false;
    std::uint64_t short_core_runs_ = 0;
    std::uint64_t scalar_core_runs_ = 0;
    std::uint64_t exact_simd_core_runs_ = 0;
    std::uint64_t paper_nthash_core_runs_ = 0;
    std::uint64_t vulkan_core_runs_ = 0;
    bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof vulkan_proof_{};
    bool route_evidence_saturated_ = false;

    void increment_route_counter_(std::uint64_t& counter) noexcept {
        if (counter == (std::numeric_limits<std::uint64_t>::max)()) {
            route_evidence_saturated_ = true;
            return;
        }
        ++counter;
    }

    void record_packed_paper_result(
            const PackedPaperNthashIndexBridgeResult& result) noexcept {
        packed_paper_status_ = result.status;
        packed_paper_proof_.combine(result.proof);
        packed_paper_proof_present_ = true;
    }

    [[nodiscard]] std::expected<
        std::vector<CanonicalMinimizerEvent>,
        MinimizerGenerationWireError>
    collect_core_run(
            std::uint64_t core_lo,
            std::uint64_t core_hi) {
        constexpr std::uint64_t lpu = traits::letters_per_unit;
        const std::uint64_t target_length = sequence_->bp_size();
        const std::uint64_t halo = static_cast<std::uint64_t>(window_size_)
            + kmer_size_ - 2u;
        const std::uint64_t collect_lo = core_lo >= halo
            ? core_lo - halo : 0u;
        const std::uint64_t collect_hi = core_hi
                > target_length - std::min(target_length, halo)
            ? target_length : core_hi + halo;
        const std::uint64_t lo_unit = collect_lo / lpu;
        const std::uint64_t hi_unit = collect_hi / lpu
            + (collect_hi % lpu != 0 ? 1u : 0u);
        const std::uint64_t size_units = target_length / lpu
            + (target_length % lpu != 0 ? 1u : 0u);
        if (lo_unit > hi_unit || hi_unit > size_units) {
            return std::unexpected(MinimizerGenerationWireError::bp_range);
        }
        const std::uint64_t emit_base = lo_unit * lpu;
        const std::uint64_t minimum_bases =
            static_cast<std::uint64_t>(window_size_) + kmer_size_ - 1u;
        std::vector<CanonicalMinimizerEvent> run;
        if (collect_hi <= emit_base
            || collect_hi - emit_base < minimum_bases) {
            increment_route_counter_(short_core_runs_);
            return run;
        }

        if (backend_ == MinimizerSketchBackend::VulkanSegment) {
            increment_route_counter_(vulkan_core_runs_);
            bioseq::gpu::vulkan::VulkanSegmentPairExecutionProof core_proof{};
            auto pairs =
                bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                    *sequence_,
                    static_cast<std::size_t>(lo_unit),
                    static_cast<std::size_t>(hi_unit),
                    emit_base,
                    core_lo,
                    core_hi,
                    window_size_,
                    kmer_size_,
                    &core_proof);
            vulkan_proof_.combine(core_proof);
            run.reserve(pairs.size());
            for (const auto& pair : pairs) {
                run.push_back(CanonicalMinimizerEvent{
                    .hash = pair.first,
                    .encoded_position = pair.second,
                });
            }
        } else {
            sketch_collector collector;
            if (backend_ == MinimizerSketchBackend::PaperNtHashSimd) {
                increment_route_counter_(paper_nthash_core_runs_);
                std::uint64_t materialized_window_bp = 0;
                const auto paper_result =
                    collect_bounded_packed_paper_nthash_index_lazy(
                        *sequence_,
                        lo_unit,
                        hi_unit,
                        collect_hi - emit_base,
                        window_size_,
                        kmer_size_,
                        collector,
                        &materialized_window_bp);
                peak_paper_materialized_window_bp_ = std::max(
                    peak_paper_materialized_window_bp_,
                    materialized_window_bp);
                record_packed_paper_result(paper_result);
                if (paper_result.status
                    != PackedPaperNthashIndexStatus::ok) {
                    return std::unexpected(
                        MinimizerGenerationWireError::compatibility_mismatch);
                }
            } else if (backend_ == MinimizerSketchBackend::ExactSimd) {
                increment_route_counter_(exact_simd_core_runs_);
                try {
                    const auto proof = collect_exact_simd_index_range_or_throw(
                        collector,
                        *sequence_,
                        static_cast<std::size_t>(lo_unit),
                        static_cast<std::size_t>(hi_unit),
                        window_size_,
                        kmer_size_,
                        0,
                        "MPF generation replay");
                    exact_simd_proof_.combine(proof);
                    exact_simd_proof_present_ = true;
                } catch (const ExactSimdIndexRuntimeError&) {
                    return std::unexpected(
                        MinimizerGenerationWireError::compatibility_mismatch);
                }
            } else if (backend_ == MinimizerSketchBackend::Scalar) {
                increment_route_counter_(scalar_core_runs_);
                auto begin = sequence_->cbegin();
                begin += static_cast<sequence_size_t>(lo_unit);
                auto end = sequence_->cbegin();
                end += static_cast<sequence_size_t>(hi_unit);
                collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
                    collector,
                    *sequence_,
                    begin,
                    end,
                    window_size_,
                    kmer_size_);
            } else {
                return std::unexpected(
                    MinimizerGenerationWireError::compatibility_mismatch);
            }
            const auto& sketches = collector.extract_sketches_data();
            run.reserve(sketches.size());
            for (const MinimapSketchData& minimizer : sketches) {
                const std::uint64_t position = emit_base
                    + static_cast<std::uint64_t>(minimizer.last_pos());
                if (position < core_lo || position >= core_hi) {
                    continue;
                }
                run.push_back(CanonicalMinimizerEvent{
                    .hash = minimizer.kmer(),
                    .encoded_position = (position << 1u)
                        | static_cast<std::uint64_t>(minimizer.strand()),
                });
            }
        }
        std::ranges::sort(run, {}, [](const CanonicalMinimizerEvent& event) {
            return std::pair{event.hash, event.encoded_position};
        });
        return run;
    }

public:
    MpfFragmentGenerationReplayProducer(
            sequence_type& sequence,
            MinimizerSketchBackend backend,
            std::uint16_t window_size = 10,
            std::uint16_t kmer_size = 15) noexcept
        : sequence_(&sequence),
          backend_(backend),
          window_size_(window_size),
          kmer_size_(kmer_size) {}

    [[nodiscard]] MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        return minimizer_scheme_identity_for_backend(backend_);
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    scan_sorted_events(
            std::uint64_t persisted_span,
            CanonicalMinimizerEventReceiver receiver) {
        constexpr std::uint64_t lpu = traits::letters_per_unit;
        constexpr std::uint64_t fragment_units =
            Scanner::fragment_storage_units;
        static_assert(fragment_units
            <= (std::numeric_limits<std::uint64_t>::max)() / lpu);
        constexpr std::uint64_t fragment_bp = fragment_units * lpu;
        increment_route_counter_(scan_count_);
        if (has_last_scan_span_ && persisted_span != last_scan_span_) {
            increment_route_counter_(geometry_retry_count_);
        }
        last_scan_span_ = persisted_span;
        has_last_scan_span_ = true;
        if (sequence_ == nullptr || persisted_span == 0
            || fragment_bp == 0 || window_size_ == 0 || kmer_size_ == 0
            || static_cast<std::uint32_t>(window_size_)
                    + static_cast<std::uint32_t>(kmer_size_) < 2u) {
            return std::unexpected(MinimizerGenerationWireError::segment_span);
        }
        if (backend_ != MinimizerSketchBackend::Scalar
            && backend_ != MinimizerSketchBackend::ExactSimd
            && backend_ != MinimizerSketchBackend::PaperNtHashSimd
            && backend_ != MinimizerSketchBackend::VulkanSegment) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        const std::uint64_t target_length = sequence_->bp_size();
        const std::uint64_t expected_units = target_length / lpu
            + (target_length % lpu != 0 ? 1u : 0u);
        if (static_cast<std::uint64_t>(sequence_->size()) != expected_units) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }

        std::uint64_t segment_lo = 0;
        while (segment_lo < target_length) {
            const std::uint64_t segment_width = std::min(
                persisted_span, target_length - segment_lo);
            const std::uint64_t segment_hi = segment_lo + segment_width;
            std::vector<std::vector<CanonicalMinimizerEvent>> runs;
            std::uint64_t buffered_events = 0;
            std::uint64_t fragment_lo = (segment_lo / fragment_bp) * fragment_bp;
            while (fragment_lo < segment_hi) {
                const std::uint64_t fragment_hi = std::min(
                    target_length, fragment_lo + fragment_bp);
                const std::uint64_t core_lo = std::max(segment_lo, fragment_lo);
                const std::uint64_t core_hi = std::min(segment_hi, fragment_hi);
                if (core_lo < core_hi) {
                    auto run = collect_core_run(core_lo, core_hi);
                    if (!run) {
                        return std::unexpected(run.error());
                    }
                    if (buffered_events
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - run->size()) {
                        return std::unexpected(
                            MinimizerGenerationWireError::arithmetic_overflow);
                    }
                    buffered_events += run->size();
                    if (!run->empty()) {
                        runs.push_back(std::move(*run));
                    }
                }
                if (fragment_hi <= fragment_lo) {
                    return std::unexpected(
                        MinimizerGenerationWireError::arithmetic_overflow);
                }
                fragment_lo = fragment_hi;
            }
            peak_buffered_events_ = std::max(
                peak_buffered_events_, buffered_events);
            peak_buffered_core_bp_ = std::max(
                peak_buffered_core_bp_, segment_width);
            peak_active_fragment_runs_ = std::max<std::uint64_t>(
                peak_active_fragment_runs_, runs.size());

            struct Cursor {
                std::size_t run = 0;
                std::size_t index = 0;
            };
            const auto after = [&runs](const Cursor& lhs, const Cursor& rhs) {
                const auto& left = runs[lhs.run][lhs.index];
                const auto& right = runs[rhs.run][rhs.index];
                return std::pair{left.hash, left.encoded_position}
                    > std::pair{right.hash, right.encoded_position};
            };
            std::priority_queue<
                Cursor, std::vector<Cursor>, decltype(after)> heap{after};
            for (std::size_t run = 0; run < runs.size(); ++run) {
                heap.push(Cursor{.run = run, .index = 0});
            }
            while (!heap.empty()) {
                Cursor cursor = heap.top();
                heap.pop();
                if (auto accepted = receiver(
                        runs[cursor.run][cursor.index]); !accepted) {
                    return accepted;
                }
                ++cursor.index;
                if (cursor.index < runs[cursor.run].size()) {
                    heap.push(cursor);
                }
            }
            segment_lo = segment_hi;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return scan_count_;
    }
    [[nodiscard]] std::uint64_t peak_buffered_events() const noexcept {
        return peak_buffered_events_;
    }
    [[nodiscard]] std::uint64_t peak_buffered_core_bp() const noexcept {
        return peak_buffered_core_bp_;
    }
    [[nodiscard]] std::uint64_t peak_active_fragment_runs() const noexcept {
        return peak_active_fragment_runs_;
    }
    [[nodiscard]] std::uint64_t
    peak_paper_materialized_window_bp() const noexcept {
        return peak_paper_materialized_window_bp_;
    }
    [[nodiscard]] bool exact_simd_proof_present() const noexcept {
        return exact_simd_proof_present_;
    }
    [[nodiscard]] const ExactSimdIndexExecutionProof&
    exact_simd_proof() const noexcept {
        return exact_simd_proof_;
    }
    [[nodiscard]] bool packed_paper_proof_present() const noexcept {
        return packed_paper_proof_present_;
    }
    [[nodiscard]] PackedPaperNthashIndexStatus
    packed_paper_status() const noexcept {
        return packed_paper_status_;
    }
    [[nodiscard]] const PackedPaperNthashIndexExecutionProof&
    packed_paper_proof() const noexcept {
        return packed_paper_proof_;
    }
    [[nodiscard]] MpfGenerationExecutionEvidence
    execution_evidence() const noexcept {
        MpfGenerationExecutionEvidence evidence;
        evidence.selected_backend = backend_;
        evidence.minimizer_scheme = minimizer_scheme_identity();
        evidence.target_length =
            sequence_ == nullptr ? 0u : sequence_->bp_size();
        evidence.window_size = window_size_;
        evidence.kmer_size = kmer_size_;
        evidence.producer_scan_count = scan_count_;
        evidence.geometry_retry_count = geometry_retry_count_;
        evidence.short_core_runs = short_core_runs_;
        evidence.scalar_core_runs = scalar_core_runs_;
        evidence.exact_simd_core_runs = exact_simd_core_runs_;
        evidence.paper_nthash_core_runs = paper_nthash_core_runs_;
        evidence.vulkan_core_runs = vulkan_core_runs_;
        evidence.exact_simd_proof = exact_simd_proof_;
        evidence.exact_simd_proof_present = exact_simd_proof_present_;
        evidence.packed_paper_status = packed_paper_status_;
        evidence.packed_paper_proof = packed_paper_proof_;
        evidence.packed_paper_proof_present = packed_paper_proof_present_;
        evidence.vulkan_proof = vulkan_proof_;
        evidence.backend_bound = true;
        evidence.saturated = route_evidence_saturated_
            || vulkan_proof_.saturated;
        return evidence;
    }
};

// (2) the storage-bound CHUNKED_PER_FRAGMENT index template
template <class S>
struct MinimizersPerFragmentIndex {
    static_assert(is_index_chunked_storage_for<S, SequenceIndexType::CHUNKED_PER_FRAGMENT>);
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = MinimizerHashmapFragmentIndex::index_name;
    using storage_type = S;

    template <class R, class Scanner>
    [[nodiscard]] static auto make_generation_replay_producer(
            lazy_compact_sequence<R, Scanner>& sequence,
            MinimizerSketchBackend backend) {
        return MpfFragmentGenerationReplayProducer<R, Scanner>{
            sequence, backend};
    }

    /// Concrete MPF construction seam. Live v2 routing remains unchanged;
    /// this only proves the bounded source-fragment producer used by rev3.
    template <class R, class Scanner, MinimizerGenerationRowSink Sink>
    [[nodiscard]] static std::expected<
        EncodedMinimizerGeneration,
        MinimizerGenerationWireError>
    encode_persisted_generation(
            lazy_compact_sequence<R, Scanner>& sequence,
            MinimizerSketchBackend backend,
            Sink& sink,
            const MinimizerGenerationEncodeOptions& options,
            const MinimizerGenerationFixedWidthLimits& limits = {},
            MpfGenerationExecutionEvidence* execution_evidence_out = nullptr) {
        if (execution_evidence_out != nullptr) {
            *execution_evidence_out = {};
        }
        if (options.window_size != 10 || options.kmer_size != 15) {
            return std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        auto producer = make_generation_replay_producer(sequence, backend);
        auto encoded = encode_minimizer_generation(
            producer, sink, options, limits);
        MpfGenerationExecutionEvidence evidence =
            producer.execution_evidence();
        if (encoded) {
            evidence.bind_publication_(*encoded);
        }
        if (encoded && !evidence.publication_accepted(
                minimizer_generation_publication_binding(*encoded))) {
            encoded = std::unexpected(
                MinimizerGenerationWireError::compatibility_mismatch);
        }
        if (execution_evidence_out != nullptr) {
            *execution_evidence_out = evidence;
        }
        return encoded;
    }

    template <class R, class Scanner>
    static void build(
        S& storage,
        lazy_compact_sequence<R, Scanner>& seq,
        MinimizerSketchBackend backend) {
        using traits = typename lazy_compact_sequence<R, Scanner>::traits;
        constexpr std::size_t w = 10, k = 15;                                  // pinned, == sidecar today
        constexpr std::size_t lpu        = traits::letters_per_unit;
        constexpr std::size_t frag_units = Scanner::fragment_storage_units;    // 1024
        constexpr std::size_t frag_bp    = frag_units * lpu;                   // e.g. 32768 @ dna4/uint64
        constexpr std::size_t W          = w + k;                              // backward context
        static_assert(frag_bp < (std::size_t{1} << 28),
            "fragment-core-local positions must fit the 28-bit MinimapSketchData field");
        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if ((std::size_t)seq.size() != (std::size_t)((total_bp + lpu - 1) / lpu))
            index_corruption_panic("minimizers_per_fragment build: scanner unit-span != ceil(total_bp/lpu) (fragment geometry)");

        const std::size_t size_u = (std::size_t)((total_bp + lpu - 1) / lpu);   // total units

        // DOC0345 Patch F — GPU path: batch whole fragments through the GPU
        // sorted-pair seam (fused GPU minimizer emit + host pair sort), scatter
        // each global position into its fragment, and serialize the existing
        // per-fragment blob format. No CPU minimizer COLLECT. Exits before the
        // scalar per-fragment path below.
        if (backend == MinimizerSketchBackend::VulkanSegment) {
            const std::size_t fragments_per_batch = gpu_mpf_fragments_per_batch(
                nfrag,
                frag_bp,
                2u * W,
                storage.run_buffer_bytes());
            std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> per_fragment;
            std::vector<char> blob;
            per_fragment.resize(fragments_per_batch);

            auto write_fragment_blob =
                [&storage](std::size_t fragment_no, const std::vector<char>& fragment_blob) {
                const std::size_t bytes = fragment_blob.size();
                storage.resize(static_cast<index_sub_key_t>(fragment_no), bytes);
                storage.template write_to_each<char>(
                    static_cast<index_sub_key_t>(fragment_no),
                    [&fragment_blob, bytes](
                        std::size_t chunk_no,
                        char* buffer,
                        std::size_t capacity_units) {
                    DEBUG_ASSERT(
                        chunk_no == 0 && capacity_units >= bytes,
                        "one fragment index fits one chunk");
                    if (bytes != 0u) {
                        std::memcpy(buffer, fragment_blob.data(), bytes);
                    }
                    return chunk_no;
                });
            };

            for (std::size_t batch_lo_frag = 0;
                 batch_lo_frag < nfrag;
                 batch_lo_frag += fragments_per_batch) {
                const std::size_t batch_hi_frag =
                    std::min(nfrag, batch_lo_frag + fragments_per_batch);
                const sequence_size_t core_lo =
                    static_cast<sequence_size_t>(batch_lo_frag) * frag_bp;
                const sequence_size_t core_hi = std::min(
                    static_cast<sequence_size_t>(batch_hi_frag) * frag_bp,
                    total_bp);
                const sequence_size_t coll_lo = core_lo >= W ? core_lo - W : 0;
                const sequence_size_t coll_hi = std::min(
                    core_hi + static_cast<sequence_size_t>(W),
                    total_bp);
                const std::size_t lo_unit =
                    static_cast<std::size_t>(coll_lo / lpu);
                const std::size_t hi_unit = std::min(
                    static_cast<std::size_t>((coll_hi + lpu - 1) / lpu),
                    size_u);
                const sequence_size_t emit_base =
                    static_cast<sequence_size_t>(lo_unit) * lpu;

                std::vector<std::pair<std::uint64_t, std::uint64_t>> batch_pairs =
                    bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                        seq,
                        lo_unit,
                        hi_unit,
                        emit_base,
                        core_lo,
                        core_hi,
                        w,
                        k);

                for (auto& fragment_pairs : per_fragment) {
                    fragment_pairs.clear();
                }
                for (const auto& pair : batch_pairs) {
                    const sequence_size_t global_pos =
                        static_cast<sequence_size_t>(pair.second >> 1);
                    const std::size_t fragment_no =
                        static_cast<std::size_t>(global_pos / frag_bp);
                    if (fragment_no < batch_lo_frag || fragment_no >= batch_hi_frag) {
                        continue;
                    }
                    const sequence_size_t fragment_core_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    const std::uint64_t local_encoded =
                        (static_cast<std::uint64_t>(global_pos - fragment_core_lo) << 1)
                        | (pair.second & 1ull);
                    per_fragment[fragment_no - batch_lo_frag].emplace_back(
                        pair.first,
                        local_encoded);
                }

                for (std::size_t fragment_no = batch_lo_frag;
                     fragment_no < batch_hi_frag;
                     ++fragment_no) {
                    const sequence_size_t fragment_core_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    const sequence_size_t fragment_core_hi = std::min(
                        fragment_core_lo + static_cast<sequence_size_t>(frag_bp),
                        total_bp);
                    std::span<const std::pair<std::uint64_t, std::uint64_t>> fragment_pairs{
                        per_fragment[fragment_no - batch_lo_frag].data(),
                        per_fragment[fragment_no - batch_lo_frag].size()};
                    serialize_sorted_pairs_to_fragment_blob(
                        fragment_pairs,
                        fragment_core_hi - fragment_core_lo,
                        blob);
                    write_fragment_blob(fragment_no, blob);
                }
            }

            const std::int64_t manifest = static_cast<std::int64_t>(nfrag);
            storage.resize(DIRECTORY_SUB_KEY, sizeof(std::int64_t));
            storage.template write_to_each<std::int64_t>(
                DIRECTORY_SUB_KEY,
                [manifest](
                    std::size_t chunk_no,
                    std::int64_t* manifest_buffer,
                    std::size_t) {
                manifest_buffer[0] = manifest;
                return chunk_no;
            });
            return;
        }

        auto build_fragment_blob =
            [&seq, backend, total_bp, size_u](std::size_t fragment_no,
                                     sketch_collector& collector,
                                     std::vector<MinimapSketchData>& core,
                                     std::vector<char>& out_blob) {
            const sequence_size_t core_lo = (sequence_size_t)fragment_no * frag_bp;
            const sequence_size_t core_hi = std::min(core_lo + (sequence_size_t)frag_bp, total_bp);
            const sequence_size_t coll_lo = core_lo >= W ? core_lo - W : 0;
            const sequence_size_t coll_hi = std::min(core_hi + (sequence_size_t)W, total_bp);
            collector.clear();
            const std::size_t lo_unit = (std::size_t)(coll_lo / lpu);
            const std::size_t hi_unit = std::min((std::size_t)((coll_hi + lpu - 1) / lpu), size_u);
            DEBUG_ASSERT(lo_unit <= hi_unit && hi_unit <= size_u, "sub-range unit bounds out of range");
            auto begin_iter = seq.cbegin(); begin_iter += (sequence_size_t)lo_unit;
            auto end_iter   = seq.cbegin(); end_iter   += (sequence_size_t)hi_unit;
            const sequence_size_t emit_base = (sequence_size_t)lo_unit * lpu;
            const sequence_size_t collect_bp = coll_hi > emit_base ? coll_hi - emit_base : 0;
            core.clear();
            if (collect_bp >= (sequence_size_t)W) {
                if (backend == MinimizerSketchBackend::ExactSimd) {
                    (void)collect_exact_simd_index_range_or_throw(
                            collector,
                            seq,
                            lo_unit,
                            hi_unit,
                            w,
                            k,
                            0,
                            "minimizers_per_fragment build");
                } else if (backend == MinimizerSketchBackend::Scalar) {
                    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(collector, seq, begin_iter, end_iter, w, k);
                } else {
                    index_corruption_panic(
                        "minimizers_per_fragment build: unsupported sketch backend");
                }
                for (const MinimapSketchData& minimizer : collector.extract_sketches_data()) {
                    const sequence_size_t global_pos = (sequence_size_t)minimizer.last_pos() + emit_base;
                    if (global_pos < core_lo || global_pos >= core_hi) continue; // belongs to a neighbour fragment's core
                    core.push_back(MinimapSketchData{ minimizer.kmer(), (std::uint64_t)(global_pos - core_lo), minimizer.strand() });
                }
            }
            MinimizerHashmapFragmentIndex frag_idx;
            frag_idx.build(core, core_hi - core_lo);   // fragment-local target_len
            out_blob.resize(frag_idx.serialize_binary_size());
            frag_idx.serialize_to_binary(out_blob.data());
        };

        auto write_fragment_blob =
            [&storage](std::size_t fragment_no, const std::vector<char>& blob) {
            const std::size_t bytes = blob.size();
            storage.resize((index_sub_key_t)fragment_no, bytes);
            storage.template write_to_each<char>((index_sub_key_t)fragment_no,
                [&blob, bytes](std::size_t chunk_no, char* buf, std::size_t cap_units) {
                DEBUG_ASSERT(chunk_no == 0 && cap_units >= bytes, "one fragment index fits one chunk");
                if (bytes > 0) {
                    std::memcpy(buf, blob.data(), bytes);
                }
                return chunk_no;
            });
        };

        auto build_and_write_serial =
            [&build_fragment_blob, &write_fragment_blob](std::size_t fragment_no) {
            sketch_collector collector;
            std::vector<MinimapSketchData> core;
            std::vector<char> blob;
            build_fragment_blob(fragment_no, collector, core, blob);
            write_fragment_blob(fragment_no, blob);
        };

        auto& pool = bioseq::parallelism::GlobalPool::instance();
        const bool may_parallel_fragments =
            bioseq::parallelism::task_parallel(pool.mode())
            && pool.max_concurrency() > 1
            && bioseq::storage::lazy_is_worker_shareable(seq)
            && nfrag >= bioseq::parallelism::TaskParallelMinTriggers::MIN_FRAGMENTS_COUNT;

        if (!may_parallel_fragments) {
            for (std::size_t fragment_no = 0; fragment_no < nfrag; ++fragment_no) {
                build_and_write_serial(fragment_no);
            }
        } else {
            const std::uint64_t build_peak =
                bioseq::parallelism::minimizer_index_peak_bytes_for_bp(frag_bp);
            const std::uint64_t peak_per_fragment =
                bioseq::parallelism::saturating_mul(2, build_peak);

            for (std::size_t wave_lo = 0; wave_lo < nfrag;) {
                auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
                    pool.max_concurrency(), peak_per_fragment);
                if (!wave.admitted) {
                    build_and_write_serial(wave_lo);
                    ++wave_lo;
                    continue;
                }

                const std::size_t wave_hi = std::min(
                    nfrag, wave_lo + static_cast<std::size_t>(wave.fanout));
                std::vector<std::vector<char>> blobs(wave_hi - wave_lo);

                bioseq::parallelism::parallel_for_pipeline(
                    0, blobs.size(), 1,
                    bioseq::parallelism::TaskParallelMinTriggers::MIN_FRAGMENTS_COUNT,
                    bioseq::parallelism::TaskParallelRegion::fragment_build,
                    [&build_fragment_blob, &blobs, wave_lo](
                            std::size_t lo, std::size_t hi) {
                        sketch_collector collector;
                        std::vector<MinimapSketchData> core;
                        for (std::size_t rel = lo; rel < hi; ++rel) {
                            build_fragment_blob(wave_lo + rel, collector, core, blobs[rel]);
                        }
                    });

                for (std::size_t rel = 0; rel < blobs.size(); ++rel) {
                    write_fragment_blob(wave_lo + rel, blobs[rel]);
                }
                wave_lo = wave_hi;
            }
        }
        // COMPLETENESS MANIFEST (written LAST): a tiny chunk at DIRECTORY_SUB_KEY recording nfrag.
        { const std::int64_t manifest = (std::int64_t)nfrag;
          storage.resize(DIRECTORY_SUB_KEY, sizeof(std::int64_t));
          storage.template write_to_each<std::int64_t>(DIRECTORY_SUB_KEY, [manifest](std::size_t chunk_no, std::int64_t* manifest_buf, std::size_t){ manifest_buf[0]=manifest; return chunk_no; }); }
    }

    // DOC0345 Patch F — scalar-default overload delegates to the explicit backend.
    template <class R, class Scanner>
    static void build(S& storage, lazy_compact_sequence<R, Scanner>& seq) {
        build(storage, seq, MinimizerSketchBackend::Scalar);
    }

    template <class R, class Scanner>
    static void rebuild(
        S& storage,
        lazy_compact_sequence<R, Scanner>& seq,
        MinimizerSketchBackend backend)
    {
        storage.clear_entity();
        build(storage, seq, backend);
    }

    template <class R, class Scanner>
    static void rebuild(S& storage, lazy_compact_sequence<R, Scanner>& seq) {
        rebuild(storage, seq, MinimizerSketchBackend::Scalar);
    }

    /// query-time: materialise the single pipeline IndexT for one target SEGMENT.
    static MinimizerHashmapFragmentIndex assemble_segment(
            S& storage, std::size_t frag_lo, std::size_t frag_hi, sequence_size_t frag_bp,
            sequence_size_t total_bp, sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            MinimizerAssemblyScratch& scratch) {
        auto& pairs = scratch.pairs;
        scratch.clear_segment();
        const std::size_t expected_nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(DIRECTORY_SUB_KEY) != 1) storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(DIRECTORY_SUB_KEY, 0);
        if (*manifest_p != (std::int64_t)expected_nfrag) storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag) storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = (index_sub_key_t)frag_lo;
        storage.read_chunks_in_range((index_sub_key_t)frag_lo, (index_sub_key_t)frag_hi,
                [&](index_sub_key_t f, const char* blob, std::size_t blob_bytes){
            if (f != expected_f) storage.index_error("minimizers_per_fragment: missing or duplicate fragment chunk");
            ++expected_f;
            const sequence_size_t off = (sequence_size_t)f * frag_bp;
            storage.validate_minimizer_blob_or_error(blob, blob_bytes);
            wire_for_each_entry(blob, blob_bytes,
                [&](std::uint64_t kmer, std::uint64_t local_encoded){
                    const sequence_size_t gpos = (sequence_size_t)(local_encoded >> 1) + off;
                    if (gpos < clip_lo_bp || gpos >= clip_hi_bp) return;      // CLIP to the segment's collection range
                    pairs.emplace_back(kmer, ((std::uint64_t)gpos << 1) | (local_encoded & 1ULL));
                });
        });
        if (expected_f != (index_sub_key_t)frag_hi)
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        MinimizerHashmapFragmentIndex merged;
        merged.build_from_pairs(pairs, total_bp);   // FULL target length
        return merged;
    }

    /// Query-filtered segment assembly for GPU seed lookup. Only query kmers
    /// can be probed by the seed stage, so persisted MPF does not need to
    /// materialise unrelated buckets before uploading the segment index.
    static MinimizerHashmapFragmentIndex assemble_segment_for_query(
            S& storage,
            std::span<const MinimapSketchData> query_minimizers,
            std::size_t frag_lo, std::size_t frag_hi, sequence_size_t frag_bp,
            sequence_size_t total_bp, sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            MinimizerAssemblyScratch& scratch) {
        scratch.clear_query();
        auto& pairs = scratch.pairs;
        auto& qkmers = scratch.query_kmers;
        qkmers.reserve(query_minimizers.size());
        for (const MinimapSketchData& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
        }
        std::sort(qkmers.begin(), qkmers.end());
        qkmers.erase(std::unique(qkmers.begin(), qkmers.end()), qkmers.end());

        clip_hi_bp = std::min(clip_hi_bp, total_bp);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }

        const std::size_t expected_nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(DIRECTORY_SUB_KEY) != 1) storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(DIRECTORY_SUB_KEY, 0);
        if (*manifest_p != (std::int64_t)expected_nfrag) storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag) storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = (index_sub_key_t)frag_lo;
        storage.read_chunks_in_range((index_sub_key_t)frag_lo, (index_sub_key_t)frag_hi,
                [&](index_sub_key_t f, const char* blob, std::size_t blob_bytes){
            if (f != expected_f) storage.index_error("minimizers_per_fragment: missing or duplicate fragment chunk");
            ++expected_f;
            const sequence_size_t off = (sequence_size_t)f * frag_bp;
            storage.validate_minimizer_blob_or_error(blob, blob_bytes);
            wire_for_each_entry(blob, blob_bytes,
                [&](std::uint64_t kmer, std::uint64_t local_encoded){
                    if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer)) return;
                    const sequence_size_t gpos = (sequence_size_t)(local_encoded >> 1) + off;
                    if (gpos < clip_lo_bp || gpos >= clip_hi_bp) return;
                    pairs.emplace_back(kmer, ((std::uint64_t)gpos << 1) | (local_encoded & 1ULL));
                });
        });
        if (expected_f != (index_sub_key_t)frag_hi)
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        MinimizerHashmapFragmentIndex merged;
        merged.build_from_pairs(pairs, total_bp);
        return merged;
    }
};

}  // namespace bioseq::indexing

export namespace bioseq::indexing {
template <> struct index_descriptor<MinimizersPerFragmentIndex> {
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_FRAGMENT;
    static constexpr std::string_view  index_name = MinimizerHashmapFragmentIndex::index_name;
    static constexpr bool allows_update_on_insert = true;   // cheap per-fragment build; DEFAULT on-insert
};
static_assert(requires { index_descriptor<MinimizersPerFragmentIndex>::index_name; },
              "MinimizersPerFragmentIndex index_descriptor must be exported");
}
