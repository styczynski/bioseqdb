/**
 * @file minimizers_vulkan.cppm
 * @brief DOC0307 Phase B — Vulkan minimizer-sketch backend for
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
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;  // DOC0345 Patch C — bioseq::storage::sequence_size_t
import bioseq.instr;  // DOC0310 §21 — phase_c_* timing labels

export namespace bioseq::gpu::vulkan {

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

[[nodiscard]] inline std::uint64_t ceil_div_u64(
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
    const std::uint64_t base = window_size == 0 ? bp_size : (bp_size / window_size + 64u);
    return std::max<std::uint64_t>(64u, saturating_mul(base, 3u));
}

[[nodiscard]] inline std::uint64_t worst_case_vulkan_minimizer_capacity(
    std::uint64_t bp_size) noexcept
{
    return std::max<std::uint64_t>(64u, bp_size);
}

template <class MinimizerT>
[[nodiscard]] std::uint64_t vulkan_sketch_resident_bytes(
    std::uint64_t max_minimizers) noexcept
{
    return minimizer_wire_traits<MinimizerT>::device_bytes_for_count(max_minimizers);
}

[[nodiscard]] inline std::uint64_t vulkan_sketch_transient_reserved_bytes(
    std::uint64_t bp, std::uint64_t sequence_bytes, std::uint64_t max_minimizers,
    std::uint64_t minimizer_device_output_bytes)
{
    using bioseq::parallelism::saturating_mul;
    (void)max_minimizers;
    const std::uint64_t padded_seq = round_up4(sequence_bytes);
    const std::uint64_t candidates = saturating_mul(bp, sizeof(CandidateWire));
    const std::uint64_t counter_block = 8u;
    const std::uint64_t rank_lut_bytes = sizeof(VulkanRankLut);
    const std::uint64_t host_download = minimizer_device_output_bytes;
    const std::uint64_t staging = std::max({padded_seq, candidates, host_download});
    std::uint64_t total = 0;
    total = sat_add(total, padded_seq);
    total = sat_add(total, candidates);
    total = sat_add(total, counter_block);
    total = sat_add(total, rank_lut_bytes);
    total = sat_add(total, host_download);
    total = sat_add(total, staging);
    return total;
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
    bool fused_frontend,
    std::uint64_t capacity_bound = 0u)
{
    using bioseq::parallelism::MemoryAdmissionController;
    using bioseq::parallelism::saturating_mul;

    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()
        || upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    auto& runtime = Runtime::instance();
    if (!runtime.usable()) {
        return std::nullopt;
    }
    constexpr std::uint32_t candidate_local_size = 128u;
    if (ceil_div_u64(upload.bp_size, candidate_local_size)
        > runtime.features().max_workgroup_count_x) {
        return std::nullopt;
    }

    const std::uint64_t max_minimizers =
        capacity_bound == 0u
            ? estimate_vulkan_minimizer_capacity(upload.bp_size, window_size)
            : capacity_bound;
    if (max_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    const std::uint64_t resident_bytes =
        vulkan_sketch_resident_bytes<MinimizerT>(max_minimizers);
    const std::uint64_t host_minimizer_bytes =
        saturating_mul(max_minimizers, sizeof(MinimizerT));
    const std::uint64_t transient_bytes =
        vulkan_sketch_transient_reserved_bytes(
            upload.bp_size,
            upload.padded_byte_size,
            max_minimizers,
            resident_bytes);

    auto resident_permit =
        MemoryAdmissionController::instance().try_reserve(resident_bytes);
    if (!resident_permit) {
        return std::nullopt;
    }
    auto transient_permit =
        MemoryAdmissionController::instance().try_reserve(transient_bytes);
    if (!transient_permit) {
        return std::nullopt;
    }
    auto host_permit =
        MemoryAdmissionController::instance().try_reserve(host_minimizer_bytes);
    if (!host_permit) {
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

        RuntimeSketchResult raw =
            fused_frontend
                ? runtime.run_phase_c_sketch_roundtrip(
                    std::span<const std::byte>{
                        padded_sequence.data(),
                        padded_sequence.size()},
                    rank_lut,
                    sketch_params,
                    max_minimizers)
                : runtime.run_minimizer_sketch_roundtrip(
                    std::span<const std::byte>{
                        padded_sequence.data(),
                        padded_sequence.size()},
                    rank_lut,
                    sketch_params,
                    max_minimizers);
        if (raw.overflow) {
            if (capacity_bound != 0u) {
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
        host_permit->shrink_to(saturating_mul(raw.count, sizeof(MinimizerT)));
        raw.host_minimizer_bytes.clear();
        raw.host_minimizer_bytes.shrink_to_fit();

        auto storage = std::make_shared<VulkanDeviceMinimizerStorage<MinimizerT>>();
        storage->words = std::move(raw.minimizer_words);
        storage->permit = std::move(*resident_permit);
        storage->count = raw.count;
        storage->capacity = max_minimizers;

        return VulkanSketchResult<MinimizerT>{
            .host_minimizers = std::move(host_minimizers),
            .host_permit =
                std::optional<bioseq::parallelism::MemoryPermit>{
                    std::move(*host_permit)},
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
    std::size_t kmer_size)
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

    const std::uint64_t max_minimizers =
        estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
    if (max_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }

    // Resident index footprint (~ slots + positions); admitted via the
    // existing controller. Transient sort scratch is bounded by the arena.
    const std::uint64_t resident_bytes = saturating_mul(max_minimizers, 24u);
    auto permit = MemoryAdmissionController::instance().try_reserve(resident_bytes);
    if (!permit) {
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
            max_minimizers);
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
    std::size_t kmer_size,
    bool fused_frontend = false)
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
                    kmer_size,
                    fused_frontend);
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
                kmer_size,
                fused_frontend);
        }
    }
}

/// DOC0310 §12 — build the target minimizer index entirely on the GPU from the
/// target sequence (parallel sketch → device radix sort → race-free slot table).
/// Returns `std::nullopt` for shapes the GPU cannot run before dispatch.
/// Runtime failures propagate. Alphabet-generic; only the shader specialises.
/// The returned `GpuBuiltIndex` owns the resident
/// slots/positions device buffers — the indexing layer adopts them.
template <class MinimizerT, class Seq>
[[nodiscard]] std::optional<GpuBuiltIndex> build_gpu_index_on_device(
    const Seq& seq, std::size_t window_size, std::size_t kmer_size)
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
                    kmer_size);
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
                kmer_size);
        }
    }
}

/// Collector-compatible wrapper for existing minimizer call sites. Feeds the
/// GPU sketch into `collector`; returns false only for pre-dispatch ineligible
/// shapes. Runtime failures propagate.
template <class MinimizerT, class Collector, class Seq>
[[nodiscard]] bool collect_minimizers_vulkan(
    Collector& collector, const Seq& seq,
    std::size_t window_size, std::size_t kmer_size,
    bool fused_frontend = false)
{
    auto result = collect_minimizers_vulkan_result<MinimizerT>(
        seq, window_size, kmer_size, fused_frontend);
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
/// storage, runs the selected minimizer sketch on the GPU (dna4 uses the tiled
/// Phase-C path; dna5/generic stay on the exact Phase-B GPU path pending
/// DOC0348's ambiguous-base tile-boundary fix), globalises/filters to the core
/// range, then returns pairs sorted by (kmer, encoded_pos).
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

        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0
                      || traits::bits_per_char > 32) {
            throw VulkanRuntimeError{"Vulkan segment minimizer pair path rejected alphabet shape"};
        } else {
            constexpr std::size_t step_bits =
                alphabet_kernel_kind_of_traits<traits>() == AlphabetKernelKind::generic
                    ? traits::bits_per_char
                    : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                throw VulkanRuntimeError{"Vulkan segment minimizer pair path rejected kmer size"};
            }
            if (window_size == 0 || window_size > 256) {
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

            const std::size_t unit_count = descriptor.hi_unit - descriptor.lo_unit;
            std::vector<storage_unit> scratch;
            scratch.resize(unit_count);
            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(descriptor.lo_unit);
            const storage_unit* base = seq.read_cptr(
                begin_iter,
                std::span<storage_unit>{scratch.data(), scratch.size()},
                unit_count);
            const SequenceUploadView upload =
                make_upload_view_from_window<bare>(seq, base, descriptor);
            if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()
                || upload.padded_byte_size
                    == std::numeric_limits<std::uint64_t>::max()) {
                throw VulkanRuntimeError{"Vulkan segment minimizer pair upload is too large"};
            }

            auto& runtime = Runtime::instance();
            if (!runtime.usable()) {
                throw VulkanRuntimeError{"Vulkan segment minimizer pair path selected but runtime is unusable"};
            }
            constexpr std::uint32_t candidate_local_size = 128u;
            if (ceil_div_u64(upload.bp_size, candidate_local_size)
                > runtime.features().max_workgroup_count_x) {
                throw VulkanRuntimeError{"Vulkan segment minimizer pair path exceeds workgroup limit"};
            }

            // DOC0345 / BUGFIX 0348 — dna4 has no ambiguity/reset state, so the
            // tiled Phase-C GPU sketch is bit-exact and avoids the Phase-B
            // single-invocation emit pass for large index-build windows. dna5 and
            // generic alphabets stay on the exact Phase-B GPU sketch until the
            // tiled reset/init ownership bug is fixed for ambiguous bases.
            const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();
            const std::uint64_t max_minimizers =
                worst_case_vulkan_minimizer_capacity(upload.bp_size);
            const bool use_tiled_phase_c =
                upload.alphabet_kind == AlphabetKernelKind::dna4_nt4;
            auto result = sketch_uploaded_window_result<MinimizerT>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                use_tiled_phase_c,
                max_minimizers);
            if (!result) {
                const std::uint64_t resident_bytes =
                    vulkan_sketch_resident_bytes<MinimizerT>(max_minimizers);
                const std::uint64_t host_minimizer_bytes =
                    bioseq::parallelism::saturating_mul(max_minimizers, sizeof(MinimizerT));
                const std::uint64_t transient_bytes =
                    vulkan_sketch_transient_reserved_bytes(
                        upload.bp_size,
                        upload.padded_byte_size,
                        max_minimizers,
                        resident_bytes);
                throw VulkanRuntimeError(fmt::format(
                    "Vulkan segment minimizer pair build returned invalid "
                    "(bp={} units=[{},{}) emit_base={} core=[{},{}) max_min={} "
                    "resident_bytes={} transient_bytes={} host_bytes={} reserved_bytes={} allocatable_bytes={})",
                    upload.bp_size,
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

            std::vector<std::pair<std::uint64_t, std::uint64_t>> filtered;
            filtered.reserve(result->host_minimizers.size());
            for (const MinimizerT& minimizer : result->host_minimizers) {
                const bioseq::storage::sequence_size_t local_pos =
                    static_cast<bioseq::storage::sequence_size_t>(minimizer.last_pos());
                const bioseq::storage::sequence_size_t global_pos =
                    emit_base_bp + local_pos;
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
            // std::pair's default ordering sorts by (kmer, encoded_pos), exactly
            // the order the mps/mpf builders expect from a "sorted pair run".
            std::sort(filtered.begin(), filtered.end());
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
    std::size_t kmer_size)
{
    using bare = std::remove_cvref_t<Seq>;
    if constexpr (!vulkan_sketchable_segment_sequence<bare>) {
        throw VulkanRuntimeError{
            "Vulkan segment minimizer sketch cannot read this sequence shape"};
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;
        using MinimizerLimit = bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

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

        const std::size_t unit_count = descriptor.hi_unit - descriptor.lo_unit;
        std::vector<storage_unit> scratch;
        scratch.resize(unit_count);
        auto begin_iter = seq.cbegin();
        begin_iter += static_cast<bioseq::storage::sequence_size_t>(descriptor.lo_unit);
        const storage_unit* base = seq.read_cptr(
            begin_iter,
            std::span<storage_unit>{scratch.data(), scratch.size()},
            unit_count);
        const SequenceUploadView upload =
            make_upload_view_from_window<bare>(seq, base, descriptor);
        const VulkanRankLut rank_lut = make_vulkan_rank_lut<bare>();

        // DOC0345 / BUGFIX 0348 — use the NON-tiled Phase B frontend
        // (fused_frontend=false). The tiled fused frontend over-emits ~8% at
        // tile boundaries for dna5 (letters_per_unit=21, non-power-of-2); Phase B
        // is bit-exact against the scalar reference for every alphabet. Segment
        // sketching is not on the GPU-index hot path, so the tiling speedup is
        // not needed here.
        auto result = sketch_uploaded_window_result<MinimizerT>(
            upload,
            rank_lut,
            window_size,
            kmer_size,
            false,
            worst_case_vulkan_minimizer_capacity(upload.bp_size));
        if (!result) {
            throw VulkanRuntimeError{"Vulkan segment minimizer sketch selected but declined"};
        }
        for (const MinimizerT& minimizer : result->host_minimizers) {
            collector.collect(minimizer);
        }
    }
}

} // namespace bioseq::gpu::vulkan
