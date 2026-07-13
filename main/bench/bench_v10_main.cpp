/**
 * @file bench_v10_main.cpp
 * @brief Aligner bench entry point on top of bench_framework (V10).
 *
 * Replaces the legacy Catch2-based harness in `bench/bench_main.cpp` and
 * its surrounding `bench/aligners/` + `bench/bench_scheduler/` trees.
 * One TU per binary, three lambdas (`build_input` / `run_aligner` /
 * `evaluate_aligner`), one `bioseq::bench_framework::run(...)` call.
 *
 * Benchmark blocks declared in `bench_v10_configuration.json` include:
 *   * bench_grouped32_local32_validation — benchmark-only contiguous u32 validation POC
 *   * bench_aligners_grid          — full length × error × algorithm matrix
 *   * bench_aligners_repr_storage  — 32-bit / 128-bit MinimizerRepr cases
 *   * bench_aligners_rmq_repro     — pinned LONG/CCS RMQ-chaining seed
 *   * bench_aligners_curated       — curated correctness probes
 *   * bench_aligners_xxl_hifi_relative — report_76 relative-performance gate
 *   * bench_aligners_xxl_hifi_paired — serial paired-ratio companion gate
 *
 * Sample input reference: bench_framework/example_main.cpp.
 */
#include "common_simd.h"
#include "common.h"

#include <simdjson.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.simd.utils;
import bioseq.simd.vector_stream;
import bioseq.sequences;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.chaining.simd;
import bioseq.alignment.block_aligner;
import bioseq.sequences.minimap_port;
import bioseq.sequences.pipe;            // for minimap_port entry point (DOC0249 §9.7)
import bioseq.storage.lazy;             // DOC0263 — make_lazy_in_memory / lazy wrapper
import bioseq.instr;
import bioseq.parallelism.memory_admission;
import bench_framework;
import bench.tool_minimap2;
import bioseq.sequences.generation;
import bioseq.gpu.vulkan.runtime;

#ifndef BIOSEQ_BENCH_REPORT_DIR
#error "BIOSEQ_BENCH_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_BENCH_DATA_DIR
#error "BIOSEQ_BENCH_DATA_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_BENCH_CONFIG_PATH
#error "BIOSEQ_BENCH_CONFIG_PATH must be defined by the build system"
#endif

HWY_BEFORE_NAMESPACE();

namespace {

using bioseq::bench_framework::ParamMap;
using bioseq::bench_framework::RunConstraints;
using bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
using bioseq::simd::HWY_NAMESPACE::Minimap2Params;
using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof;
using bioseq::simd::HWY_NAMESPACE::TwoStackExecutionProof;
using bioseq::simd::HWY_NAMESPACE::PackedPaperNthashExecutionProof;
using bioseq::simd::HWY_NAMESPACE::SimdChainingExecutionProof;
using bioseq::simd::HWY_NAMESPACE::BlockAlignerExecutionProof;
namespace gen = bioseq::sequences::generation;
using bench_generator_t = gen::alphabet_sequence_generator<seq::dna4, std::uint32_t>;

// Comparable mappings may differ from the reference coordinate, but the
// contract is strict: a delta of 15 bp is already a rejection.
constexpr std::int32_t kPositionTolerance = 15;

struct AlignerInput {
    /// Generated pairs are read-only after `on_input` returns, but
    /// `tool_minimap2` / `minimap_port` take non-const `compact_sequence_range&`
    /// refs even though they don't mutate. `mutable` lets concurrent
    /// `on_run` calls access them through their declared signatures
    /// without forcing a `const_cast` at every call site.
    mutable std::vector<typename bench_generator_t::query_target_pair> pairs;
    /// Pre-decoded ACGT strings in lock-step with `pairs`. Populated
    /// once in `build_input` so the per-iteration timing window of
    /// `run_aligner` does not include the O(bp_size) compact-to-ACGT
    /// conversion that `tool_minimap2` used to do inline. See issue
    /// 1b in main/DOCS/REPORTS/STRANGE_TIMING_COMPREHESIVE_REPORT__DOC0215.md.
    std::vector<std::string> queries_acgt;
    std::vector<std::string> targets_acgt;
    /// Optional kmer-size override pulled from the test case `data`
    /// blob. 0 means "use the algorithm default (15)". Set on the
    /// REPR_STORAGE_* test cases to force a specific MinimizerRepr
    /// storage instantiation (K=7 → 32-bit, K=27 → 128-bit).
    std::int32_t kmer_size_override = 0;
};

struct AlignerOutput {
    std::size_t total_results          = 0;
    std::size_t correct_pairs          = 0;
    std::size_t total_pairs            = 0;
    double      sum_abs_position_offset = 0.0;
    bool        vulkan_hardware_unavailable = false;
    /// The three Vulkan-labelled rows in this historical report_76 matrix are
    /// deliberately mixed pipelines. None is evidence for the user's
    /// all-on-GPU acceptance condition.
    bool        historical_partial_gpu_probe = false;
    bool        all_gpu_acceptance_eligible = false;
    bool        phase_c_gpu_index_ownership_proof_accepted = false;
    PartialVulkanExecutionProof partial_vulkan_execution_proof{};
    TwoStackExecutionProof exact_simd_query_execution_proof{};
    TwoStackExecutionProof exact_simd_target_execution_proof{};
    PackedPaperNthashExecutionProof paper_nthash_query_execution_proof{};
    PackedPaperNthashExecutionProof paper_nthash_target_execution_proof{};
    SimdChainingExecutionProof simd_chaining_execution_proof{};
    BlockAlignerExecutionProof block_aligner_execution_proof{};
    bool exact_simd_query_execution_proof_present = false;
    bool exact_simd_target_execution_proof_present = false;
    bool paper_nthash_query_execution_proof_present = false;
    bool paper_nthash_target_execution_proof_present = false;
    bool simd_chaining_execution_proof_present = false;
    bool block_aligner_execution_proof_present = false;
    /// Set only after `require_selected_route_execution` accepts the
    /// operation-originated proof for every processed pair. The strict XXL
    /// raw companion persists this beside each duration.
    bool        selected_route_execution_proof_accepted = false;
};

/// Pull a numeric field out of a simdjson element with a fallback.
[[nodiscard]] double read_double_field(simdjson::dom::element data,
                                        std::string_view name,
                                        double fallback) noexcept {
    double value = fallback;
    auto element_or_err = data[name];
    if (element_or_err.error() == simdjson::SUCCESS) {
        const auto& element = element_or_err.value_unsafe();
        if (element.is<double>()) {
            value = element.get_double().value_unsafe();
        } else if (element.is<std::int64_t>()) {
            value = static_cast<double>(element.get_int64().value_unsafe());
        }
    }
    return value;
}

[[nodiscard]] std::int64_t read_int_field(simdjson::dom::element data,
                                            std::string_view name,
                                            std::int64_t fallback) noexcept {
    std::int64_t value = fallback;
    std::int64_t parsed = 0;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

// --------------------------------------------------------------------------
// Grouped32 local-position validation experiment
// --------------------------------------------------------------------------
//
// This is deliberately benchmark-only. It models the contiguous, non-scatter
// portion of PreparedGrouped32QueryReader::finish_load without changing the
// reader or exposing a benchmark selector to production. In particular, none
// of these variants performs a gather or constructs global u64 positions.

enum class Grouped32ValidationError : std::uint8_t {
    none = 0,
    byte_count,
    position_range,
    arithmetic_overflow,
};

enum class Grouped32ValidationRoute : std::uint8_t {
    scalar_exact = 1,
    scalar_optimized,
    scalar_unrolled,
    vector_stream_1,
    vector_stream_2,
    vector_stream_4,
};

struct Grouped32ValidationSpan {
    std::size_t byte_offset = 0;
    std::size_t position_count = 0;
    std::uint64_t segment_base_bp = 0;
    std::uint64_t segment_end_bp = 0;
};

struct Grouped32ValidationResult {
    Grouped32ValidationError error = Grouped32ValidationError::none;
    std::uint64_t first_error_word =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t words_examined = 0;
    std::uint32_t terminal_word_witness = 0;
    Grouped32ValidationRoute selected_route =
        Grouped32ValidationRoute::scalar_exact;
    std::uint8_t selected_vector_stream_width = 0;
    std::uint64_t vectorized_words = 0;
    std::uint64_t scalar_boundary_words = 0;
    std::uint64_t scalar_tail_words = 0;
    std::uint64_t failure_replay_count = 0;

    [[nodiscard]] bool valid() const noexcept {
        return error == Grouped32ValidationError::none;
    }
};

struct Grouped32ValidationInput {
    std::vector<std::byte> encoded_bytes;
    std::vector<Grouped32ValidationSpan> spans;
    std::uint64_t total_words = 0;
    Grouped32ValidationResult exact_reference{};
};

struct Grouped32FastValidationResult {
    bool valid = true;
    std::uint64_t words_examined = 0;
    std::uint32_t terminal_word_witness = 0;
    Grouped32ValidationRoute selected_route =
        Grouped32ValidationRoute::scalar_optimized;
    std::uint8_t selected_vector_stream_width = 0;
    std::uint64_t vectorized_words = 0;
    std::uint64_t scalar_boundary_words = 0;
    std::uint64_t scalar_tail_words = 0;
};

inline constexpr std::uint64_t kGrouped32MaximumLocalBp = 0x7fffffffu;
inline constexpr std::size_t kGrouped32EncodedWordBytes = 4u;

/// Compare only the biological/validation semantics. Route proof is expected
/// to differ across implementations and is checked independently.
[[nodiscard]] constexpr bool same_grouped32_validation_semantics(
    const Grouped32ValidationResult& lhs,
    const Grouped32ValidationResult& rhs) noexcept
{
    return lhs.error == rhs.error
        && lhs.first_error_word == rhs.first_error_word
        && lhs.words_examined == rhs.words_examined
        && lhs.terminal_word_witness == rhs.terminal_word_witness;
}

template <std::size_t StreamWidth>
[[nodiscard]] consteval Grouped32ValidationRoute
grouped32_vector_route() noexcept {
    static_assert(StreamWidth == 1u
        || StreamWidth == 2u
        || StreamWidth == 4u);
    if constexpr (StreamWidth == 1u) {
        return Grouped32ValidationRoute::vector_stream_1;
    } else if constexpr (StreamWidth == 2u) {
        return Grouped32ValidationRoute::vector_stream_2;
    } else {
        return Grouped32ValidationRoute::vector_stream_4;
    }
}

/// Decode one persisted little-endian u32 without alignment or host-endian
/// assumptions. This is the exact scalar reference used for failure replay.
[[nodiscard, gnu::always_inline]] inline std::uint32_t
read_grouped32_word_le(
    std::span<const std::byte> bytes,
    std::size_t byte_offset) noexcept
{
    return std::to_integer<std::uint32_t>(bytes[byte_offset])
        | (std::to_integer<std::uint32_t>(bytes[byte_offset + 1u]) << 8u)
        | (std::to_integer<std::uint32_t>(bytes[byte_offset + 2u]) << 16u)
        | (std::to_integer<std::uint32_t>(bytes[byte_offset + 3u]) << 24u);
}

/// Fast scalar load after a caller has admitted the complete byte range.
/// memcpy is both alias-safe and optimized to one native load on little-endian
/// NEON/x86 hosts.
[[nodiscard, gnu::always_inline]] inline std::uint32_t
read_grouped32_word_native(
    std::span<const std::byte> bytes,
    std::size_t byte_offset) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + byte_offset, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        return std::byteswap(value);
    } else if constexpr (std::endian::native == std::endian::little) {
        return value;
    } else {
        return read_grouped32_word_le(bytes, byte_offset);
    }
}

