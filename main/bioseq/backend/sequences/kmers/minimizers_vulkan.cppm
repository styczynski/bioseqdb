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
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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

struct VulkanSketchQueryGate {
    std::span<const std::uint32_t> words{};
    std::uint32_t mask = 0u;

    [[nodiscard]] bool enabled() const noexcept { return !words.empty(); }
};

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
    PhaseCSketchUse use)
{
    using traits = typename std::remove_cvref_t<Seq>::traits;
    const std::uint64_t collected_limit =
        phase_c_admitted_collected_limit_bp<MinimizerT, traits>(
            window_size, kmer_size, use);
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

template <class MinimizerT>
struct VulkanDeviceMinimizerStorage {
    Buffer words;
    bioseq::parallelism::MemoryPermit permit;
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

template <class MinimizerT>
struct VulkanSketchResult {
    std::vector<MinimizerT> host_minimizers;
    std::optional<bioseq::parallelism::MemoryPermit> host_permit;
    std::shared_ptr<VulkanDeviceMinimizerStorage<MinimizerT>> device;
    bool overflow = false;

    [[nodiscard]] VulkanDeviceMinimizerSpan<MinimizerT> device_span() const noexcept {
        return VulkanDeviceMinimizerSpan<MinimizerT>{.owner = device};
    }
};

template <class MinimizerT>
struct VulkanBatchSketchResult {
    std::vector<MinimizerT> host_minimizers;
    std::vector<PhaseCSeedQueryRecordWire> query_records;
    std::shared_ptr<VulkanDeviceMinimizerStorage<MinimizerT>> device;
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
    // bound that keeps the single-pass sketch's capacity buffers and the
    // admission reservation small enough to avoid spurious memory pressure at
    // XXL_HIFI. An under-estimate is caught by the emit's capacity guard
    // (counters[1]) and reported as a GPU decline, never an out-of-bounds write.
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
    return sat_add(plan.arena_scaffold_bytes, plan.host_transient_bytes);
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
    VulkanSketchQueryGate query_gate = {})
{
    using bioseq::parallelism::MemoryAdmissionController;
    using bioseq::parallelism::saturating_mul;

    if (capacity_overflowed != nullptr) {
        *capacity_overflowed = false;
    }
    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()
        || upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    auto& runtime = Runtime::instance();
    if (!runtime.usable()) {
        return std::nullopt;
    }
    const std::uint64_t max_minimizers =
        capacity_bound == 0u
            ? estimate_vulkan_minimizer_capacity(upload.bp_size, window_size)
            : capacity_bound;
    if (max_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    if (query_gate.words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            static_cast<std::uint64_t>(sizeof(std::uint32_t)));
    const std::uint64_t resident_bytes =
        vulkan_sketch_resident_bytes<MinimizerT>(max_minimizers);
    const std::uint64_t host_minimizer_bytes =
        saturating_mul(max_minimizers, sizeof(MinimizerT));
    const std::uint64_t transient_bytes =
        vulkan_sketch_transient_reserved_bytes(
            upload.bp_size,
            upload.padded_byte_size,
            upload.letters_per_unit,
            window_size,
            kmer_size,
            max_minimizers,
            resident_bytes);

    const std::uint64_t dispatch_admission_bytes =
        sat_add(
            sat_add(sat_add(resident_bytes, transient_bytes), host_minimizer_bytes),
            query_gate_bytes);
    auto dispatch_permit =
        MemoryAdmissionController::instance().try_reserve(dispatch_admission_bytes);
    if (!dispatch_permit) {
        return std::nullopt;
    }

    try {
        std::vector<std::byte> padded_sequence(
            static_cast<std::size_t>(upload.padded_byte_size));
        if (upload.byte_size != 0u && upload.bytes != nullptr) {
            std::memcpy(
                padded_sequence.data(),
                upload.bytes,
                static_cast<std::size_t>(upload.byte_size));
        }

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

        RuntimeSketchResult raw = runtime.run_phase_c_sketch_roundtrip(
            std::span<const std::byte>{
                padded_sequence.data(),
                padded_sequence.size()},
            rank_lut,
            sketch_params,
            max_minimizers,
            query_gate.words,
            query_gate.mask);
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
            return std::nullopt;
        }

        auto host_minimizers = decode_host_minimizer_bytes<MinimizerT>(
            std::span<const std::byte>{
                raw.host_minimizer_bytes.data(),
                raw.host_minimizer_bytes.size()},
            raw.count);
        dispatch_permit->shrink_to(
            sat_add(resident_bytes, saturating_mul(raw.count, sizeof(MinimizerT))));
        raw.host_minimizer_bytes.clear();
        raw.host_minimizer_bytes.shrink_to_fit();

        auto storage = std::make_shared<VulkanDeviceMinimizerStorage<MinimizerT>>();
        storage->words = std::move(raw.minimizer_words);
        storage->permit = std::move(*dispatch_permit);
        storage->count = raw.count;
        storage->capacity = max_minimizers;

        return VulkanSketchResult<MinimizerT>{
            .host_minimizers = std::move(host_minimizers),
            .host_permit = std::nullopt,
            .device = std::move(storage),
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
    VulkanSketchQueryGate query_gate = {})
{
    using bioseq::parallelism::MemoryAdmissionController;
    using bioseq::parallelism::saturating_mul;

    if (upload.bp_size == 0 ||
        upload.bp_size > std::numeric_limits<std::uint32_t>::max() ||
        upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    auto& runtime = Runtime::instance();
    if (!runtime.usable()) {
        return std::nullopt;
    }

    static std::mutex phase_c_target_index_build_mutex;
    std::lock_guard<std::mutex> phase_c_target_index_build_guard{
        phase_c_target_index_build_mutex};

    const std::uint64_t max_minimizers =
        estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
    if (max_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
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
        upload.letters_per_unit);
    if (!memory_plan.valid) {
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            sizeof(std::uint32_t));
    const std::uint64_t admission_bytes =
        sat_add(memory_plan.admission_bytes, query_gate_bytes);
    auto& admission = MemoryAdmissionController::instance();
    auto permit = admission.try_reserve(admission_bytes);
    if (!permit) {
        bioseq_log<ERR>("[phase_c index] target-index build admission failed "
                        "(bp={} max_min={} admission_bytes={} sketch_admission_bytes={} "
                        "query_gate_bytes={} reserved_bytes={} allocatable_bytes={} "
                        "budget_ceiling={})",
                        upload.bp_size,
                        max_minimizers,
                        admission_bytes,
                        memory_plan.admission_bytes,
                        query_gate_bytes,
                        admission.reserved_bytes(),
                        admission.allocatable_after_reservations(),
                        admission.budget_ceiling());
        return std::nullopt;
    }

    try {
        std::vector<std::byte> padded_sequence(
            static_cast<std::size_t>(upload.padded_byte_size));
        if (upload.byte_size != 0u && upload.bytes != nullptr) {
            std::memcpy(
                padded_sequence.data(),
                upload.bytes,
                static_cast<std::size_t>(upload.byte_size));
        }
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
        GpuBuiltIndex built = runtime.run_phase_c_build_target_index(
            std::span<const std::byte>{padded_sequence.data(), padded_sequence.size()},
            rank_lut,
            sketch_params,
            max_minimizers,
            query_gate.words,
            query_gate.mask);
        if (!built.valid) {
            throw VulkanRuntimeError(fmt::format(
                "Vulkan GPU index build returned invalid (bp={}, max_min={})",
                upload.bp_size,
                max_minimizers));
        }
        return built;  // permit released here; indexing layer re-reserves.
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
    std::size_t kmer_size)
{
    using bare = std::remove_cvref_t<Seq>;
    if constexpr (is_fragmented_sequence<bare>()) {
        if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
            return std::nullopt;
        } else {
            using traits = typename bare::traits;
            using storage_unit = typename traits::storage_unit;
            if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0
                          || traits::bits_per_char > 32) {
                return std::nullopt;
            } else {
                constexpr std::size_t step_bits =
                    alphabet_kernel_kind_of_traits<traits>() == AlphabetKernelKind::generic
                        ? traits::bits_per_char
                        : std::size_t{2};
                if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                    return std::nullopt;
                }
                if (window_size == 0 || window_size > 256) {
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
                    kmer_size);
            }
        }
    } else if constexpr (!vulkan_sketchable_sequence<bare>) {
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0
                      || traits::bits_per_char > 32) {
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == AlphabetKernelKind::generic
                    ? traits::bits_per_char
                    : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                return std::nullopt;
            }
            const SequenceUploadView upload = make_sequence_upload_view(seq);
            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            return sketch_uploaded_window_result<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size);
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
    std::size_t kmer_size)
{
    using MinimizerT = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using bioseq::parallelism::MemoryAdmissionController;
    using bioseq::parallelism::saturating_mul;

    VulkanBatchSketchResult<MinimizerT> result{};
    result.query_records.resize(uploads.size());
    if (uploads.empty()) {
        return result;
    }
    if (kmer_size == 0 || kmer_size > 32 || window_size == 0 || window_size > 256) {
        return std::nullopt;
    }

    auto& runtime = Runtime::instance();
    if (!runtime.usable() || runtime.features().max_workgroup_count_x == 0u) {
        return std::nullopt;
    }

    struct QueryTileRange {
        std::uint32_t begin = 0;
        std::uint32_t count = 0;
    };

    std::vector<std::byte> packed_sequence;
    std::vector<PhaseCSketchTileWire> tiles;
    std::vector<QueryTileRange> tile_ranges(uploads.size());

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

        const std::size_t aligned_begin =
            (packed_sequence.size() + sizeof(std::uint32_t) - 1u)
            & ~(sizeof(std::uint32_t) - 1u);
        packed_sequence.resize(aligned_begin);
        const std::uint32_t seq_word_offset =
            static_cast<std::uint32_t>(aligned_begin / sizeof(std::uint32_t));
        const std::size_t append_begin = packed_sequence.size();
        packed_sequence.resize(
            append_begin + static_cast<std::size_t>(upload.padded_byte_size));
        std::memset(
            packed_sequence.data() + append_begin,
            0,
            static_cast<std::size_t>(upload.padded_byte_size));
        if (upload.byte_size != 0u && upload.bytes != nullptr) {
            std::memcpy(
                packed_sequence.data() + append_begin,
                upload.bytes,
                static_cast<std::size_t>(upload.byte_size));
        }

        const std::uint32_t tile_begin = static_cast<std::uint32_t>(tiles.size());
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
            .count = static_cast<std::uint32_t>(tiles.size() - tile_begin),
        };
    }

    if (tiles.empty() || max_minimizers == 0u) {
        return result;
    }

    const std::uint64_t resident_bytes =
        vulkan_sketch_resident_bytes<MinimizerT>(max_minimizers);
    const std::uint64_t host_minimizer_bytes =
        saturating_mul(max_minimizers, sizeof(MinimizerT));
    const std::uint64_t scaffold_bytes = sat_add(
        sat_add(
            static_cast<std::uint64_t>(packed_sequence.size()),
            static_cast<std::uint64_t>(tiles.size())
                * sizeof(PhaseCSketchTileWire)),
        sat_add(
            static_cast<std::uint64_t>(tiles.size()) * 8u,
            static_cast<std::uint64_t>(uploads.size())
                * sizeof(PhaseCSeedQueryRecordWire)));
    auto dispatch_permit = MemoryAdmissionController::instance().try_reserve(
        sat_add(sat_add(resident_bytes, host_minimizer_bytes), scaffold_bytes));
    if (!dispatch_permit) {
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

    RuntimeSketchBatchResult raw =
        runtime.run_phase_c_sketch_tile_batch_roundtrip(
            std::span<const std::byte>{
                packed_sequence.data(),
                packed_sequence.size()},
            rank_lut,
            sketch_params,
            std::span<const PhaseCSketchTileWire>{tiles.data(), tiles.size()},
            max_minimizers);
    if (raw.overflow) {
        result.overflow = true;
        return std::nullopt;
    }

    result.host_minimizers = decode_host_minimizer_bytes<MinimizerT>(
        std::span<const std::byte>{
            raw.host_minimizer_bytes.data(),
            raw.host_minimizer_bytes.size()},
        raw.count);
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const QueryTileRange range = tile_ranges[query_index];
        std::uint64_t minimizer_count = 0u;
        for (std::uint32_t tile = 0u; tile < range.count; ++tile) {
            minimizer_count += raw.tile_counts[range.begin + tile];
        }
        const std::uint32_t minimizer_offset =
            range.count == 0u ? 0u : raw.tile_offsets[range.begin];
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
            .minimizer_offset = minimizer_offset,
            .minimizer_count = static_cast<std::uint32_t>(minimizer_count),
            .query_len = static_cast<std::uint32_t>(uploads[query_index].bp_size),
            .flags = 0u,
        };
    }
    dispatch_permit->shrink_to(
        sat_add(resident_bytes, saturating_mul(raw.count, sizeof(MinimizerT))));
    raw.host_minimizer_bytes.clear();
    raw.host_minimizer_bytes.shrink_to_fit();

    auto storage = std::make_shared<VulkanDeviceMinimizerStorage<MinimizerT>>();
    storage->words = std::move(raw.minimizer_words);
    storage->permit = std::move(*dispatch_permit);
    storage->count = raw.count;
    storage->capacity = max_minimizers;
    result.device = std::move(storage);
    return result;
}

