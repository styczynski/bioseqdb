/**
 * @file minimizers_vulkan.cppm
 * @brief DOC0351 Phase C — Vulkan minimizer-sketch backend for
 *        `compact_sequence_range` / `lazy_compact_sequence`.
 *
 * Owns the `MinimizerRepr<L,K>` wire traits, the sequence upload view (a direct
 * window onto bioseq's packed storage units — no G3SA ASCII transcode), the
 * rank LUT builder (dna4 identity / dna5 nt4 / generic complement), and the two
 * sketch entry points:
 *   - `collect_minimizers_vulkan_result(...)` owns memory admission and returns
 *     both the host `std::vector<MinimizerT>` and the resident device buffer for
 *     seed-lookup reuse.
 *   - `collect_minimizers_vulkan(...)` is the collector-compatible wrapper for
 *     existing call sites; null optional results mean the GPU shape was
 *     ineligible, while runtime failures propagate as `VulkanRuntimeError`.
 *
 * Every C++ entry point is alphabet-generic; only the shader specialises
 * (DOC0307 §0, §9). `params.vulkan_enabled == false` is handled by the caller
 * (pipe trait) before this module is ever reached.
 */
module;
#include <cstdio>

#include "common.h"
#include <hwy/highway.h>  // ONLY for the HWY_NAMESPACE macro (nt4_normalize lives there)

#include <cstring>
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>        // DOC0345 Patch C — std::pair / std::declval for segment pair path
#include <vector>

export module bioseq.sequences.kmers.vulkan;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.base;
import bioseq.gpu.vulkan.phase_b_abi;
import bioseq.gpu.vulkan.phase_c_abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;  // DOC0345 Patch C — bioseq::storage::sequence_size_t
import bioseq.instr;  // DOC0310 §21 — phase_c_* timing labels

export namespace bioseq::gpu::vulkan {

using VulkanSketchQueryGateIdentity = GpuBuiltIndexGateIdentity;

[[nodiscard]] inline VulkanSketchQueryGateIdentity
canonical_vulkan_sketch_query_gate_identity(
    std::span<const std::uint32_t> words,
    std::uint32_t mask,
    std::shared_ptr<const std::vector<std::uint32_t>> canonical_words = {})
{
    if (words.empty()) return {};
    if (canonical_words) {
        if (canonical_words->size() != words.size()
            || !std::equal(
                words.begin(), words.end(), canonical_words->begin())) {
            throw VulkanRuntimeError(
                "query gate immutable owner does not match its word span");
        }
    } else {
        canonical_words =
            std::make_shared<const std::vector<std::uint32_t>>(
                words.begin(), words.end());
    }
    std::uint64_t lo = 1469598103934665603ULL;
    std::uint64_t hi = 0x9e3779b97f4a7c15ULL;
    for (const std::uint32_t word : words) {
        for (unsigned shift = 0u; shift < 32u; shift += 8u) {
            const std::uint8_t byte =
                static_cast<std::uint8_t>(word >> shift);
            lo = (lo ^ byte) * 1099511628211ULL;
            hi ^= static_cast<std::uint64_t>(byte)
                + 0x9e3779b97f4a7c15ULL + (hi << 6u) + (hi >> 2u);
            hi *= 0xbf58476d1ce4e5b9ULL;
        }
    }
    return VulkanSketchQueryGateIdentity{
        .enabled = true,
        .word_count = static_cast<std::uint32_t>(words.size()),
        .mask = mask,
        .digest_lo = lo,
        .digest_hi = hi,
        .canonical_words = std::move(canonical_words),
    };
}

struct VulkanSketchQueryGate {
    std::span<const std::uint32_t> words{};
    std::uint32_t mask = 0u;
    VulkanSketchQueryGateIdentity identity{};
    bool identity_valid = false;

    [[nodiscard]] bool enabled() const noexcept { return !words.empty(); }
    [[nodiscard]] VulkanSketchQueryGateIdentity canonical_identity() const {
        if (!enabled()) return {};
        const bool same_frozen_storage = identity.canonical_words
            && words.data() == identity.canonical_words->data()
            && words.size() == identity.canonical_words->size();
        if (identity_valid && identity.enabled
            && identity.word_count == words.size() && identity.mask == mask
            && identity.well_formed()
            // The pointer fast path is valid only for the immutable owner
            // carried in the identity; raw/mutable spans still exact-compare.
            && (same_frozen_storage
                || std::equal(
                    words.begin(), words.end(),
                    identity.canonical_words->begin()))) {
            return identity;
        }
        if (identity_valid) {
            throw VulkanRuntimeError(
                "query gate supplied identity does not match exact words");
        }
        return canonical_vulkan_sketch_query_gate_identity(words, mask);
    }
};

/// Fail-closed diagnostic for a selected Vulkan minimizer route.  Optional
/// sketch results remain the compatibility surface for probe callers, while
/// strict pipeline callers request this record and include it in the thrown
/// `VulkanRuntimeError`.  This is deliberately data, not a fallback policy.
enum class VulkanSketchDeclineReason : std::uint8_t {
    none,
    empty_sequence,
    segment_window_too_short,
    segment_position_field_overflow,
    target_index_invalid_shape,
    device_allocation_failed,
    target_index_build_invariant_failed,
    fragmented_sequence_unsupported,
    sequence_type_unsupported,
    alphabet_unsupported,
    kmer_size_unsupported,
    window_size_unsupported,
    sequence_size_exceeds_u32,
    padded_sequence_size_overflow,
    hardware_device_unavailable,
    minimizer_capacity_exceeds_u32,
    query_gate_exceeds_u32,
    query_gate_requires_single_window,
    memory_plan_invalid,
    memory_admission_denied,
    output_capacity_overflow,
};

struct VulkanSketchDecline {
    VulkanSketchDeclineReason reason = VulkanSketchDeclineReason::none;
    std::uint64_t bp_size = 0;
    std::uint64_t padded_sequence_bytes = 0;
    std::uint64_t max_minimizers = 0;
    std::uint64_t query_gate_words = 0;
    std::uint64_t window_size = 0;
    std::uint64_t kmer_size = 0;
    bioseq::parallelism::MemoryAdmissionFailure admission{};

    [[nodiscard]] constexpr bool declined() const noexcept {
        return reason != VulkanSketchDeclineReason::none;
    }
};

/// Read a sequence length for diagnostics without strengthening the public
/// probe templates: unsupported sequence shapes must still instantiate and
/// return a typed `sequence_type_unsupported` decline.
template <class Seq>
[[nodiscard]] inline std::uint64_t vulkan_decline_bp_size(
    const Seq& seq) noexcept
{
    if constexpr (requires { seq.bp_size(); }) {
        return static_cast<std::uint64_t>(seq.bp_size());
    } else {
        return 0u;
    }
}

[[nodiscard]] inline constexpr std::string_view
vulkan_sketch_decline_reason_name(VulkanSketchDeclineReason reason) noexcept
{
    switch (reason) {
        case VulkanSketchDeclineReason::none:
            return "none";
        case VulkanSketchDeclineReason::empty_sequence:
            return "empty_sequence";
        case VulkanSketchDeclineReason::segment_window_too_short:
            return "segment_window_too_short";
        case VulkanSketchDeclineReason::segment_position_field_overflow:
            return "segment_position_field_overflow";
        case VulkanSketchDeclineReason::target_index_invalid_shape:
            return "target_index_invalid_shape";
        case VulkanSketchDeclineReason::device_allocation_failed:
            return "device_allocation_failed";
        case VulkanSketchDeclineReason::target_index_build_invariant_failed:
            return "target_index_build_invariant_failed";
        case VulkanSketchDeclineReason::fragmented_sequence_unsupported:
            return "fragmented_sequence_unsupported";
        case VulkanSketchDeclineReason::sequence_type_unsupported:
            return "sequence_type_unsupported";
        case VulkanSketchDeclineReason::alphabet_unsupported:
            return "alphabet_unsupported";
        case VulkanSketchDeclineReason::kmer_size_unsupported:
            return "kmer_size_unsupported";
        case VulkanSketchDeclineReason::window_size_unsupported:
            return "window_size_unsupported";
        case VulkanSketchDeclineReason::sequence_size_exceeds_u32:
            return "sequence_size_exceeds_u32";
        case VulkanSketchDeclineReason::padded_sequence_size_overflow:
            return "padded_sequence_size_overflow";
        case VulkanSketchDeclineReason::hardware_device_unavailable:
            return "hardware_device_unavailable";
        case VulkanSketchDeclineReason::minimizer_capacity_exceeds_u32:
            return "minimizer_capacity_exceeds_u32";
        case VulkanSketchDeclineReason::query_gate_exceeds_u32:
            return "query_gate_exceeds_u32";
        case VulkanSketchDeclineReason::query_gate_requires_single_window:
            return "query_gate_requires_single_window";
        case VulkanSketchDeclineReason::memory_plan_invalid:
            return "memory_plan_invalid";
        case VulkanSketchDeclineReason::memory_admission_denied:
            return "memory_admission_denied";
        case VulkanSketchDeclineReason::output_capacity_overflow:
            return "output_capacity_overflow";
    }
    return "unknown";
}

[[nodiscard]] inline std::string format_vulkan_sketch_decline(
    const VulkanSketchDecline& decline)
{
    const std::string_view reason =
        vulkan_sketch_decline_reason_name(decline.reason);
    if (decline.reason == VulkanSketchDeclineReason::memory_admission_denied) {
        const auto& admission = decline.admission;
        return fmt::format(
            "reason={} admission_reason={} required_bytes={} "
            "allocatable_after_reservations_bytes={} reserved_bytes={} "
            "budget_ceiling_bytes={} process_footprint_bytes={} "
            "system_available_bytes={} system_total_bytes={} "
            "process_soft_cap_bytes={} bp={} padded_bytes={} max_min={} "
            "query_gate_words={} w={} k={}",
            reason,
            bioseq::parallelism::memory_admission_failure_reason_name(
                admission.reason),
            admission.requested_bytes,
            admission.allocatable_after_reservations_bytes,
            admission.reserved_bytes,
            admission.budget_ceiling_bytes,
            admission.process_footprint_bytes,
            admission.system_available_bytes,
            admission.system_total_bytes,
            admission.process_soft_cap_bytes,
            decline.bp_size,
            decline.padded_sequence_bytes,
            decline.max_minimizers,
            decline.query_gate_words,
            decline.window_size,
            decline.kmer_size);
    }
    return fmt::format(
        "reason={} bp={} padded_bytes={} max_min={} query_gate_words={} w={} k={}",
        reason,
        decline.bp_size,
        decline.padded_sequence_bytes,
        decline.max_minimizers,
        decline.query_gate_words,
        decline.window_size,
        decline.kmer_size);
}

/// Return true only when reducing a fragmented upload can remove the exact
/// predicate that declined the current dispatch.  Shape/device/query-gate
/// failures are invariant under a target split and must fail immediately;
/// retrying them would both waste I/O and obscure the original cause.
[[nodiscard]] inline constexpr bool
vulkan_sketch_decline_allows_segment_split(
    VulkanSketchDeclineReason reason) noexcept
{
    switch (reason) {
        case VulkanSketchDeclineReason::sequence_size_exceeds_u32:
        case VulkanSketchDeclineReason::padded_sequence_size_overflow:
        case VulkanSketchDeclineReason::minimizer_capacity_exceeds_u32:
        case VulkanSketchDeclineReason::memory_plan_invalid:
        case VulkanSketchDeclineReason::memory_admission_denied:
        case VulkanSketchDeclineReason::output_capacity_overflow:
            return true;
        case VulkanSketchDeclineReason::none:
        case VulkanSketchDeclineReason::empty_sequence:
        case VulkanSketchDeclineReason::segment_window_too_short:
        case VulkanSketchDeclineReason::segment_position_field_overflow:
        case VulkanSketchDeclineReason::target_index_invalid_shape:
        case VulkanSketchDeclineReason::device_allocation_failed:
        case VulkanSketchDeclineReason::target_index_build_invariant_failed:
        case VulkanSketchDeclineReason::fragmented_sequence_unsupported:
        case VulkanSketchDeclineReason::sequence_type_unsupported:
        case VulkanSketchDeclineReason::alphabet_unsupported:
        case VulkanSketchDeclineReason::kmer_size_unsupported:
        case VulkanSketchDeclineReason::window_size_unsupported:
        case VulkanSketchDeclineReason::hardware_device_unavailable:
        case VulkanSketchDeclineReason::query_gate_exceeds_u32:
        case VulkanSketchDeclineReason::query_gate_requires_single_window:
            return false;
    }
    return false;
}

/// Translate the runtime's cycle-free target-index result status into the
/// shared strict accelerator decline vocabulary consumed by pipeline callers.
[[nodiscard]] inline constexpr VulkanSketchDeclineReason
vulkan_target_index_failure_decline_reason(
    GpuBuiltIndexFailureReason reason) noexcept
{
    switch (reason) {
        case GpuBuiltIndexFailureReason::none:
            return VulkanSketchDeclineReason::
                target_index_build_invariant_failed;
        case GpuBuiltIndexFailureReason::invalid_shape:
            return VulkanSketchDeclineReason::target_index_invalid_shape;
        case GpuBuiltIndexFailureReason::minimizer_capacity_exceeds_u32:
            return VulkanSketchDeclineReason::minimizer_capacity_exceeds_u32;
        case GpuBuiltIndexFailureReason::memory_plan_invalid:
            return VulkanSketchDeclineReason::memory_plan_invalid;
        case GpuBuiltIndexFailureReason::device_arena_allocation_failed:
        case GpuBuiltIndexFailureReason::device_buffer_allocation_failed:
            return VulkanSketchDeclineReason::device_allocation_failed;
        case GpuBuiltIndexFailureReason::count_peak_memory_admission_denied:
        case GpuBuiltIndexFailureReason::main_peak_memory_admission_denied:
            return VulkanSketchDeclineReason::memory_admission_denied;
        case GpuBuiltIndexFailureReason::peak_allocation_exceeds_admission:
            return VulkanSketchDeclineReason::memory_plan_invalid;
        case GpuBuiltIndexFailureReason::output_capacity_overflow:
            return VulkanSketchDeclineReason::output_capacity_overflow;
        case GpuBuiltIndexFailureReason::index_invariant_failed:
            return VulkanSketchDeclineReason::
                target_index_build_invariant_failed;
    }
    return VulkanSketchDeclineReason::target_index_build_invariant_failed;
}

/// Raise the shared fail-closed selected-Vulkan error while preserving the
/// machine-readable decline identity and its admission-time memory snapshot.
[[noreturn]] inline void throw_vulkan_strict_decline(
    std::string_view stage,
    const VulkanSketchDecline& decline)
{
    throw VulkanRuntimeError(fmt::format(
        "{} selected but strict Vulkan execution was rejected ({})",
        stage,
        format_vulkan_sketch_decline(decline)));
}

// ===========================================================================
// MinimizerRepr wire traits (DOC0307 §4.3)
// ===========================================================================

template <class M, class = void>
struct minimizer_wire_traits;

/// 16/32/64-bit single-integer `MinimizerRepr` specialisation.
template <class M>
    requires requires (M m) {
        typename M::storage_t;
        { m.raw_value() };
    }
struct minimizer_wire_traits<M> {
    using minimizer_type = M;
    using storage_t = typename M::storage_t;
    static constexpr bool is_128 = false;
    static constexpr std::uint32_t storage_bits =
        static_cast<std::uint32_t>(sizeof(storage_t) * 8u);
    static constexpr std::uint32_t storage_bytes =
        static_cast<std::uint32_t>(sizeof(storage_t));
    static constexpr bool packed_u16 = storage_bits == 16u;
    static constexpr std::uint32_t words32_per_minimizer =
        storage_bits == 16u ? 0u :
        storage_bits == 32u ? 1u : 2u;
    static constexpr std::uint32_t pos_bits = M::layout_t::pos_bits;
    static constexpr std::uint32_t hash_bits = M::layout_t::hash_bits;
    static constexpr std::uint32_t hash_shift = M::hash_shift;
    static constexpr std::uint64_t pos_mask =
        static_cast<std::uint64_t>(M::pos_field_mask);

    static constexpr MinimizerWordKind word_kind =
        storage_bits == 16u ? MinimizerWordKind::u16 :
        storage_bits == 32u ? MinimizerWordKind::u32 :
                              MinimizerWordKind::u64;

    [[nodiscard]] static constexpr std::uint64_t device_words32_for_count(
        std::uint64_t n) noexcept
    {
        return storage_bits == 16u
            ? (bioseq::gpu::vulkan::sat_add(n, 1u) / 2u)
            : bioseq::parallelism::saturating_mul(n, words32_per_minimizer);
    }

    [[nodiscard]] static constexpr std::uint64_t device_bytes_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::parallelism::saturating_mul(
            device_words32_for_count(n), sizeof(std::uint32_t));
    }
};