/// Exact current-reader semantics: byte bounds precede encoded ordering,
/// which precedes segment globalization. Optimized variants replay this whole
/// function after any fast-path failure so the first error remains exact.
[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_scalar_exact_bench(
    const Grouped32ValidationInput& input) noexcept
{
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32ValidationResult result;
    std::uint64_t logical_word = 0;

    for (const Grouped32ValidationSpan& span : input.spans) {
        bool has_previous = false;
        std::uint32_t previous = 0;
        for (std::size_t position_index = 0;
             position_index < span.position_count;
             ++position_index) {
            if (position_index
                > ((std::numeric_limits<std::size_t>::max)()
                    - span.byte_offset) / kGrouped32EncodedWordBytes) {
                result.error = Grouped32ValidationError::byte_count;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            const std::size_t byte_offset = span.byte_offset
                + position_index * kGrouped32EncodedWordBytes;
            if (byte_offset > bytes.size()
                || kGrouped32EncodedWordBytes > bytes.size() - byte_offset) {
                result.error = Grouped32ValidationError::byte_count;
                result.first_error_word = logical_word + position_index;
                return result;
            }

            const std::uint32_t encoded =
                read_grouped32_word_le(bytes, byte_offset);
            result.words_examined = logical_word + position_index + 1u;
            result.terminal_word_witness = encoded;
            if (has_previous && encoded <= previous) {
                result.error = Grouped32ValidationError::position_range;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            previous = encoded;
            has_previous = true;

            if (span.segment_base_bp >= span.segment_end_bp
                || span.segment_end_bp - span.segment_base_bp
                    > kGrouped32MaximumLocalBp + 1u) {
                result.error = Grouped32ValidationError::position_range;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            const std::uint64_t local_bp = encoded >> 1u;
            if (local_bp
                > (std::numeric_limits<std::uint64_t>::max)()
                    - span.segment_base_bp) {
                result.error = Grouped32ValidationError::arithmetic_overflow;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            if (span.segment_base_bp + local_bp >= span.segment_end_bp) {
                result.error = Grouped32ValidationError::position_range;
                result.first_error_word = logical_word + position_index;
                return result;
            }
        }
        if (span.position_count != 0u) {
            ++result.scalar_boundary_words;
            result.scalar_tail_words += span.position_count - 1u;
        }
        logical_word += span.position_count;
    }
    result.words_examined = logical_word;
    return result;
}

/// Admit the conditions that make the valid fast path equivalent to the exact
/// per-word globalization checks. A false result always triggers exact replay.
[[nodiscard, gnu::always_inline]] inline bool
grouped32_span_fast_path_admitted(
    std::span<const std::byte> bytes,
    const Grouped32ValidationSpan& span) noexcept
{
    if (span.segment_base_bp >= span.segment_end_bp
        || span.segment_end_bp - span.segment_base_bp
            > kGrouped32MaximumLocalBp + 1u
        || span.byte_offset > bytes.size()) {
        return false;
    }
    return span.position_count
        <= (bytes.size() - span.byte_offset) / kGrouped32EncodedWordBytes;
}

/// Hoisted scalar candidate. It is intentionally left auto-vectorizable: the
/// explicit Highway candidates must beat the compiler's best ordinary loop,
/// not a deliberately weakened scalar baseline.
[[nodiscard, gnu::always_inline]] inline Grouped32FastValidationResult
validate_grouped32_local32_scalar_optimized_fast(
    const Grouped32ValidationInput& input) noexcept
{
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32FastValidationResult result;
    result.selected_route = Grouped32ValidationRoute::scalar_optimized;
    for (const Grouped32ValidationSpan& span : input.spans) {
        if (!grouped32_span_fast_path_admitted(bytes, span)) {
            result.valid = false;
            return result;
        }
        const auto segment_width = static_cast<std::uint32_t>(
            span.segment_end_bp - span.segment_base_bp);
        if (span.position_count == 0u) {
            continue;
        }
        std::uint32_t previous = read_grouped32_word_native(
            bytes, span.byte_offset);
        ++result.scalar_boundary_words;
        if ((previous >> 1u) >= segment_width) {
            result.valid = false;
            return result;
        }
        result.terminal_word_witness = previous;
        for (std::size_t position_index = 1;
             position_index < span.position_count;
             ++position_index) {
            const std::uint32_t encoded = read_grouped32_word_native(
                bytes,
                span.byte_offset
                    + position_index * kGrouped32EncodedWordBytes);
            if (encoded <= previous
                || (encoded >> 1u) >= segment_width) {
                result.scalar_tail_words += position_index;
                result.valid = false;
                return result;
            }
            previous = encoded;
            result.terminal_word_witness = encoded;
        }
        result.scalar_tail_words += span.position_count - 1u;
        result.words_examined += span.position_count;
    }
    return result;
}

[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_scalar_optimized_bench(
    const Grouped32ValidationInput& input) noexcept
{
    const Grouped32FastValidationResult fast =
        validate_grouped32_local32_scalar_optimized_fast(input);
    if (!fast.valid) {
        Grouped32ValidationResult replay =
            validate_grouped32_local32_scalar_exact_bench(input);
        replay.selected_route = fast.selected_route;
        replay.selected_vector_stream_width =
            fast.selected_vector_stream_width;
        replay.vectorized_words = fast.vectorized_words;
        replay.scalar_boundary_words = fast.scalar_boundary_words;
        replay.scalar_tail_words = fast.scalar_tail_words;
        replay.failure_replay_count = 1u;
        return replay;
    }
    return Grouped32ValidationResult{
        .words_examined = fast.words_examined,
        .terminal_word_witness = fast.terminal_word_witness,
        .selected_route = fast.selected_route,
        .selected_vector_stream_width =
            fast.selected_vector_stream_width,
        .vectorized_words = fast.vectorized_words,
        .scalar_boundary_words = fast.scalar_boundary_words,
        .scalar_tail_words = fast.scalar_tail_words,
    };
}

/// Four-word scalar candidate. Cross-group and intra-group predecessor checks
/// are explicit, while native loads remain alias-safe.
[[nodiscard, gnu::always_inline]] inline Grouped32FastValidationResult
validate_grouped32_local32_scalar_unrolled_fast(
    const Grouped32ValidationInput& input) noexcept
{
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32FastValidationResult result;
    result.selected_route = Grouped32ValidationRoute::scalar_unrolled;
    for (const Grouped32ValidationSpan& span : input.spans) {
        if (!grouped32_span_fast_path_admitted(bytes, span)) {
            result.valid = false;
            return result;
        }
        if (span.position_count == 0u) {
            continue;
        }
        const auto segment_width = static_cast<std::uint32_t>(
            span.segment_end_bp - span.segment_base_bp);
        std::size_t position_index = 0;
        std::uint32_t previous = read_grouped32_word_native(
            bytes, span.byte_offset);
        ++result.scalar_boundary_words;
        if ((previous >> 1u) >= segment_width) {
            result.valid = false;
            return result;
        }
        ++position_index;
        result.terminal_word_witness = previous;

        while (span.position_count - position_index >= 4u) {
            const std::size_t byte_offset = span.byte_offset
                + position_index * kGrouped32EncodedWordBytes;
            const std::array<std::uint32_t, 4> values{
                read_grouped32_word_native(bytes, byte_offset),
                read_grouped32_word_native(bytes, byte_offset + 4u),
                read_grouped32_word_native(bytes, byte_offset + 8u),
                read_grouped32_word_native(bytes, byte_offset + 12u),
            };
            if (values[0] <= previous
                || values[1] <= values[0]
                || values[2] <= values[1]
                || values[3] <= values[2]
                || (values[0] >> 1u) >= segment_width
                || (values[1] >> 1u) >= segment_width
                || (values[2] >> 1u) >= segment_width
                || (values[3] >> 1u) >= segment_width) {
                result.scalar_tail_words += position_index + 3u;
                result.valid = false;
                return result;
            }
            previous = values[3];
            result.terminal_word_witness = previous;
            position_index += values.size();
        }
        while (position_index < span.position_count) {
            const std::uint32_t encoded = read_grouped32_word_native(
                bytes,
                span.byte_offset
                    + position_index * kGrouped32EncodedWordBytes);
            if (encoded <= previous
                || (encoded >> 1u) >= segment_width) {
                result.scalar_tail_words += position_index;
                result.valid = false;
                return result;
            }
            previous = encoded;
            result.terminal_word_witness = encoded;
            ++position_index;
        }
        result.scalar_tail_words += span.position_count - 1u;
        result.words_examined += span.position_count;
    }
    return result;
}

[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_scalar_unrolled_bench(
    const Grouped32ValidationInput& input) noexcept
{
    const Grouped32FastValidationResult fast =
        validate_grouped32_local32_scalar_unrolled_fast(input);
    if (!fast.valid) {
        Grouped32ValidationResult replay =
            validate_grouped32_local32_scalar_exact_bench(input);
        replay.selected_route = fast.selected_route;
        replay.selected_vector_stream_width =
            fast.selected_vector_stream_width;
        replay.vectorized_words = fast.vectorized_words;
        replay.scalar_boundary_words = fast.scalar_boundary_words;
        replay.scalar_tail_words = fast.scalar_tail_words;
        replay.failure_replay_count = 1u;
        return replay;
    }
    return Grouped32ValidationResult{
        .words_examined = fast.words_examined,
        .terminal_word_witness = fast.terminal_word_witness,
        .selected_route = fast.selected_route,
        .selected_vector_stream_width =
            fast.selected_vector_stream_width,
        .vectorized_words = fast.vectorized_words,
        .scalar_boundary_words = fast.scalar_boundary_words,
        .scalar_tail_words = fast.scalar_tail_words,
    };
}

#if HWY_TARGET != HWY_SCALAR

namespace grouped32_hn = hwy::HWY_NAMESPACE;
namespace grouped32_vs = bioseq::simd::HWY_NAMESPACE;

/// Validate W adjacent vector blocks from one contiguous span per iteration.
/// W=2 is the expected NEON issue-width candidate; W=1 and W=4 diagnose
/// wrapper overhead and register pressure respectively.
template <std::size_t StreamWidth>
[[nodiscard, gnu::always_inline]] inline Grouped32FastValidationResult
validate_grouped32_local32_vec_stream_fast(
    const Grouped32ValidationInput& input) noexcept
{
    static_assert(StreamWidth == 1u
        || StreamWidth == 2u
        || StreamWidth == 4u);
    using D8 = grouped32_hn::ScalableTag<std::uint8_t>;
    using D32 = grouped32_hn::Repartition<std::uint32_t, D8>;
    D8 d8;
    D32 d32;
    const std::size_t lanes = grouped32_hn::Lanes(d32);
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32FastValidationResult result;
    result.selected_route = grouped32_vector_route<StreamWidth>();
    result.selected_vector_stream_width =
        static_cast<std::uint8_t>(StreamWidth);

    for (const Grouped32ValidationSpan& span : input.spans) {
        if (!grouped32_span_fast_path_admitted(bytes, span)) {
            result.valid = false;
            return result;
        }
        if (span.position_count == 0u) {
            continue;
        }

        const auto segment_width = static_cast<std::uint32_t>(
            span.segment_end_bp - span.segment_base_bp);
        const auto* span_bytes = reinterpret_cast<const std::uint8_t*>(
            bytes.data() + span.byte_offset);
        std::size_t position_index = 0;
        const std::uint32_t first = read_grouped32_word_native(
            bytes, span.byte_offset);
        ++result.scalar_boundary_words;
        if ((first >> 1u) >= segment_width) {
            result.valid = false;
            return result;
        }
        ++position_index;
        std::uint32_t previous_scalar = first;
        result.terminal_word_witness = first;

        const std::size_t words_per_stream_iteration = StreamWidth * lanes;
        if (span.position_count - position_index
                >= words_per_stream_iteration) {
            auto previous_vector = grouped32_hn::Set(d32, first);
            auto invalid_masks = grouped32_vs::mask_stream<
                D32, StreamWidth>::splat_false(d32);
            const auto widths = grouped32_vs::vec_stream<
                D32, std::uint32_t, StreamWidth>::splat(
                    d32, segment_width);
            while (span.position_count - position_index
                    >= words_per_stream_iteration) {
                const auto loaded_bytes = grouped32_vs::make_vec_stream<
                    D8, std::uint8_t, StreamWidth>(
                    [d8, span_bytes, position_index, lanes](auto slot_constant)
                        [[gnu::always_inline]] {
                        constexpr std::size_t slot =
                            decltype(slot_constant)::value;
                        const std::size_t word_offset = position_index
                            + slot * lanes;
                        return grouped32_hn::LoadU(
                            d8,
                            span_bytes
                                + word_offset * kGrouped32EncodedWordBytes);
                });
                const auto current = grouped32_vs::BitCast(
                    d32, loaded_bytes);
                const auto predecessors = grouped32_vs::make_vec_stream<
                    D32, std::uint32_t, StreamWidth>(
                    [d32, &current, previous_vector](auto slot_constant)
                        [[gnu::always_inline]] {
                        constexpr std::size_t slot =
                            decltype(slot_constant)::value;
                        if constexpr (slot == 0u) {
                            return grouped32_vs::lanes_shift_left_1_borrow(
                                d32, current[slot], previous_vector);
                        } else {
                            return grouped32_vs::lanes_shift_left_1_borrow(
                                d32, current[slot], current[slot - 1u]);
                        }
                    });
                const auto invalid_order = grouped32_vs::Le(
                    current, predecessors);
                const auto invalid_range = grouped32_vs::Ge(
                    grouped32_vs::ShiftRight<1>(current), widths);
                invalid_masks = grouped32_vs::Or(
                    invalid_masks,
                    grouped32_vs::Or(invalid_order, invalid_range));
                previous_vector = current[StreamWidth - 1u];
                position_index += words_per_stream_iteration;
            }
            const std::uint64_t vectorized_words_this_span =
                position_index - 1u;
            const auto all_clear = grouped32_vs::AllFalsePerStream(
                d32, invalid_masks);
            for (std::size_t slot = 0; slot < StreamWidth; ++slot) {
                if (!all_clear[slot]) {
                    result.vectorized_words += vectorized_words_this_span;
                    result.valid = false;
                    return result;
                }
            }
            result.vectorized_words += vectorized_words_this_span;
            previous_scalar = grouped32_vs::extract_last(
                d32, previous_vector);
            result.terminal_word_witness = previous_scalar;
        }

        const std::size_t scalar_tail_begin = position_index;
        while (position_index < span.position_count) {
            const std::uint32_t encoded = read_grouped32_word_native(
                bytes,
                span.byte_offset
                    + position_index * kGrouped32EncodedWordBytes);
            if (encoded <= previous_scalar
                || (encoded >> 1u) >= segment_width) {
                result.scalar_tail_words +=
                    position_index - scalar_tail_begin + 1u;
                result.valid = false;
                return result;
            }
            previous_scalar = encoded;
            result.terminal_word_witness = encoded;
            ++position_index;
        }
        result.scalar_tail_words += position_index - scalar_tail_begin;
        result.words_examined += span.position_count;
    }
    return result;
}

#endif

template <std::size_t StreamWidth>
[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_vec_stream_bench(
    const Grouped32ValidationInput& input)
{
#if HWY_TARGET == HWY_SCALAR
    throw std::runtime_error(fmt::format(
        "grouped32 vector_stream_{} was selected on the Highway scalar target",
        StreamWidth));
#else
    if constexpr (std::endian::native != std::endian::little) {
        throw std::runtime_error(fmt::format(
            "grouped32 vector_stream_{} requires a little-endian host; "
            "silent scalar fallback is forbidden",
            StreamWidth));
    } else {
        const Grouped32FastValidationResult fast =
            validate_grouped32_local32_vec_stream_fast<StreamWidth>(input);
        if (!fast.valid) {
            Grouped32ValidationResult replay =
                validate_grouped32_local32_scalar_exact_bench(input);
            replay.selected_route = fast.selected_route;
            replay.selected_vector_stream_width =
                fast.selected_vector_stream_width;
            replay.vectorized_words = fast.vectorized_words;
            replay.scalar_boundary_words = fast.scalar_boundary_words;
            replay.scalar_tail_words = fast.scalar_tail_words;
            replay.failure_replay_count = 1u;
            return replay;
        }
        return Grouped32ValidationResult{
            .words_examined = fast.words_examined,
            .terminal_word_witness = fast.terminal_word_witness,
            .selected_route = fast.selected_route,
            .selected_vector_stream_width =
                fast.selected_vector_stream_width,
            .vectorized_words = fast.vectorized_words,
            .scalar_boundary_words = fast.scalar_boundary_words,
            .scalar_tail_words = fast.scalar_tail_words,
        };
    }
#endif
}

/// Return one exact four-byte little-endian benchmark word.
[[nodiscard]] constexpr std::array<std::byte, 4>
encode_grouped32_benchmark_word(std::uint32_t value) noexcept {
    return {
        static_cast<std::byte>(value & 0xffu),
        static_cast<std::byte>((value >> 8u) & 0xffu),
        static_cast<std::byte>((value >> 16u) & 0xffu),
        static_cast<std::byte>((value >> 24u) & 0xffu),
    };
}

struct Grouped32AdversarialProbe {
    std::string_view name;
    Grouped32ValidationInput input;
    Grouped32ValidationError expected_error =
        Grouped32ValidationError::none;
    std::uint64_t required_vectorized_words = 0;
};

/// Small owned input used only by the untimed evaluator probes.
[[nodiscard]] Grouped32ValidationInput make_grouped32_adversarial_input(
    std::size_t position_count,
    std::uint64_t segment_base_bp,
    std::uint64_t segment_end_bp,
    std::uint32_t encoded_bias = 0u)
{
    Grouped32ValidationInput input;
    input.encoded_bytes.resize(4u, std::byte{0});
    input.encoded_bytes.reserve(
        4u + position_count * kGrouped32EncodedWordBytes);
    input.spans.push_back(Grouped32ValidationSpan{
        .byte_offset = input.encoded_bytes.size(),
        .position_count = position_count,
        .segment_base_bp = segment_base_bp,
        .segment_end_bp = segment_end_bp,
    });
    input.total_words = position_count;
    for (std::size_t position_index = 0;
         position_index < position_count;
         ++position_index) {
        const auto encoded = encode_grouped32_benchmark_word(
            static_cast<std::uint32_t>(position_index) + encoded_bias);
        input.encoded_bytes.insert(
            input.encoded_bytes.end(), encoded.begin(), encoded.end());
    }
    return input;
}

[[nodiscard]] Grouped32ValidationInput with_grouped32_probe_word(
    Grouped32ValidationInput input,
    std::size_t position_index,
    std::uint32_t encoded_word)
{
    const std::size_t byte_offset = input.spans.front().byte_offset
        + position_index * kGrouped32EncodedWordBytes;
    const auto encoded = encode_grouped32_benchmark_word(encoded_word);
    std::copy(
        encoded.begin(), encoded.end(),
        input.encoded_bytes.begin()
            + static_cast<std::ptrdiff_t>(byte_offset));
    return input;
}

[[nodiscard]] Grouped32AdversarialProbe finalize_grouped32_probe(
    std::string_view name,
    Grouped32ValidationInput input,
    Grouped32ValidationError expected_error,
    std::uint64_t required_vectorized_words = 0)
{
    input.exact_reference =
        validate_grouped32_local32_scalar_exact_bench(input);
    return Grouped32AdversarialProbe{
        .name = name,
        .input = std::move(input),
        .expected_error = expected_error,
        .required_vectorized_words = required_vectorized_words,
    };
}

[[nodiscard]] std::size_t grouped32_probe_u32_lanes() noexcept {
#if HWY_TARGET == HWY_SCALAR
    return 4u;
#else
    using D8 = hwy::HWY_NAMESPACE::ScalableTag<std::uint8_t>;
    using D32 = hwy::HWY_NAMESPACE::Repartition<std::uint32_t, D8>;
    D32 d32;
    return hwy::HWY_NAMESPACE::Lanes(d32);
#endif
}

/// Build corruption at one strict-order boundary. Equal and decreasing probes
/// are kept separate so both comparisons are exercised by every route.
[[nodiscard]] Grouped32AdversarialProbe make_grouped32_order_probe(
    std::string_view name,
    std::size_t corrupt_index,
    std::size_t minimum_count,
    std::uint64_t required_vectorized_words,
    bool decreasing)
{
    const std::size_t count = std::max(
        corrupt_index + 2u, minimum_count);
    Grouped32ValidationInput input = make_grouped32_adversarial_input(
        count,
        0u,
        static_cast<std::uint64_t>(count + 32u),
        4u);
    const auto previous = static_cast<std::uint32_t>(corrupt_index) + 3u;
    input = with_grouped32_probe_word(
        std::move(input),
        corrupt_index,
        decreasing ? previous - 1u : previous);
    return finalize_grouped32_probe(
        name,
        std::move(input),
        Grouped32ValidationError::position_range,
        required_vectorized_words);
}

[[nodiscard]] std::vector<Grouped32AdversarialProbe>
build_grouped32_adversarial_probes(std::size_t stream_width)
{
    const std::size_t lanes = grouped32_probe_u32_lanes();
    const std::size_t admitted_stream_width =
        stream_width == 1u || stream_width == 2u || stream_width == 4u
        ? stream_width : 2u;
    const std::size_t vector_words = admitted_stream_width * lanes;
    const std::size_t one_iteration_count = 1u + vector_words;
    const std::size_t scalar_to_vector_boundary = 1u;
    const std::size_t lane_boundary = lanes;
    const std::size_t stream_boundary = 1u + lanes;
    const std::size_t cross_iteration_boundary = 1u + vector_words;
    const std::size_t two_iteration_count = 1u + 2u * vector_words;
    const std::size_t scalar_tail_boundary = two_iteration_count;
    const std::size_t scalar_tail_probe_count = scalar_tail_boundary + 2u;

    std::vector<Grouped32AdversarialProbe> probes;
    probes.reserve(14u);

    Grouped32ValidationInput truncated = make_grouped32_adversarial_input(
        two_iteration_count + 1u, 0u,
        static_cast<std::uint64_t>(two_iteration_count + 64u));
    truncated.encoded_bytes.pop_back();
    probes.push_back(finalize_grouped32_probe(
        "byte_truncation",
        std::move(truncated),
        Grouped32ValidationError::byte_count));

    probes.push_back(finalize_grouped32_probe(
        "invalid_segment_geometry",
        make_grouped32_adversarial_input(2u, 5u, 5u),
        Grouped32ValidationError::position_range));

    probes.push_back(finalize_grouped32_probe(
        "order_valid_local_range_inside_vector_block",
        make_grouped32_adversarial_input(
            one_iteration_count, 0u, 1u),
        Grouped32ValidationError::position_range,
        vector_words));

    probes.push_back(finalize_grouped32_probe(
        "global_addition_overflow",
        with_grouped32_probe_word(
            make_grouped32_adversarial_input(
                1u,
                (std::numeric_limits<std::uint64_t>::max)() - 2u,
                (std::numeric_limits<std::uint64_t>::max)()),
            0u,
            6u),
        Grouped32ValidationError::arithmetic_overflow));

    probes.push_back(make_grouped32_order_probe(
        "equal_at_scalar_to_vector_borrow",
        scalar_to_vector_boundary,
        one_iteration_count,
        vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_scalar_to_vector_borrow",
        scalar_to_vector_boundary,
        one_iteration_count,
        vector_words,
        true));
    probes.push_back(make_grouped32_order_probe(
        "equal_at_vector_lane_boundary",
        lane_boundary,
        one_iteration_count,
        vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_vector_lane_boundary",
        lane_boundary,
        one_iteration_count,
        vector_words,
        true));
    if (admitted_stream_width > 1u) {
        probes.push_back(make_grouped32_order_probe(
            "equal_at_intra_iteration_stream_boundary",
            stream_boundary,
            one_iteration_count,
            vector_words,
            false));
        probes.push_back(make_grouped32_order_probe(
            "decreasing_at_intra_iteration_stream_boundary",
            stream_boundary,
            one_iteration_count,
            vector_words,
            true));
    }
    probes.push_back(make_grouped32_order_probe(
        "equal_at_cross_iteration_slot_zero_borrow",
        cross_iteration_boundary,
        two_iteration_count,
        2u * vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_cross_iteration_slot_zero_borrow",
        cross_iteration_boundary,
        two_iteration_count,
        2u * vector_words,
        true));
    probes.push_back(make_grouped32_order_probe(
        "equal_at_scalar_tail_after_two_vector_iterations",
        scalar_tail_boundary,
        scalar_tail_probe_count,
        2u * vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_scalar_tail_after_two_vector_iterations",
        scalar_tail_boundary,
        scalar_tail_probe_count,
        2u * vector_words,
        true));
    return probes;
}

[[nodiscard]] const std::vector<Grouped32AdversarialProbe>&
grouped32_adversarial_probes(std::size_t stream_width)
{
    static const std::vector<Grouped32AdversarialProbe> probes_w1 =
        build_grouped32_adversarial_probes(1u);
    static const std::vector<Grouped32AdversarialProbe> probes_w2 =
        build_grouped32_adversarial_probes(2u);
    static const std::vector<Grouped32AdversarialProbe> probes_w4 =
        build_grouped32_adversarial_probes(4u);
    if (stream_width == 1u) {
        return probes_w1;
    }
    if (stream_width == 4u) {
        return probes_w4;
    }
    return probes_w2;
}

[[nodiscard]] RunConstraints estimate_grouped32_validation_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data) noexcept
{
    const std::uint64_t total_words = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0,
            read_int_field(data, "total_words", 65'536)));
    const std::uint64_t span_count = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0,
            read_int_field(data, "span_count", 1)));
    const std::uint64_t encoded_bytes = total_words
        > (std::numeric_limits<std::uint64_t>::max)()
            / kGrouped32EncodedWordBytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : total_words * kGrouped32EncodedWordBytes;
    const std::uint64_t span_bytes = span_count
        > (std::numeric_limits<std::uint64_t>::max)()
            / (sizeof(Grouped32ValidationSpan) + 15u)
        ? (std::numeric_limits<std::uint64_t>::max)()
        : span_count * (sizeof(Grouped32ValidationSpan) + 15u);
    const std::uint64_t total_bytes = encoded_bytes
        > (std::numeric_limits<std::uint64_t>::max)() - span_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : encoded_bytes + span_bytes;
    return {
        .base_memory = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                total_bytes,
                (std::numeric_limits<std::size_t>::max)())),
        .extra_memory = 0u,
    };
}

/// Build deterministic valid spans. Every span starts on a production-real
/// four-byte boundary while rotating through offsets 0/4/8/12 modulo 16.
/// Consecutive encoded values exercise same-bp forward/reverse ordering.
[[nodiscard]] Grouped32ValidationInput build_grouped32_validation_input(
    std::string_view /*data_dir*/,
    std::string_view test_case_name,
    simdjson::dom::element data,
    const RunConstraints& /*constraints*/)
{
    const auto configured_total_words = static_cast<std::size_t>(
        std::max<std::int64_t>(0,
            read_int_field(data, "total_words", 65'536)));
    const auto configured_span_count = static_cast<std::size_t>(
        std::max<std::int64_t>(1,
            read_int_field(data, "span_count", 1)));
    const auto configured_words_per_span = static_cast<std::size_t>(
        std::max<std::int64_t>(1,
            read_int_field(data, "words_per_span", 1)));

    std::vector<std::size_t> counts;
    counts.reserve(configured_span_count);
    if (test_case_name == "GROUPED32_LOCAL32_ZIPF_64K") {
        std::size_t assigned = 0;
        for (std::size_t rank = 1; rank <= configured_span_count; ++rank) {
            const std::size_t count = std::max<std::size_t>(
                1u, 8192u / rank);
            counts.push_back(count);
            assigned += count;
        }
        if (assigned <= configured_total_words) {
            counts.front() += configured_total_words - assigned;
        } else {
            std::size_t excess = assigned - configured_total_words;
            for (std::size_t index = 0;
                 index < counts.size() && excess != 0u;
                 ++index) {
                const std::size_t removable = counts[index] - 1u;
                const std::size_t removed = std::min(removable, excess);
                counts[index] -= removed;
                excess -= removed;
            }
        }
    } else {
        counts.assign(configured_span_count, configured_words_per_span);
    }

    std::uint64_t total_words = 0;
    for (const std::size_t count : counts) {
        total_words += count;
    }

    Grouped32ValidationInput input;
    input.total_words = total_words;
    input.spans.reserve(counts.size());
    const std::uint64_t reserve_bytes = total_words
        * kGrouped32EncodedWordBytes
        + static_cast<std::uint64_t>(counts.size()) * 15u;
    input.encoded_bytes.reserve(static_cast<std::size_t>(reserve_bytes));

    for (std::size_t span_index = 0;
         span_index < counts.size();
         ++span_index) {
        const std::size_t desired_modulo = (span_index % 4u) * 4u;
        const std::size_t current_modulo = input.encoded_bytes.size() % 16u;
        const std::size_t padding =
            (desired_modulo + 16u - current_modulo) % 16u;
        input.encoded_bytes.resize(
            input.encoded_bytes.size() + padding, std::byte{0});

        const std::size_t count = counts[span_index];
        const std::uint64_t segment_base_bp =
            static_cast<std::uint64_t>(span_index) * 1'000'000u;
        const std::uint64_t segment_width =
            static_cast<std::uint64_t>((count + 1u) / 2u) + 32u;
        input.spans.push_back(Grouped32ValidationSpan{
            .byte_offset = input.encoded_bytes.size(),
            .position_count = count,
            .segment_base_bp = segment_base_bp,
            .segment_end_bp = segment_base_bp + segment_width,
        });
        for (std::size_t position_index = 0;
             position_index < count;
             ++position_index) {
            const auto encoded = encode_grouped32_benchmark_word(
                static_cast<std::uint32_t>(position_index));
            input.encoded_bytes.insert(
                input.encoded_bytes.end(), encoded.begin(), encoded.end());
        }
    }
    input.exact_reference =
        validate_grouped32_local32_scalar_exact_bench(input);
    return input;
}

struct Grouped32ExpectedRoute {
    Grouped32ValidationRoute route =
        Grouped32ValidationRoute::scalar_exact;
    std::uint8_t vector_stream_width = 0;
};

[[nodiscard]] Grouped32ExpectedRoute grouped32_expected_route(
    std::string_view algorithm)
{
    if (algorithm == "grouped32_local32_scalar_exact") {
        return {Grouped32ValidationRoute::scalar_exact, 0u};
    }
    if (algorithm == "grouped32_local32_scalar_optimized") {
        return {Grouped32ValidationRoute::scalar_optimized, 0u};
    }
    if (algorithm == "grouped32_local32_scalar_unrolled") {
        return {Grouped32ValidationRoute::scalar_unrolled, 0u};
    }
    if (algorithm == "grouped32_local32_vec_stream_1") {
        return {Grouped32ValidationRoute::vector_stream_1, 1u};
    }
    if (algorithm == "grouped32_local32_vec_stream_2") {
        return {Grouped32ValidationRoute::vector_stream_2, 2u};
    }
    if (algorithm == "grouped32_local32_vec_stream_4") {
        return {Grouped32ValidationRoute::vector_stream_4, 4u};
    }
    throw std::invalid_argument(fmt::format(
        "unknown grouped32 validation algorithm '{}'", algorithm));
}

/// Dispatch without instrumentation. The timed callback wraps exactly one
/// call; the evaluator reuses this function for untimed adversarial probes.
[[nodiscard]] Grouped32ValidationResult run_grouped32_validation_selected(
    std::string_view algorithm,
    const Grouped32ValidationInput& input)
{
    if (algorithm == "grouped32_local32_scalar_exact") {
        return validate_grouped32_local32_scalar_exact_bench(input);
    }
    if (algorithm == "grouped32_local32_scalar_optimized") {
        return validate_grouped32_local32_scalar_optimized_bench(input);
    }
    if (algorithm == "grouped32_local32_scalar_unrolled") {
        return validate_grouped32_local32_scalar_unrolled_bench(input);
    }
    if (algorithm == "grouped32_local32_vec_stream_1") {
        return validate_grouped32_local32_vec_stream_bench<1u>(input);
    }
    if (algorithm == "grouped32_local32_vec_stream_2") {
        return validate_grouped32_local32_vec_stream_bench<2u>(input);
    }
    if (algorithm == "grouped32_local32_vec_stream_4") {
        return validate_grouped32_local32_vec_stream_bench<4u>(input);
    }
    throw std::invalid_argument(fmt::format(
        "unknown grouped32 validation algorithm '{}'", algorithm));
}

[[nodiscard]] bool grouped32_route_identity_matches(
    const Grouped32ValidationResult& result,
    const Grouped32ExpectedRoute& expected) noexcept
{
    return result.selected_route == expected.route
        && result.selected_vector_stream_width
            == expected.vector_stream_width
        && (expected.vector_stream_width != 0u
            || result.vectorized_words == 0u);
}

[[nodiscard]] bool grouped32_complete_route_proof_matches(
    const Grouped32ValidationResult& result,
    const Grouped32ExpectedRoute& expected) noexcept
{
    return grouped32_route_identity_matches(result, expected)
        && result.failure_replay_count == 0u
        && result.vectorized_words
            + result.scalar_boundary_words
            + result.scalar_tail_words
            == result.words_examined;
}

[[nodiscard]] Grouped32ValidationResult run_grouped32_validation(
    std::string_view algorithm,
    const ParamMap& /*params*/,
    const Grouped32ValidationInput& input)
{
    static const auto* descriptor =
        bioseq::instr::register_region_static(
            "grouped32_local32_validation");
    auto scope = bioseq::instr::region(descriptor);
    return run_grouped32_validation_selected(algorithm, input);
}

[[nodiscard]] std::map<std::string, double>
evaluate_grouped32_validation(
    std::string_view algorithm,
    const ParamMap& /*params*/,
    const Grouped32ValidationInput& input,
    const Grouped32ValidationResult& output)
{
    const Grouped32ValidationResult reference =
        validate_grouped32_local32_scalar_exact_bench(input);
    const Grouped32ExpectedRoute expected =
        grouped32_expected_route(algorithm);
    bool input_requires_vector_words = false;
    if (expected.vector_stream_width != 0u) {
        const std::size_t vector_words =
            static_cast<std::size_t>(expected.vector_stream_width)
            * grouped32_probe_u32_lanes();
        for (const Grouped32ValidationSpan& span : input.spans) {
            if (span.position_count > vector_words) {
                input_requires_vector_words = true;
                break;
            }
        }
    }
    if (!same_grouped32_validation_semantics(output, reference)
        || !same_grouped32_validation_semantics(
            reference, input.exact_reference)
        || (reference.valid()
            && reference.words_examined != input.total_words)
        || !grouped32_complete_route_proof_matches(output, expected)
        || (input_requires_vector_words
            && output.vectorized_words == 0u)) {
        throw std::runtime_error(fmt::format(
            "grouped32 validation/parity route proof failed for '{}' "
            "(error={}, first={}, words={}, witness={}; "
            "reference error={}, first={}, words={}, witness={}; "
            "route={}, width={}, vector={}, boundary={}, tail={}, replay={})",
            algorithm,
            static_cast<unsigned>(output.error),
            output.first_error_word,
            output.words_examined,
            output.terminal_word_witness,
            static_cast<unsigned>(reference.error),
            reference.first_error_word,
            reference.words_examined,
            reference.terminal_word_witness,
            static_cast<unsigned>(output.selected_route),
            static_cast<unsigned>(output.selected_vector_stream_width),
            output.vectorized_words,
            output.scalar_boundary_words,
            output.scalar_tail_words,
            output.failure_replay_count));
    }

    const std::size_t probe_width = expected.vector_stream_width == 0u
        ? 2u : expected.vector_stream_width;
    const auto& probes = grouped32_adversarial_probes(probe_width);
    std::uint64_t adversarial_vectorized_words = 0;
    std::uint64_t adversarial_replay_count = 0;
    for (const Grouped32AdversarialProbe& probe : probes) {
        const Grouped32ValidationResult probe_reference =
            validate_grouped32_local32_scalar_exact_bench(probe.input);
        const Grouped32ValidationResult candidate =
            run_grouped32_validation_selected(algorithm, probe.input);
        const std::uint64_t expected_replay_count =
            expected.route == Grouped32ValidationRoute::scalar_exact
            ? 0u : 1u;
        if (probe_reference.error != probe.expected_error
            || !same_grouped32_validation_semantics(
                probe_reference, probe.input.exact_reference)
            || !same_grouped32_validation_semantics(
                candidate, probe_reference)
            || !grouped32_route_identity_matches(candidate, expected)
            || candidate.failure_replay_count != expected_replay_count
            || (expected.vector_stream_width != 0u
                && candidate.vectorized_words
                    < probe.required_vectorized_words)) {
            throw std::runtime_error(fmt::format(
                "grouped32 adversarial parity failed for '{}' probe '{}' "
                "(expected_error={}, reference_error={}, candidate_error={}, "
                "first={}, route={}, width={}, vector={}, required_vector={}, "
                "boundary={}, tail={}, replay={})",
                algorithm,
                probe.name,
                static_cast<unsigned>(probe.expected_error),
                static_cast<unsigned>(probe_reference.error),
                static_cast<unsigned>(candidate.error),
                candidate.first_error_word,
                static_cast<unsigned>(candidate.selected_route),
                static_cast<unsigned>(
                    candidate.selected_vector_stream_width),
                candidate.vectorized_words,
                probe.required_vectorized_words,
                candidate.scalar_boundary_words,
                candidate.scalar_tail_words,
                candidate.failure_replay_count));
        }
        adversarial_vectorized_words += candidate.vectorized_words;
        adversarial_replay_count += candidate.failure_replay_count;
    }
    if (expected.vector_stream_width != 0u
        && adversarial_vectorized_words == 0u) {
        throw std::runtime_error(fmt::format(
            "grouped32 vector_stream_{} adversarial probes executed no "
            "vector words",
            expected.vector_stream_width));
    }
    return {
        {"validation_parity", 1.0},
        {"adversarial_parity", 1.0},
        {"primary_correct_fraction", 1.0},
        {"route_proof_accepted", 1.0},
        {"selected_route",
            static_cast<double>(static_cast<unsigned>(
                output.selected_route))},
        {"selected_vector_stream_width",
            static_cast<double>(output.selected_vector_stream_width)},
        {"vectorized_words",
            static_cast<double>(output.vectorized_words)},
        {"scalar_boundary_words",
            static_cast<double>(output.scalar_boundary_words)},
        {"scalar_tail_words",
            static_cast<double>(output.scalar_tail_words)},
        {"failure_replay_count",
            static_cast<double>(output.failure_replay_count)},
        {"adversarial_probe_count",
            static_cast<double>(probes.size())},
        {"adversarial_vectorized_words",
            static_cast<double>(adversarial_vectorized_words)},
        {"adversarial_replay_count",
            static_cast<double>(adversarial_replay_count)},
        {"valid_result", output.valid() ? 1.0 : 0.0},
        {"validated_words", static_cast<double>(output.words_examined)},
        {"terminal_word_witness",
            static_cast<double>(output.terminal_word_witness)},
    };
}

/// Estimate the memory required by one `(benchmark, test_case, group)`
/// batch so the framework can throttle parallel batches against the
/// global memory budget (see `main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V11_IMPL_PLAN__DOC0224.md` §3.5 / §4 and
/// `main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V12_IMPL_PLAN__DOC0226.md` §3.4).
///
/// We size off the data already in the JSON blob, before any sequence
/// generation. The cached `AlignerInput` keeps every generated pair in compact
/// dna4/u32 storage and also keeps pre-decoded ACGT strings for the minimap2
/// reference path. DOC0335 generation now shares the compact target across
/// pairs and fills packed storage directly, so generation no longer carries a
/// per-pair full-reference copy.
[[nodiscard]] RunConstraints estimate_aligner_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data) noexcept
{
    const auto fragment_count = static_cast<std::size_t>(
        read_int_field(data, "fragment_count", 100));
    const auto fragment_length_mean = static_cast<std::size_t>(
        read_int_field(data, "fragment_length_mean", 150));
    const auto reference_length = static_cast<std::size_t>(
        read_int_field(data, "reference_length", 10000));

    using range_t = compact_sequence_range<
        sequence_custom_alphabet<seq::dna4, std::uint32_t>>;
    using traits = typename range_t::traits;
    const auto packed_bytes_for = [] (std::size_t bp) noexcept -> std::size_t {
        return ((bp + traits::letters_per_unit - 1) / traits::letters_per_unit)
             * sizeof(typename traits::storage_unit);
    };

    const std::size_t target_packed = packed_bytes_for(reference_length);
    const std::size_t query_packed  = packed_bytes_for(fragment_length_mean);
    const std::size_t acgt_bytes_per_pair = reference_length + fragment_length_mean;
    const std::size_t cached_per_pair =
        target_packed + query_packed + acgt_bytes_per_pair;
    const std::size_t cached_input_bytes = fragment_count * cached_per_pair;

    // One unpacked dna4 reference is live while the compact pairs are produced.
    // Add DP/anchor scratch plus the possible resident target-index footprint.
    // Phase-C GPU builds hold a target index resident for the duration of the
    // timed run; omitting it let XXL_HIFI co-schedule two 500 Mbp GPU rows and
    // trip the fail-closed Vulkan admission gate before dispatch.
    const std::uint64_t generation_peak_extra = reference_length;
    const std::uint64_t aligner_scratch_extra =
        fragment_count * (fragment_length_mean / 2 + 1024);
    const std::uint64_t target_index_extra =
        bioseq::parallelism::minimizer_index_peak_bytes_for_bp(reference_length);
    const std::uint64_t extra_memory =
        bioseq::parallelism::saturating_add(
            bioseq::parallelism::saturating_add(
                generation_peak_extra,
                aligner_scratch_extra),
            target_index_extra);
    return {
        .base_memory  = cached_input_bytes,
        .extra_memory = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                extra_memory,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)()))),
    };
}

/// Materialise a `(query, target)` batch from the `data` JSON blob.
/// Lives in `on_input`; runs once per `(benchmark, test_case, group)`
/// batch.  The `constraints` argument carries the same numbers
/// `estimate_aligner_resources` returned, but we don't need them — the
/// generator dimensions come straight from the JSON blob — so the
/// parameter is unused.
[[nodiscard]] AlignerInput build_input(std::string_view /*data_dir*/,
                                        std::string_view test_case_name,
                                        simdjson::dom::element data,
                                        const RunConstraints& /*constraints*/) {
    gen::generator_params params{};
    params.reference_length = static_cast<std::size_t>(
        read_int_field(data, "reference_length",
                        static_cast<std::int64_t>(params.reference_length)));
    params.fragment_count = static_cast<std::size_t>(
        read_int_field(data, "fragment_count",
                        static_cast<std::int64_t>(params.fragment_count)));
    params.fragment_length_mean = static_cast<std::size_t>(
        read_int_field(data, "fragment_length_mean",
                        static_cast<std::int64_t>(params.fragment_length_mean)));
    params.fragment_length_stddev = read_double_field(
        data, "fragment_length_stddev", params.fragment_length_stddev);
    params.substitution_rate = read_double_field(
        data, "substitution_rate", params.substitution_rate);
    params.insertion_rate = read_double_field(
        data, "insertion_rate", params.insertion_rate);
    params.deletion_rate = read_double_field(
        data, "deletion_rate", params.deletion_rate);
    params.insertion_length_mean = read_double_field(
        data, "insertion_length_mean", params.insertion_length_mean);
    params.deletion_length_mean = read_double_field(
        data, "deletion_length_mean", params.deletion_length_mean);
    params.reverse_complement_rate = read_double_field(
        data, "reverse_complement_rate", params.reverse_complement_rate);
    params.seed = static_cast<std::uint64_t>(
        read_int_field(data, "seed", static_cast<std::int64_t>(params.seed)));

    AlignerInput input;
    input.pairs = bench_generator_t(params).generate_sequences_pairs(params.fragment_count);
    input.kmer_size_override = static_cast<std::int32_t>(
        read_int_field(data, "kmer_size", 0));

    // Pre-decode each pair's compact representation into ACGT so the
    // tool_minimap2 cell does not pay the O(bp_size) decompress
    // inside its timing window. Cost lives in on_input (excluded from
    // wall_clock_ns by `std::call_once`-guarded caching). Memory cost
    // is one ASCII byte per base per pair; for XXL_HIFI's 500 Mbp
    // target that's an extra ~500 MB beyond the existing packed
    // representation. Acceptable: the cached input is held only for
    // the duration of the benchmark.
    input.queries_acgt.reserve(input.pairs.size());
    input.targets_acgt.reserve(input.pairs.size());
    for (auto& pair : input.pairs) {
        input.queries_acgt.push_back(
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(pair.query));
        input.targets_acgt.push_back(
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(*pair.target));
    }

    bioseq_log<DBG>("[bench_v10] build_input '{}' produced {} pairs (k_override={})",
             test_case_name, input.pairs.size(), input.kmer_size_override);
    return input;
}

/// Map an algorithm key from the JSON config to a fully-resolved
/// `Minimap2Params` plus a flag selecting the C reference vs the bioseq
/// port. A fixed kmer_size variant is selected for the special K=7 / K=27
/// REPR_STORAGE test cases via the test-case name passed via the input.
struct AlgorithmDispatch {
    bool is_minimap2_reference = false;
    bool explicit_vulkan = false;
    bool historical_partial_gpu_probe = false;
    Minimap2Params params{};
};

[[nodiscard]] bool is_explicit_vulkan_hardware_available(std::string_view algo) {
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    if (bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        return true;
    }

    static std::atomic_bool logged_unavailable{false};
    if (!logged_unavailable.exchange(true, std::memory_order_relaxed)) {
        if (runtime.usable()) {
            bioseq_log<WRN>(
                "[bench_v10] explicit Vulkan benchmark rows are not measured on this host "
                "(first algo='{}'): active Vulkan device '{}' is a CPU/software device "
                "(VkPhysicalDeviceType={}); hardware Vulkan is required for GPU timing/parity",
                algo,
                runtime.features().device_name.data(),
                runtime.features().device_type);
        } else {
            bioseq_log<WRN>(
                "[bench_v10] explicit Vulkan benchmark rows are not measured on this host "
                "(first algo='{}'): Vulkan runtime is unavailable: {}",
                algo,
                runtime.initialization_error());
        }
    }
    return false;
}

[[nodiscard]] AlgorithmDispatch dispatch_algorithm(std::string_view algo,
                                                    std::int32_t kmer_override) {
    AlgorithmDispatch dispatch;
    dispatch.params.kmer_size   = kmer_override > 0 ? kmer_override : 15;
    dispatch.params.window_size = 10;

    if (algo == "minimap2_dp") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SCALAR_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "minimap2_rmq") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::RMQ;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "bioseq_ksw2_scalar_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SCALAR_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "bioseq_ksw2_simd_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "bioseq_ksw2_simd_dp_two_stack_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
    } else if (algo == "bioseq_ksw2_simd_dp_paper_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
    } else if (algo == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
        // DOC0304 Phase A historical PARTIAL-GPU probe: CPU ExactSimd
        // minimizers, Vulkan score generation, CPU backtracking and CPU KSW2.
        // Explicit Vulkan selection fails loudly when no hardware device exists.
        // Identical minimizer (two-stack nthash) + base aligner (KSW2) to
        // bioseq_ksw2_simd_dp_two_stack_nthash, so the only varied dimension is
        // the chaining backend: SIMD_MM2 vs VULKAN_TILED_DP.
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::VULKAN_TILED_DP;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
        dispatch.explicit_vulkan = true;
        dispatch.historical_partial_gpu_probe = true;
    } else if (algo == "bioseq_ksw2_batched_simd_dp_two_stack_nthash") {
        // DOC0254 §11.9: KSW2_BATCHED selected via runtime params.
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2_BATCHED;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
    } else if (algo == "bioseq_ksw2_simd_dp_simd_minimap2_slow") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW;
    } else if (algo == "bioseq_block_simd_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_simd_dp_two_stack_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_simd_dp_paper_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
        // Historical PARTIAL-GPU probe: Vulkan minimap-int minimizers and seed
        // lookup, CPU target-index construction, Vulkan tiled-DP score
        // generation, CPU backtracking, and CPU Block Aligner. The minimizer
        // sketch is always the fused tiled Phase-C path
        // after DOC0351; this row keeps the older seed/index frontend settings
        // for comparison. The only varied
        // dimensions versus bioseq_block_simd_dp_two_stack_nthash (SIMD
        // minimizers + SIMD chaining + block aligner) are the seeding (GPU
        // sketch + GPU seed-hit lookup) and the chaining backend; the base
        // aligner stays block (no G3SA extension kernel in Phase B, §0/§10).
        // All GPU stages fail loudly when no hardware Vulkan device exists.
        dispatch.params.chaining_algorithm        = ChainingAlgorithmType::VULKAN_TILED_DP;
        dispatch.params.base_alignment_algorithm  = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm       = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
        dispatch.params.vulkan_enabled            = true;
        dispatch.params.vulkan_seed_hits_enabled  = true;
        dispatch.params.vulkan_index_cache_enabled = true;
        dispatch.params.do_cigar                  = true;
        // Keep the resident two-submit seed/index frontend OFF for the legacy
        // comparison row. This no longer selects a Phase-B minimizer sketch.
        dispatch.params.vulkan_fused_frontend_enabled  = false;
        dispatch.params.vulkan_gpu_index_build_enabled = false;
        dispatch.explicit_vulkan = true;
        dispatch.historical_partial_gpu_probe = true;
    } else if (algo == "bioseq_block_vulkan_phase_c_frontend") {
        // DOC0310 Phase C historical PARTIAL-GPU probe — the resident two-submit
        // Vulkan frontend: parallel
        // ordered tile sketch + GPU target-index build (device radix sort +
        // race-free slot table) + GPU seed count/emit + device anchor sort +
        // Vulkan tiled-DP score generation, followed by CPU backtracking and CPU
        // Block Aligner (`vulkan_extend_enabled=false`). Same aligner as the
        // fastest CPU row; the varied dimensions are the GPU frontend and GPU
        // index build. It fails loudly when no hardware Vulkan device
        // exists. This row is compared against
        // `bioseq_block_simd_dp_two_stack_nthash` (SIMD minimizers + SIMD
        // chaining + block aligner) and `minimap2_dp`.
        dispatch.params.chaining_algorithm        = ChainingAlgorithmType::VULKAN_TILED_DP;
        dispatch.params.base_alignment_algorithm  = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm       = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
        dispatch.params.vulkan_enabled            = true;
        dispatch.params.vulkan_seed_hits_enabled  = true;
        dispatch.params.vulkan_index_cache_enabled = true;
        dispatch.params.vulkan_fused_frontend_enabled  = true;
        dispatch.params.vulkan_gpu_index_build_enabled = true;
        dispatch.params.vulkan_extend_enabled          = false;
        dispatch.params.do_cigar                       = true;
        dispatch.explicit_vulkan = true;
        dispatch.historical_partial_gpu_probe = true;
    } else if (algo == "bioseq_ksw2_rmq_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::RMQ;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else {
        bioseq_log<ERR>("[bench_v10] unknown algorithm '{}'", algo);
        throw std::invalid_argument(fmt::format(
            "bench_v10 has no dispatch route for algorithm key '{}'", algo));
    }
    return dispatch;
}