/// DOC0310 §12 — build the target minimizer index entirely on the GPU from the
/// target sequence (parallel sketch → device radix sort → race-free slot table).
/// Returns `std::nullopt` for shapes the GPU cannot run before dispatch.
/// Runtime failures propagate. Alphabet-generic; only the shader specialises.
/// The returned `GpuBuiltIndex` owns the resident
/// slots/positions device buffers — the indexing layer adopts them.
template <class MinimizerT, class Seq>
[[nodiscard]] std::optional<GpuBuiltIndex> build_gpu_index_on_device(
    const Seq& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    VulkanSketchQueryGate query_gate = {})
{
    static const auto* _pc_idx_desc =
        bioseq::instr::register_region_static("phase_c_gpu_index_build");
    auto _pc_idx_scope = bioseq::instr::region(_pc_idx_desc);
    using bare = std::remove_cvref_t<Seq>;
    if constexpr (is_fragmented_sequence<bare>()) {
        if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
            return std::nullopt;
        } else {
            using traits = typename bare::traits;
            using storage_unit = typename traits::storage_unit;
            if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                          traits::bits_per_char > 32) {
                return std::nullopt;
            } else {
                constexpr std::size_t step_bits =
                    alphabet_kernel_kind_of_traits<traits>() == AlphabetKernelKind::generic
                        ? traits::bits_per_char : std::size_t{2};
                if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                    return std::nullopt;
                }
                if (window_size == 0 || window_size > 256) {
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
                return build_gpu_index_from_upload_on_device<MinimizerT>(
                    upload,
                    rank_lut,
                    window_size,
                    kmer_size,
                    query_gate);
            }
        }
    } else if constexpr (!vulkan_sketchable_sequence<bare>) {
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == AlphabetKernelKind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) return std::nullopt;
            if (window_size == 0 || window_size > 256) return std::nullopt;

            const SequenceUploadView upload = make_sequence_upload_view(seq);
            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            return build_gpu_index_from_upload_on_device<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                query_gate);
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
    VulkanSketchQueryGate query_gate = {})
{
    static const auto* _pc_idx_desc =
        bioseq::instr::register_region_static("phase_c_gpu_segment_index_build");
    auto _pc_idx_scope = bioseq::instr::region(_pc_idx_desc);
    using bare = std::remove_cvref_t<Seq>;
    if constexpr (!vulkan_sketchable_segment_sequence<bare>
                  && !vulkan_sketchable_sequence<bare>) {
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                alphabet_kernel_kind_of_traits<traits>() == AlphabetKernelKind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                return std::nullopt;
            }

            const SegmentUploadDescriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            if (descriptor.window_bp == 0u ||
                descriptor.window_bp < static_cast<std::uint64_t>(window_size + kmer_size)) {
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
            return build_gpu_index_from_upload_on_device<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                query_gate);
        }
    }
}