/// 128-bit (`u64x2`) `MinimizerRepr` fallback specialisation.
template <class M>
    requires requires (M m) {
        { m.data_low_64() } -> std::convertible_to<std::uint64_t>;
        { m.data_high_64() } -> std::convertible_to<std::uint64_t>;
    }
struct minimizer_wire_traits<M> {
    using minimizer_type = M;
    static constexpr bool is_128 = true;
    static constexpr std::uint32_t storage_bits = 128u;
    static constexpr std::uint32_t storage_bytes = 16u;
    static constexpr bool packed_u16 = false;
    static constexpr std::uint32_t words32_per_minimizer = 4u;
    static constexpr std::uint32_t pos_bits = M::layout_t::pos_bits;
    // The 128-bit layout packs `high_ = (kmer << 8) | span` into a 64-bit word,
    // so only the low 56 bits of the kmer survive the span shift. The emit
    // shader masks/compares with this effective width so its window-minimum
    // selection matches `MinimizerRepr<L,K>::operator<=>` (which compares the
    // stored, truncated kmer), not the wider pre-truncation `layout_t::hash_bits`.
    static constexpr std::uint32_t hash_bits =
        M::layout_t::hash_bits < 56u ? static_cast<std::uint32_t>(M::layout_t::hash_bits) : 56u;
    static constexpr std::uint32_t hash_shift = 8u;
    static constexpr std::uint64_t pos_mask = M::pos_field_mask;
    static constexpr MinimizerWordKind word_kind = MinimizerWordKind::u64x2;

    [[nodiscard]] static constexpr std::uint64_t device_words32_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::parallelism::saturating_mul(n, words32_per_minimizer);
    }

    [[nodiscard]] static constexpr std::uint64_t device_bytes_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::parallelism::saturating_mul(
            device_words32_for_count(n), sizeof(std::uint32_t));
    }
};

// ===========================================================================
// Alphabet kernel selection + rank LUT (DOC0307 §6.1, §9)
// ===========================================================================

/// Pick the lowest-level shader specialisation key from the sequence alphabet.
template <class Seq>
[[nodiscard]] constexpr AlphabetKernelKind choose_alphabet_kernel() {
    using traits = typename std::remove_cvref_t<Seq>::traits;
    if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
        if constexpr (traits::bits_per_char == 2 && traits::alphabet_size == 4) {
            return AlphabetKernelKind::dna4_nt4;
        }
        if constexpr (traits::bits_per_char == 3 && traits::alphabet_size == 5) {
            return AlphabetKernelKind::dna5_nt4;
        }
    }
    return AlphabetKernelKind::generic;
}

/// DOC0345 Patch C — same selection as `choose_alphabet_kernel<Seq>()` but
/// parameterised directly on the storage traits (used by the segment upload
/// helpers, which already hold `traits`).
template <class Traits>
[[nodiscard]] constexpr AlphabetKernelKind alphabet_kernel_kind_of_traits() noexcept {
    if constexpr (seqan3::nucleotide_alphabet<typename Traits::alphabet_type>) {
        if constexpr (Traits::bits_per_char == 2 && Traits::alphabet_size == 4) {
            return AlphabetKernelKind::dna4_nt4;
        }
        if constexpr (Traits::bits_per_char == 3 && Traits::alphabet_size == 5) {
            return AlphabetKernelKind::dna5_nt4;
        }
    }
    return AlphabetKernelKind::generic;
}

template <class Traits>
[[nodiscard]] constexpr bool vulkan_minimizer_alphabet_shape_supported() noexcept {
    return Traits::alphabet_size <= 256 && Traits::bits_per_char != 0
        && Traits::bits_per_char <= 32;
}

template <class Traits>
[[nodiscard]] constexpr bool vulkan_minimizer_kmer_size_supported(
    std::size_t kmer_size) noexcept
{
    if constexpr (!vulkan_minimizer_alphabet_shape_supported<Traits>()) {
        return false;
    } else {
        constexpr std::size_t step_bits =
            alphabet_kernel_kind_of_traits<Traits>() == AlphabetKernelKind::generic
                ? Traits::bits_per_char
                : std::size_t{2};
        return kmer_size != 0 && kmer_size <= 64 / step_bits && kmer_size <= 32;
    }
}

[[nodiscard]] constexpr bool vulkan_minimizer_window_size_supported(
    std::size_t window_size) noexcept
{
    return window_size != 0 && window_size <= 256;
}

template <class Traits>
[[nodiscard]] constexpr bool vulkan_phase_c_minimizer_sketch_supported() noexcept
{
    return vulkan_minimizer_alphabet_shape_supported<Traits>();
}

/// Build the rank → forward/rc nt4-code LUT from the existing CPU helpers. For
/// SeqAn dna5 this yields `{A,C,G,N,T} -> {0,1,2,4,3}` and resets on `N` (not
/// `T`) — mandatory DOC0286 parity.
template <class Seq>
[[nodiscard]] VulkanRankLut make_vulkan_rank_lut() {
    using traits = typename std::remove_cvref_t<Seq>::traits;
    using alphabet_t = typename traits::alphabet_type;
    VulkanRankLut lut{};
    for (std::uint32_t rank = 0; rank < traits::alphabet_size; ++rank) {
        std::uint32_t code = 4u;
        std::uint32_t rc_code = 4u;
        if constexpr (seqan3::nucleotide_alphabet<alphabet_t>) {
            code = bioseq::simd::HWY_NAMESPACE::nt4_normalize<alphabet_t>(
                static_cast<std::uint8_t>(rank));
            rc_code = code < 4u ? (code ^ 3u) : 4u;
        } else {
            const bool ambiguous = traits::bits_per_char > 2 && rank >= 4u;
            const std::uint32_t comp_rank = static_cast<std::uint32_t>(
                traits::letter_complement(static_cast<typename traits::storage_unit>(rank)));
            code = ambiguous ? 4u : rank;
            rc_code = (traits::bits_per_char > 2 && comp_rank >= 4u) ? 4u : comp_rank;
        }
        lut.rank_to_code[rank] = code;
        lut.rank_to_rc_code[rank] = rc_code;
    }
    return lut;
}

// ===========================================================================
// Sequence upload view (DOC0307 §4.3)
// ===========================================================================

/// A sequence that can be uploaded as a single contiguous packed block: it must
/// expose the storage traits, a single-argument `read_cptr`, and the
/// byte/bp/padding accessors. Fragmented lazy sequences are excluded by the
/// separate fragmentation guard; selected Vulkan segment callers use the
/// checked segment upload path below and throw on GPU declines.
template <class Seq>
concept vulkan_sketchable_sequence = requires (const Seq& seq) {
    typename std::remove_cvref_t<Seq>::traits;
    typename std::remove_cvref_t<Seq>::traits::storage_unit;
    typename std::remove_cvref_t<Seq>::traits::alphabet_type;
    { seq.cbegin() };
    { seq.byte_size() } -> std::convertible_to<std::uint64_t>;
    { seq.bp_size() } -> std::convertible_to<std::uint64_t>;
    { seq.padding() };
    { seq.read_cptr(seq.cbegin()) };
};

/// True when `Seq` is a fragmented `lazy_compact_sequence` whose single-arg
/// `read_cptr` returns only the current fragment (not a single contiguous run).
template <class Seq>
[[nodiscard]] constexpr bool is_fragmented_sequence() noexcept {
    if constexpr (requires { typename std::remove_cvref_t<Seq>::scanner_type; }) {
        return std::remove_cvref_t<Seq>::scanner_type::supports_fragments();
    } else {
        return false;
    }
}

/// DOC0345 Patch C — a sequence whose lazy fragmented storage can be stitched
/// into one contiguous window via the 3-argument `read_cptr(it, scratch, n)`
/// stitch. Uses the iterator expression `seq.cbegin()` (not a `Seq::iterator`
/// member) so production `lazy_compact_sequence` (which exposes `iterator_impl`)
/// satisfies it.
template <class Seq>
concept vulkan_sketchable_segment_sequence =
    requires (
        const Seq& seq,
        decltype(std::declval<const Seq&>().cbegin()) iter,
        std::span<typename std::remove_cvref_t<Seq>::traits::storage_unit> scratch,
        std::size_t count) {
        typename std::remove_cvref_t<Seq>::traits;
        typename std::remove_cvref_t<Seq>::traits::storage_unit;
        typename std::remove_cvref_t<Seq>::traits::alphabet_type;
        { seq.cbegin() };
        { seq.size() } -> std::convertible_to<std::uint64_t>;
        { seq.bp_size() } -> std::convertible_to<std::uint64_t>;
        { seq.padding() };
        { seq.read_cptr(iter, scratch, count) }
            -> std::convertible_to<
                const typename std::remove_cvref_t<Seq>::traits::storage_unit*>;
    };

struct SequenceUploadView {
    const void* bytes = nullptr;
    std::uint64_t byte_size = 0;
    std::uint64_t padded_byte_size = 0;
    std::uint64_t bp_size = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t char_mask = 0;
    std::uint32_t padding_letters = 0;
    AlphabetKernelKind alphabet_kind = AlphabetKernelKind::generic;
};

/// Round up to the next multiple of 4 (saturating at UINT64_MAX). The shader's
/// `uint seq_words[]` view requires a 4-byte aligned upload tail.
[[nodiscard]] inline std::uint64_t round_up4(std::uint64_t n) noexcept {
    const auto max = (std::numeric_limits<std::uint64_t>::max)();
    if (n > max - 3u) return max;
    return (n + 3u) & ~std::uint64_t{3u};
}

[[nodiscard]] constexpr std::uint64_t ceil_div_u64(
    std::uint64_t n, std::uint64_t d) noexcept
{
    return d == 0u ? (std::numeric_limits<std::uint64_t>::max)()
                   : n / d + (n % d != 0u ? 1u : 0u);
}

// DOC0345 Patch C — source-window descriptor (lo/hi in storage units, the bp
// span the window actually covers, and the tail padding when the window reaches
// the sequence end). Distinct from `SequenceUploadView`, which is the GPU upload
// view of an already-stitched contiguous block.
struct SegmentUploadDescriptor {
    std::size_t lo_unit = 0;
    std::size_t hi_unit = 0;
    std::uint64_t window_bp = 0;
    std::uint32_t tail_padding = 0;
};

struct PhaseCSegmentWindow {
    std::size_t sub_lo_unit = 0;
    std::size_t sub_hi_unit = 0;
    std::size_t core_lo_unit = 0;
    std::size_t core_hi_unit = 0;
    std::uint64_t emit_base_bp = 0;
    std::uint64_t collected_bp = 0;
};

template <class Traits>
[[nodiscard]] constexpr std::uint64_t phase_c_segment_context_bp(
    std::size_t window_size,
    std::size_t kmer_size) noexcept
{
    return static_cast<std::uint64_t>(window_size)
        + static_cast<std::uint64_t>(kmer_size)
        + 1u;
}

template <class Traits>
[[nodiscard]] constexpr std::size_t phase_c_segment_context_units(
    std::size_t window_size,
    std::size_t kmer_size) noexcept
{
    const std::uint64_t context_bp =
        phase_c_segment_context_bp<Traits>(window_size, kmer_size);
    const std::uint64_t lpu =
        std::max<std::uint64_t>(1u, Traits::letters_per_unit);
    return std::max<std::size_t>(
        1u,
        static_cast<std::size_t>(ceil_div_u64(context_bp, lpu)));
}

template <class Seq>
[[nodiscard]] SegmentUploadDescriptor compute_segment_descriptor(
    const Seq& seq,
    std::size_t lo_unit,
    std::size_t hi_unit) noexcept
{
    using traits = typename std::remove_cvref_t<Seq>::traits;
    constexpr std::size_t letters_per_unit = traits::letters_per_unit;
    const std::size_t size_units = static_cast<std::size_t>(seq.size());
    const std::size_t clamped_lo = std::min(lo_unit, size_units);
    const std::size_t clamped_hi = std::min(std::max(hi_unit, clamped_lo), size_units);
    const std::uint32_t tail_padding =
        clamped_hi == size_units
            ? static_cast<std::uint32_t>(seq.padding())
            : 0u;
    const std::uint64_t raw_bp =
        static_cast<std::uint64_t>(clamped_hi - clamped_lo)
        * static_cast<std::uint64_t>(letters_per_unit);
    const std::uint64_t window_bp =
        raw_bp >= tail_padding ? raw_bp - tail_padding : 0u;
    return SegmentUploadDescriptor{
        .lo_unit = clamped_lo,
        .hi_unit = clamped_hi,
        .window_bp = window_bp,
        .tail_padding = tail_padding,
    };
}

template <class Seq>
[[nodiscard]] PhaseCSegmentWindow make_phase_c_segment_window(
    const Seq& seq,
    const SegmentUploadDescriptor& descriptor,
    std::size_t core_lo_unit,
    std::size_t core_hi_unit,
    std::size_t context_units)
{
    using traits = typename std::remove_cvref_t<Seq>::traits;
    const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
    const std::size_t core_lo =
        std::min(std::max(core_lo_unit, descriptor.lo_unit), descriptor.hi_unit);
    const std::size_t core_hi =
        std::min(std::max(core_hi_unit, core_lo), descriptor.hi_unit);
    const std::size_t sub_lo =
        core_lo > descriptor.lo_unit + context_units
            ? core_lo - context_units
            : descriptor.lo_unit;
    const std::size_t sub_hi =
        std::min(core_hi + context_units, descriptor.hi_unit);
    const SegmentUploadDescriptor sub_descriptor =
        compute_segment_descriptor(seq, sub_lo, sub_hi);
    return PhaseCSegmentWindow{
        .sub_lo_unit = sub_lo,
        .sub_hi_unit = sub_hi,
        .core_lo_unit = core_lo,
        .core_hi_unit = core_hi,
        .emit_base_bp = static_cast<std::uint64_t>(sub_lo) * lpu,
        .collected_bp = sub_descriptor.window_bp,
    };
}

template <class Seq>
[[nodiscard]] bool split_phase_c_segment_window(
    const Seq& seq,
    const SegmentUploadDescriptor& descriptor,
    const PhaseCSegmentWindow& window,
    std::size_t context_units,
    PhaseCSegmentWindow& left,
    PhaseCSegmentWindow& right)
{
    if (window.core_hi_unit <= window.core_lo_unit + 1u) {
        return false;
    }
    const std::size_t core_mid =
        window.core_lo_unit + (window.core_hi_unit - window.core_lo_unit) / 2u;
    if (core_mid == window.core_lo_unit || core_mid == window.core_hi_unit) {
        return false;
    }
    left = make_phase_c_segment_window(
        seq,
        descriptor,
        window.core_lo_unit,
        core_mid,
        context_units);
    right = make_phase_c_segment_window(
        seq,
        descriptor,
        core_mid,
        window.core_hi_unit,
        context_units);
    return left.core_lo_unit < left.core_hi_unit
        && right.core_lo_unit < right.core_hi_unit;
}

template <class MinimizerT, class Traits>
[[nodiscard]] std::uint64_t phase_c_admitted_collected_limit_bp(
    std::size_t window_size,
    std::size_t kmer_size,
    PhaseCSketchUse use) noexcept
{
    using storage_unit = typename Traits::storage_unit;
    constexpr std::uint64_t collected_floor_bp = 1'000'000u;
    // DOC0353 continuation: match the moderate Vulkan segment collected cap.
    // 64M stays below the previously failed 100M/128M shape while giving the
    // grid-stride sketch enough room to avoid splitting the new 51M-core target
    // segments under normal memory admission.
    constexpr std::uint64_t collected_ceil_bp = 96'000'000u;
    const std::uint64_t available =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .allocatable_after_reservations();
    if (available == (std::numeric_limits<std::uint64_t>::max)()) {
        return collected_ceil_bp;
    }

    const std::uint64_t lpu =
        std::max<std::uint64_t>(1u, Traits::letters_per_unit);
    std::uint64_t candidate = collected_ceil_bp;
    while (candidate > collected_floor_bp) {
        const std::uint64_t capacity = std::max<std::uint64_t>(64u, candidate);
        const std::uint64_t unit_count = ceil_div_u64(candidate, lpu);
        const std::uint64_t sequence_bytes = round_up4(
            bioseq::parallelism::saturating_mul(
                unit_count,
                static_cast<std::uint64_t>(sizeof(storage_unit))));
        const std::uint64_t output_bytes =
            use == PhaseCSketchUse::minimizer_words
                ? minimizer_wire_traits<MinimizerT>::device_bytes_for_count(capacity)
                : 0u;
        const PhaseCSketchMemoryPlan plan = phase_c_sketch_memory_plan(
            candidate,
            sequence_bytes,
            sizeof(VulkanRankLut),
            capacity,
            output_bytes,
            use,
            static_cast<std::uint32_t>(kmer_size),
            static_cast<std::uint32_t>(window_size),
            static_cast<std::uint32_t>(lpu));
        if (plan.valid && plan.admission_bytes <= available / 2u) {
            return candidate;
        }
        candidate /= 2u;
    }
    return collected_floor_bp;
}