[[nodiscard]] constexpr bool
legacy_gpu_index_materialization_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    const bool open_addressed =
        proof.gpu_index_open_addressed_materializations == 1u
        && proof.gpu_index_sorted_dense_materializations == 0u
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_open_addressed_lookup_dispatches
            == proof.seed_lookup_dispatches
        && proof.seed_sorted_dense_lookup_dispatches == 0u
        && proof.gpu_index_slot_capacity >= proof.gpu_index_unique_keys
        && proof.gpu_index_max_load_ppm != 0u
        && proof.gpu_index_max_load_ppm <= 700'000u
        && proof.gpu_index_max_probe != 0u;
    const bool sorted_dense =
        proof.gpu_index_open_addressed_materializations == 0u
        && proof.gpu_index_sorted_dense_materializations == 1u
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_open_addressed_lookup_dispatches == 0u
        && proof.seed_sorted_dense_lookup_dispatches
            == proof.seed_lookup_dispatches
        && proof.gpu_index_slot_capacity == proof.gpu_index_unique_keys
        && proof.gpu_index_max_load_ppm == 1'000'000u
        && proof.gpu_index_max_probe == 0u;
    return !proof.saturated
        && proof.gpu_index_materializations == 1u
        && (open_addressed || sorted_dense)
        && proof.gpu_index_unique_keys != 0u
        && proof.gpu_index_slot_capacity != 0u
        && proof.gpu_index_logical_resident_bytes != 0u
        && proof.gpu_index_actual_allocation_bytes
            >= proof.gpu_index_logical_resident_bytes
        && proof.gpu_index_fully_touched_bytes
            == proof.gpu_index_actual_allocation_bytes
        && proof.gpu_index_staging_bytes == 0u
        && proof.gpu_index_permit_releases == 1u
        && proof.gpu_index_process_footprint_reflections == 1u
        && proof.gpu_index_reflected_footprint_bytes
            >= proof.gpu_index_actual_allocation_bytes
        && proof.post_seed_owned_gpu_cache_evictions == 1u
        && proof.post_seed_owned_gpu_resident_bytes_released
            == proof.gpu_index_actual_allocation_bytes
        && proof.post_seed_owned_cpu_index_releases == 1u
        && proof.post_seed_owned_cpu_position_bytes_released != 0u;
}

