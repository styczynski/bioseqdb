/**
 * @file contract.cppm
 * @brief Minimizer record layouts, input checks, and Vulkan rejection reasons.
 */
module;

#include <libassert/assert.hpp>

#include <cstring>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.vulkan_minimizers:contract;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.hashers.base;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.instrumentation;

export namespace bioseq::gpu::vulkan {

/** @brief Query-gate identity backed by shared read-only bitmap words. */
using vulkan_sketch_query_gate_identity = gpu_index_gate_identity;

/**
 * @brief Builds an identity for a query-gate bitmap.
 *
 * Copies words into shared read-only storage unless canonical_words supplies
 * a matching owner The identity includes the gate mask and a digest  of the
 * words. An empty gate returns a disabled identity.
 * @throws vulkan_runtime_error If canonical_words has different, contents.
 */
[[nodiscard]] inline vulkan_sketch_query_gate_identity
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
            throw vulkan_runtime_error(
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
    return vulkan_sketch_query_gate_identity{
        .enabled = true,
        .word_count = static_cast<std::uint32_t>(words.size()),
        .mask = mask,
        .digest_lo = lo,
        .digest_hi = hi,
        .canonical_words = std::move(canonical_words),
    };
}

/** @brief Borrowed query-gate words and their optional cached identity. */
struct vulkan_sketch_query_gate {
    std::span<const std::uint32_t> words{};
    std::uint32_t mask = 0u;
    vulkan_sketch_query_gate_identity identity{};
    bool identity_valid = false;

    /** @brief Reports whether the gate contains any words. */
    [[nodiscard]] bool enabled() const noexcept { return !words.empty(); }

    /**
     * @brief Checks the cached gate identity or builds one from the current words.
     *
     * An empty gate returns a disabled identity.
     * @throws vulkan_runtime_error If a supplied identity fails the word, mask,
     * or layout checks.
     */
    [[nodiscard]] vulkan_sketch_query_gate_identity canonical_identity() const {
        if (!enabled()) return {};
        const bool same_frozen_storage = identity.canonical_words
            && words.data() == identity.canonical_words->data()
            && words.size() == identity.canonical_words->size();
        if (identity_valid && identity.enabled
            && identity.word_count == words.size() && identity.mask == mask
            && identity.is_well_formed()
            // A span over the identity owner already has the same words. Other spans
            // need a content comparison.
            && (same_frozen_storage
                || std::equal(
                    words.begin(), words.end(),
                    identity.canonical_words->begin()))) {
            return identity;
        }
        if (identity_valid) {
            throw vulkan_runtime_error(
                "query gate supplied identity does not match exact words");
        }
        return canonical_vulkan_sketch_query_gate_identity(words, mask);
    }
};

/** @brief Reasons a Vulkan minimizer request can be rejected. */
enum class vulkan_sketch_decline_reason : std::uint8_t {
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

/** @brief Rejection reason with input dimensions and memory-budget details. */
struct vulkan_sketch_decline {
    vulkan_sketch_decline_reason reason = vulkan_sketch_decline_reason::none;
    std::uint64_t bp_size = 0;
    std::uint64_t padded_sequence_bytes = 0;
    std::uint64_t max_minimizers = 0;
    std::uint64_t query_gate_words = 0;
    std::uint64_t window_size = 0;
    std::uint64_t kmer_size = 0;
    bioseq::parallelism::memory_admission_failure admission{};