template <class MinimizerT, class Seq>
[[nodiscard]] std::vector<PhaseCSegmentWindow> phase_c_segment_windows(
    const Seq& seq,
    const SegmentUploadDescriptor& descriptor,
    std::size_t window_size,
    std::size_t kmer_size,
    PhaseCSketchUse use,
    std::uint64_t collected_limit_override = 0u)
{
    using traits = typename std::remove_cvref_t<Seq>::traits;
    const std::uint64_t collected_limit =
        collected_limit_override == 0u
            ? phase_c_admitted_collected_limit_bp<MinimizerT, traits>(
                window_size, kmer_size, use)
            : collected_limit_override;
    const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
    const std::uint64_t context_bp =
        phase_c_segment_context_bp<traits>(window_size, kmer_size);
    constexpr std::uint64_t tile_safety_bp = 4096u;
    const std::uint64_t core_bp =
        collected_limit > context_bp * 2u + tile_safety_bp + 1u
            ? collected_limit - context_bp * 2u - tile_safety_bp
            : std::max<std::uint64_t>(1u, collected_limit / 2u);
    const std::size_t core_units =
        std::max<std::size_t>(1u, static_cast<std::size_t>(core_bp / lpu));
    const std::size_t context_units =
        phase_c_segment_context_units<traits>(window_size, kmer_size);

    std::vector<PhaseCSegmentWindow> windows;
    if (descriptor.window_bp <= collected_limit) {
        windows.push_back(PhaseCSegmentWindow{
            .sub_lo_unit = descriptor.lo_unit,
            .sub_hi_unit = descriptor.hi_unit,
            .core_lo_unit = descriptor.lo_unit,
            .core_hi_unit = descriptor.hi_unit,
            .emit_base_bp = static_cast<std::uint64_t>(descriptor.lo_unit) * lpu,
            .collected_bp = descriptor.window_bp,
        });
        return windows;
    }

    for (std::size_t core_lo = descriptor.lo_unit;
         core_lo < descriptor.hi_unit;
         core_lo += core_units) {
        const std::size_t core_hi =
            std::min(core_lo + core_units, descriptor.hi_unit);
        windows.push_back(make_phase_c_segment_window(
            seq,
            descriptor,
            core_lo,
            core_hi,
            context_units));
        if (core_hi == descriptor.hi_unit) {
            break;
        }
    }
    return windows;
}

template <class Seq, class StorageUnit>
[[nodiscard]] inline SequenceUploadView make_upload_view_from_window(
    const Seq&,
    const StorageUnit* base,
    const SegmentUploadDescriptor& descriptor)
{
    using traits = typename std::remove_cvref_t<Seq>::traits;
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(descriptor.hi_unit - descriptor.lo_unit)
        * static_cast<std::uint64_t>(sizeof(StorageUnit));
    return SequenceUploadView{
        .bytes = byte_size == 0u ? nullptr : static_cast<const void*>(base),
        .byte_size = byte_size,
        .padded_byte_size = round_up4(byte_size),
        .bp_size = descriptor.window_bp,
        .unit_bits = static_cast<std::uint32_t>(sizeof(StorageUnit) * 8u),
        .bits_per_char = static_cast<std::uint32_t>(traits::bits_per_char),
        .letters_per_unit = static_cast<std::uint32_t>(traits::letters_per_unit),
        .alphabet_size = static_cast<std::uint32_t>(traits::alphabet_size),
        .char_mask = static_cast<std::uint32_t>(traits::char_mask),
        .padding_letters = descriptor.tail_padding,
        .alphabet_kind = alphabet_kernel_kind_of_traits<traits>(),
    };
}

template <vulkan_sketchable_sequence Seq>
[[nodiscard]] inline SequenceUploadView make_sequence_upload_view(const Seq& seq) {
    using traits = typename std::remove_cvref_t<Seq>::traits;
    using storage_unit = typename traits::storage_unit;
    auto begin = seq.cbegin();
    const storage_unit* ptr =
        seq.byte_size() == 0 ? nullptr : seq.read_cptr(begin);
    const SegmentUploadDescriptor descriptor{
        .lo_unit = 0,
        .hi_unit = static_cast<std::size_t>(seq.size()),
        .window_bp = static_cast<std::uint64_t>(seq.bp_size()),
        .tail_padding = static_cast<std::uint32_t>(seq.padding()),
    };
    return make_upload_view_from_window<Seq>(seq, ptr, descriptor);
}

// ===========================================================================
// Device minimizer storage + sketch result (DOC0307 §4.3)
// ===========================================================================

/// Shared lifetime anchor for a batch result whose one reconciled permit
/// covers both the resident Vulkan allocation and its host-side vectors.  The
/// batch result and device storage retain the same owner so scheduler moves of
/// those objects cannot release admission while either side is still live.
struct VulkanBatchSketchMemoryRetention {
    bioseq::parallelism::MemoryPermit permit;
};

template <class MinimizerT>
struct VulkanDeviceMinimizerStorage {
    // Declaration order is intentional: reverse destruction frees `words`
    // before either admission owner can release its reservation.
    std::shared_ptr<VulkanBatchSketchMemoryRetention> batch_memory_retention;
    bioseq::parallelism::MemoryPermit permit;
    Buffer words;
    std::uint64_t count = 0;
    std::uint64_t capacity = 0;
    std::uint32_t words32_per_minimizer =
        minimizer_wire_traits<MinimizerT>::words32_per_minimizer;
    MinimizerWordKind word_kind =
        minimizer_wire_traits<MinimizerT>::word_kind;
};

template <class MinimizerT>
struct VulkanDeviceMinimizerSpan {
    std::shared_ptr<const VulkanDeviceMinimizerStorage<MinimizerT>> owner{};

    [[nodiscard]] const Buffer* words() const noexcept {
        return owner ? &owner->words : nullptr;
    }
    [[nodiscard]] std::uint64_t count() const noexcept {
        return owner ? owner->count : 0u;
    }
    [[nodiscard]] std::uint32_t words32_per_minimizer() const noexcept {
        return owner ? owner->words32_per_minimizer : 0u;
    }
    [[nodiscard]] MinimizerWordKind word_kind() const noexcept {
        return owner ? owner->word_kind : MinimizerWordKind::u32;
    }
};

/// Host-result ownership selected by the caller before dispatch.  The default
/// collector-compatible route retains geometric-growth headroom because the
/// collector wrapper duplicates records into a second vector.  The fixed
/// zero-copy route is intentionally opt-in: its exact allocation admission is
/// valid only when the caller adopts the returned vector allocation and drops
/// the device result before constructing the CPU target index.
enum class VulkanSketchHostOwnershipRoute : std::uint8_t {
    collector_compatible = 0,
    fixed_zero_copy_adoption = 1,
};

/// Operation-originated evidence that the collector-compatible Vulkan sketch
/// leaf completed its runtime roundtrip. A default token is deliberately
/// invalid, and route identity is validated before the pipeline may translate
/// it into query/target stage evidence.
enum class VulkanSketchDispatchRoute : std::uint8_t {
    none,
    collect_minimizers,
};

struct VulkanSketchDispatchToken {
    VulkanSketchDispatchRoute route = VulkanSketchDispatchRoute::none;
    std::uint64_t completed_roundtrips = 0;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return route == VulkanSketchDispatchRoute::collect_minimizers
            && completed_roundtrips == 1;
    }

    [[nodiscard]] static constexpr VulkanSketchDispatchToken
    completed_collect_minimizers_roundtrip() noexcept {
        return {
            .route = VulkanSketchDispatchRoute::collect_minimizers,
            .completed_roundtrips = 1,
        };
    }

    constexpr bool operator==(
        const VulkanSketchDispatchToken&) const noexcept = default;
};

static_assert(!VulkanSketchDispatchToken{}.accepted());
static_assert(VulkanSketchDispatchToken::
    completed_collect_minimizers_roundtrip().accepted());

/// Aggregate operation-originated evidence for the strict segment-pair path.
/// A completed roundtrip is counted only after the runtime-produced dispatch
/// token has been checked. Capacity-driven GPU window splitting is recorded
/// separately and never reclassified as a CPU fallback.
struct VulkanSegmentPairExecutionProof {
    std::uint64_t completed_roundtrips = 0;
    std::uint64_t downloaded_minimizers = 0;
    std::uint64_t gpu_window_splits = 0;
    bool saturated = false;

    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment
                > (std::numeric_limits<std::uint64_t>::max)()
                    - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    void record_completed_roundtrip(
            std::uint64_t minimizer_count) noexcept {
        add(completed_roundtrips, 1u, saturated);
        add(downloaded_minimizers, minimizer_count, saturated);
    }

    void record_gpu_window_split() noexcept {
        add(gpu_window_splits, 1u, saturated);
    }

    void combine(const VulkanSegmentPairExecutionProof& other) noexcept {
        saturated = saturated || other.saturated;
        add(completed_roundtrips, other.completed_roundtrips, saturated);
        add(downloaded_minimizers, other.downloaded_minimizers, saturated);
        add(gpu_window_splits, other.gpu_window_splits, saturated);
    }

    [[nodiscard]] bool accepted(
            bool require_completed_roundtrip = true) const noexcept {
        return !saturated
            && (!require_completed_roundtrip || completed_roundtrips != 0);
    }
};

struct VulkanOwnedSegmentPairs {
    using value_type = std::pair<std::uint64_t, std::uint64_t>;

    // Reverse destruction releases pair capacity before admission.
    bioseq::parallelism::MemoryPermit permit;
    std::vector<value_type> pairs;

    VulkanOwnedSegmentPairs() = default;
    VulkanOwnedSegmentPairs(const VulkanOwnedSegmentPairs&) = delete;
    VulkanOwnedSegmentPairs& operator=(const VulkanOwnedSegmentPairs&) = delete;
    VulkanOwnedSegmentPairs(VulkanOwnedSegmentPairs&& other) noexcept {
        pairs.swap(other.pairs);
        permit = std::move(other.permit);
    }
    VulkanOwnedSegmentPairs& operator=(VulkanOwnedSegmentPairs&& other) noexcept {
        if (this != &other) {
            bioseq::parallelism::release_admitted_vector_capacity_before_permit(
                pairs, permit);
            pairs.swap(other.pairs);
            permit = std::move(other.permit);
        }
        return *this;
    }

    void release_capacity_before_permit() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            pairs, permit);
    }

    [[nodiscard]] bool empty() const noexcept { return pairs.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return pairs.size(); }
    [[nodiscard]] auto begin() noexcept { return pairs.begin(); }
    [[nodiscard]] auto begin() const noexcept { return pairs.begin(); }
    [[nodiscard]] auto end() noexcept { return pairs.end(); }
    [[nodiscard]] auto end() const noexcept { return pairs.end(); }
    [[nodiscard]] operator const std::vector<value_type>&() const noexcept {
        return pairs;
    }
    friend bool operator==(
        const VulkanOwnedSegmentPairs& lhs,
        const std::vector<value_type>& rhs) noexcept {
        return lhs.pairs == rhs;
    }
    friend bool operator==(
        const std::vector<value_type>& lhs,
        const VulkanOwnedSegmentPairs& rhs) noexcept {
        return lhs == rhs.pairs;
    }
};

template <class MinimizerT>
struct VulkanSketchResult {
    // Both possible admission owners precede the host vector so reverse
    // destruction frees vector storage before either reservation is released.
    std::optional<bioseq::parallelism::MemoryPermit> host_permit;
    std::shared_ptr<VulkanDeviceMinimizerStorage<MinimizerT>> device;
    std::vector<MinimizerT> host_minimizers;
    VulkanSketchDispatchToken dispatch_token{};
    VulkanSketchHostOwnershipRoute host_ownership_route =
        VulkanSketchHostOwnershipRoute::collector_compatible;
    bool host_permit_released_to_process_footprint = false;
    std::uint64_t host_footprint_reflected_delta_bytes = 0u;
    bool arena_trimmed_before_host_materialization = false;
    bool overflow = false;

    [[nodiscard]] VulkanDeviceMinimizerSpan<MinimizerT> device_span() const noexcept {
        return VulkanDeviceMinimizerSpan<MinimizerT>{.owner = device};
    }
};

template <class MinimizerT>
struct VulkanBatchSketchResult {
    // Consumers that detach either host vector without retaining `device`
    // must move `memory_retention` with it.  GPU schedulers naturally retain
    // `device`, which shares the same anchor.
    // It is declared first so reverse destruction releases every accounted
    // vector/device before the final admission anchor.
    std::shared_ptr<VulkanBatchSketchMemoryRetention> memory_retention;
    std::vector<MinimizerT> host_minimizers;
    std::vector<PhaseCSeedQueryRecordWire> query_records;
    std::shared_ptr<VulkanDeviceMinimizerStorage<MinimizerT>> device;
    RuntimeSketchBatchDispatchProof dispatch_proof{};
    DeviceQuerySketchAuthority query_authority{};
    VulkanSketchHostMaterialization materialization =
        VulkanSketchHostMaterialization::host_and_device;
    std::uint64_t device_minimizer_count = 0;
    bool overflow = false;

    [[nodiscard]] VulkanDeviceMinimizerSpan<MinimizerT> device_span() const noexcept {
        return VulkanDeviceMinimizerSpan<MinimizerT>{.owner = device};
    }
};

template <class MinimizerT>
[[nodiscard]] std::vector<MinimizerT> decode_host_minimizer_bytes(
    std::span<const std::byte> bytes, std::uint64_t count)
{
    if (count > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)() / sizeof(MinimizerT))) {
        throw VulkanRuntimeError{"Vulkan minimizer count exceeds host address space"};
    }
    const std::size_t n = static_cast<std::size_t>(count);
    const std::size_t expected = n * sizeof(MinimizerT);
    if (bytes.size() < expected) {
        throw VulkanRuntimeError{"Vulkan minimizer download was truncated"};
    }
    std::vector<MinimizerT> out(n);
    if (expected != 0u) {
        std::memcpy(out.data(), bytes.data(), expected);
    }
    return out;
}

// ===========================================================================
// Byte estimators (DOC0307 §11)
// ===========================================================================

[[nodiscard]] inline std::uint64_t estimate_vulkan_minimizer_capacity(
    std::uint64_t bp_size, std::uint64_t window_size) noexcept
{
    using bioseq::parallelism::saturating_mul;
    // ~2/(w+1) minimizers per bp in practice; 3·(bp/w) is a safe (>1.5×) upper
    // planning estimate that keeps ordinary single-pass sketch buffers small.
    // Those APIs fail closed if it underestimates. Target-index construction is
    // different: it first obtains exact T without an output allocation, records
    // whether this estimate was low, then sizes and admits the deterministic
    // emit from T rather than treating this heuristic as a semantic cap.
    if (bp_size <= k_phase_c_sketch_tile_bp) {
        return std::max<std::uint64_t>(64u, bp_size);
    }
    const std::uint64_t base = window_size == 0 ? bp_size : (bp_size / window_size + 64u);
    return std::max<std::uint64_t>(64u, saturating_mul(base, 3u));
}

template <class MinimizerT>
[[nodiscard]] std::uint64_t vulkan_sketch_resident_bytes(
    std::uint64_t max_minimizers) noexcept
{
    return minimizer_wire_traits<MinimizerT>::device_bytes_for_count(max_minimizers);
}

[[nodiscard]] inline std::uint64_t vulkan_phase_c_sketch_transient_reserved_bytes(
    std::uint64_t bp,
    std::uint64_t sequence_bytes,
    std::uint32_t letters_per_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    std::uint64_t max_minimizers,
    std::uint64_t minimizer_device_output_bytes)
{
    const PhaseCSketchMemoryPlan plan = phase_c_sketch_memory_plan(
        bp,
        round_up4(sequence_bytes),
        sizeof(VulkanRankLut),
        max_minimizers,
        minimizer_device_output_bytes,
        PhaseCSketchUse::minimizer_words,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        letters_per_unit);
    if (!plan.valid) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return sat_add(
        plan.arena_scaffold_bytes,
        plan.count_phase_host_transient_bytes);
}

[[nodiscard]] inline std::uint64_t vulkan_sketch_transient_reserved_bytes(
    std::uint64_t bp,
    std::uint64_t sequence_bytes,
    std::uint32_t letters_per_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    std::uint64_t max_minimizers,
    std::uint64_t minimizer_device_output_bytes)
{
    return vulkan_phase_c_sketch_transient_reserved_bytes(
        bp,
        sequence_bytes,
        letters_per_unit,
        window_size,
        kmer_size,
        max_minimizers,
        minimizer_device_output_bytes);
}

/// The dispatch phase owns only the physical arena, host dispatch transients,
/// query gate, and pessimistic device result allocation.  Host result capacity
/// is deliberately absent: its exact reservation is made after dispatch
/// transients are destroyed and the emitted count is known.
inline constexpr std::uint64_t kVulkanExactHostAllocationHeadroomBytes =
    64ull * 1024ull;