static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.gpu_index_materializations = 1u;
    proof.gpu_index_open_addressed_materializations = 1u;
    proof.seed_lookup_dispatches = 1u;
    proof.seed_open_addressed_lookup_dispatches = 1u;
    proof.gpu_index_unique_keys = 3u;
    proof.gpu_index_slot_capacity = 8u;
    proof.gpu_index_logical_resident_bytes = 128u;
    proof.gpu_index_actual_allocation_bytes = 256u;
    proof.gpu_index_fully_touched_bytes = 256u;
    proof.gpu_index_permit_releases = 1u;
    proof.gpu_index_process_footprint_reflections = 1u;
    proof.gpu_index_reflected_footprint_bytes = 256u;
    proof.gpu_index_max_load_ppm = 375'000u;
    proof.gpu_index_max_probe = 3u;
    proof.post_seed_owned_gpu_cache_evictions = 1u;
    proof.post_seed_owned_gpu_resident_bytes_released = 256u;
    proof.post_seed_owned_cpu_index_releases = 1u;
    proof.post_seed_owned_cpu_position_bytes_released = 128u;
    if (!legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_open_addressed_materializations = 0u;
    proof.gpu_index_sorted_dense_materializations = 1u;
    proof.seed_open_addressed_lookup_dispatches = 0u;
    proof.seed_sorted_dense_lookup_dispatches = 1u;
    proof.gpu_index_slot_capacity = 3u;
    proof.gpu_index_max_load_ppm = 1'000'000u;
    proof.gpu_index_max_probe = 0u;
    if (!legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_open_addressed_materializations = 1u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_open_addressed_materializations = 0u;
    proof.seed_open_addressed_lookup_dispatches = 1u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.seed_open_addressed_lookup_dispatches = 0u;
    proof.gpu_index_max_probe = 1u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_max_probe = 0u;
    proof.gpu_index_permit_releases = 0u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_permit_releases = 1u;
    proof.gpu_index_process_footprint_reflections = 0u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_process_footprint_reflections = 1u;
    proof.gpu_index_reflected_footprint_bytes = 255u;
    return !legacy_gpu_index_materialization_proof_accepted(proof);
}());