/// Collector-compatible wrapper for existing minimizer call sites. Feeds the
/// GPU sketch into `collector`; returns false only for pre-dispatch ineligible
/// shapes. Runtime failures propagate.
template <class MinimizerT, class Collector, class Seq>
[[nodiscard]] bool collect_minimizers_vulkan(
    Collector& collector, const Seq& seq,
    std::size_t window_size, std::size_t kmer_size)
{
    auto result = collect_minimizers_vulkan_result<MinimizerT>(
        seq, window_size, kmer_size);
    if (!result) {
        return false;
    }
    for (const MinimizerT& minimizer : result->host_minimizers) {
        collector.collect(minimizer);
    }
    return true;
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
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>>
collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
    const Seq& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    bioseq::storage::sequence_size_t emit_base_bp,
    bioseq::storage::sequence_size_t core_lo_bp,
    bioseq::storage::sequence_size_t core_hi_bp,
    std::size_t window_size,
    std::size_t kmer_size)
{
    using bare = std::remove_cvref_t<Seq>;
    using MinimizerT = bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
        throw VulkanRuntimeError{
            "Vulkan segment minimizer pair path cannot read this sequence shape"};
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;
        using bioseq::parallelism::MemoryAdmissionController;
        using bioseq::parallelism::saturating_mul;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            throw VulkanRuntimeError{"Vulkan segment minimizer pair path rejected alphabet shape"};
        } else {
            if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
                throw VulkanRuntimeError{"Vulkan segment minimizer pair path rejected kmer size"};
            }
            if (!vulkan_minimizer_window_size_supported(window_size)) {
                throw VulkanRuntimeError{"Vulkan segment minimizer pair path rejected window size"};
            }

            const SegmentUploadDescriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            if (descriptor.window_bp == 0u
                || descriptor.window_bp
                    > static_cast<std::uint64_t>(MinimizerT::pos_field_mask)) {
                throw VulkanRuntimeError{"Vulkan segment minimizer pair window exceeds position field"};
            }
            if (descriptor.window_bp
                < static_cast<std::uint64_t>(window_size + kmer_size)) {
                return {};
            }

            auto& runtime = Runtime::instance();
            if (!runtime.usable()) {
                throw VulkanRuntimeError{fmt::format(
                    "Vulkan segment minimizer pair path selected but runtime is unusable: {}",
                    runtime.initialization_error())};
            }

            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            std::vector<std::pair<std::uint64_t, std::uint64_t>> filtered;
            std::vector<PhaseCSegmentWindow> windows =
                phase_c_segment_windows<MinimizerT>(
                    seq,
                    descriptor,
                    window_size,
                    kmer_size,
                    PhaseCSketchUse::sorted_pairs);
            const std::size_t lpu =
                std::max<std::size_t>(1u, traits::letters_per_unit);
            const std::size_t context_units =
                phase_c_segment_context_units<traits>(window_size, kmer_size);
            for (std::size_t window_index = 0u;
                 window_index < windows.size();
                 ++window_index) {
                const PhaseCSegmentWindow window = windows[window_index];
                const SegmentUploadDescriptor sub_descriptor =
                    compute_segment_descriptor(
                        seq, window.sub_lo_unit, window.sub_hi_unit);
                if (sub_descriptor.window_bp
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
                if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()
                    || upload.padded_byte_size
                        == std::numeric_limits<std::uint64_t>::max()) {
                    throw VulkanRuntimeError{
                        "Vulkan segment minimizer pair upload is too large"};
                }

                const std::uint64_t max_minimizers =
                    estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
                bool capacity_overflowed = false;
                auto result = sketch_uploaded_window_result<MinimizerT>(
                    upload,
                    rank_lut,
                    window_size,
                    kmer_size,
                    max_minimizers,
                    &capacity_overflowed);
                if (!result) {
                    if (capacity_overflowed) {
                        throw VulkanRuntimeError(fmt::format(
                            "Vulkan segment minimizer pair build overflowed "
                            "estimated Phase-C capacity (bp={} cap={})",
                            upload.bp_size,
                            max_minimizers));
                    }
                    PhaseCSegmentWindow left{};
                    PhaseCSegmentWindow right{};
                    if (split_phase_c_segment_window(
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
                    const std::uint64_t resident_bytes =
                        vulkan_sketch_resident_bytes<MinimizerT>(max_minimizers);
                    const std::uint64_t host_minimizer_bytes =
                        bioseq::parallelism::saturating_mul(
                            max_minimizers, sizeof(MinimizerT));
                    const std::uint64_t transient_bytes =
                        vulkan_sketch_transient_reserved_bytes(
                            upload.bp_size,
                            upload.padded_byte_size,
                            upload.letters_per_unit,
                            window_size,
                            kmer_size,
                            max_minimizers,
                            resident_bytes);
                    throw VulkanRuntimeError(fmt::format(
                        "Vulkan segment minimizer pair build returned invalid "
                        "(bp={} units=[{},{}) segment_units=[{},{}) "
                        "emit_base={} core=[{},{}) max_min={} resident_bytes={} "
                        "transient_bytes={} host_bytes={} reserved_bytes={} "
                        "allocatable_bytes={})",
                        upload.bp_size,
                        sub_descriptor.lo_unit,
                        sub_descriptor.hi_unit,
                        descriptor.lo_unit,
                        descriptor.hi_unit,
                        static_cast<std::uint64_t>(emit_base_bp),
                        static_cast<std::uint64_t>(core_lo_bp),
                        static_cast<std::uint64_t>(core_hi_bp),
                        max_minimizers,
                        resident_bytes,
                        transient_bytes,
                        host_minimizer_bytes,
                        MemoryAdmissionController::instance().reserved_bytes(),
                        MemoryAdmissionController::instance().allocatable_after_reservations()));
                }

                const std::uint64_t sub_emit_base_bp =
                    static_cast<std::uint64_t>(emit_base_bp)
                    + static_cast<std::uint64_t>(
                        sub_descriptor.lo_unit - descriptor.lo_unit)
                        * static_cast<std::uint64_t>(lpu);
                filtered.reserve(filtered.size() + result->host_minimizers.size());
                for (const MinimizerT& minimizer : result->host_minimizers) {
                    const bioseq::storage::sequence_size_t global_pos =
                        static_cast<bioseq::storage::sequence_size_t>(
                            sub_emit_base_bp + minimizer.last_pos());
                    if (global_pos < core_lo_bp || global_pos >= core_hi_bp) {
                        continue;
                    }
                    filtered.emplace_back(
                        minimizer.kmer(),
                        (static_cast<std::uint64_t>(global_pos) << 1)
                            | static_cast<std::uint64_t>(minimizer.strand()));
                }
                result->host_minimizers.clear();
                result->host_minimizers.shrink_to_fit();
                result->host_permit.reset();
                result->device.reset();
            }
            // std::pair's default ordering sorts by (kmer, encoded_pos), exactly
            // the order the mps/mpf builders expect from a "sorted pair run".
            std::sort(filtered.begin(), filtered.end());
            filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());
            return filtered;
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
    if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
        throw VulkanRuntimeError{
            "Vulkan segment minimizer sketch cannot read this sequence shape"};
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;
        using MinimizerLimit = bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            throw VulkanRuntimeError{"Vulkan segment minimizer sketch rejected alphabet shape"};
        } else if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
            throw VulkanRuntimeError{"Vulkan segment minimizer sketch rejected kmer size"};
        } else if (!vulkan_minimizer_window_size_supported(window_size)) {
            throw VulkanRuntimeError{"Vulkan segment minimizer sketch rejected window size"};
        }

        const SegmentUploadDescriptor descriptor =
            compute_segment_descriptor(seq, lo_unit, hi_unit);
        if (descriptor.window_bp == 0u
            || descriptor.window_bp
                > static_cast<std::uint64_t>(MinimizerLimit::pos_field_mask)) {
            throw VulkanRuntimeError{"Vulkan segment sketch window exceeds position field"};
        }
        if (descriptor.window_bp
            < static_cast<std::uint64_t>(window_size + kmer_size)) {
            return;
        }

        auto& runtime = Runtime::instance();
        if (!runtime.usable()) {
            throw VulkanRuntimeError{fmt::format(
                "Vulkan segment minimizer sketch selected but runtime is unusable: {}",
                runtime.initialization_error())};
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
            throw VulkanRuntimeError{
                "query-gated Vulkan segment sketch requires one admitted Phase-C window"};
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
            auto result = sketch_uploaded_window_result<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                max_minimizers,
                &capacity_overflowed,
                query_gate);
            if (!result) {
                using bioseq::parallelism::MemoryAdmissionController;
                if (capacity_overflowed) {
                    throw VulkanRuntimeError(fmt::format(
                        "Vulkan segment minimizer sketch overflowed estimated "
                        "Phase-C capacity (bp={} cap={})",
                        upload.bp_size,
                        max_minimizers));
                }
                if (query_gate.enabled()) {
                    throw VulkanRuntimeError{
                        "query-gated Vulkan segment sketch declined one-window Phase-C dispatch"};
                }
                PhaseCSegmentWindow left{};
                PhaseCSegmentWindow right{};
                if (split_phase_c_segment_window(
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
                const std::uint64_t resident_bytes =
                    vulkan_sketch_resident_bytes<MinimizerT>(max_minimizers);
                const std::uint64_t host_minimizer_bytes =
                    bioseq::parallelism::saturating_mul(
                        max_minimizers, sizeof(MinimizerT));
                const std::uint64_t transient_bytes =
                    vulkan_sketch_transient_reserved_bytes(
                        upload.bp_size,
                        upload.padded_byte_size,
                        upload.letters_per_unit,
                        window_size,
                        kmer_size,
                        max_minimizers,
                        resident_bytes);
                throw VulkanRuntimeError(fmt::format(
                    "Vulkan segment minimizer sketch selected but declined "
                    "(bp={} units=[{},{}) outer_units=[{},{}) core_units=[{},{}) "
                    "max_min={} phase_c=1 resident_bytes={} "
                    "transient_bytes={} host_bytes={} reserved_bytes={} "
                    "allocatable_bytes={})",
                    upload.bp_size,
                    sub_descriptor.lo_unit,
                    sub_descriptor.hi_unit,
                    descriptor.lo_unit,
                    descriptor.hi_unit,
                    window.core_lo_unit,
                    window.core_hi_unit,
                    max_minimizers,
                    resident_bytes,
                    transient_bytes,
                    host_minimizer_bytes,
                    MemoryAdmissionController::instance().reserved_bytes(),
                    MemoryAdmissionController::instance().allocatable_after_reservations()));
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