// Fixed-size host staging cannot grow after allocation. Admit the requested
// payload plus a bounded allowance for allocator metadata and VM page rounding
// instead of the 2x capacity headroom required by growable vectors.
[[nodiscard]] constexpr std::uint64_t
vulkan_exact_host_allocation_admission_bytes(
    std::uint64_t requested_bytes) noexcept
{
    using bioseq::parallelism::saturating_add;
    return saturating_add(
        requested_bytes,
        kVulkanExactHostAllocationHeadroomBytes);
}

/// Exact fixed-result reservation.  An empty GPU sketch owns no host
/// allocation, so it must not be charged allocator slop or be rejectable under
/// pressure for a fictitious 64-KiB vector.
[[nodiscard]] constexpr std::uint64_t
vulkan_fixed_host_result_admission_bytes(
        std::uint64_t exact_host_size_bytes) noexcept {
    return exact_host_size_bytes == 0u
        ? 0u
        : vulkan_exact_host_allocation_admission_bytes(
            exact_host_size_bytes);
}

[[nodiscard]] constexpr std::uint64_t
vulkan_sketch_dispatch_admission_bytes(
    std::uint64_t physical_arena_bytes,
    std::uint64_t host_transient_headroom_bytes,
    std::uint64_t query_gate_bytes,
    std::uint64_t resident_allocation_bytes) noexcept
{
    using bioseq::parallelism::saturating_add;
    return saturating_add(
        resident_allocation_bytes,
        saturating_add(
            physical_arena_bytes,
            saturating_add(
                host_transient_headroom_bytes,
                query_gate_bytes)));
}

/// Peak retained admission while the exact host result vector is allocated.
/// Dispatch transients have already been released at this point.
[[nodiscard]] constexpr std::uint64_t
vulkan_sketch_host_materialization_admission_bytes(
    std::uint64_t actual_device_allocation_bytes,
    std::uint64_t exact_host_headroom_bytes) noexcept
{
    return bioseq::parallelism::saturating_add(
        actual_device_allocation_bytes,
        exact_host_headroom_bytes);
}

// ===========================================================================
// Phase C memory estimate (DOC0310 §22)
// ===========================================================================

/// Per-batch Phase C device-memory estimate. Used for initial admission and
/// batch splitting before the count submit reports exact counts. Every field is
/// summed with the shared saturating add so the total never wraps.
struct PhaseCMemoryEstimate {
    std::uint64_t packed_sequence_bytes = 0;
    std::uint64_t query_minimizer_bytes = 0;
    std::uint64_t target_minimizer_bytes = 0;
    std::uint64_t index_slot_bytes = 0;
    std::uint64_t index_position_bytes = 0;
    std::uint64_t anchor_bytes = 0;
    std::uint64_t sort_scratch_bytes = 0;
    std::uint64_t chain_bytes = 0;

    [[nodiscard]] std::uint64_t total() const noexcept {
        using bioseq::parallelism::saturating_add;
        std::uint64_t sum = 0;
        sum = saturating_add(sum, packed_sequence_bytes);
        sum = saturating_add(sum, query_minimizer_bytes);
        sum = saturating_add(sum, target_minimizer_bytes);
        sum = saturating_add(sum, index_slot_bytes);
        sum = saturating_add(sum, index_position_bytes);
        sum = saturating_add(sum, anchor_bytes);
        sum = saturating_add(sum, sort_scratch_bytes);
        sum = saturating_add(sum, chain_bytes);
        return sum;
    }
};

// ===========================================================================
// Sketch entry points (DOC0307 §6.1)
// ===========================================================================

/// DOC0345 Patch C — extracted admission + dispatch + decode tail of the GPU
/// sketch. Works on an already-prepared `SequenceUploadView` + rank LUT so the
/// whole-sequence path and the segment-window path share one implementation.
/// `upload.alphabet_kind` is the single source of truth for the shader kernel
/// (no `choose_alphabet_kernel<bare>()` inside). Returns `std::nullopt` for
/// pre-dispatch declines; `VulkanRuntimeError` propagates.
template <class MinimizerT>
[[nodiscard]] std::optional<VulkanSketchResult<MinimizerT>>
sketch_uploaded_window_result(
    const SequenceUploadView& upload,
    const VulkanRankLut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    std::uint64_t capacity_bound = 0u,
    bool* capacity_overflowed = nullptr,
    VulkanSketchQueryGate query_gate = {},
    VulkanSketchDecline* decline_out = nullptr,
    bioseq::parallelism::MemoryPermit* borrowed_dispatch_permit = nullptr,
    std::uint64_t borrowed_dispatch_budget = 0u,
    VulkanSketchHostOwnershipRoute host_ownership_route =
        VulkanSketchHostOwnershipRoute::collector_compatible)
{
    using bioseq::parallelism::MemoryAdmissionController;
    using bioseq::parallelism::saturating_mul;

    if (capacity_overflowed != nullptr) {
        *capacity_overflowed = false;
    }
    if (decline_out != nullptr) {
        *decline_out = VulkanSketchDecline{
            .bp_size = upload.bp_size,
            .padded_sequence_bytes = upload.padded_byte_size,
            .query_gate_words = static_cast<std::uint64_t>(query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            VulkanSketchDeclineReason reason,
            std::uint64_t max_minimizers = 0u) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
            decline_out->max_minimizers = max_minimizers;
        }
    };
    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()) {
        decline(VulkanSketchDeclineReason::sequence_size_exceeds_u32);
        return std::nullopt;
    }
    if (upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()) {
        decline(VulkanSketchDeclineReason::padded_sequence_size_overflow);
        return std::nullopt;
    }
    auto& runtime = Runtime::instance();
    if (!hardware_device_available(runtime)) {
        decline(VulkanSketchDeclineReason::hardware_device_unavailable);
        return std::nullopt;
    }
    const std::uint64_t max_minimizers =
        capacity_bound == 0u
            ? estimate_vulkan_minimizer_capacity(upload.bp_size, window_size)
            : capacity_bound;
    if (max_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        decline(
            VulkanSketchDeclineReason::minimizer_capacity_exceeds_u32,
            max_minimizers);
        return std::nullopt;
    }
    if (decline_out != nullptr) {
        decline_out->max_minimizers = max_minimizers;
    }
    if (query_gate.words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        decline(
            VulkanSketchDeclineReason::query_gate_exceeds_u32,
            max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            static_cast<std::uint64_t>(sizeof(std::uint32_t)));
    const std::uint64_t requested_device_bytes =
        vulkan_sketch_resident_bytes<MinimizerT>(max_minimizers);
    const PhaseCSketchMemoryPlan dispatch_plan = phase_c_sketch_memory_plan(
        upload.bp_size,
        upload.padded_byte_size,
        sizeof(VulkanRankLut),
        max_minimizers,
        requested_device_bytes,
        PhaseCSketchUse::minimizer_words,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        upload.letters_per_unit,
        runtime.features().max_workgroup_count_x);
    if (requested_device_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !dispatch_plan.valid) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const PhaseCSketchBatchAllocationRequirements allocation_requirements =
        runtime.phase_c_sketch_batch_allocation_requirements(
            upload.padded_byte_size,
            dispatch_plan.tiles.tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<MinimizerT>::word_kind),
            minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            max_minimizers);
    if (!allocation_requirements.valid) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_arena_bytes = query_gate_bytes == 0u
        ? 0u
        : sat_add(
            query_gate_bytes,
            std::uint64_t{k_phase_c_slot_offset_align});
    const std::uint64_t arena_request_bytes = sat_add(
        dispatch_plan.arena_scaffold_bytes,
        query_gate_arena_bytes);
    if (query_gate_arena_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || arena_request_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t physical_arena_bytes =
        runtime.phase_c_arena_allocation_requirement(arena_request_bytes);
    const std::uint64_t host_transient_headroom_bytes =
        vulkan_exact_host_allocation_admission_bytes(
            dispatch_plan.count_phase_host_transient_bytes);
    const std::uint64_t resident_allocation_bytes =
        allocation_requirements.pessimistic_output_allocation_bytes;
    const std::uint64_t transient_bytes = sat_add(
        physical_arena_bytes,
        sat_add(host_transient_headroom_bytes, query_gate_bytes));

    if (host_transient_headroom_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || resident_allocation_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || transient_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }

    const std::uint64_t dispatch_admission_bytes =
        vulkan_sketch_dispatch_admission_bytes(
            physical_arena_bytes,
            host_transient_headroom_bytes,
            query_gate_bytes,
            resident_allocation_bytes);
    if (dispatch_admission_bytes
        == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    std::optional<bioseq::parallelism::MemoryPermit> owned_dispatch_permit;
    bioseq::parallelism::MemoryPermit* dispatch_permit = borrowed_dispatch_permit;
    if (dispatch_permit == nullptr) {
        owned_dispatch_permit = MemoryAdmissionController::instance().try_reserve(
            dispatch_admission_bytes,
            &admission_failure);
        if (owned_dispatch_permit) {
            dispatch_permit = std::addressof(*owned_dispatch_permit);
        }
    }
    if (dispatch_permit == nullptr
        || dispatch_permit->bytes() < dispatch_admission_bytes
        || (borrowed_dispatch_permit != nullptr
            && borrowed_dispatch_budget < dispatch_admission_bytes)) {
        if (decline_out != nullptr) {
            decline_out->reason =
                VulkanSketchDeclineReason::memory_admission_denied;
            decline_out->admission = admission_failure;
        }
        bioseq_log<ERR>(
            "[vulkan sketch] strict dispatch admission denied: {}",
            format_vulkan_sketch_decline(
                decline_out != nullptr
                    ? *decline_out
                    : VulkanSketchDecline{
                        .reason = VulkanSketchDeclineReason::memory_admission_denied,
                        .bp_size = upload.bp_size,
                        .padded_sequence_bytes = upload.padded_byte_size,
                        .max_minimizers = max_minimizers,
                        .query_gate_words = static_cast<std::uint64_t>(
                            query_gate.words.size()),
                        .window_size = window_size,
                        .kmer_size = kmer_size,
                        .admission = admission_failure,
                    }));
        return std::nullopt;
    }

    try {
        const VulkanSketchParams sketch_params{
            .bp_size = static_cast<std::uint32_t>(upload.bp_size),
            .window_size = static_cast<std::uint32_t>(window_size),
            .kmer_size = static_cast<std::uint32_t>(kmer_size),
            .hash_bits = minimizer_wire_traits<MinimizerT>::hash_bits,
            .hash_shift = minimizer_wire_traits<MinimizerT>::hash_shift,
            .pos_bits = minimizer_wire_traits<MinimizerT>::pos_bits,
            .minimizer_word_kind = static_cast<std::uint32_t>(
                minimizer_wire_traits<MinimizerT>::word_kind),
            .minimizer_words32 =
                minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
            .unit_bits = upload.unit_bits,
            .bits_per_char = upload.bits_per_char,
            .letters_per_unit = upload.letters_per_unit,
            .alphabet_size = upload.alphabet_size,
            .seq_word_offset = 0u,
            .char_mask = upload.char_mask,
            .padding_letters = upload.padding_letters,
        };

        RuntimeSketchResult raw;
        {
            // The packed host copy is intentionally scoped around the Vulkan
            // dispatch.  The runtime also destroys its host tile vector and
            // rewinds the arena mark before returning.  Exact host result
            // admission therefore happens only after every dispatch transient
            // has gone away.
            const auto padded_sequence_size =
                static_cast<std::size_t>(upload.padded_byte_size);
            auto padded_sequence_owner =
                std::make_unique_for_overwrite<std::byte[]>(
                    padded_sequence_size);
            std::span<std::byte> padded_sequence{
                padded_sequence_owner.get(),
                padded_sequence_size};
            if (upload.byte_size != 0u && upload.bytes != nullptr) {
                std::memcpy(
                    padded_sequence.data(),
                    upload.bytes,
                    static_cast<std::size_t>(upload.byte_size));
            }
            if (upload.padded_byte_size > upload.byte_size) {
                std::memset(
                    padded_sequence.data() +
                        static_cast<std::size_t>(upload.byte_size),
                    0,
                    static_cast<std::size_t>(
                        upload.padded_byte_size - upload.byte_size));
            }
            raw = runtime.run_phase_c_sketch_roundtrip(
                std::span<const std::byte>{
                    padded_sequence.data(),
                    padded_sequence.size()},
                rank_lut,
                sketch_params,
                max_minimizers,
                query_gate.words,
                query_gate.mask);
        }
        if (!raw.arena_trimmed_before_return) {
            throw VulkanRuntimeError{
                "Vulkan minimizer roundtrip returned before arena trim"};
        }
        if (raw.overflow) {
            if (capacity_overflowed != nullptr) {
                *capacity_overflowed = true;
            }
            if (capacity_bound != 0u && capacity_overflowed == nullptr) {
                throw VulkanRuntimeError(fmt::format(
                    "Vulkan minimizer sketch overflowed explicit capacity bound "
                    "(bp={}, cap={})",
                    upload.bp_size,
                    max_minimizers));
            }
            decline(
                VulkanSketchDeclineReason::output_capacity_overflow,
                max_minimizers);
            return std::nullopt;
        }

        const std::uint64_t actual_device_allocation_bytes =
            raw.minimizer_words.allocation_bytes;
        if (actual_device_allocation_bytes > resident_allocation_bytes) {
            throw VulkanRuntimeError{
                "Vulkan minimizer device allocation exceeded admission"};
        }
        if (borrowed_dispatch_permit == nullptr) {
            dispatch_permit->shrink_to(actual_device_allocation_bytes);
        }

        if (raw.count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)()
                / sizeof(MinimizerT))) {
            throw VulkanRuntimeError{
                "Vulkan minimizer count exceeds host address space"};
        }
        const std::uint64_t exact_host_size_bytes = saturating_mul(
            raw.count,
            static_cast<std::uint64_t>(sizeof(MinimizerT)));
        const std::uint64_t exact_host_headroom_bytes =
            host_ownership_route
                    == VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption
                ? vulkan_fixed_host_result_admission_bytes(
                      exact_host_size_bytes)
                : saturating_mul(
                      exact_host_size_bytes,
                      bioseq::parallelism::kVectorPreallocationHeadroomFactor);
        const std::uint64_t host_materialization_peak_bytes =
            vulkan_sketch_host_materialization_admission_bytes(
                actual_device_allocation_bytes,
                exact_host_headroom_bytes);
        if (exact_host_size_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || exact_host_headroom_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || host_materialization_peak_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
            return std::nullopt;
        }

        std::optional<bioseq::parallelism::MemoryPermit> owned_host_permit;
        if (borrowed_dispatch_permit == nullptr) {
            bioseq::parallelism::MemoryAdmissionFailure host_failure{};
            owned_host_permit =
                MemoryAdmissionController::instance().try_reserve(
                    exact_host_headroom_bytes,
                    &host_failure);
            if (!owned_host_permit) {
                if (decline_out != nullptr) {
                    decline_out->reason =
                        VulkanSketchDeclineReason::memory_admission_denied;
                    decline_out->admission = host_failure;
                }
                bioseq_log<ERR>(
                    "[vulkan sketch] strict exact-host admission denied: {}",
                    format_vulkan_sketch_decline(
                        decline_out != nullptr
                            ? *decline_out
                            : VulkanSketchDecline{
                                .reason = VulkanSketchDeclineReason::
                                    memory_admission_denied,
                                .bp_size = upload.bp_size,
                                .padded_sequence_bytes =
                                    upload.padded_byte_size,
                                .max_minimizers = max_minimizers,
                                .query_gate_words =
                                    static_cast<std::uint64_t>(
                                        query_gate.words.size()),
                                .window_size = window_size,
                                .kmer_size = kmer_size,
                                .admission = host_failure,
                            }));
                return std::nullopt;
            }
        } else if (host_materialization_peak_bytes
                   > borrowed_dispatch_budget) {
            decline(VulkanSketchDeclineReason::memory_admission_denied,
                    max_minimizers);
            return std::nullopt;
        }

        if (host_ownership_route
                == VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption
            && borrowed_dispatch_permit != nullptr) {
            throw VulkanRuntimeError{
                "Vulkan fixed zero-copy host adoption requires an owned "
                "materialization permit"};
        }

        // Reserve once, download once: the former byte-vector staging copy is
        // gone.  Permit declaration precedes vector declaration so reverse
        // destruction always frees vector capacity before returning admission.
        std::vector<MinimizerT> host_minimizers;
        std::uint64_t exact_host_capacity_bytes = 0u;
        bool host_permit_released_to_process_footprint = false;
        std::uint64_t host_footprint_reflected_delta_bytes = 0u;
        const auto allocate_and_download = [
                &host_minimizers,
                &exact_host_capacity_bytes,
                &raw,
                host_ownership_route,
                exact_host_headroom_bytes,
                &owned_host_permit,
                &runtime] {
            host_minimizers.resize(static_cast<std::size_t>(raw.count));
            exact_host_capacity_bytes = saturating_mul(
                static_cast<std::uint64_t>(host_minimizers.capacity()),
                static_cast<std::uint64_t>(sizeof(MinimizerT)));
            if ((host_ownership_route
                        == VulkanSketchHostOwnershipRoute::
                            fixed_zero_copy_adoption
                    && host_minimizers.size()
                        != host_minimizers.capacity())
                || exact_host_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || exact_host_capacity_bytes > exact_host_headroom_bytes) {
                throw VulkanRuntimeError{
                    "Vulkan decoded fixed host minimizer capacity exceeded "
                    "admission"};
            }
            if (owned_host_permit) {
                owned_host_permit->shrink_to(exact_host_capacity_bytes);
            }
            runtime.download_slice(
                raw.minimizer_words,
                0u,
                std::as_writable_bytes(std::span<MinimizerT>{
                    host_minimizers.data(), host_minimizers.size()}));
        };

        if (host_ownership_route
                == VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption
            && exact_host_size_bytes != 0u) {
            auto materialization =
                MemoryAdmissionController::instance().materialize(
                    *owned_host_permit,
                    [&allocate_and_download,
                     &exact_host_capacity_bytes]() -> bioseq::parallelism::
                            MemoryMaterializationEvidence {
                        allocate_and_download();
                        return {
                            .actual_allocation_bytes =
                                exact_host_capacity_bytes,
                            .fully_touched_bytes = exact_host_capacity_bytes,
                            .host_visible = true,
                            .host_coherent = true,
                            .process_footprint_reflected = true,
                        };
                    });
            host_permit_released_to_process_footprint =
                materialization.permit_released;
            host_footprint_reflected_delta_bytes =
                materialization.reflected_delta_bytes;
            if (!materialization.permit_released) {
                // A large target does not imply a large emitted vector.  An
                // ambiguity-heavy dna5 target can produce a small non-empty
                // allocation whose allocator pages were already resident, so
                // the serialized RSS probe may correctly decline to release
                // the reservation.  Keep the exact, already-shrunk permit as
                // the alternative ownership authority.  The pipeline moves it
                // with the vector and releases it only after index construction
                // has consumed the vector and proved capacity()==0.
                bioseq_log<DBG>(
                    "[vulkan sketch] fixed host footprint reconciliation "
                    "retained exact permit: actual={} touched={} before={} "
                    "after={} delta={} permit={}",
                    materialization.evidence.actual_allocation_bytes,
                    materialization.evidence.fully_touched_bytes,
                    materialization.footprint_before_bytes,
                    materialization.footprint_after_bytes,
                    materialization.reflected_delta_bytes,
                    owned_host_permit->bytes());
            } else {
                owned_host_permit.reset();
            }
        } else {
            allocate_and_download();
            if (host_ownership_route
                    == VulkanSketchHostOwnershipRoute::
                        fixed_zero_copy_adoption) {
                // Empty vectors own no capacity and need no footprint delta.
                if (owned_host_permit) {
                    owned_host_permit->shrink_to(0u);
                }
                owned_host_permit.reset();
                host_permit_released_to_process_footprint = true;
            }
        }

        auto storage = std::make_shared<VulkanDeviceMinimizerStorage<MinimizerT>>();
        storage->words = std::move(raw.minimizer_words);
        if (borrowed_dispatch_permit == nullptr) {
            storage->permit = std::move(*dispatch_permit);
        }
        storage->count = raw.count;
        storage->capacity = max_minimizers;

        return VulkanSketchResult<MinimizerT>{
            .host_permit = std::move(owned_host_permit),
            .device = std::move(storage),
            .host_minimizers = std::move(host_minimizers),
            .dispatch_token = VulkanSketchDispatchToken::
                completed_collect_minimizers_roundtrip(),
            .host_ownership_route = host_ownership_route,
            .host_permit_released_to_process_footprint =
                host_permit_released_to_process_footprint,
            .host_footprint_reflected_delta_bytes =
                host_footprint_reflected_delta_bytes,
            .arena_trimmed_before_host_materialization =
                raw.arena_trimmed_before_return,
            .overflow = false,
        };
    } catch (const VulkanRuntimeError&) {
        throw;
    }
}