static_assert(bioseq::gpu::vulkan::kPhaseCQueryGateWordCount == 2'097'152u);
static_assert(bioseq::gpu::vulkan::kPhaseCQueryGateMask == 67'108'863u);

[[nodiscard]] constexpr bool
phase_c_gpu_index_ownership_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    return !proof.saturated
        && proof.target_sketch_dispatches == 1u
        && proof.target_index_build_dispatches == 1u
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_open_addressed_lookup_dispatches == 0u
        && proof.seed_sorted_dense_lookup_dispatches
            == proof.seed_lookup_dispatches
        && proof.gpu_index_count_phase_admissions == 1u
        && proof.gpu_index_main_peak_admissions == 1u
        && proof.gpu_index_resident_transfers == 1u
        && proof.gpu_index_adoption_rereserve_attempts == 0u
        && proof.gpu_index_adoption_successes == 1u
        && proof.gpu_index_owned_resident_bytes != 0u
        && proof.gpu_index_gate_enabled == 0u
        && proof.gpu_index_gate_word_count == 0u
        && proof.gpu_index_gate_mask == 0u
        && proof.gpu_index_gate_set_bits == 0u
        && proof.gpu_index_gate_count_dispatches == 0u
        && proof.gpu_index_gated_emit_dispatches == 0u
        && proof.gpu_index_ungated_emit_dispatches == 1u
        && proof.gpu_index_retained_minimizer_count != 0u
        && proof.gpu_index_distinct_minimizer_count != 0u
        && proof.gpu_index_distinct_minimizer_count
            <= proof.gpu_index_retained_minimizer_count
        && proof.post_seed_owned_gpu_cache_evictions == 1u
        && proof.post_seed_owned_gpu_resident_bytes_released
            == proof.gpu_index_owned_resident_bytes
        && proof.post_seed_owned_cpu_index_releases == 0u
        && proof.post_seed_owned_cpu_position_bytes_released == 0u;
}

static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.target_sketch_dispatches = 1u;
    proof.target_index_build_dispatches = 1u;
    proof.seed_lookup_dispatches = 1u;
    proof.seed_sorted_dense_lookup_dispatches = 1u;
    proof.gpu_index_count_phase_admissions = 1u;
    proof.gpu_index_main_peak_admissions = 1u;
    proof.gpu_index_resident_transfers = 1u;
    proof.gpu_index_adoption_successes = 1u;
    proof.gpu_index_owned_resident_bytes = 4096u;
    proof.gpu_index_ungated_emit_dispatches = 1u;
    proof.gpu_index_retained_minimizer_count = 53u;
    proof.gpu_index_distinct_minimizer_count = 17u;
    proof.post_seed_owned_gpu_cache_evictions = 1u;
    proof.post_seed_owned_gpu_resident_bytes_released = 4096u;
    if (!phase_c_gpu_index_ownership_proof_accepted(proof)) return false;

    auto invalid = proof;
    invalid.target_sketch_dispatches = 2u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.target_index_build_dispatches = 2u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_adoption_rereserve_attempts = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gated_emit_dispatches = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.seed_open_addressed_lookup_dispatches = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_word_count = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_mask = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_set_bits = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_distinct_minimizer_count = 54u;
    return !phase_c_gpu_index_ownership_proof_accepted(invalid);
}());

[[nodiscard]] constexpr bool seed_radix_sort_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    return !proof.saturated
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_bitonic_sort_dispatches == 0u
        && proof.seed_radix_sort_operations == proof.seed_lookup_dispatches
        && proof.seed_radix_anchor_count > 1024u
        && proof.seed_radix_passes != 0u
        && proof.seed_radix_histogram_dispatches == proof.seed_radix_passes
        && proof.seed_radix_scan_dispatches == proof.seed_radix_passes
        && proof.seed_radix_scatter_dispatches == proof.seed_radix_passes
        && proof.seed_host_sort_fallbacks == 0u;
}

static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.seed_lookup_dispatches = 1u;
    proof.seed_radix_sort_operations = 1u;
    proof.seed_radix_anchor_count = 1'520'944u;
    proof.seed_radix_passes = 4u;
    proof.seed_radix_histogram_dispatches = 4u;
    proof.seed_radix_scan_dispatches = 4u;
    proof.seed_radix_scatter_dispatches = 4u;
    if (!seed_radix_sort_proof_accepted(proof)) return false;
    proof.seed_radix_sort_operations = 2u;
    if (seed_radix_sort_proof_accepted(proof)) return false;
    proof.seed_radix_sort_operations = 1u;
    proof.seed_bitonic_sort_dispatches = 1u;
    if (seed_radix_sort_proof_accepted(proof)) return false;
    proof.seed_bitonic_sort_dispatches = 0u;
    proof.seed_host_sort_fallbacks = 1u;
    return !seed_radix_sort_proof_accepted(proof);
}());

[[nodiscard]] constexpr bool exact_b8_execution_accepted(
        const TwoStackExecutionProof& proof) noexcept {
    return proof.accepted(/*require_nonempty_vector_work=*/true)
        && proof.generic_u64_kernel_calls == 0
        && proof.generic_u64_candidate_blocks == 0
        && proof.generic_u64_lane_visits == 0
        && proof.k15_w10_b8_u32_hash_kernel_calls > 0
        && proof.k15_w10_b8_u32_hash_candidate_blocks
            == proof.vector_batches
        && proof.k15_w10_b8_u32_hash_lane_visits
            == proof.vector_lanes_visited
        && proof.k15_w10_b8_packed22_groups
            == proof.k15_w10_b8_u32_hash_candidate_blocks
        && proof.k15_w10_b8_packed22_lane_visits
            == proof.k15_w10_b8_u32_hash_lane_visits;
}