    /** @brief Reports whether the route was rejected. */
    [[nodiscard]] constexpr bool declined() const noexcept {
        return reason != vulkan_sketch_decline_reason::none;
    }
};

/**
 * @brief Reads sequence  length for a rejection message.
 * @return bp_size() when available, otherwise zero.
 */
template <class p_sequence>
[[nodiscard]] inline std::uint64_t vulkan_decline_bp_size(
    const p_sequence& seq) noexcept
{
    if constexpr (requires { seq.bp_size(); }) {
        return static_cast<std::uint64_t>(seq.bp_size());
    } else {
        return 0u;
    }
}

/**
 * @brief Returns the diagnostic name of a Vulkan rejection reson.
 * @return A static string, or "unknown" for an unrecognized value.
 */
[[nodiscard]] inline constexpr std::string_view
vulkan_sketch_decline_reason_name(vulkan_sketch_decline_reason reason) noexcept
{
    switch (reason) {
        case vulkan_sketch_decline_reason::none:
            return "none";
        case vulkan_sketch_decline_reason::empty_sequence:
            return "empty_sequence";
        case vulkan_sketch_decline_reason::segment_window_too_short:
            return "segment_window_too_short";
        case vulkan_sketch_decline_reason::segment_position_field_overflow:
            return "segment_position_field_overflow";
        case vulkan_sketch_decline_reason::target_index_invalid_shape:
            return "target_index_invalid_shape";
        case vulkan_sketch_decline_reason::device_allocation_failed:
            return "device_allocation_failed";
        case vulkan_sketch_decline_reason::target_index_build_invariant_failed:
            return "target_index_build_invariant_failed";
        case vulkan_sketch_decline_reason::fragmented_sequence_unsupported:
            return "fragmented_sequence_unsupported";
        case vulkan_sketch_decline_reason::sequence_type_unsupported:
            return "sequence_type_unsupported";
        case vulkan_sketch_decline_reason::alphabet_unsupported:
            return "alphabet_unsupported";
        case vulkan_sketch_decline_reason::kmer_size_unsupported:
            return "kmer_size_unsupported";
        case vulkan_sketch_decline_reason::window_size_unsupported:
            return "window_size_unsupported";
        case vulkan_sketch_decline_reason::sequence_size_exceeds_u32:
            return "sequence_size_exceeds_u32";
        case vulkan_sketch_decline_reason::padded_sequence_size_overflow:
            return "padded_sequence_size_overflow";
        case vulkan_sketch_decline_reason::hardware_device_unavailable:
            return "hardware_device_unavailable";
        case vulkan_sketch_decline_reason::minimizer_capacity_exceeds_u32:
            return "minimizer_capacity_exceeds_u32";
        case vulkan_sketch_decline_reason::query_gate_exceeds_u32:
            return "query_gate_exceeds_u32";
        case vulkan_sketch_decline_reason::query_gate_requires_single_window:
            return "query_gate_requires_single_window";
        case vulkan_sketch_decline_reason::memory_plan_invalid:
            return "memory_plan_invalid";
        case vulkan_sketch_decline_reason::memory_admission_denied:
            return "memory_admission_denied";
        case vulkan_sketch_decline_reason::output_capacity_overflow:
            return "output_capacity_overflow";
    }
    return "unknown";
}

/**
 * @brief Formats a rejection reason and input sizes on one line.
 *
 * Memory-budget rejections also include the requested bytes and the memory
 * snapshot used for the decision.
 */
[[nodiscard]] inline std::string format_vulkan_sketch_decline(
    const vulkan_sketch_decline& decline)
{
    const std::string_view reason =
        vulkan_sketch_decline_reason_name(decline.reason);
    if (decline.reason == vulkan_sketch_decline_reason::memory_admission_denied) {
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

/**
 * @brief Reports whether a smaller segment may avoid this rejection.
 *
 * Allows retry for sequence-size, output-capacity, and memory-budget limits.
 */
[[nodiscard]] inline constexpr bool
vulkan_sketch_decline_allows_segment_split(
    vulkan_sketch_decline_reason reason) noexcept
{
    switch (reason) {
        case vulkan_sketch_decline_reason::sequence_size_exceeds_u32:
        case vulkan_sketch_decline_reason::padded_sequence_size_overflow:
        case vulkan_sketch_decline_reason::minimizer_capacity_exceeds_u32:
        case vulkan_sketch_decline_reason::memory_plan_invalid:
        case vulkan_sketch_decline_reason::memory_admission_denied:
        case vulkan_sketch_decline_reason::output_capacity_overflow:
            return true;
        case vulkan_sketch_decline_reason::none:
        case vulkan_sketch_decline_reason::empty_sequence:
        case vulkan_sketch_decline_reason::segment_window_too_short:
        case vulkan_sketch_decline_reason::segment_position_field_overflow:
        case vulkan_sketch_decline_reason::target_index_invalid_shape:
        case vulkan_sketch_decline_reason::device_allocation_failed:
        case vulkan_sketch_decline_reason::target_index_build_invariant_failed:
        case vulkan_sketch_decline_reason::fragmented_sequence_unsupported:
        case vulkan_sketch_decline_reason::sequence_type_unsupported:
        case vulkan_sketch_decline_reason::alphabet_unsupported:
        case vulkan_sketch_decline_reason::kmer_size_unsupported:
        case vulkan_sketch_decline_reason::window_size_unsupported:
        case vulkan_sketch_decline_reason::hardware_device_unavailable:
        case vulkan_sketch_decline_reason::query_gate_exceeds_u32:
        case vulkan_sketch_decline_reason::query_gate_requires_single_window:
            return false;
    }
    return false;
}

/**
 * @brief Maps a  target-index build failure to a minimizer rejection reason
 *
 * An unspecified failure is reported as a target-index consistency failure.
 */
[[nodiscard]] inline constexpr vulkan_sketch_decline_reason
vulkan_target_index_failure_decline_reason(
    gpu_index_build_failure reason) noexcept
{
    switch (reason) {
        case gpu_index_build_failure::none:
            return vulkan_sketch_decline_reason::
                target_index_build_invariant_failed;
        case gpu_index_build_failure::invalid_shape:
            return vulkan_sketch_decline_reason::target_index_invalid_shape;
        case gpu_index_build_failure::minimizer_capacity_exceeds_u32:
            return vulkan_sketch_decline_reason::minimizer_capacity_exceeds_u32;
        case gpu_index_build_failure::memory_plan_invalid:
            return vulkan_sketch_decline_reason::memory_plan_invalid;
        case gpu_index_build_failure::device_arena_allocation_failed:
        case gpu_index_build_failure::device_buffer_allocation_failed:
            return vulkan_sketch_decline_reason::device_allocation_failed;
        case gpu_index_build_failure::count_peak_memory_admission_denied:
        case gpu_index_build_failure::execution_peak_memory_admission_denied:
            return vulkan_sketch_decline_reason::memory_admission_denied;
        case gpu_index_build_failure::peak_allocation_exceeds_admission:
            return vulkan_sketch_decline_reason::memory_plan_invalid;
        case gpu_index_build_failure::output_capacity_overflow:
            return vulkan_sketch_decline_reason::output_capacity_overflow;
        case gpu_index_build_failure::index_invariant_failed:
            return vulkan_sketch_decline_reason::
                target_index_build_invariant_failed;
    }
    return vulkan_sketch_decline_reason::target_index_build_invariant_failed;
}

/**
 * @brief Throws a Vulkan error describing the rejected operation.
 * @param stage Operation name included in the message.
 * @throws vulkan_runtime_error Always.
 */
[[noreturn]] inline void throw_vulkan_strict_decline(
    std::string_view stage,
    const vulkan_sketch_decline& decline)
{
    throw vulkan_runtime_error(fmt::format(
        "{} selected but strict Vulkan execution was rejected ({})",
        stage,
        format_vulkan_sketch_decline(decline)));
}

/** Describes how a minimizer record is stored in device buffers. */
template <class p_record_type, class = void>
struct minimizer_wire_traits;

/**
 * Describes 16-, 32-, and 64-bit single-word minimizer records. Supplies
 * field widths and buffer sizes for the shader interface.
 */
template <class p_record_type>
    requires requires (p_record_type m) {
        typename p_record_type::storage_type;
        { m.raw_value() };
    }
struct minimizer_wire_traits<p_record_type> {
    using minimizer_type = p_record_type;
    using storage_type = typename p_record_type::storage_type;
    static constexpr bool k_is_128_bit = false;
    static constexpr std::uint32_t k_storage_bits =
        static_cast<std::uint32_t>(sizeof(storage_type) * 8u);
    static constexpr std::uint32_t k_storage_bytes =
        static_cast<std::uint32_t>(sizeof(storage_type));
    static constexpr bool k_is_packed_uint16 = k_storage_bits == 16u;
    static constexpr std::uint32_t k_words32_per_minimizer =
        k_storage_bits == 16u ? 0u :
        k_storage_bits == 32u ? 1u : 2u;
    static constexpr std::uint32_t k_position_bits = p_record_type::layout_type::k_position_bits;
    static constexpr std::uint32_t k_hash_bits = p_record_type::layout_type::k_hash_bits;
    static constexpr std::uint32_t k_hash_shift = p_record_type::k_hash_shift;
    static constexpr std::uint64_t k_position_mask =
        static_cast<std::uint64_t>(p_record_type::k_position_field_mask);

    static constexpr minimizer_word_kind k_word_kind =
        k_storage_bits == 16u ? minimizer_word_kind::u16 :
        k_storage_bits == 32u ? minimizer_word_kind::u32 :
                              minimizer_word_kind::u64;

    /**
     * @brief Sizes a record count in 32-bit device words.
     *
     * Two 16-bit records share a word. Other records use one or  two words each.
     * Size arithmetic saturates at uint64_t maxium.
     */
    [[nodiscard]] static constexpr std::uint64_t device_words32_for_count(
        std::uint64_t n) noexcept
    {
        return k_storage_bits == 16u
            ? (bioseq::common::saturating_add(n, 1u) / 2u)
            : bioseq::common::saturating_mul(
                n, k_words32_per_minimizer);
    }

    /**
     * @brief Returns device storage bytes for n records.
     *
     * Includes padding to complete the last 32-bit word. Arithmetic saturates
     * at uint64_t maximum.
     */
    [[nodiscard]] static constexpr std::uint64_t device_bytes_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::common::saturating_mul(
            device_words32_for_count(n), sizeof(std::uint32_t));
    }
};

/** Describes 128-bit minimizers stored as, four 32-bit device words. */
template <class p_record_type>
    requires requires (p_record_type m) {
        { m.data_low_64() } -> std::convertible_to<std::uint64_t>;
        { m.data_high_64() } -> std::convertible_to<std::uint64_t>;
    }
struct minimizer_wire_traits<p_record_type> {
    using minimizer_type = p_record_type;
    static constexpr bool k_is_128_bit = true;
    static constexpr std::uint32_t k_storage_bits = 128u;
    static constexpr std::uint32_t k_storage_bytes = 16u;
    static constexpr bool k_is_packed_uint16 = false;
    static constexpr std::uint32_t k_words32_per_minimizer = 4u;
    static constexpr std::uint32_t k_position_bits = p_record_type::layout_type::k_position_bits;
    // The high word reserves eight bits for the span, leaving at most 56 hash
    // bits. Use that stored width so shader comparisons agree with host records.
    static constexpr std::uint32_t k_hash_bits =
        p_record_type::layout_type::k_hash_bits < 56u ? static_cast<std::uint32_t>(p_record_type::layout_type::k_hash_bits) : 56u;
    static constexpr std::uint32_t k_hash_shift = 8u;
    static constexpr std::uint64_t k_position_mask =
        p_record_type::k_position_field_mask;
    static constexpr minimizer_word_kind k_word_kind =
        minimizer_word_kind::u64x2;

    /**
     * @brief Returns the 32-bit device-word count for n 128-bit records.
     *
     * The count saturates at uint64_t maximum.
     */
    [[nodiscard]] static constexpr std::uint64_t device_words32_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::common::saturating_mul(
            n, k_words32_per_minimizer);
    }

    /**
     * @brief Returns device bytes for n 128-bit records.
     *
     * The  count saturates at uint64_t maximum.
     */
    [[nodiscard]] static constexpr std::uint64_t device_bytes_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::common::saturating_mul(
            device_words32_for_count(n), sizeof(std::uint32_t));
    }
};

/**
 * @brief Shader mode that maps nucleotide ranks through the uploaded lookup table.
 *
 * The shader interface calls this value dna5_nt4, but it supports other
 * nucleotide alphabets through the same table. Its numeric value is 1.
 */
inline constexpr alphabet_kernel_kind k_lut_normalized_nucleotide_kernel =
    alphabet_kernel_kind::dna5_nt4;
static_assert(
    static_cast<std::uint8_t>(k_lut_normalized_nucleotide_kernel) == 1u);

/**
 * @brief Chooses how the shader decodes an alphabet
 *
 * Standard two-bit DNA4 uses ranks directly. Other nucleotide alphabets use
 * a lookup table, and non-nucleotide alphabets use generic rank handling.
 */
template <class p_alphabet,
          std::size_t p_bits_per_char,
          std::size_t p_alphabet_size>
[[nodiscard]] constexpr alphabet_kernel_kind
alphabet_kernel_kind_for_shape() noexcept {
    if constexpr (seqan3::nucleotide_alphabet<p_alphabet>) {
        // Direct decoding requires the known DNA4 rank  order. Four-rank alphabets
        // can assign different ranks to the bases.
        if constexpr (std::same_as<p_alphabet, seq::dna4>
                      && p_bits_per_char == 2u && p_alphabet_size == 4u) {
            return alphabet_kernel_kind::dna4_nt4;
        }
        return k_lut_normalized_nucleotide_kernel;
    }
    return alphabet_kernel_kind::generic;
}

/** Chooses the shader alphabet mode from a packed sequence type. */
template <class p_sequence>
[[nodiscard]] constexpr alphabet_kernel_kind choose_alphabet_kernel() {
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    return alphabet_kernel_kind_for_shape<
        typename traits::alphabet_type,
        traits::bits_per_char,
        traits::alphabet_size>();
}

/** Chooses the shader alphabet mode from packed sequence traits. */
template <class p_traits>
[[nodiscard]] constexpr alphabet_kernel_kind alphabet_kernel_kind_of_traits() noexcept {
    return alphabet_kernel_kind_for_shape<
        typename p_traits::alphabet_type,
        p_traits::bits_per_char,
        p_traits::alphabet_size>();
}

/**
 * @brief Checks that the shader can decode the packed units and alphabet.
 *
 * Requires 8-, 16-, 32-, or 64-bit units, at moxt 256 ranks, and consistent
 * symbol widths, masks, and symbols per unit.
 */
[[nodiscard]] constexpr bool vulkan_minimizer_packed_shape_supported(
    std::size_t unit_bits,
    std::size_t bits_per_char,
    std::size_t letters_per_unit,
    std::size_t alphabet_size,
    std::uint64_t char_mask) noexcept {
    const bool unit_width_supported =
        unit_bits == 8u || unit_bits == 16u || unit_bits == 32u
        || unit_bits == 64u;
    if (!unit_width_supported || alphabet_size == 0u || alphabet_size > 256u
        || bits_per_char == 0u || bits_per_char > 32u
        || bits_per_char > unit_bits) {
        return false;
    }
    const std::uint64_t expected_char_mask =
        bits_per_char == 32u
            ? std::numeric_limits<std::uint32_t>::max()
            : (std::uint64_t{1u} << bits_per_char) - 1u;
    const std::uint64_t rank_capacity =
        std::uint64_t{1u} << bits_per_char;
    return letters_per_unit == unit_bits / bits_per_char
        && letters_per_unit != 0u && char_mask == expected_char_mask
        && alphabet_size <= rank_capacity;
}

/**
 * @brief Checks alphabet and storage traits against the shader decoder limits.
 *
 * Returns false when required trait fields are missing.
 */
template <class p_traits>
[[nodiscard]] constexpr bool vulkan_minimizer_alphabet_shape_supported() noexcept {
    if constexpr (!requires {
        typename p_traits::storage_unit;
        p_traits::bits_per_char;
        p_traits::alphabet_size;
        p_traits::char_mask;
    }) {
        return false;
    } else {
        constexpr std::size_t unit_bits =
            sizeof(typename p_traits::storage_unit) * 8u;
        constexpr std::size_t bits_per_char = p_traits::bits_per_char;
        constexpr std::size_t packed_letters_per_unit =
            bits_per_char == 0u ? 0u : unit_bits / bits_per_char;
        return vulkan_minimizer_packed_shape_supported(
            unit_bits,
            bits_per_char,
            packed_letters_per_unit,
            p_traits::alphabet_size,
            static_cast<std::uint64_t>(p_traits::char_mask));
    }
}

/**
 * @brief Checks whether a k-mer fits the shader rolling state.
 *
 * Nucleotides use two bits per base. Generic ranks use bits_per_char.
 * At  most 32 symbols and 64 bits are supported.
 */
[[nodiscard]] constexpr bool vulkan_minimizer_kmer_size_supported(
    alphabet_kernel_kind alphabet_kind,
    std::size_t bits_per_char,
    std::size_t kmer_size) noexcept {
    if (bits_per_char == 0u || bits_per_char > 32u) {
        return false;
    }
    const std::size_t step_bits =
        alphabet_kind == alphabet_kernel_kind::generic
            ? bits_per_char
            : std::size_t{2u};
    return kmer_size != 0u && kmer_size <= 64u / step_bits
        && kmer_size <= 32u;
}

/**
 * @brief Checks a k-mer length using the alphabet and packing traits.
 *
 * Both the packed input and the shader rolling state must support it.
 */
template <class p_traits>
[[nodiscard]] constexpr bool vulkan_minimizer_kmer_size_supported(
    std::size_t kmer_size) noexcept
{
    if constexpr (!vulkan_minimizer_alphabet_shape_supported<p_traits>()) {
        return false;
    } else {
        return vulkan_minimizer_kmer_size_supported(
            alphabet_kernel_kind_of_traits<p_traits>(),
            p_traits::bits_per_char,
            kmer_size);
    }
}

/** Accepts minimizer windows containing 1 through 256 k-mers. */
[[nodiscard]] constexpr bool vulkan_minimizer_window_size_supported(
    std::size_t window_size) noexcept
{
    return window_size != 0 && window_size <= 256;
}

/** Checks the packed alphabet metadata required for Vulkan minimizer sketching. */
template <class p_traits>
[[nodiscard]] constexpr bool is_vulkan_minimizer_sketch_supported() noexcept
{
    return vulkan_minimizer_alphabet_shape_supported<p_traits>();
}

/**
 * @brief Builds shader lookup tables for forward and complemented ranks.
 *
 * Nucleotide alphabets map, to A/C/G/T codes through nt4_normalize. Ambiguous
 * symbols map to code 4. For generic alphabets, only ranks 0 through 3 and
 * complement ranks in that range are valid codes.
 */
template <class p_sequence>
[[nodiscard]] vulkan_rank_lut make_vulkan_rank_lut() {
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    using alphabet_type = typename traits::alphabet_type;
    vulkan_rank_lut lut{};
    for (std::uint32_t rank = 0; rank < traits::alphabet_size; ++rank) {
        std::uint32_t code = 4u;
        std::uint32_t rc_code = 4u;
        if constexpr (seqan3::nucleotide_alphabet<alphabet_type>) {
            code = bioseq::simd::nt4_normalize<alphabet_type>(
                static_cast<std::uint8_t>(rank));
            const std::uint32_t complement_rank =
                static_cast<std::uint32_t>(traits::letter_complement(
                    static_cast<typename traits::storage_unit>(rank)));
            rc_code = complement_rank < traits::alphabet_size
                ? bioseq::simd::nt4_normalize<alphabet_type>(
                    static_cast<std::uint8_t>(complement_rank))
                : 4u;
        } else {
            const std::uint32_t comp_rank = static_cast<std::uint32_t>(
                traits::letter_complement(static_cast<typename traits::storage_unit>(rank)));
            code = rank < 4u ? rank : 4u;
            rc_code = comp_rank < 4u ? comp_rank : 4u;
        }
        lut.rank_to_code[rank] = code;
        lut.rank_to_rc_code[rank] = rc_code;
    }
    return lut;
}

} // namespace bioseq::gpu::vulkan