template <class MinimizerT>
[[nodiscard]] std::optional<GpuBuiltIndex>
build_gpu_index_from_upload_on_device(
    const SequenceUploadView& upload,
    const VulkanRankLut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSketchQueryGate query_gate = {},
    VulkanSketchDecline* decline_out = nullptr,
    std::uint64_t owned_sequence_staging_bytes = 0u,
    std::function<void()> release_owned_sequence_staging = {})
{
    using bioseq::parallelism::MemoryAdmissionController;

    if (decline_out != nullptr) {
        *decline_out = VulkanSketchDecline{
            .bp_size = upload.bp_size,
            .padded_sequence_bytes = upload.padded_byte_size,
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            VulkanSketchDeclineReason reason,
            std::uint64_t max_minimizers = 0u) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
            decline_out->max_minimizers = max_minimizers;
        }
    };

    if (upload.bp_size == 0u) {
        decline(VulkanSketchDeclineReason::empty_sequence);
        return std::nullopt;
    }
    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()) {
        decline(VulkanSketchDeclineReason::sequence_size_exceeds_u32);
        return std::nullopt;
    }
    if (upload.padded_byte_size
        == std::numeric_limits<std::uint64_t>::max()) {
        decline(VulkanSketchDeclineReason::padded_sequence_size_overflow);
        return std::nullopt;
    }
    if (query_gate.words.size()
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        decline(VulkanSketchDeclineReason::query_gate_exceeds_u32);
        return std::nullopt;
    }
    auto& runtime = Runtime::instance();
    if (!hardware_device_available(runtime)) {
        decline(VulkanSketchDeclineReason::hardware_device_unavailable);
        return std::nullopt;
    }

    static std::mutex phase_c_target_index_build_mutex;
    std::lock_guard<std::mutex> phase_c_target_index_build_guard{
        phase_c_target_index_build_mutex};

    const std::uint64_t max_minimizers = std::min(
        estimate_vulkan_minimizer_capacity(upload.bp_size, window_size),
        phase_c_target_minimizer_hard_bound(
            upload.bp_size, static_cast<std::uint32_t>(kmer_size)));
    if (decline_out != nullptr) {
        decline_out->max_minimizers = max_minimizers;
    }

    const PhaseCSketchMemoryPlan memory_plan = phase_c_sketch_memory_plan(
        upload.bp_size,
        upload.padded_byte_size,
        sizeof(VulkanRankLut),
        max_minimizers,
        0u,
        PhaseCSketchUse::target_index_build,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        upload.letters_per_unit,
        runtime.features().max_workgroup_count_x);
    if (!memory_plan.valid) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const PhaseCTargetIndexHostScratchPlan host_scratch_plan =
        phase_c_target_index_host_scratch_plan(
            memory_plan, owned_sequence_staging_bytes);
    if (!host_scratch_plan.valid
        || (owned_sequence_staging_bytes != 0u
            && !release_owned_sequence_staging)) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            sizeof(std::uint32_t));
    const std::uint64_t count_arena_request = sat_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : sat_add(query_gate_bytes,
                      std::uint64_t{k_phase_c_slot_offset_align}));
    const std::uint64_t count_arena_bytes =
        runtime.phase_c_arena_allocation_requirement(count_arena_request);
    const std::uint64_t admission_bytes = sat_add(
        count_arena_bytes, host_scratch_plan.count_phase_bytes);
    if (query_gate_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || admission_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(VulkanSketchDeclineReason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    auto& admission = MemoryAdmissionController::instance();
    bioseq::parallelism::MemoryAdmissionFailure admission_failure{};
    auto permit = admission.try_reserve(admission_bytes, &admission_failure);
    if (!permit) {
        if (decline_out != nullptr) {
            decline_out->reason =
                VulkanSketchDeclineReason::memory_admission_denied;
            decline_out->max_minimizers = max_minimizers;
            decline_out->admission = admission_failure;
        }
        bioseq_log<ERR>("[phase_c index] target-index build admission failed "
                        "({})",
                        format_vulkan_sketch_decline(
                            decline_out != nullptr
                                ? *decline_out
                                : VulkanSketchDecline{
                                    .reason = VulkanSketchDeclineReason::
                                        memory_admission_denied,
                                    .bp_size = upload.bp_size,
                                    .padded_sequence_bytes =
                                        upload.padded_byte_size,
                                    .max_minimizers = max_minimizers,
                                    .query_gate_words =
                                        static_cast<std::uint64_t>(
                                            query_gate.words.size()),
                                    .window_size = window_size,
                                    .kmer_size = kmer_size,
                                    .admission = admission_failure,
                                }));
        return std::nullopt;
    }

    try {
        const auto query_gate_identity = query_gate.canonical_identity();
        const VulkanSketchParams sketch_params{
            .bp_size = static_cast<std::uint32_t>(upload.bp_size),
            .window_size = static_cast<std::uint32_t>(window_size),
            .kmer_size = static_cast<std::uint32_t>(kmer_size),
            .hash_bits = minimizer_wire_traits<MinimizerT>::hash_bits,
            .hash_shift = minimizer_wire_traits<MinimizerT>::hash_shift,
            .pos_bits = minimizer_wire_traits<MinimizerT>::pos_bits,
            .minimizer_word_kind = static_cast<std::uint32_t>(
                minimizer_wire_traits<MinimizerT>::word_kind),
            .minimizer_words32 =
                minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
            .unit_bits = upload.unit_bits,
            .bits_per_char = upload.bits_per_char,
            .letters_per_unit = upload.letters_per_unit,
            .alphabet_size = upload.alphabet_size,
            .seq_word_offset = 0u,
            .char_mask = upload.char_mask,
            .padding_letters = upload.padding_letters,
        };
        GpuBuiltIndex built{};
        const auto materialization = admission.materialize(
            *permit,
            [&]() -> bioseq::parallelism::MemoryMaterializationEvidence {
                built = runtime.run_phase_c_build_target_index(
                    std::span<const std::byte>{
                        static_cast<const std::byte*>(upload.bytes),
                        static_cast<std::size_t>(upload.byte_size)},
                    upload.padded_byte_size,
                    owned_sequence_staging_bytes,
                    std::move(release_owned_sequence_staging),
                    rank_lut,
                    sketch_params,
                    max_minimizers,
                    query_gate.words,
                    query_gate.mask,
                    query_gate_identity,
                    &*permit);

                if (!built.valid) {
                    return {};
                }
                // Runtime publishes this bit only after rewinding and
                // destroying all Phase-C arena backing buffers while its mutex
                // is still held. Do not inspect the shared arena here: another
                // Vulkan request may already own it after Runtime returned.
                if (built.ownership.arena_trimmed_before_permit_shrink != 1u) {
                    built.valid = false;
                    built.failure_reason = GpuBuiltIndexFailureReason::
                        index_invariant_failed;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                if (built.ownership.peak_observed_bytes > permit->bytes()) {
                    built.valid = false;
                    built.failure_reason = GpuBuiltIndexFailureReason::
                        peak_allocation_exceeds_admission;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                const std::uint64_t resident_bytes = sat_add(
                    built.slots.allocation_bytes,
                    built.positions.allocation_bytes);
                if (resident_bytes > permit->bytes()) {
                    built.valid = false;
                    built.failure_reason = GpuBuiltIndexFailureReason::
                        peak_allocation_exceeds_admission;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                permit->shrink_to(resident_bytes);
                built.ownership.resident_bytes = resident_bytes;
                const bool host_visible_coherent =
                    built.slots.host_visible_coherent()
                    && built.positions.host_visible_coherent();
                return bioseq::parallelism::MemoryMaterializationEvidence{
                    .actual_allocation_bytes = resident_bytes,
                    .fully_touched_bytes = resident_bytes,
                    .host_visible = host_visible_coherent,
                    .host_coherent = host_visible_coherent,
                    .process_footprint_reflected =
                        built.slots.process_footprint_reflected
                        && built.positions.process_footprint_reflected,
                };
            });
        if (!built.valid) {
            if (decline_out != nullptr
                && built.main_peak_admission_failure.failed()) {
                decline_out->admission = built.main_peak_admission_failure;
            }
            decline(
                vulkan_target_index_failure_decline_reason(
                    built.failure_reason),
                max_minimizers);
            return std::nullopt;
        }
        built.materialization = materialization;
        built.ownership.materialization_admitted_bytes =
            materialization.admitted_bytes;
        built.ownership.materialization_actual_allocation_bytes =
            materialization.evidence.actual_allocation_bytes;
        built.ownership.materialization_fully_touched_bytes =
            materialization.evidence.fully_touched_bytes;
        built.ownership.materialization_reflected_delta_bytes =
            materialization.reflected_delta_bytes;
        built.ownership.materialization_permit_released =
            materialization.permit_released ? 1u : 0u;
        built.ownership.materialization_host_visible =
            materialization.evidence.host_visible ? 1u : 0u;
        built.ownership.materialization_host_coherent =
            materialization.evidence.host_coherent ? 1u : 0u;
        built.ownership.materialization_process_footprint_reflected =
            materialization.evidence.process_footprint_reflected ? 1u : 0u;
        built.permit = std::move(*permit);
        return built;
    } catch (const VulkanRuntimeError&) {
        throw;
    }
}

/// GPU minimizer sketch with full memory admission, returning both the host
/// minimizers (for the arena slab) and the resident device buffer (for
/// seed-lookup reuse). Returns `std::nullopt` for pre-dispatch shapes the GPU
/// cannot run (fragmented, oversized k/alphabet, no usable device, or admission
/// failure). Selected Vulkan pipeline callers convert that decline into a hard
/// error; runtime errors propagate directly.
template <class MinimizerT, class Seq>
[[nodiscard]] std::optional<VulkanSketchResult<MinimizerT>>
collect_minimizers_vulkan_result(
    const Seq& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSketchDecline* decline_out = nullptr,
    VulkanSketchHostOwnershipRoute host_ownership_route =
        VulkanSketchHostOwnershipRoute::collector_compatible)
{
    using bare = std::remove_cvref_t<Seq>;
    if (decline_out != nullptr) {
        *decline_out = VulkanSketchDecline{
            .bp_size = vulkan_decline_bp_size(seq),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            VulkanSketchDeclineReason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (is_fragmented_sequence<bare>()) {
        if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
            decline(VulkanSketchDeclineReason::fragmented_sequence_unsupported);
            return std::nullopt;
        } else {
            using traits = typename bare::traits;
            using storage_unit = typename traits::storage_unit;
            if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0
                          || traits::bits_per_char > 32) {
                decline(VulkanSketchDeclineReason::alphabet_unsupported);
                return std::nullopt;
            } else {
                constexpr std::size_t step_bits =
                    alphabet_kernel_kind_of_traits<traits>() == AlphabetKernelKind::generic
                        ? traits::bits_per_char
                        : std::size_t{2};
                if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                    decline(VulkanSketchDeclineReason::kmer_size_unsupported);
                    return std::nullopt;
                }
                if (window_size == 0 || window_size > 256) {
                    decline(VulkanSketchDeclineReason::window_size_unsupported);
                    return std::nullopt;
                }
                const SegmentUploadDescriptor descriptor =
                    compute_segment_descriptor(seq, 0u, static_cast<std::size_t>(seq.size()));
                std::vector<storage_unit> scratch;
                scratch.resize(descriptor.hi_unit - descriptor.lo_unit);
                const storage_unit* base = seq.read_cptr(
                    seq.cbegin(),
                    std::span<storage_unit>{scratch.data(), scratch.size()},
                    scratch.size());
                const SequenceUploadView upload =
                    make_upload_view_from_window<bare>(seq, base, descriptor);
                const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
                return sketch_uploaded_window_result<MinimizerT>(
                    upload,
                    rank_lut,
                    window_size,
                    kmer_size,
                    0u,
                    nullptr,
                    {},
                    decline_out,
                    nullptr,
                    0u,
                    host_ownership_route);
            }
        }
    } else if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(VulkanSketchDeclineReason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0
                      || traits::bits_per_char > 32) {
            decline(VulkanSketchDeclineReason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == AlphabetKernelKind::generic
                    ? traits::bits_per_char
                    : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(VulkanSketchDeclineReason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(VulkanSketchDeclineReason::window_size_unsupported);
                return std::nullopt;
            }
            const SequenceUploadView upload = make_sequence_upload_view(seq);
            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            return sketch_uploaded_window_result<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                0u,
                nullptr,
                {},
                decline_out,
                nullptr,
                0u,
                host_ownership_route);
        }
    }
}

template <class MinimizerT, class Seq>
[[nodiscard]] std::optional<VulkanBatchSketchResult<MinimizerT>>
collect_minimizers_vulkan_batch_result(
    std::span<const Seq> seqs,
    std::size_t window_size,
    std::size_t kmer_size) = delete;

[[nodiscard]] std::optional<
    VulkanBatchSketchResult<::bioseq::simd::HWY_NAMESPACE::MinimapSketchData>>
collect_minimap_sketch_data_vulkan_upload_batch_result(
    std::span<const SequenceUploadView> uploads,
    const VulkanRankLut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSketchHostMaterialization materialization)
{
    using MinimizerT = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using bioseq::parallelism::MemoryAdmissionController;
    using bioseq::parallelism::saturating_mul;

    if (materialization != VulkanSketchHostMaterialization::host_and_device
        && materialization != VulkanSketchHostMaterialization::device_only) {
        throw VulkanRuntimeError{
            "Vulkan batch sketch has invalid host materialization mode"};
    }
    if (uploads.empty()) {
        return VulkanBatchSketchResult<MinimizerT>{
            .materialization = materialization,
        };
    }
    if (kmer_size == 0 || kmer_size > 32 || window_size == 0 || window_size > 256) {
        return std::nullopt;
    }
    if (uploads.size()
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return std::nullopt;
    }

    auto& runtime = Runtime::instance();
    if (!hardware_device_available(runtime) || runtime.features().max_workgroup_count_x == 0u) {
        return std::nullopt;
    }

    using QueryTileRange = PhaseCSketchQueryLayoutWire;

    // Pass 1 is scalar-only: validate every upload and compute exact vector
    // sizes without constructing any owned vector or Vulkan Buffer.
    std::uint64_t packed_sequence_bytes = 0u;
    std::uint64_t total_tile_count = 0u;
    std::uint64_t max_minimizers = 0u;
    std::uint64_t max_bp = 0u;
    const SequenceUploadView& first = uploads.front();
    const std::uint32_t letters_per_unit = first.letters_per_unit;
    const std::uint32_t unit_bits = first.unit_bits;
    const std::uint32_t bits_per_char = first.bits_per_char;
    const std::uint32_t char_mask = first.char_mask;
    const std::uint32_t alphabet_size = first.alphabet_size;
    const std::uint32_t padding_letters = first.padding_letters;
    const std::uint32_t alphabet_kind = static_cast<std::uint32_t>(first.alphabet_kind);

    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const SequenceUploadView& upload = uploads[query_index];
        if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()
            || upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()
            || upload.padded_byte_size
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || upload.byte_size > upload.padded_byte_size
            || (upload.byte_size != 0u && upload.bytes == nullptr)
            || upload.letters_per_unit != letters_per_unit
            || upload.unit_bits != unit_bits
            || upload.bits_per_char != bits_per_char
            || upload.char_mask != char_mask
            || upload.alphabet_size != alphabet_size
            || static_cast<std::uint32_t>(upload.alphabet_kind) != alphabet_kind) {
            return std::nullopt;
        }
        max_bp = std::max(max_bp, upload.bp_size);
        max_minimizers = sat_add(
            max_minimizers,
            estimate_vulkan_minimizer_capacity(upload.bp_size, window_size));
        if (max_minimizers
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return std::nullopt;
        }

        const std::uint64_t aligned_begin = phase_c_align_up(
            packed_sequence_bytes,
            sizeof(std::uint32_t));
        if (aligned_begin
                == (std::numeric_limits<std::uint64_t>::max)()
            || aligned_begin / sizeof(std::uint32_t)
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return std::nullopt;
        }
        packed_sequence_bytes = sat_add(
            aligned_begin,
            upload.padded_byte_size);
        if (packed_sequence_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || packed_sequence_bytes
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return std::nullopt;
        }

        if (upload.bp_size >= kmer_size) {
            const PhaseCSketchTilePlan tile_plan = phase_c_tile_plan(
                upload.bp_size,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
            if (!tile_plan.dispatchable || tile_plan.tile_count == 0u) {
                return std::nullopt;
            }
            total_tile_count = sat_add(
                total_tile_count,
                tile_plan.tile_count);
            if (total_tile_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
                return std::nullopt;
            }
        }
    }

    if (total_tile_count == 0u || max_minimizers == 0u) {
        // A nonempty caller requested GPU work, but no dispatchable query
        // exists.  Do not return a result that could be mistaken for accepted
        // zero-emission Vulkan execution.
        return std::nullopt;
    }

    const PhaseCSketchBatchAllocationRequirements initial_requirements =
        runtime.phase_c_sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<MinimizerT>::word_kind),
            minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            max_minimizers);
    if (!initial_requirements.valid) {
        return std::nullopt;
    }
    const std::uint64_t pessimistic_host_minimizer_bytes =
        materialization == VulkanSketchHostMaterialization::host_and_device
        ? saturating_mul(max_minimizers, sizeof(MinimizerT))
        : 0u;
    const std::uint64_t peak_host_minimizer_bytes = saturating_mul(
        pessimistic_host_minimizer_bytes,
        materialization == VulkanSketchHostMaterialization::host_and_device
            ? 2u
            : 0u);
    const auto vector_capacity_bytes = []<class Value>(
        const std::vector<Value>& values) noexcept -> std::uint64_t {
        if (values.capacity()
            > (std::numeric_limits<std::uint64_t>::max)()) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            sizeof(Value));
    };
    const auto release_vector_storage = []<class Value>(
        std::vector<Value>& values) noexcept {
        std::vector<Value>{}.swap(values);
    };
    const std::uint64_t exact_scaffold_bytes = sat_add(
        sat_add(
            packed_sequence_bytes,
            saturating_mul(
                total_tile_count,
                sizeof(PhaseCSketchTileWire))),
        sat_add(
            sat_add(
                saturating_mul(uploads.size(), sizeof(QueryTileRange)),
                saturating_mul(
                    uploads.size(),
                    2u * sizeof(PhaseCSeedQueryRecordWire)
                        + sizeof(std::uint32_t))),
            saturating_mul(
                total_tile_count,
                2u * sizeof(std::uint32_t))));
    const std::uint64_t initial_peak_bytes = sat_add(
        sat_add(
            initial_requirements.arena_allocation_bytes,
            initial_requirements.pessimistic_output_allocation_bytes),
        sat_add(peak_host_minimizer_bytes, exact_scaffold_bytes));
    if (initial_peak_bytes
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::nullopt;
    }
    auto initial_permit = MemoryAdmissionController::instance().try_reserve(
        initial_peak_bytes);
    if (!initial_permit) return std::nullopt;

    // Declaration order is a correctness invariant: the shared permit holder
    // precedes result/scaffold/raw ownership, so reverse destruction always
    // releases vectors and Vulkan Buffers before admission on every exit.
    auto memory_retention =
        std::make_shared<VulkanBatchSketchMemoryRetention>();
    memory_retention->permit = std::move(*initial_permit);
    VulkanBatchSketchResult<MinimizerT> result{
        .memory_retention = memory_retention,
        .materialization = materialization,
    };
    std::vector<std::byte> packed_sequence;
    std::vector<PhaseCSketchTileWire> tiles;
    std::vector<QueryTileRange> tile_ranges;

    result.query_records.resize(uploads.size());
    packed_sequence.reserve(
        static_cast<std::size_t>(packed_sequence_bytes));
    tiles.reserve(static_cast<std::size_t>(total_tile_count));
    tile_ranges.resize(uploads.size());

    // Pass 2 fills the exact preflight shapes without capacity growth under
    // normal allocators.  Actual capacities are reconciled before dispatch.
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const SequenceUploadView& upload = uploads[query_index];
        const std::size_t aligned_begin = static_cast<std::size_t>(
            phase_c_align_up(
                packed_sequence.size(),
                sizeof(std::uint32_t)));
        packed_sequence.resize(aligned_begin);
        const std::uint32_t seq_word_offset =
            static_cast<std::uint32_t>(
                aligned_begin / sizeof(std::uint32_t));
        const std::size_t append_begin = packed_sequence.size();
        const std::size_t padded_bytes =
            static_cast<std::size_t>(upload.padded_byte_size);
        packed_sequence.resize(append_begin + padded_bytes);
        if (padded_bytes != 0u) {
            std::memset(
                packed_sequence.data() + append_begin,
                0,
                padded_bytes);
        }
        if (upload.byte_size != 0u) {
            std::memcpy(
                packed_sequence.data() + append_begin,
                upload.bytes,
                static_cast<std::size_t>(upload.byte_size));
        }

        const std::uint32_t tile_begin =
            static_cast<std::uint32_t>(tiles.size());
        if (upload.bp_size >= kmer_size) {
            const PhaseCSketchTilePlan tile_plan = phase_c_tile_plan(
                upload.bp_size,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
            const std::uint32_t bp =
                static_cast<std::uint32_t>(upload.bp_size);
            const std::uint32_t overlap = phase_c_replay_overlap_bp(
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size));
            for (std::uint32_t tile = 0u; tile < tile_plan.tile_count; ++tile) {
                const std::uint32_t lo = tile * tile_plan.tile_bp;
                const std::uint32_t hi =
                    std::min(bp, (tile + 1u) * tile_plan.tile_bp);
                tiles.push_back(PhaseCSketchTileWire{
                    .seq_word_offset = seq_word_offset,
                    .bp_count = bp,
                    .replay_lo = lo > overlap ? lo - overlap : 0u,
                    .tile_lo = lo,
                    .tile_hi = hi,
                    .out_base = 0u,
                });
            }
        }
        tile_ranges[query_index] = QueryTileRange{
            .begin = tile_begin,
            .count = static_cast<std::uint32_t>(
                tiles.size() - tile_begin),
            .query_len = static_cast<std::uint32_t>(upload.bp_size),
        };
    }
    if (packed_sequence.size() != packed_sequence_bytes
        || tiles.size() != total_tile_count) {
        throw VulkanRuntimeError{
            "Vulkan batch sketch preflight/fill shape mismatch"};
    }

    const PhaseCSketchBatchAllocationRequirements pre_dispatch_requirements =
        runtime.phase_c_sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<MinimizerT>::word_kind),
            minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            max_minimizers);
    if (!pre_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t actual_scaffold_capacity_bytes = sat_add(
        sat_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        sat_add(
            vector_capacity_bytes(tile_ranges),
            sat_add(
                vector_capacity_bytes(result.query_records),
                vector_capacity_bytes(result.host_minimizers))));
    const std::uint64_t anticipated_raw_count_bytes = saturating_mul(
        total_tile_count,
        2u * sizeof(std::uint32_t));
    const std::uint64_t authority_query_record_headroom = saturating_mul(
        saturating_mul(
            uploads.size(),
            sizeof(PhaseCSeedQueryRecordWire) + sizeof(std::uint32_t)),
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    const std::uint64_t pre_dispatch_peak_bytes = sat_add(
        sat_add(
            pre_dispatch_requirements.arena_allocation_bytes,
            pre_dispatch_requirements.pessimistic_output_allocation_bytes),
        sat_add(
            peak_host_minimizer_bytes,
            sat_add(
                actual_scaffold_capacity_bytes,
                sat_add(
                    anticipated_raw_count_bytes,
                    authority_query_record_headroom))));
    bioseq::parallelism::MemoryAdmissionFailure growth_failure{};
    if (pre_dispatch_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            pre_dispatch_peak_bytes,
            &growth_failure)) {
        return std::nullopt;
    }

    const VulkanSketchParams sketch_params{
        .bp_size = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                max_bp,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)()))),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = minimizer_wire_traits<MinimizerT>::hash_bits,
        .hash_shift = minimizer_wire_traits<MinimizerT>::hash_shift,
        .pos_bits = minimizer_wire_traits<MinimizerT>::pos_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            minimizer_wire_traits<MinimizerT>::word_kind),
        .minimizer_words32 =
            minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
        .alphabet_kind = alphabet_kind,
        .unit_bits = unit_bits,
        .bits_per_char = bits_per_char,
        .letters_per_unit = letters_per_unit,
        .alphabet_size = alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = char_mask,
        .padding_letters = padding_letters,
    };

    // Declared after every other owner: any later failure destroys this raw
    // Buffer/vector aggregate first, then scaffolds/result, then the permit.
    RuntimeSketchBatchResult raw =
        runtime.run_phase_c_sketch_tile_batch_roundtrip(
            std::span<const std::byte>{
                packed_sequence.data(),
                packed_sequence.size()},
            rank_lut,
            sketch_params,
            std::span<const PhaseCSketchTileWire>{tiles.data(), tiles.size()},
            std::span<const PhaseCSketchQueryLayoutWire>{
                tile_ranges.data(), tile_ranges.size()},
            max_minimizers,
            materialization);
    const PhaseCSketchBatchAllocationRequirements post_dispatch_requirements =
        runtime.phase_c_sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<MinimizerT>::word_kind),
            minimizer_wire_traits<MinimizerT>::words32_per_minimizer,
            max_minimizers);
    if (!post_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t post_dispatch_live_vector_bytes = sat_add(
        sat_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        sat_add(
            vector_capacity_bytes(tile_ranges),
            sat_add(
                sat_add(
                    vector_capacity_bytes(result.query_records),
                    vector_capacity_bytes(result.host_minimizers)),
                sat_add(
                    vector_capacity_bytes(raw.host_minimizer_bytes),
                    sat_add(
                        vector_capacity_bytes(raw.tile_counts),
                        sat_add(
                            vector_capacity_bytes(raw.tile_offsets),
                            raw.query_authority.owned_capacity_bytes()))))));
    const std::uint64_t post_dispatch_live_bytes = sat_add(
        raw.minimizer_words.allocation_bytes,
        sat_add(
            post_dispatch_requirements.arena_allocation_bytes,
            post_dispatch_live_vector_bytes));
    if (post_dispatch_live_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            post_dispatch_live_bytes,
            &growth_failure)) {
        return std::nullopt;
    }
    if (raw.overflow) {
        return std::nullopt;
    }
    if (raw.dispatch_proof.materialization() != materialization
        || !raw.dispatch_proof.accepted()) {
        throw VulkanRuntimeError{
            "Vulkan batch sketch returned without matching accepted "
            "count/scan/emit proof"};
    }
    if (materialization == VulkanSketchHostMaterialization::device_only
        && !raw.query_authority.covers_device_output()) {
        throw VulkanRuntimeError{
            "device-only Vulkan batch sketch returned without runtime-issued "
            "device-output coverage authority"};
    }

    if (materialization == VulkanSketchHostMaterialization::host_and_device) {
        result.host_minimizers = decode_host_minimizer_bytes<MinimizerT>(
            std::span<const std::byte>{
                raw.host_minimizer_bytes.data(),
                raw.host_minimizer_bytes.size()},
            raw.count);
    } else if (!raw.host_minimizer_bytes.empty()) {
        throw VulkanRuntimeError{
            "device-only Vulkan batch sketch unexpectedly materialized host hashes"};
    }
    std::uint64_t query_minimizer_cursor = 0u;
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const QueryTileRange range = tile_ranges[query_index];
        std::uint64_t minimizer_count = 0u;
        for (std::uint32_t tile = 0u; tile < range.count; ++tile) {
            minimizer_count += raw.tile_counts[range.begin + tile];
        }
        const std::uint64_t minimizer_offset = range.count == 0u
            ? query_minimizer_cursor
            : static_cast<std::uint64_t>(raw.tile_offsets[range.begin]);
        if (minimizer_offset != query_minimizer_cursor) {
            throw VulkanRuntimeError{
                "Vulkan batch query minimizer slices are not contiguous"};
        }
        if (minimizer_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw VulkanRuntimeError{
                "Vulkan batch query minimizer count exceeds ABI"};
        }
        if (uploads[query_index].bp_size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw VulkanRuntimeError{"Vulkan batch query length exceeds ABI"};
        }
        result.query_records[query_index] = PhaseCSeedQueryRecordWire{
            .minimizer_offset = static_cast<std::uint32_t>(minimizer_offset),
            .minimizer_count = static_cast<std::uint32_t>(minimizer_count),
            .query_len = static_cast<std::uint32_t>(uploads[query_index].bp_size),
            .flags = 0u,
        };
        query_minimizer_cursor = sat_add(
            query_minimizer_cursor,
            minimizer_count);
    }
    if (query_minimizer_cursor != raw.count) {
        throw VulkanRuntimeError{
            "Vulkan batch query minimizer slices do not cover device output"};
    }

    const std::uint64_t retained_host_capacity_bytes =
        vector_capacity_bytes(result.host_minimizers);
    const std::uint64_t retained_query_record_capacity_bytes =
        vector_capacity_bytes(result.query_records);
    const std::uint64_t retained_bytes = sat_add(
        raw.minimizer_words.allocation_bytes,
        sat_add(
            retained_host_capacity_bytes,
            sat_add(
                retained_query_record_capacity_bytes,
                raw.query_authority.owned_capacity_bytes())));
    const std::uint64_t transient_vector_bytes = sat_add(
        sat_add(
            vector_capacity_bytes(raw.host_minimizer_bytes),
            vector_capacity_bytes(raw.tile_counts)),
        sat_add(
            vector_capacity_bytes(raw.tile_offsets),
            sat_add(
                vector_capacity_bytes(packed_sequence),
                sat_add(
                    vector_capacity_bytes(tiles),
                    vector_capacity_bytes(tile_ranges)))));
    const std::uint64_t final_live_peak_bytes = sat_add(
        post_dispatch_requirements.arena_allocation_bytes,
        sat_add(retained_bytes, transient_vector_bytes));
    if (final_live_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            final_live_peak_bytes,
            &growth_failure)) {
        return std::nullopt;
    }

    release_vector_storage(raw.host_minimizer_bytes);
    release_vector_storage(raw.tile_counts);
    release_vector_storage(raw.tile_offsets);
    release_vector_storage(packed_sequence);
    release_vector_storage(tiles);
    release_vector_storage(tile_ranges);
    memory_retention->permit.shrink_to(retained_bytes);

    auto storage = std::make_shared<VulkanDeviceMinimizerStorage<MinimizerT>>();
    storage->words = std::move(raw.minimizer_words);
    storage->batch_memory_retention = memory_retention;
    storage->count = raw.count;
    storage->capacity =
        materialization == VulkanSketchHostMaterialization::host_and_device
        ? max_minimizers
        : raw.count;
    result.device = std::move(storage);
    result.dispatch_proof = raw.dispatch_proof;
    result.query_authority = std::move(raw.query_authority);
    result.device_minimizer_count = raw.count;
    return result;
}