/// Fail-closed proof policy for one completed pair. The minimap2 reference is
/// intentionally outside Bioseq's proof ABI and therefore must emit no Bioseq
/// algorithm proof. Every Bioseq row is validated against the selectors that
/// fixed its template leaf. The strict XXL gate additionally requires positive
/// non-empty vector work; ordinary benchmark rows still reject missing,
/// malformed, forbidden, or unexpected evidence without requiring work from an
/// empty input. Routes that did not select an algorithm must not emit its proof.
void require_selected_route_execution(
        std::string_view algo,
        const AlgorithmDispatch& dispatch,
        const MatchResultCollection& results,
        bool strict_xxl_gate) {
    if (dispatch.is_minimap2_reference) {
        if (dispatch.params.minimizer_algorithm != MinimizerAlgorithmType::SCALAR
            || dispatch.params.base_alignment_algorithm
                != BaseAlignmentAlgorithmType::KSW2
            || results.has_any_algorithm_execution_proof()) {
            throw std::runtime_error(fmt::format(
                "minimap2 reference row '{}' violated scalar/KSW2/no-Bioseq-proof policy",
                algo));
        }
        return;
    }

    const bool exact_simd_sketch_selected =
        dispatch.params.minimizer_algorithm
            == MinimizerAlgorithmType::TWO_STACK_NTHASH;
    if (exact_simd_sketch_selected) {
        if (!results.exact_simd_query_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)
            || !results.exact_simd_target_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected ExactSimd sketch route lacks accepted query/target proof");
        }
        if (strict_xxl_gate
            && (!exact_b8_execution_accepted(
                    results.exact_simd_query_execution_proof)
                || !exact_b8_execution_accepted(
                    results.exact_simd_target_execution_proof))) {
            const auto& query = results.exact_simd_query_execution_proof;
            const auto& target = results.exact_simd_target_execution_proof;
            throw std::runtime_error(fmt::format(
                "strict XXL ExactSimd route did not execute B8/u32 for both roles: "
                "query(generic_calls={},generic_blocks={},generic_lanes={},"
                "b8_calls={},b8_blocks={},b8_lanes={},packed22_groups={},"
                "packed22_lanes={},vector_blocks={},vector_lanes={}); "
                "target(generic_calls={},generic_blocks={},generic_lanes={},"
                "b8_calls={},b8_blocks={},b8_lanes={},packed22_groups={},"
                "packed22_lanes={},vector_blocks={},vector_lanes={})",
                query.generic_u64_kernel_calls,
                query.generic_u64_candidate_blocks,
                query.generic_u64_lane_visits,
                query.k15_w10_b8_u32_hash_kernel_calls,
                query.k15_w10_b8_u32_hash_candidate_blocks,
                query.k15_w10_b8_u32_hash_lane_visits,
                query.k15_w10_b8_packed22_groups,
                query.k15_w10_b8_packed22_lane_visits,
                query.vector_batches,
                query.vector_lanes_visited,
                target.generic_u64_kernel_calls,
                target.generic_u64_candidate_blocks,
                target.generic_u64_lane_visits,
                target.k15_w10_b8_u32_hash_kernel_calls,
                target.k15_w10_b8_u32_hash_candidate_blocks,
                target.k15_w10_b8_u32_hash_lane_visits,
                target.k15_w10_b8_packed22_groups,
                target.k15_w10_b8_packed22_lane_visits,
                target.vector_batches,
                target.vector_lanes_visited));
        }
    } else if (results.exact_simd_query_execution_proof_present
               || results.exact_simd_target_execution_proof_present) {
        throw std::runtime_error(
            "non-ExactSimd sketch route emitted ExactSimd proof");
    }

    const bool paper_nthash_sketch_selected =
        dispatch.params.minimizer_algorithm
            == MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
    if (paper_nthash_sketch_selected) {
        if (!results.paper_nthash_query_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)
            || !results.paper_nthash_target_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected PaperNtHashSimd route lacks accepted query/target proof");
        }
        if (strict_xxl_gate
            && results.paper_nthash_target_execution_proof.segments_combined < 2) {
            throw std::runtime_error(
                "strict XXL PaperNtHashSimd route did not execute a segmented target sketch");
        }
    } else if (results.paper_nthash_query_execution_proof_present
               || results.paper_nthash_target_execution_proof_present) {
        throw std::runtime_error(
            "non-PaperNtHashSimd route emitted Paper ntHash proof");
    }

    const bool simd_chaining_selected =
        dispatch.params.chaining_algorithm == ChainingAlgorithmType::SIMD_MM2;
    if (simd_chaining_selected) {
        if (!results.simd_chaining_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected SIMD chaining route lacks accepted score/backtrack proof");
        }
    } else if (results.simd_chaining_execution_proof_present) {
        throw std::runtime_error(
            "non-SIMD chaining route emitted SIMD chaining proof");
    }

    const bool block_aligner_selected =
        dispatch.params.base_alignment_algorithm
            == BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    if (block_aligner_selected) {
        if (!dispatch.params.do_cigar) {
            if (strict_xxl_gate) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Block Aligner row '{}' must request CIGAR execution",
                    algo));
            }
            if (results.block_aligner_execution_proof_present) {
                throw std::runtime_error(fmt::format(
                    "Block Aligner row '{}' emitted proof although CIGAR was disabled",
                    algo));
            }
        } else if (!results.block_aligner_execution_accepted(
                       /*require_nonempty_simd_work=*/strict_xxl_gate)) {
            const auto& proof = results.block_aligner_execution_proof;
            throw std::runtime_error(fmt::format(
                "selected Block Aligner lacks accepted SIMD proof: "
                "algo='{}' cigar={} present={} accepted={} executed={} "
                "nonempty={} vector_batches={} cells={} ksw2={} scalar_fallbacks={}",
                algo, dispatch.params.do_cigar,
                results.block_aligner_execution_proof_present,
                proof.accepted(), proof.executed_calls(), proof.nonempty_calls,
                proof.simd_dp_vector_batches, proof.simd_dp_cells,
                proof.forbidden_ksw2_calls,
                proof.forbidden_scalar_biological_fallbacks));
        }
    } else if (results.block_aligner_execution_proof_present) {
        throw std::runtime_error(fmt::format(
            "non-Block row '{}' emitted Block Aligner execution proof", algo));
    }

    if (dispatch.explicit_vulkan) {
        bool expect_query_sketch = false;
        bool expect_target_sketch = false;
        bool expect_target_index_build = false;
        bool expect_seed_lookup = false;
        bool expect_chain_score = true;
        if (algo == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
            // CPU ExactSimd sketch/index/seed, Vulkan chain score only.
        } else if (algo
                   == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
            expect_query_sketch = true;
            expect_target_sketch = true;
            expect_seed_lookup = true;
        } else if (algo == "bioseq_block_vulkan_phase_c_frontend") {
            expect_query_sketch = true;
            expect_target_sketch = true;
            expect_target_index_build = true;
            expect_seed_lookup = true;
        } else {
            throw std::logic_error(fmt::format(
                "explicit Vulkan row '{}' has no proof-stage contract", algo));
        }
        if (strict_xxl_gate
            && !results.partial_vulkan_execution_proof
                    .exact_stage_mask_accepted(
                        expect_query_sketch,
                        expect_target_sketch,
                        expect_target_index_build,
                        expect_seed_lookup,
                        expect_chain_score)) {
            const auto& proof = results.partial_vulkan_execution_proof;
            throw std::runtime_error(fmt::format(
                "strict XXL Vulkan row '{}' lacks exact positive dispatch proof: "
                "query_sketch={} target_sketch={} target_index_build={} "
                "seed_lookup={} chain_score={} saturated={}",
                algo,
                proof.query_sketch_dispatches,
                proof.target_sketch_dispatches,
                proof.target_index_build_dispatches,
                proof.seed_lookup_dispatches,
                proof.chain_score_dispatches,
                proof.saturated));
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
            const auto& proof = results.partial_vulkan_execution_proof;
            const bool materialization_proof_accepted =
                legacy_gpu_index_materialization_proof_accepted(proof);
            if (!materialization_proof_accepted) {
                throw std::runtime_error(fmt::format(
                    "strict XXL legacy Vulkan row lacks exact GPU-index "
                    "materialization proof: materializations={} open={} "
                    "sorted={} seed_lookup={} seed_open={} seed_sorted={} "
                    "D={} capacity={} load_ppm={} max_probe={} "
                    "logical_bytes={} actual_bytes={} touched_bytes={} "
                    "staging_bytes={} permit_releases={} reflections={} "
                    "reflected_delta_bytes={} post_seed_gpu_evictions={} "
                    "post_seed_gpu_bytes={} post_seed_cpu_releases={} "
                    "post_seed_cpu_position_bytes={}",
                    proof.gpu_index_materializations,
                    proof.gpu_index_open_addressed_materializations,
                    proof.gpu_index_sorted_dense_materializations,
                    proof.seed_lookup_dispatches,
                    proof.seed_open_addressed_lookup_dispatches,
                    proof.seed_sorted_dense_lookup_dispatches,
                    proof.gpu_index_unique_keys,
                    proof.gpu_index_slot_capacity,
                    proof.gpu_index_max_load_ppm,
                    proof.gpu_index_max_probe,
                    proof.gpu_index_logical_resident_bytes,
                    proof.gpu_index_actual_allocation_bytes,
                    proof.gpu_index_fully_touched_bytes,
                    proof.gpu_index_staging_bytes,
                    proof.gpu_index_permit_releases,
                    proof.gpu_index_process_footprint_reflections,
                    proof.gpu_index_reflected_footprint_bytes,
                    proof.post_seed_owned_gpu_cache_evictions,
                    proof.post_seed_owned_gpu_resident_bytes_released,
                    proof.post_seed_owned_cpu_index_releases,
                    proof.post_seed_owned_cpu_position_bytes_released));
            }
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_phase_c_frontend") {
            const auto& proof = results.partial_vulkan_execution_proof;
            if (!phase_c_gpu_index_ownership_proof_accepted(proof)) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Phase-C row lacks exact GPU target-index "
                    "ownership/gated-build proof: target_sketches={} target_builds={} "
                    "seed_lookup={} seed_open={} seed_sorted={} "
                    "count_admissions={} main_admissions={} transfers={} "
                    "rereserve={} adoptions={} resident_bytes={} "
                    "gate_enabled={} gate_words={} gate_mask={} gate_set_bits={} "
                    "gate_count={} gated_emit={} ungated_emit={} T={} D={} "
                    "post_seed_gpu_evictions={} post_seed_gpu_bytes={} "
                    "post_seed_cpu_releases={} post_seed_cpu_position_bytes={} "
                    "saturated={}",
                    proof.target_sketch_dispatches,
                    proof.target_index_build_dispatches,
                    proof.seed_lookup_dispatches,
                    proof.seed_open_addressed_lookup_dispatches,
                    proof.seed_sorted_dense_lookup_dispatches,
                    proof.gpu_index_count_phase_admissions,
                    proof.gpu_index_main_peak_admissions,
                    proof.gpu_index_resident_transfers,
                    proof.gpu_index_adoption_rereserve_attempts,
                    proof.gpu_index_adoption_successes,
                    proof.gpu_index_owned_resident_bytes,
                    proof.gpu_index_gate_enabled,
                    proof.gpu_index_gate_word_count,
                    proof.gpu_index_gate_mask,
                    proof.gpu_index_gate_set_bits,
                    proof.gpu_index_gate_count_dispatches,
                    proof.gpu_index_gated_emit_dispatches,
                    proof.gpu_index_ungated_emit_dispatches,
                    proof.gpu_index_retained_minimizer_count,
                    proof.gpu_index_distinct_minimizer_count,
                    proof.post_seed_owned_gpu_cache_evictions,
                    proof.post_seed_owned_gpu_resident_bytes_released,
                    proof.post_seed_owned_cpu_index_releases,
                    proof.post_seed_owned_cpu_position_bytes_released,
                    proof.saturated));
            }
        }
        if (strict_xxl_gate
            && (algo == "bioseq_block_vulkan_dp_vulkan_minimap_int"
                || algo == "bioseq_block_vulkan_phase_c_frontend")) {
            const auto& proof = results.partial_vulkan_execution_proof;
            if (!seed_radix_sort_proof_accepted(proof)) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Vulkan row '{}' lacks scalable GPU seed-sort proof: "
                    "seed_dispatches={} bitonic={} radix_ops={} radix_anchors={} "
                    "radix_passes={} histogram={} scan={} scatter={} host_sort={}",
                    algo,
                    proof.seed_lookup_dispatches,
                    proof.seed_bitonic_sort_dispatches,
                    proof.seed_radix_sort_operations,
                    proof.seed_radix_anchor_count,
                    proof.seed_radix_passes,
                    proof.seed_radix_histogram_dispatches,
                    proof.seed_radix_scan_dispatches,
                    proof.seed_radix_scatter_dispatches,
                    proof.seed_host_sort_fallbacks));
            }
        }
    } else if (results.partial_vulkan_execution_proof.has_any_dispatch()) {
        throw std::runtime_error(fmt::format(
            "non-Vulkan row '{}' emitted partial Vulkan execution proof", algo));
    }
}

/// Hand the (query, target) pair off to either the C reference or the
/// bioseq port, then aggregate the per-pair primary-correctness flags
/// into the AlignerOutput.
[[nodiscard]] AlignerOutput run_aligner_impl(
                                          std::string_view algo,
                                          const ParamMap& /*params*/,
                                          const AlignerInput& input,
                                          bool strict_xxl_gate,
                                          bool capture_full_route_proof) {
    const AlgorithmDispatch dispatch = dispatch_algorithm(algo, input.kmer_size_override);

    AlignerOutput output;
    output.total_pairs = input.pairs.size();
    output.historical_partial_gpu_probe =
        dispatch.historical_partial_gpu_probe;
    output.all_gpu_acceptance_eligible = false;
    if (dispatch.explicit_vulkan != dispatch.historical_partial_gpu_probe) {
        throw std::logic_error(fmt::format(
            "Vulkan row '{}' lacks explicit historical partial-GPU classification",
            algo));
    }
    if (dispatch.explicit_vulkan && !is_explicit_vulkan_hardware_available(algo)) {
        if (strict_xxl_gate) {
            throw std::runtime_error(
                "strict XXL gate requires every selected Vulkan row to execute");
        }
        output.vulkan_hardware_unavailable = true;
        return output;
    }
    for (std::size_t pair_index = 0; pair_index < input.pairs.size(); ++pair_index) {
        auto& pair = input.pairs[pair_index];     // pairs are `mutable` in AlignerInput; see field comment
        MatchResultCollection results;
        // Defensive try-block: a few algorithm × test_case combinations
        // (notably wide-substitution low-indel patterns on LONG / VERY_LONG
        // / XL inputs in `bench_aligners_grid`) currently throw
        // `std::length_error` from inside the chaining or alignment
        // back-end. Without this catch, the unhandled exception unwinds
        // through the framework's TBB worker, calls `std::terminate`, and
        // aborts the whole bench mid-grid.  We log the offending tuple so
        // a follow-up can drill in, and surface a zero-result iteration
        // so the run can still complete.
        try {
            if (dispatch.is_minimap2_reference) {
                results = bioseq::bench::HWY_NAMESPACE::tool_minimap2(
                    input.queries_acgt[pair_index],
                    input.targets_acgt[pair_index],
                    dispatch.params);
            } else {
                // DOC0263 — minimap_port now takes lazy_compact_sequence&; wrap the
                // in-memory ranges in the zero-overhead no-fragment lazy wrapper.
                auto bench_lazy_query  = bioseq::storage::make_lazy_in_memory(pair.query);
                auto bench_lazy_target = bioseq::storage::make_lazy_in_memory(*pair.target);
                if (dispatch.params.minimizer_algorithm
                    == MinimizerAlgorithmType::PAPER_NTHASH_SIMD) {
                    // Paper ntHash uses a 32-bit hash plus segment-local position
                    // representation.  XXL targets must therefore traverse the
                    // production segmented entry point; the unsegmented port can
                    // select a query-sized position layout and correctly rejects
                    // the 500-Mbp target rather than truncating coordinates.
                    results = bioseq::simd::HWY_NAMESPACE::minimap_port_segmented(
                        bench_lazy_query, bench_lazy_target, dispatch.params);
                } else {
                    results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                        bench_lazy_query, bench_lazy_target, dispatch.params);
                }
            }
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& exception) {
            bioseq_log<ERR>("[bench_v10] selected Vulkan aligner failed (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            if (dispatch.explicit_vulkan || strict_xxl_gate) {
                throw;
            }
            continue;
        } catch (const std::exception& exception) {
            bioseq_log<ERR>("[bench_v10] aligner threw exception (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            if (strict_xxl_gate) throw;
            continue;
        } catch (...) {
            bioseq_log<ERR>("[bench_v10] aligner threw non-std exception (algo='{}', pair_index={})",
                 algo, pair_index);
            if (strict_xxl_gate) throw;
            continue;
        }
        require_selected_route_execution(
            algo, dispatch, results, strict_xxl_gate);
        output.selected_route_execution_proof_accepted = true;
        if (results.paper_nthash_query_execution_proof_present) {
            output.paper_nthash_query_execution_proof.combine(
                results.paper_nthash_query_execution_proof);
            output.paper_nthash_query_execution_proof_present = true;
        }
        if (results.paper_nthash_target_execution_proof_present) {
            output.paper_nthash_target_execution_proof.combine(
                results.paper_nthash_target_execution_proof);
            output.paper_nthash_target_execution_proof_present = true;
        }
        if (results.exact_simd_query_execution_proof_present) {
            output.exact_simd_query_execution_proof.combine(
                results.exact_simd_query_execution_proof);
            output.exact_simd_query_execution_proof_present = true;
        }
        if (results.exact_simd_target_execution_proof_present) {
            output.exact_simd_target_execution_proof.combine(
                results.exact_simd_target_execution_proof);
            output.exact_simd_target_execution_proof_present = true;
        }
        if (capture_full_route_proof) {
            if (results.simd_chaining_execution_proof_present) {
                output.simd_chaining_execution_proof.combine(
                    results.simd_chaining_execution_proof);
                output.simd_chaining_execution_proof_present = true;
            }
            if (results.block_aligner_execution_proof_present) {
                output.block_aligner_execution_proof.combine(
                    results.block_aligner_execution_proof);
                output.block_aligner_execution_proof_present = true;
            }
        }
        output.partial_vulkan_execution_proof.combine(
            results.partial_vulkan_execution_proof);
        bool found_primary = false;
        for (const auto& match : results.view()) {
            output.total_results += 1;
            if (!match.is_primary || found_primary) continue;
            found_primary = true;
            const std::int32_t offset =
                match.target_start - static_cast<std::int32_t>(pair.truth_target_start);
            const std::int32_t abs_offset = std::abs(offset);
            output.sum_abs_position_offset += static_cast<double>(abs_offset);
            if (abs_offset < kPositionTolerance &&
                match.is_reverse == pair.is_reverse) {
                output.correct_pairs += 1;
            }
        }
    }
    output.phase_c_gpu_index_ownership_proof_accepted =
        algo == "bioseq_block_vulkan_phase_c_frontend"
        && phase_c_gpu_index_ownership_proof_accepted(
            output.partial_vulkan_execution_proof);
    if (strict_xxl_gate
        && (output.total_pairs != 1
            || output.total_results != 1
            || output.correct_pairs != 1)) {
        throw std::runtime_error(fmt::format(
            "strict XXL correctness gate rejected algo='{}': pairs={} "
            "match_count={} correct_pairs={} (required 1/1/1)",
            algo, output.total_pairs, output.total_results,
            output.correct_pairs));
    }
    return output;
}

[[nodiscard]] AlignerOutput run_aligner(
        std::string_view algo,
        const ParamMap& params,
        const AlignerInput& input) {
    return run_aligner_impl(algo, params, input, false, false);
}