/// DOC0310 §12 — build the target minimizer index entirely on the GPU from the
/// target sequence (parallel sketch → device radix sort → race-free slot table).
/// Returns `std::nullopt` with `decline_out` populated for every typed shape,
/// admission, allocation, capacity, or invariant refusal. Vulkan API failures
/// still propagate. Alphabet-generic; only the shader specialises.
/// The returned `GpuBuiltIndex` owns the resident
/// slots/positions device buffers — the indexing layer adopts them.
template <class MinimizerT, class Seq>
[[nodiscard]] std::optional<GpuBuiltIndex> build_gpu_index_on_device(
    const Seq& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSketchQueryGate query_gate = {},
    VulkanSketchDecline* decline_out = nullptr)
{
    static const auto* _pc_idx_desc =
        bioseq::instr::register_region_static("phase_c_gpu_index_build");
    auto _pc_idx_scope = bioseq::instr::region(_pc_idx_desc);
    using bare = std::remove_cvref_t<Seq>;
    if (decline_out != nullptr) {
        *decline_out = VulkanSketchDecline{
            .bp_size = vulkan_decline_bp_size(seq),
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            VulkanSketchDeclineReason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (is_fragmented_sequence<bare>()) {
        if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
            decline(VulkanSketchDeclineReason::fragmented_sequence_unsupported);
            return std::nullopt;
        } else {
            using traits = typename bare::traits;
            using storage_unit = typename traits::storage_unit;
            if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                          traits::bits_per_char > 32) {
                decline(VulkanSketchDeclineReason::alphabet_unsupported);
                return std::nullopt;
            } else {
                constexpr std::size_t step_bits =
                    alphabet_kernel_kind_of_traits<traits>() == AlphabetKernelKind::generic
                        ? traits::bits_per_char : std::size_t{2};
                if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                    decline(VulkanSketchDeclineReason::kmer_size_unsupported);
                    return std::nullopt;
                }
                if (window_size == 0 || window_size > 256) {
                    decline(VulkanSketchDeclineReason::window_size_unsupported);
                    return std::nullopt;
                }

                const SegmentUploadDescriptor descriptor =
                    compute_segment_descriptor(seq, 0u, static_cast<std::size_t>(seq.size()));
                std::vector<storage_unit> scratch;
                scratch.resize(descriptor.hi_unit - descriptor.lo_unit);
                const storage_unit* base = seq.read_cptr(
                    seq.cbegin(),
                    std::span<storage_unit>{scratch.data(), scratch.size()},
                    scratch.size());
                const SequenceUploadView upload =
                    make_upload_view_from_window<bare>(seq, base, descriptor);
                const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
                const std::uint64_t scratch_capacity_bytes =
                    bioseq::parallelism::saturating_mul(
                        static_cast<std::uint64_t>(scratch.capacity()),
                        sizeof(storage_unit));
                auto release_scratch =
                    [scratch = std::move(scratch)]() mutable noexcept {
                        std::vector<storage_unit>{}.swap(scratch);
                    };
                return build_gpu_index_from_upload_on_device<MinimizerT>(
                    upload,
                    rank_lut,
                    window_size,
                    kmer_size,
                    query_gate,
                    decline_out,
                    scratch_capacity_bytes,
                    std::move(release_scratch));
            }
        }
    } else if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(VulkanSketchDeclineReason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            decline(VulkanSketchDeclineReason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == AlphabetKernelKind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(VulkanSketchDeclineReason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(VulkanSketchDeclineReason::window_size_unsupported);
                return std::nullopt;
            }

            const SequenceUploadView upload = make_sequence_upload_view(seq);
            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            return build_gpu_index_from_upload_on_device<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        }
    }
}

/// Build a dense GPU-resident minimizer index for one stitched segment window.
/// Positions in the returned index are local to `lo_unit`; callers that seed
/// against the full target must attach the global base to the resident handle.
template <class MinimizerT, class Seq>
[[nodiscard]] std::optional<GpuBuiltIndex> build_gpu_segment_index_on_device(
    const Seq& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSketchQueryGate query_gate = {},
    VulkanSketchDecline* decline_out = nullptr)
{
    static const auto* _pc_idx_desc =
        bioseq::instr::register_region_static("phase_c_gpu_segment_index_build");
    auto _pc_idx_scope = bioseq::instr::region(_pc_idx_desc);
    using bare = std::remove_cvref_t<Seq>;
    if (decline_out != nullptr) {
        *decline_out = VulkanSketchDecline{
            .bp_size = vulkan_decline_bp_size(seq),
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            VulkanSketchDeclineReason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (!vulkan_sketchable_segment_sequence<bare>
                  && !vulkan_sketchable_sequence<bare>) {
        decline(VulkanSketchDeclineReason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            decline(VulkanSketchDeclineReason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                alphabet_kernel_kind_of_traits<traits>() == AlphabetKernelKind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(VulkanSketchDeclineReason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(VulkanSketchDeclineReason::window_size_unsupported);
                return std::nullopt;
            }

            const SegmentUploadDescriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            if (decline_out != nullptr) {
                decline_out->bp_size = descriptor.window_bp;
            }
            if (descriptor.window_bp == 0u) {
                decline(VulkanSketchDeclineReason::empty_sequence);
                return std::nullopt;
            }
            if (descriptor.window_bp
                < static_cast<std::uint64_t>(window_size + kmer_size)) {
                decline(VulkanSketchDeclineReason::segment_window_too_short);
                return std::nullopt;
            }

            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                descriptor.lo_unit);
            const storage_unit* base = nullptr;
            std::vector<storage_unit> scratch;
            if constexpr (vulkan_sketchable_segment_sequence<bare>) {
                const std::size_t unit_count =
                    descriptor.hi_unit - descriptor.lo_unit;
                scratch.resize(unit_count);
                base = seq.read_cptr(
                    begin_iter,
                    std::span<storage_unit>{scratch.data(), scratch.size()},
                    unit_count);
            } else {
                base = seq.read_cptr(begin_iter);
            }
            const SequenceUploadView upload =
                make_upload_view_from_window<bare>(seq, base, descriptor);
            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            const std::uint64_t scratch_capacity_bytes =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(scratch.capacity()),
                    sizeof(storage_unit));
            std::function<void()> release_scratch;
            if (!scratch.empty()) {
                release_scratch =
                    [scratch = std::move(scratch)]() mutable noexcept {
                        std::vector<storage_unit>{}.swap(scratch);
                    };
            }
            return build_gpu_index_from_upload_on_device<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                query_gate,
                decline_out,
                scratch_capacity_bytes,
                std::move(release_scratch));
        }
    }
}

/// Collector-compatible wrapper for existing minimizer call sites. Feeds the
/// GPU sketch into `collector`; returns false only for pre-dispatch ineligible
/// shapes. Runtime failures propagate.
template <class MinimizerT, class Collector, class Seq>
[[nodiscard]] std::optional<VulkanSketchDispatchToken>
collect_minimizers_vulkan(
    Collector& collector, const Seq& seq,
    std::size_t window_size, std::size_t kmer_size,
    VulkanSketchDecline* decline_out = nullptr)
{
    auto result = collect_minimizers_vulkan_result<MinimizerT>(
        seq, window_size, kmer_size, decline_out);
    if (!result) {
        return std::nullopt;
    }
    for (const MinimizerT& minimizer : result->host_minimizers) {
        collector.collect(minimizer);
    }
    if (!result->dispatch_token.accepted()) {
        throw VulkanRuntimeError{
            "Vulkan minimizer sketch returned without a completed dispatch token"};
    }
    return result->dispatch_token;
}

// ===========================================================================
// DOC0345 Patch C — checked Vulkan segment (kmer, encoded_pos) pair path
// ===========================================================================

/// Decode the GPU's two parallel uint64 host arrays into (kmer, encoded_pos)
/// pairs. The runtime already emitted them in (kmer, encoded_pos) sorted order.
[[nodiscard]] inline std::vector<std::pair<std::uint64_t, std::uint64_t>>
decode_sorted_pair_bytes(
    std::span<const std::byte> key_bytes,
    std::span<const std::byte> value_bytes,
    std::uint64_t pair_count)
{
    if (pair_count
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)()
            / sizeof(std::pair<std::uint64_t, std::uint64_t>))) {
        throw VulkanRuntimeError{"Vulkan sorted-pair count exceeds host address space"};
    }
    const std::size_t count = static_cast<std::size_t>(pair_count);
    const std::size_t expected = count * sizeof(std::uint64_t);
    if (key_bytes.size() < expected || value_bytes.size() < expected) {
        throw VulkanRuntimeError{"Vulkan sorted-pair download was truncated"};
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t key = 0;
        std::uint64_t value = 0;
        std::memcpy(&key, key_bytes.data() + i * sizeof(std::uint64_t), sizeof(key));
        std::memcpy(&value, value_bytes.data() + i * sizeof(std::uint64_t), sizeof(value));
        out[i] = std::pair<std::uint64_t, std::uint64_t>{key, value};
    }
    return out;
}