[[nodiscard]] AlignerOutput run_aligner_strict_xxl(
        std::string_view algo,
        const ParamMap& params,
        const AlignerInput& input) {
    return run_aligner_impl(algo, params, input, true, false);
}

[[nodiscard]] AlignerOutput run_aligner_strict_xxl_paired(
        std::string_view algo,
        const ParamMap& params,
        const AlignerInput& input) {
    return run_aligner_impl(algo, params, input, true, true);
}

/// Translate `Output` into the evaluation metrics declared in the JSON
/// config. The framework drops keys that aren't declared on the active
/// benchmark, so we always emit the full set; only the keys the running
/// benchmark consumes land in the report.
[[nodiscard]] std::map<std::string, double> evaluate_aligner(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const AlignerInput& /*input*/,
    const AlignerOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("selected_route_execution_proof_accepted",
        output.selected_route_execution_proof_accepted ? 1.0 : 0.0);
    metrics.emplace("paper_nthash_query_execution_proof_accepted",
        output.paper_nthash_query_execution_proof_present
            && output.paper_nthash_query_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("paper_nthash_target_execution_proof_accepted",
        output.paper_nthash_target_execution_proof_present
            && output.paper_nthash_target_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("paper_nthash_query_segments_combined",
        static_cast<double>(
            output.paper_nthash_query_execution_proof.segments_combined));
    metrics.emplace("paper_nthash_target_segments_combined",
        static_cast<double>(
            output.paper_nthash_target_execution_proof.segments_combined));
    metrics.emplace("paper_nthash_fallback_calls",
        static_cast<double>(
            output.paper_nthash_query_execution_proof.fallback_calls
            + output.paper_nthash_target_execution_proof.fallback_calls));
    metrics.emplace("exact_simd_query_execution_proof_accepted",
        output.exact_simd_query_execution_proof_present
            && output.exact_simd_query_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_execution_proof_accepted",
        output.exact_simd_target_execution_proof_present
            && output.exact_simd_target_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_b8_route_accepted",
        output.exact_simd_query_execution_proof_present
            && exact_b8_execution_accepted(
                output.exact_simd_query_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_b8_route_accepted",
        output.exact_simd_target_execution_proof_present
            && exact_b8_execution_accepted(
                output.exact_simd_target_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_generic_kernel_calls",
        static_cast<double>(output.exact_simd_query_execution_proof
            .generic_u64_kernel_calls));
    metrics.emplace("exact_simd_target_generic_kernel_calls",
        static_cast<double>(output.exact_simd_target_execution_proof
            .generic_u64_kernel_calls));
    metrics.emplace("exact_simd_query_b8_kernel_calls",
        static_cast<double>(output.exact_simd_query_execution_proof
            .k15_w10_b8_u32_hash_kernel_calls));
    metrics.emplace("exact_simd_target_b8_kernel_calls",
        static_cast<double>(output.exact_simd_target_execution_proof
            .k15_w10_b8_u32_hash_kernel_calls));
    metrics.emplace("historical_partial_gpu_probe",
                    output.historical_partial_gpu_probe ? 1.0 : 0.0);
    metrics.emplace("all_gpu_acceptance_eligible",
                    output.all_gpu_acceptance_eligible ? 1.0 : 0.0);
    metrics.emplace("phase_c_gpu_index_ownership_proof_accepted",
        output.phase_c_gpu_index_ownership_proof_accepted ? 1.0 : 0.0);
    metrics.emplace("vulkan_query_sketch_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .query_sketch_dispatches));
    metrics.emplace("vulkan_target_sketch_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .target_sketch_dispatches));
    metrics.emplace("vulkan_target_index_build_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .target_index_build_dispatches));
    metrics.emplace("vulkan_seed_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_lookup_dispatches));
    metrics.emplace("vulkan_seed_open_addressed_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_open_addressed_lookup_dispatches));
    metrics.emplace("vulkan_seed_sorted_dense_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_sorted_dense_lookup_dispatches));
    metrics.emplace("vulkan_chain_score_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .chain_score_dispatches));
    metrics.emplace("vulkan_seed_bitonic_sort_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_bitonic_sort_dispatches));
    metrics.emplace("vulkan_seed_radix_sort_operations",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_sort_operations));
    metrics.emplace("vulkan_seed_radix_anchor_count",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_anchor_count));
    metrics.emplace("vulkan_seed_radix_passes",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_passes));
    metrics.emplace("vulkan_seed_radix_histogram_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_histogram_dispatches));
    metrics.emplace("vulkan_seed_radix_scan_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_scan_dispatches));
    metrics.emplace("vulkan_seed_radix_scatter_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_scatter_dispatches));
    metrics.emplace("vulkan_seed_host_sort_fallbacks",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_host_sort_fallbacks));
    const auto& gpu_index = output.partial_vulkan_execution_proof;
    metrics.emplace("gpu_index_materializations",
                    static_cast<double>(gpu_index.gpu_index_materializations));
    metrics.emplace("gpu_index_open_addressed_materializations",
                    static_cast<double>(
                        gpu_index.gpu_index_open_addressed_materializations));
    metrics.emplace("gpu_index_sorted_dense_materializations",
                    static_cast<double>(
                        gpu_index.gpu_index_sorted_dense_materializations));
    metrics.emplace("gpu_index_unique_keys",
                    static_cast<double>(gpu_index.gpu_index_unique_keys));
    metrics.emplace("gpu_index_slot_capacity",
                    static_cast<double>(gpu_index.gpu_index_slot_capacity));
    metrics.emplace("gpu_index_logical_resident_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_logical_resident_bytes));
    metrics.emplace("gpu_index_actual_allocation_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_actual_allocation_bytes));
    metrics.emplace("gpu_index_fully_touched_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_fully_touched_bytes));
    metrics.emplace("gpu_index_staging_bytes",
                    static_cast<double>(gpu_index.gpu_index_staging_bytes));
    metrics.emplace("gpu_index_permit_releases",
                    static_cast<double>(gpu_index.gpu_index_permit_releases));
    metrics.emplace("gpu_index_process_footprint_reflections",
                    static_cast<double>(
                        gpu_index.gpu_index_process_footprint_reflections));
    metrics.emplace("gpu_index_reflected_footprint_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_reflected_footprint_bytes));
    metrics.emplace("gpu_index_max_load_ppm",
                    static_cast<double>(gpu_index.gpu_index_max_load_ppm));
    metrics.emplace("gpu_index_max_probe",
                    static_cast<double>(gpu_index.gpu_index_max_probe));
    metrics.emplace("gpu_index_count_phase_admissions",
        static_cast<double>(gpu_index.gpu_index_count_phase_admissions));
    metrics.emplace("gpu_index_main_peak_admissions",
        static_cast<double>(gpu_index.gpu_index_main_peak_admissions));
    metrics.emplace("gpu_index_resident_transfers",
        static_cast<double>(gpu_index.gpu_index_resident_transfers));
    metrics.emplace("gpu_index_adoption_rereserve_attempts",
        static_cast<double>(gpu_index.gpu_index_adoption_rereserve_attempts));
    metrics.emplace("gpu_index_adoption_successes",
        static_cast<double>(gpu_index.gpu_index_adoption_successes));
    metrics.emplace("gpu_index_owned_resident_bytes",
        static_cast<double>(gpu_index.gpu_index_owned_resident_bytes));
    metrics.emplace("gpu_index_gate_enabled",
        static_cast<double>(gpu_index.gpu_index_gate_enabled));
    metrics.emplace("gpu_index_gate_word_count",
        static_cast<double>(gpu_index.gpu_index_gate_word_count));
    metrics.emplace("gpu_index_gate_mask",
        static_cast<double>(gpu_index.gpu_index_gate_mask));
    metrics.emplace("gpu_index_gate_set_bits",
        static_cast<double>(gpu_index.gpu_index_gate_set_bits));
    metrics.emplace("gpu_index_gate_count_dispatches",
        static_cast<double>(gpu_index.gpu_index_gate_count_dispatches));
    metrics.emplace("gpu_index_gated_emit_dispatches",
        static_cast<double>(gpu_index.gpu_index_gated_emit_dispatches));
    metrics.emplace("gpu_index_ungated_emit_dispatches",
        static_cast<double>(gpu_index.gpu_index_ungated_emit_dispatches));
    metrics.emplace("gpu_index_retained_minimizer_count",
        static_cast<double>(gpu_index.gpu_index_retained_minimizer_count));
    metrics.emplace("gpu_index_distinct_minimizer_count",
        static_cast<double>(gpu_index.gpu_index_distinct_minimizer_count));
    metrics.emplace("post_seed_owned_gpu_cache_evictions",
        static_cast<double>(
            gpu_index.post_seed_owned_gpu_cache_evictions));
    metrics.emplace("post_seed_owned_gpu_resident_bytes_released",
        static_cast<double>(
            gpu_index.post_seed_owned_gpu_resident_bytes_released));
    metrics.emplace("post_seed_owned_cpu_index_releases",
        static_cast<double>(
            gpu_index.post_seed_owned_cpu_index_releases));
    metrics.emplace("post_seed_owned_cpu_position_bytes_released",
        static_cast<double>(
            gpu_index.post_seed_owned_cpu_position_bytes_released));
    if (output.vulkan_hardware_unavailable) {
        const double not_measured = std::numeric_limits<double>::quiet_NaN();
        metrics.emplace("match_count", not_measured);
        metrics.emplace("primary_correct_fraction", not_measured);
        metrics.emplace("mean_position_offset_bp", not_measured);
        return metrics;
    }
    metrics.emplace("match_count", static_cast<double>(output.total_results));
    const double pairs_d = static_cast<double>(output.total_pairs);
    metrics.emplace("primary_correct_fraction",
                     output.total_pairs == 0 ? 0.0
                     : static_cast<double>(output.correct_pairs) / pairs_d);
    metrics.emplace("mean_position_offset_bp",
                     output.total_pairs == 0 ? 0.0
                     : output.sum_abs_position_offset / pairs_d);
    return metrics;
}

[[nodiscard]] nlohmann::json two_stack_proof_json(
    const TwoStackExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        BIOSEQ_PROOF_FIELD(bp_consumed),
        BIOSEQ_PROOF_FIELD(minimizers_emitted),
        BIOSEQ_PROOF_FIELD(vector_batches),
        BIOSEQ_PROOF_FIELD(vector_lanes_visited),
        BIOSEQ_PROOF_FIELD(masked_tail_lanes),
        BIOSEQ_PROOF_FIELD(bridge_symbols_staged),
        BIOSEQ_PROOF_FIELD(bridge_vector_runs),
        BIOSEQ_PROOF_FIELD(consecutive_kmer_batches),
        BIOSEQ_PROOF_FIELD(canonical_scramble_batches),
        BIOSEQ_PROOF_FIELD(window_reduction_batches),
        BIOSEQ_PROOF_FIELD(event_generation_batches),
        BIOSEQ_PROOF_FIELD(stable_compress_batches),
        BIOSEQ_PROOF_FIELD(direct_event_compress_batches),
        BIOSEQ_PROOF_FIELD(tie_matrix_event_batches),
        BIOSEQ_PROOF_FIELD(row_transpose_batches),
        BIOSEQ_PROOF_FIELD(row_vector_stores),
        BIOSEQ_PROOF_FIELD(palindrome_lanes),
        BIOSEQ_PROOF_FIELD(first_window_tie_events),
        BIOSEQ_PROOF_FIELD(expiry_tie_events),
        BIOSEQ_PROOF_FIELD(entering_min_transitions),
        BIOSEQ_PROOF_FIELD(expiry_rescan_transitions),
        BIOSEQ_PROOF_FIELD(final_flush_records),
        BIOSEQ_PROOF_FIELD(fragment_symbols_staged),
        BIOSEQ_PROOF_FIELD(packed_decode_batches),
        BIOSEQ_PROOF_FIELD(packed_decode_lanes),
        BIOSEQ_PROOF_FIELD(ambiguous_symbols),
        BIOSEQ_PROOF_FIELD(ambiguous_reset_batches),
        BIOSEQ_PROOF_FIELD(packed_word_boundary_merges),
        BIOSEQ_PROOF_FIELD(scalar_orchestration_steps),
        BIOSEQ_PROOF_FIELD(fail_closed_rejections),
        BIOSEQ_PROOF_FIELD(scalar_backend_branches),
        BIOSEQ_PROOF_FIELD(scalar_tail_symbols),
        BIOSEQ_PROOF_FIELD(scalar_hash_symbols),
        BIOSEQ_PROOF_FIELD(scalar_window_transitions),
        BIOSEQ_PROOF_FIELD(scalar_event_records),
        BIOSEQ_PROOF_FIELD(scalar_sort_records),
        BIOSEQ_PROOF_FIELD(scalar_dedup_records),
        BIOSEQ_PROOF_FIELD(generic_u64_kernel_calls),
        BIOSEQ_PROOF_FIELD(generic_u64_candidate_blocks),
        BIOSEQ_PROOF_FIELD(generic_u64_lane_visits),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_u32_hash_kernel_calls),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_u32_hash_candidate_blocks),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_u32_hash_lane_visits),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_packed22_groups),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_packed22_lane_visits),
        BIOSEQ_PROOF_FIELD(tie_hash_prefilter_batches),
        BIOSEQ_PROOF_FIELD(tie_hash_prefilter_rejected_lanes),
        BIOSEQ_PROOF_FIELD(tie_raw_verification_batches),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

[[nodiscard]] nlohmann::json paper_nthash_proof_json(
    const PackedPaperNthashExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        {"accepted_nonempty", present && proof.accepted(true)},
        BIOSEQ_PROOF_FIELD(scheme_id),
        BIOSEQ_PROOF_FIELD(kmer_size),
        BIOSEQ_PROOF_FIELD(window_size),
        BIOSEQ_PROOF_FIELD(nthash_rotation),
        BIOSEQ_PROOF_FIELD(source_storage_unit_bytes),
        BIOSEQ_PROOF_FIELD(input_symbols),
        BIOSEQ_PROOF_FIELD(input_storage_units),
        BIOSEQ_PROOF_FIELD(segments_combined),
        BIOSEQ_PROOF_FIELD(logical_streams),
        BIOSEQ_PROOF_FIELD(physical_vector_lanes),
        BIOSEQ_PROOF_FIELD(vector_registers_per_step),
        BIOSEQ_PROOF_FIELD(packed_decode_batches),
        BIOSEQ_PROOF_FIELD(packed_decode_lane_visits),
        BIOSEQ_PROOF_FIELD(hash_initialization_batches),
        BIOSEQ_PROOF_FIELD(rolling_hash_batches),
        BIOSEQ_PROOF_FIELD(hash_table_lookup_batches),
        BIOSEQ_PROOF_FIELD(canonical_sum_batches),
        BIOSEQ_PROOF_FIELD(window_update_batches),
        BIOSEQ_PROOF_FIELD(suffix_rebuild_batches),
        BIOSEQ_PROOF_FIELD(emission_decision_batches),
        BIOSEQ_PROOF_FIELD(emission_mask_store_batches),
        BIOSEQ_PROOF_FIELD(minimizers_emitted),
        BIOSEQ_PROOF_FIELD(tied_endpoint_records_emitted),
        BIOSEQ_PROOF_FIELD(stream_boundary_records_buffered),
        BIOSEQ_PROOF_FIELD(stream_boundary_records_committed),
        BIOSEQ_PROOF_FIELD(stream_boundary_duplicates_suppressed),
        BIOSEQ_PROOF_FIELD(scalar_collector_appends),
        BIOSEQ_PROOF_FIELD(partial_chunk_callback_batches),
        BIOSEQ_PROOF_FIELD(masked_tail_lane_visits),
        BIOSEQ_PROOF_FIELD(fail_closed_rejections),
        BIOSEQ_PROOF_FIELD(scalar_hash_symbols),
        BIOSEQ_PROOF_FIELD(scalar_window_transitions),
        BIOSEQ_PROOF_FIELD(scalar_minimum_decisions),
        BIOSEQ_PROOF_FIELD(fallback_calls),
        BIOSEQ_PROOF_FIELD(positions_are_segment_local),
        BIOSEQ_PROOF_FIELD(aggregation_mismatch),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

[[nodiscard]] nlohmann::json simd_chaining_proof_json(
    const SimdChainingExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        BIOSEQ_PROOF_FIELD(anchors_consumed),
        BIOSEQ_PROOF_FIELD(score_vector_batches),
        BIOSEQ_PROOF_FIELD(score_vector_lanes),
        BIOSEQ_PROOF_FIELD(score_masked_lanes),
        BIOSEQ_PROOF_FIELD(descending_candidate_steps),
        BIOSEQ_PROOF_FIELD(visited_marks),
        BIOSEQ_PROOF_FIELD(max_chain_skip_stops),
        BIOSEQ_PROOF_FIELD(global_best_vector_batches),
        BIOSEQ_PROOF_FIELD(global_best_vector_lanes),
        BIOSEQ_PROOF_FIELD(backtrack_rank_batches),
        BIOSEQ_PROOF_FIELD(backtrack_path_vector_batches),
        BIOSEQ_PROOF_FIELD(backtrack_commit_vector_batches),
        BIOSEQ_PROOF_FIELD(backtrack_vector_lanes),
        BIOSEQ_PROOF_FIELD(backtrack_overlap_stops),
        BIOSEQ_PROOF_FIELD(scalar_orchestration_steps),
        BIOSEQ_PROOF_FIELD(fail_closed_rejections),
        BIOSEQ_PROOF_FIELD(scalar_score_branches),
        BIOSEQ_PROOF_FIELD(scalar_backtrack_branches),
        BIOSEQ_PROOF_FIELD(shared_scalar_backtrack_calls),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

[[nodiscard]] nlohmann::json block_aligner_proof_json(
    const BlockAlignerExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        BIOSEQ_PROOF_FIELD(align_region_invocations),
        BIOSEQ_PROOF_FIELD(serial_align_region_dispatches),
        BIOSEQ_PROOF_FIELD(parallel_align_region_dispatches),
        BIOSEQ_PROOF_FIELD(routed_region_scopes),
        BIOSEQ_PROOF_FIELD(left_route_candidates),
        BIOSEQ_PROOF_FIELD(gap_route_candidates),
        BIOSEQ_PROOF_FIELD(right_route_candidates),
        BIOSEQ_PROOF_FIELD(left_eligible),
        BIOSEQ_PROOF_FIELD(left_executed),
        BIOSEQ_PROOF_FIELD(gap_eligible),
        BIOSEQ_PROOF_FIELD(gap_executed),
        BIOSEQ_PROOF_FIELD(right_eligible),
        BIOSEQ_PROOF_FIELD(right_executed),
        BIOSEQ_PROOF_FIELD(global_calls),
        BIOSEQ_PROOF_FIELD(native_xdrop_calls),
        BIOSEQ_PROOF_FIELD(native_xdrop_terminations),
        BIOSEQ_PROOF_FIELD(zero_work_calls),
        BIOSEQ_PROOF_FIELD(nonempty_calls),
        BIOSEQ_PROOF_FIELD(nonempty_calls_with_simd),
        BIOSEQ_PROOF_FIELD(trace_calls),
        BIOSEQ_PROOF_FIELD(executed_trace_calls),
        BIOSEQ_PROOF_FIELD(forward_input_calls),
        BIOSEQ_PROOF_FIELD(reversed_input_calls),
        BIOSEQ_PROOF_FIELD(executed_global_calls),
        BIOSEQ_PROOF_FIELD(executed_native_xdrop_calls),
        BIOSEQ_PROOF_FIELD(executed_forward_input_calls),
        BIOSEQ_PROOF_FIELD(executed_reversed_input_calls),
        BIOSEQ_PROOF_FIELD(simd_lane8_calls),
        BIOSEQ_PROOF_FIELD(simd_lane16_calls),
        BIOSEQ_PROOF_FIELD(simd_dp_vector_batches),
        BIOSEQ_PROOF_FIELD(simd_dp_cells),
        BIOSEQ_PROOF_FIELD(adaptive_grow_events),
        BIOSEQ_PROOF_FIELD(adaptive_shrink_events),
        BIOSEQ_PROOF_FIELD(expected_reversed_compact_symbols),
        BIOSEQ_PROOF_FIELD(reversed_compact_symbols),
        BIOSEQ_PROOF_FIELD(config_identity_calls),
        BIOSEQ_PROOF_FIELD(config_identity_xor),
        BIOSEQ_PROOF_FIELD(config_identity_sum),
        BIOSEQ_PROOF_FIELD(executed_config_identity_calls),
        BIOSEQ_PROOF_FIELD(executed_config_identity_xor),
        BIOSEQ_PROOF_FIELD(executed_config_identity_sum),
        BIOSEQ_PROOF_FIELD(unsupported_contract_rejections),
        BIOSEQ_PROOF_FIELD(forbidden_ksw2_calls),
        BIOSEQ_PROOF_FIELD(forbidden_scalar_biological_fallbacks),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

/// Lossless per-repeat evidence for the statistically paired XXL companion.
/// The framework adds algorithm/test-case/repeat/schedule/wall-clock fields
/// and durably flushes the resulting object. Keeping the route proof beside
/// every duration prevents a fast sample from being accepted after a silent
/// backend change; repeat zero is also summarized separately as first-repeat
/// evidence by the framework raw manifest (not claimed globally cold).
[[nodiscard]] std::string serialize_xxl_paired_sample(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const AlignerInput& /*input*/,
    const AlignerOutput& output)
{
    const auto& proof = output.partial_vulkan_execution_proof;
    return nlohmann::json{
        {"payload_schema", "bioseq.xxl_hifi_paired.route_proof"},
        {"payload_schema_version", 2},
        {"total_pairs", output.total_pairs},
        {"total_results", output.total_results},
        {"correct_pairs", output.correct_pairs},
        {"selected_route_execution_proof_accepted",
            output.selected_route_execution_proof_accepted},
        {"historical_partial_gpu_probe",
            output.historical_partial_gpu_probe},
        {"all_gpu_acceptance_eligible",
            output.all_gpu_acceptance_eligible},
        {"phase_c_gpu_index_ownership_proof_accepted",
            output.phase_c_gpu_index_ownership_proof_accepted},
        {"exact_simd_query_execution_proof", two_stack_proof_json(
            output.exact_simd_query_execution_proof,
            output.exact_simd_query_execution_proof_present)},
        {"exact_simd_target_execution_proof", two_stack_proof_json(
            output.exact_simd_target_execution_proof,
            output.exact_simd_target_execution_proof_present)},
        {"paper_nthash_query_execution_proof", paper_nthash_proof_json(
            output.paper_nthash_query_execution_proof,
            output.paper_nthash_query_execution_proof_present)},
        {"paper_nthash_target_execution_proof", paper_nthash_proof_json(
            output.paper_nthash_target_execution_proof,
            output.paper_nthash_target_execution_proof_present)},
        {"simd_chaining_execution_proof", simd_chaining_proof_json(
            output.simd_chaining_execution_proof,
            output.simd_chaining_execution_proof_present)},
        {"block_aligner_execution_proof", block_aligner_proof_json(
            output.block_aligner_execution_proof,
            output.block_aligner_execution_proof_present)},
        {"partial_vulkan_execution_proof", {
            {"query_sketch_dispatches", proof.query_sketch_dispatches},
            {"target_sketch_dispatches", proof.target_sketch_dispatches},
            {"target_index_build_dispatches",
                proof.target_index_build_dispatches},
            {"seed_lookup_dispatches", proof.seed_lookup_dispatches},
            {"seed_open_addressed_lookup_dispatches",
                proof.seed_open_addressed_lookup_dispatches},
            {"seed_sorted_dense_lookup_dispatches",
                proof.seed_sorted_dense_lookup_dispatches},
            {"chain_score_dispatches", proof.chain_score_dispatches},
            {"seed_bitonic_sort_dispatches",
                proof.seed_bitonic_sort_dispatches},
            {"seed_radix_sort_operations", proof.seed_radix_sort_operations},
            {"seed_radix_anchor_count", proof.seed_radix_anchor_count},
            {"seed_radix_passes", proof.seed_radix_passes},
            {"seed_radix_histogram_dispatches",
                proof.seed_radix_histogram_dispatches},
            {"seed_radix_scan_dispatches",
                proof.seed_radix_scan_dispatches},
            {"seed_radix_scatter_dispatches",
                proof.seed_radix_scatter_dispatches},
            {"seed_host_sort_fallbacks", proof.seed_host_sort_fallbacks},
            {"gpu_index_materializations", proof.gpu_index_materializations},
            {"gpu_index_open_addressed_materializations",
                proof.gpu_index_open_addressed_materializations},
            {"gpu_index_sorted_dense_materializations",
                proof.gpu_index_sorted_dense_materializations},
            {"gpu_index_unique_keys", proof.gpu_index_unique_keys},
            {"gpu_index_slot_capacity", proof.gpu_index_slot_capacity},
            {"gpu_index_logical_resident_bytes",
                proof.gpu_index_logical_resident_bytes},
            {"gpu_index_actual_allocation_bytes",
                proof.gpu_index_actual_allocation_bytes},
            {"gpu_index_fully_touched_bytes",
                proof.gpu_index_fully_touched_bytes},
            {"gpu_index_staging_bytes", proof.gpu_index_staging_bytes},
            {"gpu_index_permit_releases", proof.gpu_index_permit_releases},
            {"gpu_index_process_footprint_reflections",
                proof.gpu_index_process_footprint_reflections},
            {"gpu_index_reflected_footprint_bytes",
                proof.gpu_index_reflected_footprint_bytes},
            {"gpu_index_max_load_ppm", proof.gpu_index_max_load_ppm},
            {"gpu_index_max_probe", proof.gpu_index_max_probe},
            {"gpu_index_count_phase_admissions",
                proof.gpu_index_count_phase_admissions},
            {"gpu_index_main_peak_admissions",
                proof.gpu_index_main_peak_admissions},
            {"gpu_index_resident_transfers",
                proof.gpu_index_resident_transfers},
            {"gpu_index_adoption_rereserve_attempts",
                proof.gpu_index_adoption_rereserve_attempts},
            {"gpu_index_adoption_successes",
                proof.gpu_index_adoption_successes},
            {"gpu_index_owned_resident_bytes",
                proof.gpu_index_owned_resident_bytes},
            {"gpu_index_gate_enabled", proof.gpu_index_gate_enabled},
            {"gpu_index_gate_word_count", proof.gpu_index_gate_word_count},
            {"gpu_index_gate_mask", proof.gpu_index_gate_mask},
            {"gpu_index_gate_set_bits", proof.gpu_index_gate_set_bits},
            {"gpu_index_gate_digest_lo", proof.gpu_index_gate_digest_lo},
            {"gpu_index_gate_digest_hi", proof.gpu_index_gate_digest_hi},
            {"gpu_index_gate_count_dispatches",
                proof.gpu_index_gate_count_dispatches},
            {"gpu_index_gated_emit_dispatches",
                proof.gpu_index_gated_emit_dispatches},
            {"gpu_index_ungated_emit_dispatches",
                proof.gpu_index_ungated_emit_dispatches},
            {"gpu_index_retained_minimizer_count",
                proof.gpu_index_retained_minimizer_count},
            {"gpu_index_distinct_minimizer_count",
                proof.gpu_index_distinct_minimizer_count},
            {"post_seed_owned_gpu_cache_evictions",
                proof.post_seed_owned_gpu_cache_evictions},
            {"post_seed_owned_gpu_resident_bytes_released",
                proof.post_seed_owned_gpu_resident_bytes_released},
            {"post_seed_owned_cpu_index_releases",
                proof.post_seed_owned_cpu_index_releases},
            {"post_seed_owned_cpu_position_bytes_released",
                proof.post_seed_owned_cpu_position_bytes_released},
            {"saturated", proof.saturated},
        }},
    }.dump();
}

}  // namespace

HWY_AFTER_NAMESPACE();

int main(int argc, char** argv) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::INF);

    const std::string config_path{BIOSEQ_BENCH_CONFIG_PATH};
    const std::string report_dir{BIOSEQ_BENCH_REPORT_DIR};
    const std::string data_dir{BIOSEQ_BENCH_DATA_DIR};

    using bioseq::bench_framework::ParamMap;
    using bioseq::bench_framework::RunConstraints;
    using bioseq::bench_framework::create_benchmark;

    // Mandatory `std::function<...>` casts (README §3e) — `create_benchmark`'s
    // template-arg deduction won't bind a lambda or function pointer to a
    // `std::function` parameter without help. Reused across the six blocks
    // below since all six share the same I/O types and lambdas.
    auto estimate = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_aligner_resources};
    auto build = std::function<AlignerInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>{build_input};
    auto run = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{run_aligner};
    auto run_strict_xxl = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{
            run_aligner_strict_xxl};
    auto run_strict_xxl_paired = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{
            run_aligner_strict_xxl_paired};
    auto eval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const AlignerInput&, const AlignerOutput&)>{evaluate_aligner};
    auto serialize_paired = std::function<std::string(
        std::string_view, const ParamMap&,
        const AlignerInput&, const AlignerOutput&)>{
            serialize_xxl_paired_sample};
    auto estimate_grouped32 = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_grouped32_validation_resources};
    auto build_grouped32 = std::function<Grouped32ValidationInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>{build_grouped32_validation_input};
    auto run_grouped32 = std::function<Grouped32ValidationResult(
        std::string_view, const ParamMap&,
        const Grouped32ValidationInput&)>{run_grouped32_validation};
    auto evaluate_grouped32 = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const Grouped32ValidationInput&,
        const Grouped32ValidationResult&)>{evaluate_grouped32_validation};

    return bioseq::bench_framework::run(
        argc, argv, report_dir, data_dir,
        create_benchmark<
            Grouped32ValidationInput,
            Grouped32ValidationResult>(
                "Grouped32 contiguous local32 validation",
                "bench_grouped32_local32_validation",
                config_path,
                estimate_grouped32,
                build_grouped32,
                run_grouped32,
                evaluate_grouped32),
        create_benchmark<AlignerInput, AlignerOutput>(
            "Aligner full grid (length × error × algorithm)",
            "bench_aligners_grid", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "MinimizerRepr storage exercise (K=7 / K=27)",
            "bench_aligners_repr_storage", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "RMQ chaining LONG/CCS regression repro",
            "bench_aligners_rmq_repro", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "Curated correctness probes",
            "bench_aligners_curated", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI report_76 relative-performance gate",
            "bench_aligners_xxl_hifi_relative",
            config_path, estimate, build, run_strict_xxl, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI bounded paper-ntHash SIMD proof and speed gate",
            "bench_paper_nthash_xxl_hifi_probe",
            config_path, estimate, build, run_strict_xxl_paired, eval,
            {}, serialize_paired),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI paired relative-performance companion",
            "bench_aligners_xxl_hifi_paired",
            config_path, estimate, build, run_strict_xxl_paired, eval,
            {}, serialize_paired));
}