/// Checked Vulkan segment pair path: stitches one window from lazy fragmented
/// storage, runs the fused tiled Phase-C minimizer sketch on the GPU for every
/// supported alphabet, globalises/filters to the core range, then returns pairs
/// sorted by
/// (kmer, encoded_pos).
/// Throws `VulkanRuntimeError` on every selected-GPU decline — no scalar fallback.
template <class Seq>
[[nodiscard]] VulkanOwnedSegmentPairs
collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
    const Seq& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    bioseq::storage::sequence_size_t emit_base_bp,
    bioseq::storage::sequence_size_t core_lo_bp,
    bioseq::storage::sequence_size_t core_hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSegmentPairExecutionProof* execution_proof_out = nullptr)
{
    using bare = std::remove_cvref_t<Seq>;
    using MinimizerT = bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    VulkanSegmentPairExecutionProof execution_proof{};
    if (execution_proof_out != nullptr) {
        *execution_proof_out = {};
    }
    VulkanSketchDecline path_decline{
        .bp_size = vulkan_decline_bp_size(seq),
        .window_size = window_size,
        .kmer_size = kmer_size,
    };
    if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
        path_decline.reason =
            VulkanSketchDeclineReason::fragmented_sequence_unsupported;
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer pair path", path_decline);
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            path_decline.reason = VulkanSketchDeclineReason::alphabet_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer pair path", path_decline);
        } else {
            if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
                path_decline.reason =
                    VulkanSketchDeclineReason::kmer_size_unsupported;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (!vulkan_minimizer_window_size_supported(window_size)) {
                path_decline.reason =
                    VulkanSketchDeclineReason::window_size_unsupported;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const std::uint64_t minimum_bases =
                static_cast<std::uint64_t>(window_size) + kmer_size - 1u;

            const SegmentUploadDescriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            path_decline.bp_size = descriptor.window_bp;
            if (descriptor.window_bp == 0u) {
                path_decline.reason = VulkanSketchDeclineReason::empty_sequence;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (descriptor.window_bp
                > static_cast<std::uint64_t>(MinimizerT::pos_field_mask)) {
                path_decline.reason =
                    VulkanSketchDeclineReason::segment_position_field_overflow;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (descriptor.window_bp
                < minimum_bases) {
                if (execution_proof_out != nullptr) {
                    *execution_proof_out = execution_proof;
                }
                return {};
            }

            auto& runtime = Runtime::instance();
            if (!hardware_device_available(runtime)) {
                path_decline.reason =
                    VulkanSketchDeclineReason::hardware_device_unavailable;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }

            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            const std::size_t lpu =
                std::max<std::size_t>(1u, traits::letters_per_unit);
            const std::size_t context_units =
                phase_c_segment_context_units<traits>(window_size, kmer_size);
            const std::uint64_t collected_limit =
                phase_c_admitted_collected_limit_bp<MinimizerT, traits>(
                    window_size,
                    kmer_size,
                    PhaseCSketchUse::sorted_pairs);
            const std::uint64_t context_bp =
                phase_c_segment_context_bp<traits>(window_size, kmer_size);
            constexpr std::uint64_t tile_safety_bp = 4096u;
            const std::uint64_t core_bp =
                collected_limit > context_bp * 2u + tile_safety_bp + 1u
                    ? collected_limit - context_bp * 2u - tile_safety_bp
                    : std::max<std::uint64_t>(1u, collected_limit / 2u);
            const std::size_t core_units = std::max<std::size_t>(
                1u, static_cast<std::size_t>(core_bp / lpu));
            const std::size_t descriptor_units =
                descriptor.hi_unit - descriptor.lo_unit;
            const std::size_t initial_window_count =
                descriptor.window_bp <= collected_limit
                    ? 1u
                    : static_cast<std::size_t>(ceil_div_u64(
                        static_cast<std::uint64_t>(descriptor_units),
                        static_cast<std::uint64_t>(core_units)));
            const std::size_t maximum_sub_units = std::min(
                descriptor_units,
                core_units + std::min(
                    descriptor_units,
                    context_units * 2u));
            const std::uint64_t maximum_sub_bp = std::min(
                descriptor.window_bp,
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(maximum_sub_units),
                    static_cast<std::uint64_t>(lpu)));
            const std::uint64_t maximum_dispatch_minimizers =
                estimate_vulkan_minimizer_capacity(
                    maximum_sub_bp, window_size);
            const std::uint64_t maximum_sequence_bytes = round_up4(
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(maximum_sub_units),
                    static_cast<std::uint64_t>(sizeof(storage_unit))));
            const std::uint64_t maximum_resident_requested_bytes =
                vulkan_sketch_resident_bytes<MinimizerT>(
                    maximum_dispatch_minimizers);
            const std::uint64_t maximum_host_bytes =
                bioseq::parallelism::saturating_mul(
                    bioseq::parallelism::saturating_mul(
                        maximum_dispatch_minimizers,
                        static_cast<std::uint64_t>(sizeof(MinimizerT))),
                    bioseq::parallelism::kVectorPreallocationHeadroomFactor);
            const PhaseCSketchMemoryPlan maximum_dispatch_plan =
                phase_c_sketch_memory_plan(
                    maximum_sub_bp,
                    maximum_sequence_bytes,
                    sizeof(VulkanRankLut),
                    maximum_dispatch_minimizers,
                    maximum_resident_requested_bytes,
                    PhaseCSketchUse::minimizer_words,
                    static_cast<std::uint32_t>(kmer_size),
                    static_cast<std::uint32_t>(window_size),
                    static_cast<std::uint32_t>(lpu),
                    runtime.features().max_workgroup_count_x);
            if (!maximum_dispatch_plan.valid) {
                path_decline.reason =
                    VulkanSketchDeclineReason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const PhaseCSketchBatchAllocationRequirements
                maximum_allocation_requirements =
                    runtime.phase_c_sketch_batch_allocation_requirements(
                        maximum_sequence_bytes,
                        maximum_dispatch_plan.tiles.tile_count,
                        static_cast<std::uint32_t>(
                            minimizer_wire_traits<MinimizerT>::word_kind),
                        minimizer_wire_traits<
                            MinimizerT>::words32_per_minimizer,
                        maximum_dispatch_minimizers);
            if (!maximum_allocation_requirements.valid) {
                path_decline.reason =
                    VulkanSketchDeclineReason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const std::uint64_t maximum_physical_arena_bytes =
                runtime.phase_c_arena_allocation_requirement(
                    maximum_dispatch_plan.arena_scaffold_bytes);
            const std::uint64_t maximum_host_transient_headroom_bytes =
                vulkan_exact_host_allocation_admission_bytes(
                    maximum_dispatch_plan.count_phase_host_transient_bytes);
            const std::uint64_t maximum_dispatch_phase_bytes =
                vulkan_sketch_dispatch_admission_bytes(
                    maximum_physical_arena_bytes,
                    maximum_host_transient_headroom_bytes,
                    0u,
                    maximum_allocation_requirements
                        .pessimistic_output_allocation_bytes);
            const std::uint64_t maximum_host_phase_bytes =
                vulkan_sketch_host_materialization_admission_bytes(
                    maximum_allocation_requirements
                        .pessimistic_output_allocation_bytes,
                    maximum_host_bytes);
            // The borrowed slice covers the larger of the mutually exclusive
            // dispatch and exact-host phases.  It must use the same physical
            // arena/output requirements as `sketch_uploaded_window_result`;
            // the former requested-byte estimate became stale as soon as a
            // reusable arena established a larger high-water allocation.
            const std::uint64_t dispatch_budget = std::max(
                maximum_dispatch_phase_bytes,
                maximum_host_phase_bytes);
            const std::uint64_t initial_filtered_count =
                estimate_vulkan_minimizer_capacity(
                    descriptor.window_bp, window_size);
            const auto headroom_bytes = []<class T>(std::uint64_t count) {
                return bioseq::parallelism::saturating_mul(
                    bioseq::parallelism::saturating_mul(
                        count, static_cast<std::uint64_t>(sizeof(T))),
                    bioseq::parallelism::kVectorPreallocationHeadroomFactor);
            };
            const std::uint64_t initial_owner_bytes = sat_add(
                dispatch_budget,
                sat_add(
                    headroom_bytes.template operator()<PhaseCSegmentWindow>(
                        initial_window_count),
                    sat_add(
                        headroom_bytes.template operator()<storage_unit>(
                            maximum_sub_units),
                        headroom_bytes.template operator()<
                            std::pair<std::uint64_t, std::uint64_t>>(
                                initial_filtered_count))));
            if (dispatch_budget == (std::numeric_limits<std::uint64_t>::max)()
                || initial_owner_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                path_decline.reason =
                    VulkanSketchDeclineReason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            auto permit =
                bioseq::parallelism::MemoryAdmissionController::instance()
                    .try_reserve(initial_owner_bytes);
            if (!permit) {
                path_decline.reason =
                    VulkanSketchDeclineReason::memory_admission_denied;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            struct SegmentPairOwner {
                bioseq::parallelism::MemoryPermit permit;
                std::vector<PhaseCSegmentWindow> windows;
                std::vector<storage_unit> scratch;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> filtered;
            } owner{
                .permit = std::move(*permit),
                .windows = {},
                .scratch = {},
                .filtered = {},
            };
            owner.scratch.reserve(maximum_sub_units);
            owner.filtered.reserve(static_cast<std::size_t>(
                initial_filtered_count));
            auto windows = phase_c_segment_windows<MinimizerT>(
                seq,
                descriptor,
                window_size,
                kmer_size,
                PhaseCSketchUse::sorted_pairs,
                collected_limit);
            owner.windows.swap(windows);

            const auto retained_owner_bytes = [&]() noexcept {
                std::uint64_t bytes = bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(owner.windows.capacity()),
                    static_cast<std::uint64_t>(sizeof(PhaseCSegmentWindow)));
                bytes = sat_add(bytes, bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(owner.scratch.capacity()),
                    static_cast<std::uint64_t>(sizeof(storage_unit))));
                return sat_add(bytes, bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(owner.filtered.capacity()),
                    static_cast<std::uint64_t>(
                        sizeof(std::pair<std::uint64_t, std::uint64_t>))));
            };
            const auto reconcile_owner = [&]() {
                const std::uint64_t exact = retained_owner_bytes();
                const std::uint64_t live = sat_add(exact, dispatch_budget);
                if (exact == (std::numeric_limits<std::uint64_t>::max)()
                    || live == (std::numeric_limits<std::uint64_t>::max)()
                    || live > owner.permit.bytes()) {
                    throw VulkanRuntimeError{
                        "Vulkan segment pair owner exceeded admitted capacity"};
                }
                owner.permit.shrink_to(live);
            };
            const auto ensure_owner_capacity = [&]<class T>(
                    std::vector<T>& values,
                    std::size_t required) {
                if (values.capacity() >= required) {
                    return;
                }
                const std::uint64_t peak = sat_add(
                    sat_add(retained_owner_bytes(), dispatch_budget),
                    headroom_bytes.template operator()<T>(required));
                if (peak == (std::numeric_limits<std::uint64_t>::max)()
                    || !owner.permit.grow_to(peak)) {
                    throw VulkanRuntimeError{
                        "Vulkan segment pair owner growth admission failed"};
                }
                values.reserve(required);
                reconcile_owner();
            };
            reconcile_owner();
            for (std::size_t window_index = 0u;
                 window_index < owner.windows.size();
                 ++window_index) {
                const PhaseCSegmentWindow window = owner.windows[window_index];
                const SegmentUploadDescriptor sub_descriptor =
                    compute_segment_descriptor(
                        seq, window.sub_lo_unit, window.sub_hi_unit);
                if (sub_descriptor.window_bp
                    < minimum_bases) {
                    continue;
                }
                const std::size_t unit_count =
                    sub_descriptor.hi_unit - sub_descriptor.lo_unit;
                if (unit_count > owner.scratch.capacity()) {
                    ensure_owner_capacity(owner.scratch, unit_count);
                }
                owner.scratch.resize(unit_count);
                auto begin_iter = seq.cbegin();
                begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                    sub_descriptor.lo_unit);
                const storage_unit* base = seq.read_cptr(
                    begin_iter,
                    std::span<storage_unit>{
                        owner.scratch.data(), owner.scratch.size()},
                    unit_count);
                const SequenceUploadView upload =
                    make_upload_view_from_window<bare>(seq, base, sub_descriptor);
                const std::uint64_t max_minimizers =
                    estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
                bool capacity_overflowed = false;
                VulkanSketchDecline decline{};
                auto result = sketch_uploaded_window_result<MinimizerT>(
                    upload,
                    rank_lut,
                    window_size,
                    kmer_size,
                    max_minimizers,
                    &capacity_overflowed,
                    {},
                    &decline,
                    std::addressof(owner.permit),
                    dispatch_budget);
                if (!result) {
                    if (capacity_overflowed
                        != (decline.reason
                            == VulkanSketchDeclineReason::
                                output_capacity_overflow)) {
                        throw VulkanRuntimeError{
                            "Vulkan segment minimizer pair decline/capacity "
                            "identity mismatch"};
                    }
                    PhaseCSegmentWindow left{};
                    PhaseCSegmentWindow right{};
                    if (vulkan_sketch_decline_allows_segment_split(
                            decline.reason)
                        && split_phase_c_segment_window(
                            seq,
                            descriptor,
                            window,
                            context_units,
                            left,
                            right)) {
                        execution_proof.record_gpu_window_split();
                        if (owner.windows.size()
                                > (std::numeric_limits<std::size_t>::max)() - 2u) {
                            throw VulkanRuntimeError{
                                "Vulkan segment window split count overflow"};
                        }
                        ensure_owner_capacity(
                            owner.windows, owner.windows.size() + 2u);
                        const auto insert_at =
                            static_cast<std::vector<PhaseCSegmentWindow>::difference_type>(
                                window_index + 1u);
                        owner.windows.insert(
                            owner.windows.begin() + insert_at, right);
                        owner.windows.insert(
                            owner.windows.begin() + insert_at, left);
                        continue;
                    }
                    const std::string stage = fmt::format(
                        "Vulkan segment minimizer pair build "
                        "units=[{},{}) segment_units=[{},{}) "
                        "emit_base={} core=[{},{})",
                        sub_descriptor.lo_unit,
                        sub_descriptor.hi_unit,
                        descriptor.lo_unit,
                        descriptor.hi_unit,
                        static_cast<std::uint64_t>(emit_base_bp),
                        static_cast<std::uint64_t>(core_lo_bp),
                        static_cast<std::uint64_t>(core_hi_bp));
                    throw_vulkan_strict_decline(stage, decline);
                }

                if (!result->dispatch_token.accepted()) {
                    throw VulkanRuntimeError{
                        "Vulkan segment minimizer pair path returned without "
                        "an accepted operation-originated dispatch token"};
                }
                execution_proof.record_completed_roundtrip(
                    result->host_minimizers.size());

                const std::uint64_t sub_emit_base_bp =
                    static_cast<std::uint64_t>(emit_base_bp)
                    + static_cast<std::uint64_t>(
                        sub_descriptor.lo_unit - descriptor.lo_unit)
                        * static_cast<std::uint64_t>(lpu);
                if (result->host_minimizers.size()
                        > (std::numeric_limits<std::size_t>::max)()
                            - owner.filtered.size()) {
                    throw VulkanRuntimeError{
                        "Vulkan segment filtered pair count overflow"};
                }
                ensure_owner_capacity(
                    owner.filtered,
                    owner.filtered.size() + result->host_minimizers.size());
                for (const MinimizerT& minimizer : result->host_minimizers) {
                    const bioseq::storage::sequence_size_t global_pos =
                        static_cast<bioseq::storage::sequence_size_t>(
                            sub_emit_base_bp + minimizer.last_pos());
                    if (global_pos < core_lo_bp || global_pos >= core_hi_bp) {
                        continue;
                    }
                    owner.filtered.emplace_back(
                        minimizer.kmer(),
                        (static_cast<std::uint64_t>(global_pos) << 1)
                            | static_cast<std::uint64_t>(minimizer.strand()));
                }
                {
                    std::vector<MinimizerT> empty;
                    result->host_minimizers.swap(empty);
                }
                result->host_permit.reset();
                result->device.reset();
            }
            // std::pair's default ordering sorts by (kmer, encoded_pos), exactly
            // the order the mps/mpf builders expect from a "sorted pair run".
            std::sort(owner.filtered.begin(), owner.filtered.end());
            owner.filtered.erase(
                std::unique(owner.filtered.begin(), owner.filtered.end()),
                owner.filtered.end());
            if (execution_proof_out != nullptr) {
                *execution_proof_out = execution_proof;
            }
            {
                std::vector<PhaseCSegmentWindow> empty_windows;
                std::vector<storage_unit> empty_scratch;
                owner.windows.swap(empty_windows);
                owner.scratch.swap(empty_scratch);
            }
            const std::uint64_t exact_pair_bytes =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(owner.filtered.capacity()),
                    static_cast<std::uint64_t>(
                        sizeof(std::pair<std::uint64_t, std::uint64_t>)));
            if (exact_pair_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || exact_pair_bytes > owner.permit.bytes()) {
                throw VulkanRuntimeError{
                    "Vulkan segment returned pairs exceeded admission"};
            }
            owner.permit.shrink_to(exact_pair_bytes);
            VulkanOwnedSegmentPairs output;
            output.pairs.swap(owner.filtered);
            output.permit = std::move(owner.permit);
            return output;
        }
    }
}

/// Collector-compatible segment sketch kept for tests and target alignment. It
/// is checked and throws when used as a selected Vulkan path.
template <class MinimizerT, class Collector, class Seq>
void collect_minimizers_vulkan_segment_or_throw(
    Collector& collector,
    const Seq& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSketchQueryGate query_gate = {})
{
    using bare = std::remove_cvref_t<Seq>;
    VulkanSketchDecline path_decline{
        .bp_size = vulkan_decline_bp_size(seq),
        .query_gate_words = static_cast<std::uint64_t>(
            query_gate.words.size()),
        .window_size = window_size,
        .kmer_size = kmer_size,
    };
    if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
        path_decline.reason =
            VulkanSketchDeclineReason::fragmented_sequence_unsupported;
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer sketch", path_decline);
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;
        using MinimizerLimit = bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            path_decline.reason = VulkanSketchDeclineReason::alphabet_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        } else if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
            path_decline.reason =
                VulkanSketchDeclineReason::kmer_size_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        } else if (!vulkan_minimizer_window_size_supported(window_size)) {
            path_decline.reason =
                VulkanSketchDeclineReason::window_size_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }

        const SegmentUploadDescriptor descriptor =
            compute_segment_descriptor(seq, lo_unit, hi_unit);
        path_decline.bp_size = descriptor.window_bp;
        if (descriptor.window_bp == 0u) {
            path_decline.reason = VulkanSketchDeclineReason::empty_sequence;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        if (descriptor.window_bp
            > static_cast<std::uint64_t>(MinimizerLimit::pos_field_mask)) {
            path_decline.reason =
                VulkanSketchDeclineReason::segment_position_field_overflow;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        if (descriptor.window_bp
            < static_cast<std::uint64_t>(window_size + kmer_size)) {
            return;
        }

        auto& runtime = Runtime::instance();
        if (!hardware_device_available(runtime)) {
            path_decline.reason =
                VulkanSketchDeclineReason::hardware_device_unavailable;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
        const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
        const std::uint64_t outer_base_bp =
            static_cast<std::uint64_t>(descriptor.lo_unit) * lpu;
        std::vector<PhaseCSegmentWindow> windows =
            phase_c_segment_windows<MinimizerT>(
                seq,
                descriptor,
                window_size,
                kmer_size,
                PhaseCSketchUse::minimizer_words);
        if (query_gate.enabled() && windows.size() != 1u) {
            path_decline.reason = VulkanSketchDeclineReason::
                query_gate_requires_single_window;
            throw_vulkan_strict_decline(
                "query-gated Vulkan segment minimizer sketch", path_decline);
        }
        const std::size_t context_units =
            phase_c_segment_context_units<traits>(window_size, kmer_size);
        for (std::size_t window_index = 0u;
             window_index < windows.size();
             ++window_index) {
            const PhaseCSegmentWindow window = windows[window_index];
            const SegmentUploadDescriptor sub_descriptor =
                compute_segment_descriptor(seq, window.sub_lo_unit, window.sub_hi_unit);
            if (sub_descriptor.window_bp == 0u
                || sub_descriptor.window_bp
                    < static_cast<std::uint64_t>(window_size + kmer_size)) {
                continue;
            }
            const std::size_t unit_count =
                sub_descriptor.hi_unit - sub_descriptor.lo_unit;
            std::vector<storage_unit> scratch;
            scratch.resize(unit_count);
            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                sub_descriptor.lo_unit);
            const storage_unit* base = seq.read_cptr(
                begin_iter,
                std::span<storage_unit>{scratch.data(), scratch.size()},
                unit_count);
            const SequenceUploadView upload =
                make_upload_view_from_window<bare>(seq, base, sub_descriptor);

            const std::uint64_t max_minimizers =
                estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
            bool capacity_overflowed = false;
            VulkanSketchDecline decline{};
            auto result = sketch_uploaded_window_result<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                max_minimizers,
                &capacity_overflowed,
                query_gate,
                &decline);
            if (!result) {
                if (capacity_overflowed
                    != (decline.reason
                        == VulkanSketchDeclineReason::
                            output_capacity_overflow)) {
                    throw VulkanRuntimeError{
                        "Vulkan segment minimizer sketch decline/capacity "
                        "identity mismatch"};
                }
                if (query_gate.enabled()) {
                    throw_vulkan_strict_decline(
                        "query-gated Vulkan segment minimizer sketch", decline);
                }
                PhaseCSegmentWindow left{};
                PhaseCSegmentWindow right{};
                if (vulkan_sketch_decline_allows_segment_split(decline.reason)
                    && split_phase_c_segment_window(
                        seq,
                        descriptor,
                        window,
                        context_units,
                        left,
                        right)) {
                    const auto insert_at =
                        static_cast<std::vector<PhaseCSegmentWindow>::difference_type>(
                            window_index + 1u);
                    windows.insert(windows.begin() + insert_at, right);
                    windows.insert(windows.begin() + insert_at, left);
                    continue;
                }
                const std::string stage = fmt::format(
                    "Vulkan segment minimizer sketch "
                    "units=[{},{}) outer_units=[{},{}) core_units=[{},{})",
                    sub_descriptor.lo_unit,
                    sub_descriptor.hi_unit,
                    descriptor.lo_unit,
                    descriptor.hi_unit,
                    window.core_lo_unit,
                    window.core_hi_unit);
                throw_vulkan_strict_decline(stage, decline);
            }

            const std::uint64_t core_lo_bp =
                static_cast<std::uint64_t>(window.core_lo_unit) * lpu;
            const std::uint64_t core_hi_bp =
                static_cast<std::uint64_t>(window.core_hi_unit) * lpu;
            const std::uint64_t sub_base_bp =
                static_cast<std::uint64_t>(sub_descriptor.lo_unit) * lpu;
            for (const MinimizerT& minimizer : result->host_minimizers) {
                const std::uint64_t abs_pos =
                    sub_base_bp + minimizer.last_pos();
                if (abs_pos < core_lo_bp || abs_pos >= core_hi_bp) {
                    continue;
                }
                collector.collect(MinimizerT{
                    minimizer.kmer(),
                    abs_pos - outer_base_bp,
                    minimizer.strand()});
            }
        }
    }
}

} // namespace bioseq::gpu::vulkan
