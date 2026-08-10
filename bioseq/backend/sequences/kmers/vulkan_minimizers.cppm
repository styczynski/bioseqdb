/**
 * @file vulkan_minimizers.cppm
 * @brief Vulkan minimizer backend for resident and lazy compact sequences.
 *
 * Owns minimizer wire traits, direct packed-sequence upload views, alphabet
 * rank tables, memory admission, host materialization, and reusable device
 * output. Unsupported sequence, alphabet, and coordinate shapes are reported
 * explicitly and never routed through a silent CPU fallback.
 */
module;
#include <cstdio>

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

export module bioseq.sequences.kmers.vulkan_minimizers;

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

using vulkan_sketch_query_gate_identity = gpu_index_gate_identity;

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

struct vulkan_sketch_query_gate {
    std::span<const std::uint32_t> words{};
    std::uint32_t mask = 0u;
    vulkan_sketch_query_gate_identity identity{};
    bool identity_valid = false;

    [[nodiscard]] bool enabled() const noexcept { return !words.empty(); }
    [[nodiscard]] vulkan_sketch_query_gate_identity canonical_identity() const {
        if (!enabled()) return {};
        const bool same_frozen_storage = identity.canonical_words
            && words.data() == identity.canonical_words->data()
            && words.size() == identity.canonical_words->size();
        if (identity_valid && identity.enabled
            && identity.word_count == words.size() && identity.mask == mask
            && identity.is_well_formed()
            // The pointer fast path is valid only for the immutable owner
            // carried in the identity; raw/mutable spans still exact-compare.
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

/** Fail-closed diagnostic for a selected Vulkan minimizer route. */
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

struct vulkan_sketch_decline {
    vulkan_sketch_decline_reason reason = vulkan_sketch_decline_reason::none;
    std::uint64_t bp_size = 0;
    std::uint64_t padded_sequence_bytes = 0;
    std::uint64_t max_minimizers = 0;
    std::uint64_t query_gate_words = 0;
    std::uint64_t window_size = 0;
    std::uint64_t kmer_size = 0;
    bioseq::parallelism::memory_admission_failure admission{};

    [[nodiscard]] constexpr bool declined() const noexcept {
        return reason != vulkan_sketch_decline_reason::none;
    }
};

/// Read a sequence length for diagnostics without strengthening the public
/// probe templates: unsupported sequence shapes must still instantiate and
/// return a typed `sequence_type_unsupported` decline.
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

/// Return true only when reducing a fragmented upload can remove the exact
/// predicate that declined the current dispatch.  Shape/device/query-gate
/// failures are invariant under a target split and must fail immediately;
/// retrying them would both waste I/O and obscure the original cause.
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

/// Translate the runtime's cycle-free target-index result status into the
/// shared strict accelerator decline vocabulary consumed by pipeline callers.
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

/// Raise the shared fail-closed selected-Vulkan error while preserving the
/// machine-readable decline identity and its admission-time memory snapshot.
[[noreturn]] inline void throw_vulkan_strict_decline(
    std::string_view stage,
    const vulkan_sketch_decline& decline)
{
    throw vulkan_runtime_error(fmt::format(
        "{} selected but strict Vulkan execution was rejected ({})",
        stage,
        format_vulkan_sketch_decline(decline)));
}

/** Device serialization traits for one minimizer representation. */
template <class p_record_type, class = void>
struct minimizer_wire_traits;

/// 16/32/64-bit single-integer `minimizer_representation` specialisation.
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

    [[nodiscard]] static constexpr std::uint64_t device_words32_for_count(
        std::uint64_t n) noexcept
    {
        return k_storage_bits == 16u
            ? (bioseq::common::saturating_add(n, 1u) / 2u)
            : bioseq::common::saturating_mul(
                n, k_words32_per_minimizer);
    }

    [[nodiscard]] static constexpr std::uint64_t device_bytes_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::common::saturating_mul(
            device_words32_for_count(n), sizeof(std::uint32_t));
    }
};

/// 128-bit (`u64x2`) `minimizer_representation` fallback specialisation.
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
    // The 128-bit layout packs `high_ = (kmer << 8) | span` into a 64-bit word,
    // so only the low 56 bits of the kmer survive the span shift. The emit
    // shader masks/compares with this effective width so its window-minimum
    // selection matches `minimizer_representation<L,p_kmer_size>::operator<=>` (which compares the
    // stored, truncated kmer), not the wider pre-truncation `layout_type::k_hash_bits`.
    static constexpr std::uint32_t k_hash_bits =
        p_record_type::layout_type::k_hash_bits < 56u ? static_cast<std::uint32_t>(p_record_type::layout_type::k_hash_bits) : 56u;
    static constexpr std::uint32_t k_hash_shift = 8u;
    static constexpr std::uint64_t k_position_mask =
        p_record_type::k_position_field_mask;
    static constexpr minimizer_word_kind k_word_kind =
        minimizer_word_kind::u64x2;

    [[nodiscard]] static constexpr std::uint64_t device_words32_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::common::saturating_mul(
            n, k_words32_per_minimizer);
    }

    [[nodiscard]] static constexpr std::uint64_t device_bytes_for_count(
        std::uint64_t n) noexcept
    {
        return bioseq::common::saturating_mul(
            device_words32_for_count(n), sizeof(std::uint32_t));
    }
};

/// Pick the lowest-level shader specialisation key from the sequence alphabet.
template <class p_sequence>
[[nodiscard]] constexpr alphabet_kernel_kind choose_alphabet_kernel() {
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
        if constexpr (traits::bits_per_char == 2 && traits::alphabet_size == 4) {
            return alphabet_kernel_kind::dna4_nt4;
        }
        if constexpr (traits::bits_per_char == 3 && traits::alphabet_size == 5) {
            return alphabet_kernel_kind::dna5_nt4;
        }
    }
    return alphabet_kernel_kind::generic;
}

/** Select the shader alphabet specialization directly from sequence traits. */
template <class p_traits>
[[nodiscard]] constexpr alphabet_kernel_kind alphabet_kernel_kind_of_traits() noexcept {
    if constexpr (seqan3::nucleotide_alphabet<typename p_traits::alphabet_type>) {
        if constexpr (p_traits::bits_per_char == 2 && p_traits::alphabet_size == 4) {
            return alphabet_kernel_kind::dna4_nt4;
        }
        if constexpr (p_traits::bits_per_char == 3 && p_traits::alphabet_size == 5) {
            return alphabet_kernel_kind::dna5_nt4;
        }
    }
    return alphabet_kernel_kind::generic;
}

template <class p_traits>
[[nodiscard]] constexpr bool vulkan_minimizer_alphabet_shape_supported() noexcept {
    return p_traits::alphabet_size <= 256 && p_traits::bits_per_char != 0
        && p_traits::bits_per_char <= 32;
}

template <class p_traits>
[[nodiscard]] constexpr bool vulkan_minimizer_kmer_size_supported(
    std::size_t kmer_size) noexcept
{
    if constexpr (!vulkan_minimizer_alphabet_shape_supported<p_traits>()) {
        return false;
    } else {
        constexpr std::size_t step_bits =
            alphabet_kernel_kind_of_traits<p_traits>() == alphabet_kernel_kind::generic
                ? p_traits::bits_per_char
                : std::size_t{2};
        return kmer_size != 0 && kmer_size <= 64 / step_bits && kmer_size <= 32;
    }
}

[[nodiscard]] constexpr bool vulkan_minimizer_window_size_supported(
    std::size_t window_size) noexcept
{
    return window_size != 0 && window_size <= 256;
}

template <class p_traits>
[[nodiscard]] constexpr bool is_vulkan_minimizer_sketch_supported() noexcept
{
    return vulkan_minimizer_alphabet_shape_supported<p_traits>();
}

/** Build the rank-to-forward/reverse nt4 lookup used by the shader. */
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

#ifdef BIOSEQ_TEST_INDEXES
struct vulkan_admitted_window_test_snapshot {
  std::uint64_t admitted_materializations = 0u;
  std::uint64_t live_admitted_windows = 0u;
  std::uint64_t live_admitted_permit_bytes = 0u;
  std::uintptr_t live_admitted_buffer_address = 0u;
  std::uint64_t backend_thread_materializations = 0u;
  std::uint64_t off_backend_materializations = 0u;
};

namespace admitted_window_test_detail {
inline std::atomic<std::uint8_t> enabled{0u};
inline std::atomic<std::uint64_t> admitted_materializations{0u};
inline std::atomic<std::uint64_t> live_admitted_windows{0u};
inline std::atomic<std::uint64_t> live_admitted_permit_bytes{0u};
inline std::atomic<std::uintptr_t> live_admitted_buffer_address{0u};
inline std::atomic<std::uint64_t> backend_thread_materializations{0u};
inline std::atomic<std::uint64_t> off_backend_materializations{0u};
inline std::thread::id backend_thread{};
} // namespace admitted_window_test_detail

inline void arm_vulkan_admitted_window_test_seam() noexcept {
  using namespace admitted_window_test_detail;
  enabled.store(0u, std::memory_order_release);
  admitted_materializations.store(0u, std::memory_order_relaxed);
  live_admitted_windows.store(0u, std::memory_order_relaxed);
  live_admitted_permit_bytes.store(0u, std::memory_order_relaxed);
  live_admitted_buffer_address.store(0u, std::memory_order_relaxed);
  backend_thread_materializations.store(0u, std::memory_order_relaxed);
  off_backend_materializations.store(0u, std::memory_order_relaxed);
  backend_thread = std::this_thread::get_id();
  enabled.store(1u, std::memory_order_release);
}

inline void reset_vulkan_admitted_window_test_seam() noexcept {
  admitted_window_test_detail::enabled.store(0u, std::memory_order_release);
}

[[nodiscard]] inline vulkan_admitted_window_test_snapshot
vulkan_admitted_window_test_snapshot() noexcept {
  using namespace admitted_window_test_detail;
  return {
      .admitted_materializations =
          admitted_materializations.load(std::memory_order_acquire),
      .live_admitted_windows =
          live_admitted_windows.load(std::memory_order_acquire),
      .live_admitted_permit_bytes =
          live_admitted_permit_bytes.load(std::memory_order_acquire),
      .live_admitted_buffer_address =
          live_admitted_buffer_address.load(std::memory_order_acquire),
      .backend_thread_materializations =
          backend_thread_materializations.load(std::memory_order_acquire),
      .off_backend_materializations =
          off_backend_materializations.load(std::memory_order_acquire),
  };
}

[[nodiscard]] inline bool note_vulkan_admitted_window_begin(
    std::uint64_t permit_bytes, std::uintptr_t buffer_address) noexcept {
  using namespace admitted_window_test_detail;
  if (enabled.load(std::memory_order_acquire) == 0u) {
    return false;
  }
  admitted_materializations.fetch_add(1u, std::memory_order_relaxed);
  live_admitted_windows.fetch_add(1u, std::memory_order_release);
  live_admitted_permit_bytes.store(permit_bytes, std::memory_order_release);
  live_admitted_buffer_address.store(buffer_address,
                                     std::memory_order_release);
  if (std::this_thread::get_id() == backend_thread) {
    backend_thread_materializations.fetch_add(1u, std::memory_order_relaxed);
  } else {
    off_backend_materializations.fetch_add(1u, std::memory_order_relaxed);
  }
  return true;
}

inline void note_vulkan_admitted_window_end(bool tracked) noexcept {
  if (tracked) {
    using namespace admitted_window_test_detail;
    live_admitted_buffer_address.store(0u, std::memory_order_release);
    live_admitted_permit_bytes.store(0u, std::memory_order_release);
    live_admitted_windows.fetch_sub(
        1u, std::memory_order_release);
  }
}
#endif

namespace admitted_sequence_detail {

struct borrowed_packed_header {
  const void *data = nullptr;
  bioseq::storage::sequence_size_t bp_size = 0;
  std::uint8_t storage_unit_byte_width = 0;
  std::uint16_t padding_letters = 0;
};

template <sequence_alphabet p_alphabet> struct borrowed_u32_instance {
  using alphabet_type = p_alphabet;
  using storage_unit = std::uint32_t;
  using compact_sequence_stdc_type = borrowed_packed_header;

  static constexpr std::size_t letter_bit_width =
      bioseq::storage::lazy_sequence_view<
          p_alphabet>::traits::logical_bits_per_symbol;
  static constexpr std::size_t byte_width = sizeof(storage_unit);
  static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/// Construct a typed u32 range only when the descriptor is backed by a real
/// u32 object array (native resident storage or admitted u32 materialization).
/// Reinterpreted byte descriptors are routed through byte_upload_range below
/// and must never reach this function.
template <sequence_alphabet p_alphabet, class p_function>
decltype(auto) with_typed_u32_range(
    const typename bioseq::storage::lazy_sequence_view<
        p_alphabet>::resident_packed_sequence &descriptor,
    p_function &&fn) {
  using instance = borrowed_u32_instance<p_alphabet>;
  using range_type = compact_sequence_range<instance>;
  borrowed_packed_header header{
      .data = descriptor.units,
      .bp_size = descriptor.logical_length,
      .storage_unit_byte_width = descriptor.storage_unit_width,
      .padding_letters = descriptor.trailing_padding,
  };
    range_type range{header};
  return std::forward<p_function>(fn)(range);
}

/// Unit iterator for a packed upload whose backing allocation has a different
/// effective type.  Arithmetic is performed exclusively on object-
/// representation bytes while each iterator step still means one logical u32
/// packed unit to the Vulkan geometry code.
template <std::size_t p_unit_bytes>
struct byte_packed_unit_iterator {
  const std::byte *address = nullptr;

  byte_packed_unit_iterator &operator+=(std::size_t units) noexcept {
    if (units != 0u) {
      DEBUG_ASSERT(address != nullptr,
                   "nonempty byte upload iterator requires backing bytes");
      address += units * p_unit_bytes;
    }
    return *this;
  }

  friend byte_packed_unit_iterator operator+(
      byte_packed_unit_iterator it, std::size_t units) noexcept {
    it += units;
    return it;
  }

  friend bool operator==(const byte_packed_unit_iterator &,
                         const byte_packed_unit_iterator &) = default;
};

/// Upload-only sequence surface for a layout-compatible packed byte region.
/// It deliberately has no dereference or `operator[]`: callers can derive
/// byte addresses and biological/unit geometry, but cannot form a typed u32
/// load from the u64-backed allocation.
template <sequence_alphabet p_alphabet>
class byte_upload_range {
public:
  using instance = borrowed_u32_instance<p_alphabet>;
  using traits = typename compact_sequence_range<instance>::traits;
  using iterator = byte_packed_unit_iterator<sizeof(std::uint32_t)>;
  static constexpr bool is_byte_addressed_upload_only = true;

private:
  using descriptor_type = typename bioseq::storage::lazy_sequence_view<
      p_alphabet>::resident_packed_sequence;
  descriptor_type descriptor_{};

public:
  explicit byte_upload_range(const descriptor_type &descriptor) noexcept
      : descriptor_(descriptor) {
    DEBUG_ASSERT(
        descriptor.storage_unit_width == sizeof(std::uint32_t),
        "byte-addressed Vulkan range requires canonical u32 geometry");
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return descriptor_.unit_count;
  }
  [[nodiscard]] std::size_t byte_size() const noexcept {
    return descriptor_.unit_count * sizeof(std::uint32_t);
  }
  [[nodiscard]] std::size_t bp_size() const noexcept {
    return static_cast<std::size_t>(descriptor_.logical_length);
  }
  [[nodiscard]] std::size_t padding() const noexcept {
    return descriptor_.trailing_padding;
  }
  [[nodiscard]] iterator cbegin() const noexcept {
    return iterator{static_cast<const std::byte *>(descriptor_.units)};
  }
  [[nodiscard]] iterator cend() const noexcept {
    return cbegin() + size();
  }
  // BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN byte_upload_range_address_adapter
  [[nodiscard]] const std::byte *read_cptr(const iterator &it) const noexcept {
    return it.address;
  }
  // BIOSEQ_PRIVATE_PACKED_ACCESS_END byte_upload_range_address_adapter
};

template <sequence_alphabet p_alphabet> struct canonical_u32_window {
  using owner_type = bioseq::storage::admitted_packed_sequence<p_alphabet>;
  using descriptor_type = typename bioseq::storage::lazy_sequence_view<
      p_alphabet>::resident_packed_sequence;

  std::optional<owner_type> owner;
  descriptor_type descriptor{};
  bioseq::storage::sequence_resident_borrow_kind borrow_kind{
      bioseq::storage::sequence_resident_borrow_kind::none};
#ifdef BIOSEQ_TEST_INDEXES
  bool test_admitted_window_tracked = false;
#endif

  explicit canonical_u32_window(owner_type &&admitted)
      : owner(std::in_place, std::move(admitted)),
        descriptor(owner->descriptor())
#ifdef BIOSEQ_TEST_INDEXES
        ,
        test_admitted_window_tracked(note_vulkan_admitted_window_begin(
            owner->test_admission_bytes(), owner->test_storage_address()))
#endif
  {
  }

  explicit canonical_u32_window(
      const descriptor_type &borrowed,
      bioseq::storage::sequence_resident_borrow_kind kind) noexcept
      : descriptor(borrowed), borrow_kind(kind) {
    DEBUG_ASSERT(
        kind != bioseq::storage::sequence_resident_borrow_kind::none,
        "borrowed Vulkan canonical window requires a truthful borrow kind");
  }

  canonical_u32_window(canonical_u32_window &&other) noexcept
      : owner(std::move(other.owner)), descriptor(other.descriptor),
        borrow_kind(other.borrow_kind)
#ifdef BIOSEQ_TEST_INDEXES
        ,
        test_admitted_window_tracked(
            std::exchange(other.test_admitted_window_tracked, false))
#endif
  {
  }
  ~canonical_u32_window() {
#ifdef BIOSEQ_TEST_INDEXES
    note_vulkan_admitted_window_end(test_admitted_window_tracked);
#endif
  }
  canonical_u32_window &operator=(canonical_u32_window &&) = delete;
  canonical_u32_window(const canonical_u32_window &) = delete;
  canonical_u32_window &operator=(const canonical_u32_window &) = delete;
};

/// Visit canonical Vulkan input without ever representing an effective-type
/// reinterpretation as a typed range.  Native/admitted u32 storage retains the
/// established compact range.  A u64-backed byte-layout borrow instead gets a
/// non-dereferenceable byte_upload_range whose unit stepping is std::byte
/// arithmetic.
template <sequence_alphabet p_alphabet, class p_function>
decltype(auto) with_canonical_u32_upload_source(
    const canonical_u32_window<p_alphabet> &window, p_function &&fn) {
  if (window.borrow_kind
      == bioseq::storage::sequence_resident_borrow_kind::
          reinterpreted_bytes) {
    byte_upload_range<p_alphabet> bytes{window.descriptor};
    return std::forward<p_function>(fn)(bytes);
  }
  DEBUG_ASSERT(
      window.owner.has_value()
          || window.borrow_kind
              == bioseq::storage::sequence_resident_borrow_kind::
                  native_typed_storage,
      "typed Vulkan range requires native or admitted u32 storage");
  return with_typed_u32_range<p_alphabet>(
      window.descriptor, std::forward<p_function>(fn));
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<canonical_u32_window<p_alphabet>>
prepare_canonical_u32_window(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t from, bioseq::storage::sequence_size_t to,
    vulkan_sketch_decline *decline_out) {
  if (from > to || to > sequence.bp_size()) {
    if (decline_out != nullptr) {
      decline_out->reason =
          vulkan_sketch_decline_reason::sequence_type_unsupported;
    }
    return std::nullopt;
  }
  const auto access = bioseq::storage::plan_sequence_access(
      sequence,
      bioseq::storage::sequence_access_requirement::contiguous_resident, from,
      to, sizeof(std::uint32_t),
      bioseq::storage::sequence_resident_borrow_policy::
          byte_upload_compatible);
  if (access.route
          == bioseq::storage::sequence_access_route::borrow_resident
      && access.resident.has_value()) {
    // All facade Vulkan entry points below use the range only to derive byte
    // addresses and upload its object representation; they never load a
    // requested-width storage unit on the CPU.  They also complete upload,
    // submission, fence drain, and result delivery before returning.  A
    // validated stable native descriptor or cross-width byte descriptor can
    // therefore be borrowed for that synchronous interval without an
    // admitted O(n) repack/copy.
    return std::optional<canonical_u32_window<p_alphabet>>{
        std::in_place, *access.resident, access.resident_borrow_kind};
  }
  bioseq::parallelism::memory_admission_failure failure{};
  auto permit =
      bioseq::parallelism::memory_admission_controller::instance().try_reserve(
          access.admission_bytes, &failure);
  if (!permit) {
    if (decline_out != nullptr) {
      decline_out->reason = vulkan_sketch_decline_reason::memory_admission_denied;
      decline_out->admission = failure;
    }
    return std::nullopt;
  }
  auto owner = bioseq::storage::materialize_packed_sequence(
      sequence, from, to, sizeof(std::uint32_t), std::move(*permit));
  return std::optional<canonical_u32_window<p_alphabet>>{std::in_place,
                                                       std::move(owner)};
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] bool validate_facade_sketch_shape(
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        std::size_t window_size,
        std::size_t kmer_size,
        vulkan_sketch_decline* decline_out) noexcept {
    using instance = borrowed_u32_instance<p_alphabet>;
    using traits = typename compact_sequence_range<instance>::traits;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = sequence.bp_size(),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    auto reject = [decline_out](vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
        return false;
    };
    if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
        return reject(vulkan_sketch_decline_reason::alphabet_unsupported);
    }
    if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
        return reject(vulkan_sketch_decline_reason::kmer_size_unsupported);
    }
    if (!vulkan_minimizer_window_size_supported(window_size)) {
        return reject(vulkan_sketch_decline_reason::window_size_unsupported);
    }
    return true;
}

} // namespace admitted_sequence_detail

/// A sequence that can be uploaded as a single contiguous packed block: it must
/// expose the storage traits, a single-argument `read_cptr`, and the
/// byte/bp/padding accessors. Fragmented lazy sequences are excluded by the
/// separate fragmentation guard; selected Vulkan segment callers use the
/// checked segment upload path below and throw on GPU declines.
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN vulkan_sketchable_sequence
template <class p_sequence>
concept vulkan_sketchable_sequence = requires (const p_sequence& seq) {
    typename std::remove_cvref_t<p_sequence>::traits;
    typename std::remove_cvref_t<p_sequence>::traits::storage_unit;
    typename std::remove_cvref_t<p_sequence>::traits::alphabet_type;
    { seq.cbegin() };
    { seq.byte_size() } -> std::convertible_to<std::uint64_t>;
    { seq.bp_size() } -> std::convertible_to<std::uint64_t>;
    { seq.padding() };
    { seq.read_cptr(seq.cbegin()) };
};
// BIOSEQ_PRIVATE_PACKED_ACCESS_END vulkan_sketchable_sequence

struct sequence_upload_view {
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
    alphabet_kernel_kind alphabet_kind = alphabet_kernel_kind::generic;
};

/// Round up to the next multiple of 4 (saturating at UINT64_MAX). The shader's
/// `uint seq_words[]` view requires a 4-byte aligned upload tail.
[[nodiscard]] inline std::uint64_t aligned_upload_bytes(
    std::uint64_t byte_count) noexcept {
    return bioseq::common::checked_align_up(byte_count, std::uint64_t{4u})
        .value_or((std::numeric_limits<std::uint64_t>::max)());
}

/** Packed source window and its exact biological coordinate interval. */
struct segment_upload_descriptor {
    std::size_t lo_unit = 0;
    std::size_t hi_unit = 0;
    std::uint64_t window_bp = 0;
    std::uint32_t tail_padding = 0;
};

struct gpu_sketch_segment_window {
    std::size_t sub_lo_unit = 0;
    std::size_t sub_hi_unit = 0;
    std::size_t core_lo_unit = 0;
    std::size_t core_hi_unit = 0;
    std::uint64_t emit_base_bp = 0;
    std::uint64_t collected_bp = 0;
};

template <class p_traits>
[[nodiscard]] constexpr std::uint64_t sketch_segment_context_bp(
    std::size_t window_size,
    std::size_t kmer_size) noexcept
{
    return static_cast<std::uint64_t>(window_size)
        + static_cast<std::uint64_t>(kmer_size)
        + 1u;
}

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN sketch_segment_context_units
template <class p_traits>
[[nodiscard]] constexpr std::size_t sketch_segment_context_units(
    std::size_t window_size,
    std::size_t kmer_size) noexcept
{
    const std::uint64_t context_bp =
        sketch_segment_context_bp<p_traits>(window_size, kmer_size);
    const std::uint64_t lpu =
        std::max<std::uint64_t>(1u, p_traits::letters_per_unit);
    return std::max<std::size_t>(
        1u,
        static_cast<std::size_t>(bioseq::common::ceil_div(context_bp, lpu)));
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END sketch_segment_context_units

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN compute_segment_descriptor
template <class p_sequence>
[[nodiscard]] segment_upload_descriptor compute_segment_descriptor(
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit) noexcept
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
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
    return segment_upload_descriptor{
        .lo_unit = clamped_lo,
        .hi_unit = clamped_hi,
        .window_bp = window_bp,
        .tail_padding = tail_padding,
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END compute_segment_descriptor

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN make_sketch_segment_window
template <class p_sequence>
[[nodiscard]] gpu_sketch_segment_window make_sketch_segment_window(
    const p_sequence& seq,
    const segment_upload_descriptor& descriptor,
    std::size_t core_lo_unit,
    std::size_t core_hi_unit,
    std::size_t context_units)
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
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
    const segment_upload_descriptor sub_descriptor =
        compute_segment_descriptor(seq, sub_lo, sub_hi);
    return gpu_sketch_segment_window{
        .sub_lo_unit = sub_lo,
        .sub_hi_unit = sub_hi,
        .core_lo_unit = core_lo,
        .core_hi_unit = core_hi,
        .emit_base_bp = static_cast<std::uint64_t>(sub_lo) * lpu,
        .collected_bp = sub_descriptor.window_bp,
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END make_sketch_segment_window

template <class p_sequence>
[[nodiscard]] bool split_sketch_segment_window(
    const p_sequence& seq,
    const segment_upload_descriptor& descriptor,
    const gpu_sketch_segment_window& window,
    std::size_t context_units,
    gpu_sketch_segment_window& left,
    gpu_sketch_segment_window& right)
{
    if (window.core_hi_unit <= window.core_lo_unit + 1u) {
        return false;
    }
    const std::size_t core_mid =
        window.core_lo_unit + (window.core_hi_unit - window.core_lo_unit) / 2u;
    if (core_mid == window.core_lo_unit || core_mid == window.core_hi_unit) {
        return false;
    }
    left = make_sketch_segment_window(
        seq,
        descriptor,
        window.core_lo_unit,
        core_mid,
        context_units);
    right = make_sketch_segment_window(
        seq,
        descriptor,
        core_mid,
        window.core_hi_unit,
        context_units);
    return left.core_lo_unit < left.core_hi_unit
        && right.core_lo_unit < right.core_hi_unit;
}

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN admitted_sketch_collection_limit_bp
template <class p_minimizer_type, class p_traits>
[[nodiscard]] std::uint64_t admitted_sketch_collection_limit_bp(
    std::size_t window_size,
    std::size_t kmer_size,
    gpu_sketch_purpose use) noexcept
{
    using storage_unit = typename p_traits::storage_unit;
    constexpr std::uint64_t collected_floor_bp = 1'000'000u;
    // Bound one dispatch while retaining room for large target segments under
    // ordinary memory admission.
    constexpr std::uint64_t collected_ceil_bp = 96'000'000u;
    const std::uint64_t available =
        bioseq::parallelism::memory_admission_controller::instance()
            .allocatable_after_reservations();
    if (available == (std::numeric_limits<std::uint64_t>::max)()) {
        return collected_ceil_bp;
    }

    const std::uint64_t lpu =
        std::max<std::uint64_t>(1u, p_traits::letters_per_unit);
    std::uint64_t candidate = collected_ceil_bp;
    while (candidate > collected_floor_bp) {
        const std::uint64_t capacity = std::max<std::uint64_t>(64u, candidate);
        const std::uint64_t unit_count =
            bioseq::common::ceil_div(candidate, lpu);
        const std::uint64_t sequence_bytes = aligned_upload_bytes(
            bioseq::common::saturating_mul(
                unit_count,
                static_cast<std::uint64_t>(sizeof(storage_unit))));
        const std::uint64_t output_bytes =
            use == gpu_sketch_purpose::minimizer_words
                ? minimizer_wire_traits<p_minimizer_type>::device_bytes_for_count(capacity)
                : 0u;
        const gpu_sketch_memory_plan plan = make_gpu_sketch_memory_plan(
            candidate,
            sequence_bytes,
            sizeof(vulkan_rank_lut),
            capacity,
            output_bytes,
            use,
            static_cast<std::uint32_t>(kmer_size),
            static_cast<std::uint32_t>(window_size),
            static_cast<std::uint32_t>(lpu));
        if (plan.is_valid && plan.admission_bytes <= available / 2u) {
            return candidate;
        }
        candidate /= 2u;
    }
    return collected_floor_bp;
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END admitted_sketch_collection_limit_bp

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN sketch_segment_windows
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::vector<gpu_sketch_segment_window> sketch_segment_windows(
    const p_sequence& seq,
    const segment_upload_descriptor& descriptor,
    std::size_t window_size,
    std::size_t kmer_size,
    gpu_sketch_purpose use,
    std::uint64_t collected_limit_override = 0u)
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    const std::uint64_t collected_limit =
        collected_limit_override == 0u
            ? admitted_sketch_collection_limit_bp<p_minimizer_type, traits>(
                window_size, kmer_size, use)
            : collected_limit_override;
    const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
    const std::uint64_t context_bp =
        sketch_segment_context_bp<traits>(window_size, kmer_size);
    constexpr std::uint64_t tile_safety_bp = 4096u;
    const std::uint64_t core_bp =
        collected_limit > context_bp * 2u + tile_safety_bp + 1u
            ? collected_limit - context_bp * 2u - tile_safety_bp
            : std::max<std::uint64_t>(1u, collected_limit / 2u);
    const std::size_t core_units =
        std::max<std::size_t>(1u, static_cast<std::size_t>(core_bp / lpu));
    const std::size_t context_units =
        sketch_segment_context_units<traits>(window_size, kmer_size);

    std::vector<gpu_sketch_segment_window> windows;
    if (descriptor.window_bp <= collected_limit) {
        windows.push_back(gpu_sketch_segment_window{
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
        windows.push_back(make_sketch_segment_window(
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
// BIOSEQ_PRIVATE_PACKED_ACCESS_END sketch_segment_windows

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN make_upload_view_from_window
template <class p_sequence, class p_addressed_element>
[[nodiscard]] inline sequence_upload_view make_upload_view_from_window(
    const p_sequence&,
    const p_addressed_element* base,
    const segment_upload_descriptor& descriptor)
{
    using traits = typename std::remove_cvref_t<p_sequence>::traits;
    using logical_storage_unit = typename traits::storage_unit;
    static_assert(
        std::same_as<std::remove_cv_t<p_addressed_element>, logical_storage_unit>
        || std::same_as<std::remove_cv_t<p_addressed_element>, std::byte>
        || std::same_as<std::remove_cv_t<p_addressed_element>, unsigned char>,
        "Vulkan uploads accept native storage pointers or byte addresses");
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(descriptor.hi_unit - descriptor.lo_unit)
        * static_cast<std::uint64_t>(sizeof(logical_storage_unit));
    return sequence_upload_view{
        .bytes = byte_size == 0u ? nullptr : static_cast<const void*>(base),
        .byte_size = byte_size,
        .padded_byte_size = aligned_upload_bytes(byte_size),
        .bp_size = descriptor.window_bp,
        .unit_bits = static_cast<std::uint32_t>(
            sizeof(logical_storage_unit) * 8u),
        .bits_per_char = static_cast<std::uint32_t>(traits::bits_per_char),
        .letters_per_unit = static_cast<std::uint32_t>(traits::letters_per_unit),
        .alphabet_size = static_cast<std::uint32_t>(traits::alphabet_size),
        .char_mask = static_cast<std::uint32_t>(traits::char_mask),
        .padding_letters = descriptor.tail_padding,
        .alphabet_kind = alphabet_kernel_kind_of_traits<traits>(),
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END make_upload_view_from_window

// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN make_sequence_upload_view
template <vulkan_sketchable_sequence p_sequence>
[[nodiscard]] inline sequence_upload_view make_sequence_upload_view(const p_sequence& seq) {
    auto begin = seq.cbegin();
    const auto* ptr =
        seq.byte_size() == 0 ? nullptr : seq.read_cptr(begin);
    const segment_upload_descriptor descriptor{
        .lo_unit = 0,
        .hi_unit = static_cast<std::size_t>(seq.size()),
        .window_bp = static_cast<std::uint64_t>(seq.bp_size()),
        .tail_padding = static_cast<std::uint32_t>(seq.padding()),
    };
    return make_upload_view_from_window<p_sequence>(seq, ptr, descriptor);
}

/* Canonical lazy-sequence bridge for callers that admitted a contiguous packed
 * sequence explicitly. The descriptor's backing owner must outlive the Vulkan
 * upload and completion fence; this view never acquires or copies ownership. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline sequence_upload_view make_sequence_upload_view(
    const bioseq::storage::admitted_packed_sequence<p_alphabet>& owner) {
    using lazy_traits = bioseq::storage::lazy_sequence_traits<p_alphabet>;
    const auto& descriptor = owner.descriptor();
    constexpr std::uint32_t bits = static_cast<std::uint32_t>(
        lazy_traits::logical_bits_per_symbol);
    const std::uint32_t letters_per_unit = static_cast<std::uint32_t>(
        bioseq::storage::packed_letters_per_unit(
            descriptor.storage_unit_width,
            static_cast<std::uint8_t>(bits)));
    const std::uint64_t expected_units = letters_per_unit == 0u
        ? (std::numeric_limits<std::uint64_t>::max)()
        : descriptor.logical_length / letters_per_unit
            + static_cast<std::uint64_t>(
                descriptor.logical_length % letters_per_unit != 0u);
    const bool geometry_overflow = letters_per_unit == 0u
        || expected_units > (std::numeric_limits<std::uint64_t>::max)()
            / std::max<std::uint32_t>(letters_per_unit, 1u);
    const std::uint64_t expected_padding = geometry_overflow
        ? (std::numeric_limits<std::uint64_t>::max)()
        : expected_units * letters_per_unit - descriptor.logical_length;
    const bool bytes_overflow = descriptor.unit_count
        > (std::numeric_limits<std::uint64_t>::max)()
            / std::max<std::uint8_t>(
                descriptor.storage_unit_width, 1u);
    if (descriptor.bit_order
            != bioseq::storage::lazy_sequence_view<p_alphabet>::
                packed_symbol_order::least_significant_symbol_first
        || descriptor.leading_symbol_offset != 0u
        || descriptor.bits_per_symbol != bits
        || letters_per_unit == 0u
        || geometry_overflow
        || descriptor.unit_count != expected_units
        || descriptor.trailing_padding != expected_padding
        || bytes_overflow
        || (descriptor.unit_count != 0u && descriptor.units == nullptr)) {
        throw vulkan_runtime_error(
            "admitted packed-sequence upload descriptor is invalid");
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(
        descriptor.unit_count) * descriptor.storage_unit_width;
    constexpr alphabet_kernel_kind kernel = [] {
        if constexpr (seqan3::nucleotide_alphabet<p_alphabet>
            && bits == 2u && seqan3::alphabet_size<p_alphabet> == 4u) {
            return alphabet_kernel_kind::dna4_nt4;
        } else if constexpr (seqan3::nucleotide_alphabet<p_alphabet>
            && bits == 3u && seqan3::alphabet_size<p_alphabet> == 5u) {
            return alphabet_kernel_kind::dna5_nt4;
        } else {
            return alphabet_kernel_kind::generic;
        }
    }();
    return sequence_upload_view{
        .bytes = bytes == 0u ? nullptr : descriptor.units,
        .byte_size = bytes,
        .padded_byte_size = aligned_upload_bytes(bytes),
        .bp_size = descriptor.logical_length,
        .unit_bits = static_cast<std::uint32_t>(
            descriptor.storage_unit_width * 8u),
        .bits_per_char = bits,
        .letters_per_unit = letters_per_unit,
        .alphabet_size = static_cast<std::uint32_t>(
            seqan3::alphabet_size<p_alphabet>),
        .char_mask = bits == 32u
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>((std::uint64_t{1u} << bits) - 1u),
        .padding_letters = descriptor.trailing_padding,
        .alphabet_kind = kernel,
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END make_sequence_upload_view

/// Shared lifetime anchor for a batch result whose one reconciled permit
/// covers both the resident Vulkan allocation and its host-side vectors.  The
/// batch result and device storage retain the same owner so scheduler moves of
/// those objects cannot release admission while either side is still live.
struct vulkan_batch_sketch_memory_retention {
    bioseq::parallelism::memory_permit permit;
};

template <class p_minimizer_type>
struct vulkan_device_minimizer_storage {
    // Declaration order is intentional: reverse destruction frees `words`
    // before either admission owner can release its reservation.
    std::shared_ptr<vulkan_batch_sketch_memory_retention> batch_memory_retention;
    bioseq::parallelism::memory_permit permit;
    buffer words;
    std::uint64_t count = 0;
    std::uint64_t capacity = 0;
    std::uint32_t words32_per_minimizer =
        minimizer_wire_traits<p_minimizer_type>::k_words32_per_minimizer;
    minimizer_word_kind word_kind =
        minimizer_wire_traits<p_minimizer_type>::k_word_kind;
};

template <class p_minimizer_type>
struct vulkan_device_minimizer_span {
    std::shared_ptr<const vulkan_device_minimizer_storage<p_minimizer_type>> owner{};

    [[nodiscard]] const buffer* words() const noexcept {
        return owner ? &owner->words : nullptr;
    }
    [[nodiscard]] std::uint64_t count() const noexcept {
        return owner ? owner->count : 0u;
    }
    [[nodiscard]] std::uint32_t words32_per_minimizer() const noexcept {
        return owner ? owner->words32_per_minimizer : 0u;
    }
    [[nodiscard]] minimizer_word_kind word_kind() const noexcept {
        return owner ? owner->word_kind : minimizer_word_kind::u32;
    }
};

/// Host-result ownership selected by the caller before dispatch.  The default
/// collector-compatible route retains geometric-growth headroom because the
/// collector wrapper duplicates records into a second vector.  The fixed
/// zero-copy route is intentionally opt-in: its exact allocation admission is
/// valid only when the caller adopts the returned vector allocation and drops
/// the device result before constructing the CPU target index.
enum class vulkan_sketch_host_ownership_route : std::uint8_t {
    collector_compatible = 0,
    fixed_zero_copy_adoption = 1,
};

/// Operation-originated evidence that the collector-compatible Vulkan sketch
/// leaf completed its runtime roundtrip. A default token is deliberately
/// invalid, and route identity is validated before the pipeline may translate
/// it into query/target stage evidence.
enum class vulkan_sketch_dispatch_route : std::uint8_t {
    none,
    collect_minimizers,
};

struct vulkan_sketch_dispatch_token {
    vulkan_sketch_dispatch_route route = vulkan_sketch_dispatch_route::none;
    std::uint64_t completed_roundtrips = 0;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        return route == vulkan_sketch_dispatch_route::collect_minimizers
            && completed_roundtrips == 1;
    }

    [[nodiscard]] static constexpr vulkan_sketch_dispatch_token
    completed_collect_minimizers_roundtrip() noexcept {
        return {
            .route = vulkan_sketch_dispatch_route::collect_minimizers,
            .completed_roundtrips = 1,
        };
    }

    constexpr bool operator==(
        const vulkan_sketch_dispatch_token&) const noexcept = default;
};

static_assert(!vulkan_sketch_dispatch_token{}.is_internally_consistent());
static_assert(vulkan_sketch_dispatch_token::
    completed_collect_minimizers_roundtrip().is_internally_consistent());

/// Aggregate operation-originated evidence for the strict segment-pair path.
/// A completed roundtrip is counted only after the runtime-produced dispatch
/// token has been checked. p_capacity-driven GPU window splitting is recorded
/// separately and never reclassified as a CPU fallback.
struct vulkan_segment_pair_execution_stats {
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

    void merge(const vulkan_segment_pair_execution_stats& other) noexcept {
        saturated = saturated || other.saturated;
        add(completed_roundtrips, other.completed_roundtrips, saturated);
        add(downloaded_minimizers, other.downloaded_minimizers, saturated);
        add(gpu_window_splits, other.gpu_window_splits, saturated);
    }

    [[nodiscard]] bool is_internally_consistent(
            bool require_completed_roundtrip = true) const noexcept {
        return !saturated
            && (!require_completed_roundtrip || completed_roundtrips != 0);
    }
};

struct vulkan_owned_segment_pairs {
    using value_type = std::pair<std::uint64_t, std::uint64_t>;

    // Reverse destruction releases pair capacity before admission.
    bioseq::parallelism::memory_permit permit;
    std::vector<value_type> pairs;

    vulkan_owned_segment_pairs() = default;
    vulkan_owned_segment_pairs(const vulkan_owned_segment_pairs&) = delete;
    vulkan_owned_segment_pairs& operator=(const vulkan_owned_segment_pairs&) = delete;
    vulkan_owned_segment_pairs(vulkan_owned_segment_pairs&& other) noexcept {
        pairs.swap(other.pairs);
        permit = std::move(other.permit);
    }
    vulkan_owned_segment_pairs& operator=(vulkan_owned_segment_pairs&& other) noexcept {
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
        const vulkan_owned_segment_pairs& lhs,
        const std::vector<value_type>& rhs) noexcept {
        return lhs.pairs == rhs;
    }
    friend bool operator==(
        const std::vector<value_type>& lhs,
        const vulkan_owned_segment_pairs& rhs) noexcept {
        return lhs == rhs.pairs;
    }
};

template <class p_minimizer_type>
struct vulkan_sketch_result {
    // Both possible admission owners precede the host vector so reverse
    // destruction frees vector storage before either reservation is released.
    std::optional<bioseq::parallelism::memory_permit> host_permit;
    std::shared_ptr<vulkan_device_minimizer_storage<p_minimizer_type>> device;
    std::vector<p_minimizer_type> host_minimizers;
    vulkan_sketch_dispatch_token dispatch_token{};
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible;
    bool host_permit_released_to_process_footprint = false;
    std::uint64_t host_footprint_reflected_delta_bytes = 0u;
    bool arena_trimmed_before_host_materialization = false;
    bool overflow = false;

    [[nodiscard]] vulkan_device_minimizer_span<p_minimizer_type> device_span() const noexcept {
        return vulkan_device_minimizer_span<p_minimizer_type>{.owner = device};
    }
};

template <class p_minimizer_type>
struct vulkan_batch_sketch_result {
    // Consumers that detach either host vector without retaining `device`
    // must move `memory_retention` with it.  GPU schedulers naturally retain
    // `device`, which shares the same anchor.
    // It is declared first so reverse destruction releases every accounted
    // vector/device before the final admission anchor.
    std::shared_ptr<vulkan_batch_sketch_memory_retention> memory_retention;
    std::vector<p_minimizer_type> host_minimizers;
    std::vector<gpu_seed_query_record_wire> query_records;
    std::shared_ptr<vulkan_device_minimizer_storage<p_minimizer_type>> device;
    runtime_sketch_dispatch_stats dispatch_stats{};
    device_query_sketch_authorization query_authority{};
    vulkan_sketch_host_materialization materialization =
        vulkan_sketch_host_materialization::host_and_device;
    std::uint64_t device_minimizer_count = 0;
    bool overflow = false;

    [[nodiscard]] vulkan_device_minimizer_span<p_minimizer_type> device_span() const noexcept {
        return vulkan_device_minimizer_span<p_minimizer_type>{.owner = device};
    }
};

template <class p_minimizer_type>
[[nodiscard]] std::vector<p_minimizer_type> decode_host_minimizer_bytes(
    std::span<const std::byte> bytes, std::uint64_t count)
{
    if (count > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)() / sizeof(p_minimizer_type))) {
        throw vulkan_runtime_error{"Vulkan minimizer count exceeds host address space"};
    }
    const std::size_t n = static_cast<std::size_t>(count);
    const std::size_t expected = n * sizeof(p_minimizer_type);
    if (bytes.size() < expected) {
        throw vulkan_runtime_error{"Vulkan minimizer download was truncated"};
    }
    std::vector<p_minimizer_type> out(n);
    if (expected != 0u) {
        std::memcpy(out.data(), bytes.data(), expected);
    }
    return out;
}

/** Estimate a conservative output capacity for one sketch dispatch. */
[[nodiscard]] inline std::uint64_t estimate_vulkan_minimizer_capacity(
    std::uint64_t bp_size, std::uint64_t window_size) noexcept
{
    using bioseq::common::saturating_mul;
    // ~2/(w+1) minimizers per bp in practice; 3·(bp/w) is a safe (>1.5×) upper
    // planning estimate that keeps ordinary single-pass sketch buffers small.
    // Those APIs fail closed if it underestimates. Target-index construction is
    // different: it first obtains exact T without an output allocation, records
    // whether this estimate was low, then sizes and admits the deterministic
    // emit from T rather than treating this heuristic as a semantic cap.
    if (bp_size <= k_default_sketch_tile_bp) {
        return std::max<std::uint64_t>(64u, bp_size);
    }
    const std::uint64_t base = window_size == 0 ? bp_size : (bp_size / window_size + 64u);
    return std::max<std::uint64_t>(64u, saturating_mul(base, 3u));
}

template <class p_minimizer_type>
[[nodiscard]] std::uint64_t vulkan_sketch_resident_bytes(
    std::uint64_t max_minimizers) noexcept
{
    return minimizer_wire_traits<p_minimizer_type>::device_bytes_for_count(max_minimizers);
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
    const gpu_sketch_memory_plan plan = make_gpu_sketch_memory_plan(
        bp,
        aligned_upload_bytes(sequence_bytes),
        sizeof(vulkan_rank_lut),
        max_minimizers,
        minimizer_device_output_bytes,
        gpu_sketch_purpose::minimizer_words,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        letters_per_unit);
    if (!plan.is_valid) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return bioseq::common::saturating_add(
        plan.arena_scaffold_bytes,
        plan.count_submit_host_transient_bytes);
}

/// Dispatch owns only the physical arena, host transients,
/// query gate, and pessimistic device result allocation.  Host result capacity
/// is deliberately absent: its exact reservation is made after dispatch
/// transients are destroyed and the emitted count is known.
inline constexpr std::uint64_t k_vulkan_exact_host_allocation_headroom_bytes =
    64ull * 1024ull;

// Fixed-size host staging cannot grow after allocation. Admit the requested
// payload plus a bounded allowance for allocator metadata and VM page rounding
// instead of the 2x capacity headroom required by growable vectors.
[[nodiscard]] constexpr std::uint64_t
vulkan_exact_host_allocation_admission_bytes(
    std::uint64_t requested_bytes) noexcept
{
    using bioseq::common::saturating_add;
    return saturating_add(
        requested_bytes,
        k_vulkan_exact_host_allocation_headroom_bytes);
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
    using bioseq::common::saturating_add;
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
    return bioseq::common::saturating_add(
        actual_device_allocation_bytes,
        exact_host_headroom_bytes);
}

/// Per-batch device-memory estimate. Used for initial admission and
/// batch splitting before the count submit reports exact counts. Every field is
/// summed with the shared saturating add so the total never wraps.
struct vulkan_sketch_memory_estimate {
    std::uint64_t packed_sequence_bytes = 0;
    std::uint64_t query_minimizer_bytes = 0;
    std::uint64_t target_minimizer_bytes = 0;
    std::uint64_t index_slot_bytes = 0;
    std::uint64_t index_position_bytes = 0;
    std::uint64_t anchor_bytes = 0;
    std::uint64_t sort_scratch_bytes = 0;
    std::uint64_t chain_bytes = 0;

    [[nodiscard]] std::uint64_t total() const noexcept {
        using bioseq::common::saturating_add;
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

/// Dispatch and decode an already-prepared upload view and rank table so the
/// whole-sequence path and the segment-window path share one implementation.
/// `upload.alphabet_kind` is the single source of truth for the shader kernel
/// (no `choose_alphabet_kernel<bare>()` inside). Returns `std::nullopt` for
/// pre-dispatch declines; `vulkan_runtime_error` propagates.
template <class p_minimizer_type>
[[nodiscard]] std::optional<vulkan_sketch_result<p_minimizer_type>>
sketch_uploaded_window_result(
    const sequence_upload_view& upload,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    std::uint64_t capacity_bound = 0u,
    bool* capacity_overflowed = nullptr,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr,
    bioseq::parallelism::memory_permit* borrowed_dispatch_permit = nullptr,
    std::uint64_t borrowed_dispatch_budget = 0u,
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible)
{
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::common::saturating_mul;

    if (capacity_overflowed != nullptr) {
        *capacity_overflowed = false;
    }
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = upload.bp_size,
            .padded_sequence_bytes = upload.padded_byte_size,
            .query_gate_words = static_cast<std::uint64_t>(query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason,
            std::uint64_t max_minimizers = 0u) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
            decline_out->max_minimizers = max_minimizers;
        }
    };
    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_sketch_decline_reason::sequence_size_exceeds_u32);
        return std::nullopt;
    }
    if (upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()) {
        decline(vulkan_sketch_decline_reason::padded_sequence_size_overflow);
        return std::nullopt;
    }
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!hardware_device_available(runtime)) {
        decline(vulkan_sketch_decline_reason::hardware_device_unavailable);
        return std::nullopt;
    }
    const std::uint64_t max_minimizers =
        capacity_bound == 0u
            ? estimate_vulkan_minimizer_capacity(upload.bp_size, window_size)
            : capacity_bound;
    if (max_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        decline(
            vulkan_sketch_decline_reason::minimizer_capacity_exceeds_u32,
            max_minimizers);
        return std::nullopt;
    }
    if (decline_out != nullptr) {
        decline_out->max_minimizers = max_minimizers;
    }
    if (query_gate.words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        decline(
            vulkan_sketch_decline_reason::query_gate_exceeds_u32,
            max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            static_cast<std::uint64_t>(sizeof(std::uint32_t)));
    const std::uint64_t requested_device_bytes =
        vulkan_sketch_resident_bytes<p_minimizer_type>(max_minimizers);
    const gpu_sketch_memory_plan dispatch_plan = make_gpu_sketch_memory_plan(
        upload.bp_size,
        upload.padded_byte_size,
        sizeof(vulkan_rank_lut),
        max_minimizers,
        requested_device_bytes,
        gpu_sketch_purpose::minimizer_words,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        upload.letters_per_unit,
        runtime.features().max_workgroup_count_x);
    if (requested_device_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !dispatch_plan.is_valid) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const gpu_sketch_batch_allocation_requirements allocation_requirements =
        runtime.sketch_batch_allocation_requirements(
            upload.padded_byte_size,
            dispatch_plan.tiles.tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<p_minimizer_type>::k_word_kind),
            minimizer_wire_traits<p_minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!allocation_requirements.valid) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_arena_bytes = query_gate_bytes == 0u
        ? 0u
        : bioseq::common::saturating_add(
            query_gate_bytes,
            std::uint64_t{k_gpu_buffer_offset_alignment});
    const std::uint64_t arena_request_bytes = bioseq::common::saturating_add(
        dispatch_plan.arena_scaffold_bytes,
        query_gate_arena_bytes);
    if (query_gate_arena_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || arena_request_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t physical_arena_bytes =
        runtime.gpu_arena_allocation_requirement(arena_request_bytes);
    const std::uint64_t host_transient_headroom_bytes =
        vulkan_exact_host_allocation_admission_bytes(
            dispatch_plan.count_submit_host_transient_bytes);
    const std::uint64_t resident_allocation_bytes =
        allocation_requirements.pessimistic_output_allocation_bytes;
    const std::uint64_t transient_bytes = bioseq::common::saturating_add(
        physical_arena_bytes,
        bioseq::common::saturating_add(host_transient_headroom_bytes, query_gate_bytes));

    if (host_transient_headroom_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || resident_allocation_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || transient_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
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
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    std::optional<bioseq::parallelism::memory_permit> owned_dispatch_permit;
    bioseq::parallelism::memory_permit* dispatch_permit = borrowed_dispatch_permit;
    if (dispatch_permit == nullptr) {
        owned_dispatch_permit = memory_admission_controller::instance().try_reserve(
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
                vulkan_sketch_decline_reason::memory_admission_denied;
            decline_out->admission = admission_failure;
        }
        bioseq_log<logging_level::error>(
            "[vulkan sketch] strict dispatch admission denied: {}",
            format_vulkan_sketch_decline(
                decline_out != nullptr
                    ? *decline_out
                    : vulkan_sketch_decline{
                        .reason = vulkan_sketch_decline_reason::memory_admission_denied,
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
        const vulkan_sketch_params sketch_params{
            .bp_size = static_cast<std::uint32_t>(upload.bp_size),
            .window_size = static_cast<std::uint32_t>(window_size),
            .kmer_size = static_cast<std::uint32_t>(kmer_size),
            .hash_bits = minimizer_wire_traits<p_minimizer_type>::k_hash_bits,
            .hash_shift = minimizer_wire_traits<p_minimizer_type>::k_hash_shift,
            .pos_bits = minimizer_wire_traits<p_minimizer_type>::k_position_bits,
            .minimizer_word_kind = static_cast<std::uint32_t>(
                minimizer_wire_traits<p_minimizer_type>::k_word_kind),
            .minimizer_words32 =
                minimizer_wire_traits<
                    p_minimizer_type>::k_words32_per_minimizer,
            .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
            .unit_bits = upload.unit_bits,
            .bits_per_char = upload.bits_per_char,
            .letters_per_unit = upload.letters_per_unit,
            .alphabet_size = upload.alphabet_size,
            .seq_word_offset = 0u,
            .char_mask = upload.char_mask,
            .padding_letters = upload.padding_letters,
        };

        runtime_sketch_result raw;
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
            raw = runtime.run_sketch_roundtrip(
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
            throw vulkan_runtime_error{
                "Vulkan minimizer roundtrip returned before arena trim"};
        }
        if (raw.overflow) {
            if (capacity_overflowed != nullptr) {
                *capacity_overflowed = true;
            }
            if (capacity_bound != 0u && capacity_overflowed == nullptr) {
                throw vulkan_runtime_error(fmt::format(
                    "Vulkan minimizer sketch overflowed explicit capacity bound "
                    "(bp={}, cap={})",
                    upload.bp_size,
                    max_minimizers));
            }
            decline(
                vulkan_sketch_decline_reason::output_capacity_overflow,
                max_minimizers);
            return std::nullopt;
        }

        const std::uint64_t actual_device_allocation_bytes =
            raw.minimizer_words.allocation_bytes;
        if (actual_device_allocation_bytes > resident_allocation_bytes) {
            throw vulkan_runtime_error{
                "Vulkan minimizer device allocation exceeded admission"};
        }
        if (borrowed_dispatch_permit == nullptr) {
            dispatch_permit->shrink_to(actual_device_allocation_bytes);
        }

        if (raw.count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)()
                / sizeof(p_minimizer_type))) {
            throw vulkan_runtime_error{
                "Vulkan minimizer count exceeds host address space"};
        }
        const std::uint64_t exact_host_size_bytes = saturating_mul(
            raw.count,
            static_cast<std::uint64_t>(sizeof(p_minimizer_type)));
        const std::uint64_t exact_host_headroom_bytes =
            host_ownership_route
                    == vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption
                ? vulkan_fixed_host_result_admission_bytes(
                      exact_host_size_bytes)
                : saturating_mul(
                      exact_host_size_bytes,
                      bioseq::parallelism::k_vector_preallocation_headroom_factor);
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
            decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
            return std::nullopt;
        }

        std::optional<bioseq::parallelism::memory_permit> owned_host_permit;
        if (borrowed_dispatch_permit == nullptr) {
            bioseq::parallelism::memory_admission_failure host_failure{};
            owned_host_permit =
                memory_admission_controller::instance().try_reserve(
                    exact_host_headroom_bytes,
                    &host_failure);
            if (!owned_host_permit) {
                if (decline_out != nullptr) {
                    decline_out->reason =
                        vulkan_sketch_decline_reason::memory_admission_denied;
                    decline_out->admission = host_failure;
                }
                bioseq_log<logging_level::error>(
                    "[vulkan sketch] strict exact-host admission denied: {}",
                    format_vulkan_sketch_decline(
                        decline_out != nullptr
                            ? *decline_out
                            : vulkan_sketch_decline{
                                .reason = vulkan_sketch_decline_reason::
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
            decline(vulkan_sketch_decline_reason::memory_admission_denied,
                    max_minimizers);
            return std::nullopt;
        }

        if (host_ownership_route
                == vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption
            && borrowed_dispatch_permit != nullptr) {
            throw vulkan_runtime_error{
                "Vulkan fixed zero-copy host adoption requires an owned "
                "materialization permit"};
        }

        // Reserve once and download once. p_permit_type declaration precedes
        // vector declaration so reverse
        // destruction always frees vector capacity before returning admission.
        std::vector<p_minimizer_type> host_minimizers;
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
                static_cast<std::uint64_t>(sizeof(p_minimizer_type)));
            if ((host_ownership_route
                        == vulkan_sketch_host_ownership_route::
                            fixed_zero_copy_adoption
                    && host_minimizers.size()
                        != host_minimizers.capacity())
                || exact_host_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || exact_host_capacity_bytes > exact_host_headroom_bytes) {
                throw vulkan_runtime_error{
                    "Vulkan decoded fixed host minimizer capacity exceeded "
                    "admission"};
            }
            if (owned_host_permit) {
                owned_host_permit->shrink_to(exact_host_capacity_bytes);
            }
            runtime.download_slice(
                raw.minimizer_words,
                0u,
                std::as_writable_bytes(std::span<p_minimizer_type>{
                    host_minimizers.data(), host_minimizers.size()}));
        };

        if (host_ownership_route
                == vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption
            && exact_host_size_bytes != 0u) {
            auto materialization =
                memory_admission_controller::instance().materialize(
                    *owned_host_permit,
                    [&allocate_and_download,
                     &exact_host_capacity_bytes]() -> bioseq::parallelism::
                            memory_materialization_snapshot {
                        allocate_and_download();
                        return {
                            .actual_allocation_bytes =
                                exact_host_capacity_bytes,
                            .fully_touched_bytes = exact_host_capacity_bytes,
                            .is_host_visible = true,
                            .is_host_coherent = true,
                            .is_process_footprint_reflected = true,
                        };
                    });
            host_permit_released_to_process_footprint =
                materialization.is_permit_released;
            host_footprint_reflected_delta_bytes =
                materialization.reflected_delta_bytes;
            if (!materialization.is_permit_released) {
                // A large target does not imply a large emitted vector.  An
                // ambiguity-heavy dna5 target can produce a small non-empty
                // allocation whose allocator pages were already resident, so
                // the serialized RSS probe may correctly decline to release
                // the reservation.  Keep the exact, already-shrunk permit as
                // the alternative ownership authority.  The pipeline moves it
                // with the vector and releases it only after index construction
                // has consumed the vector and proved capacity()==0.
                bioseq_log<logging_level::debug>(
                    "[vulkan sketch] fixed host footprint reconciliation "
                    "retained exact permit: actual={} touched={} before={} "
                    "after={} delta={} permit={}",
                    materialization.allocation_snapshot.actual_allocation_bytes,
                    materialization.allocation_snapshot.fully_touched_bytes,
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
                    == vulkan_sketch_host_ownership_route::
                        fixed_zero_copy_adoption) {
                // Empty vectors own no capacity and need no footprint delta.
                if (owned_host_permit) {
                    owned_host_permit->shrink_to(0u);
                }
                owned_host_permit.reset();
                host_permit_released_to_process_footprint = true;
            }
        }

        auto storage = std::make_shared<vulkan_device_minimizer_storage<p_minimizer_type>>();
        storage->words = std::move(raw.minimizer_words);
        if (borrowed_dispatch_permit == nullptr) {
            storage->permit = std::move(*dispatch_permit);
        }
        storage->count = raw.count;
        storage->capacity = max_minimizers;

        return vulkan_sketch_result<p_minimizer_type>{
            .host_permit = std::move(owned_host_permit),
            .device = std::move(storage),
            .host_minimizers = std::move(host_minimizers),
            .dispatch_token = vulkan_sketch_dispatch_token::
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
    } catch (const vulkan_runtime_error&) {
        throw;
    }
}

template <class p_minimizer_type>
[[nodiscard]] std::optional<gpu_built_index>
build_gpu_index_from_upload_on_device(
    const sequence_upload_view& upload,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr,
    std::uint64_t owned_sequence_staging_bytes = 0u,
    std::function<void()> release_owned_sequence_staging = {})
{
    using bioseq::parallelism::memory_admission_controller;

    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = upload.bp_size,
            .padded_sequence_bytes = upload.padded_byte_size,
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason,
            std::uint64_t max_minimizers = 0u) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
            decline_out->max_minimizers = max_minimizers;
        }
    };

    if (upload.bp_size == 0u) {
        decline(vulkan_sketch_decline_reason::empty_sequence);
        return std::nullopt;
    }
    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_sketch_decline_reason::sequence_size_exceeds_u32);
        return std::nullopt;
    }
    if (upload.padded_byte_size
        == std::numeric_limits<std::uint64_t>::max()) {
        decline(vulkan_sketch_decline_reason::padded_sequence_size_overflow);
        return std::nullopt;
    }
    if (query_gate.words.size()
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        decline(vulkan_sketch_decline_reason::query_gate_exceeds_u32);
        return std::nullopt;
    }
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!hardware_device_available(runtime)) {
        decline(vulkan_sketch_decline_reason::hardware_device_unavailable);
        return std::nullopt;
    }

    static std::mutex target_index_build_mutex;
    std::lock_guard<std::mutex> target_index_build_guard{
        target_index_build_mutex};

    const std::uint64_t max_minimizers = std::min(
        estimate_vulkan_minimizer_capacity(upload.bp_size, window_size),
        target_minimizer_count_upper_bound(
            upload.bp_size, static_cast<std::uint32_t>(kmer_size)));
    if (decline_out != nullptr) {
        decline_out->max_minimizers = max_minimizers;
    }

    const gpu_sketch_memory_plan memory_plan = make_gpu_sketch_memory_plan(
        upload.bp_size,
        upload.padded_byte_size,
        sizeof(vulkan_rank_lut),
        max_minimizers,
        0u,
        gpu_sketch_purpose::target_index_build,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        upload.letters_per_unit,
        runtime.features().max_workgroup_count_x);
    if (!memory_plan.is_valid) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const gpu_target_index_host_scratch_plan host_scratch_plan =
        make_gpu_target_index_host_scratch_plan(
            memory_plan, owned_sequence_staging_bytes);
    if (!host_scratch_plan.is_valid
        || (owned_sequence_staging_bytes != 0u
            && !release_owned_sequence_staging)) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            sizeof(std::uint32_t));
    const std::uint64_t count_arena_request = bioseq::common::saturating_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : bioseq::common::saturating_add(query_gate_bytes,
                      std::uint64_t{k_gpu_buffer_offset_alignment}));
    const std::uint64_t count_arena_bytes =
        runtime.gpu_arena_allocation_requirement(count_arena_request);
    const std::uint64_t admission_bytes = bioseq::common::saturating_add(
        count_arena_bytes, host_scratch_plan.count_submit_bytes);
    if (query_gate_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || admission_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }

    const auto query_gate_identity = query_gate.canonical_identity();
    const vulkan_sketch_params sketch_params{
        .bp_size = static_cast<std::uint32_t>(upload.bp_size),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = minimizer_wire_traits<p_minimizer_type>::k_hash_bits,
        .hash_shift = minimizer_wire_traits<p_minimizer_type>::k_hash_shift,
        .pos_bits = minimizer_wire_traits<p_minimizer_type>::k_position_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            minimizer_wire_traits<p_minimizer_type>::k_word_kind),
        .minimizer_words32 =
            minimizer_wire_traits<p_minimizer_type>::k_words32_per_minimizer,
        .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
        .unit_bits = upload.unit_bits,
        .bits_per_char = upload.bits_per_char,
        .letters_per_unit = upload.letters_per_unit,
        .alphabet_size = upload.alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = upload.char_mask,
        .padding_letters = upload.padding_letters,
    };
    // Pipeline-cache metadata is process-lived authority. Resolve the exact
    // target-index key set before reserving the transient count/main peak so a
    // cold pipeline cannot nest a metadata admission after large mapped Vulkan
    // allocations have become visible in the process footprint.
    runtime.prepare_target_index_pipelines(sketch_params);

    auto& admission = memory_admission_controller::instance();
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = admission.try_reserve(admission_bytes, &admission_failure);
    if (!permit) {
        if (decline_out != nullptr) {
            decline_out->reason =
                vulkan_sketch_decline_reason::memory_admission_denied;
            decline_out->max_minimizers = max_minimizers;
            decline_out->admission = admission_failure;
        }
        bioseq_log<logging_level::error>(
            "[gpu sketch index] target-index build admission failed "
            "({})",
            format_vulkan_sketch_decline(
                decline_out != nullptr
                    ? *decline_out
                    : vulkan_sketch_decline{
                        .reason = vulkan_sketch_decline_reason::
                            memory_admission_denied,
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
        gpu_built_index built{};
        const auto materialization = admission.materialize(
            *permit,
            [&]() -> bioseq::parallelism::memory_materialization_snapshot {
                built = runtime.build_target_index(
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
                // runtime publishes this bit only after rewinding and
                // destroying all sketch-arena backing buffers while its mutex
                // is still held. Do not inspect the shared arena here: another
                // Vulkan request may already own it after runtime returned.
                if (built.ownership.arena_trimmed_before_permit_shrink != 1u) {
                    built.valid = false;
                    built.failure_reason = gpu_index_build_failure::
                        index_invariant_failed;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                if (built.ownership.peak_observed_bytes > permit->bytes()) {
                    built.valid = false;
                    built.failure_reason = gpu_index_build_failure::
                        peak_allocation_exceeds_admission;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                const std::uint64_t resident_bytes = bioseq::common::saturating_add(
                    built.slots.allocation_bytes,
                    built.positions.allocation_bytes);
                if (resident_bytes > permit->bytes()) {
                    built.valid = false;
                    built.failure_reason = gpu_index_build_failure::
                        peak_allocation_exceeds_admission;
                    built.positions.reset();
                    built.slots.reset();
                    return {};
                }
                permit->shrink_to(resident_bytes);
                built.ownership.resident_bytes = resident_bytes;
                const bool host_visible_coherent =
                    built.slots.is_host_visible_coherent()
                    && built.positions.is_host_visible_coherent();
                return bioseq::parallelism::memory_materialization_snapshot{
                    .actual_allocation_bytes = resident_bytes,
                    .fully_touched_bytes = resident_bytes,
                    .is_host_visible = host_visible_coherent,
                    .is_host_coherent = host_visible_coherent,
                    .is_process_footprint_reflected =
                        built.slots.process_footprint_reflected
                        && built.positions.process_footprint_reflected,
                };
            });
        if (!built.valid) {
            if (decline_out != nullptr
                && built.execution_peak_admission_failure.failed()) {
                decline_out->admission = built.execution_peak_admission_failure;
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
            materialization.allocation_snapshot.actual_allocation_bytes;
        built.ownership.materialization_fully_touched_bytes =
            materialization.allocation_snapshot.fully_touched_bytes;
        built.ownership.materialization_reflected_delta_bytes =
            materialization.reflected_delta_bytes;
        built.ownership.materialization_permit_released =
            materialization.is_permit_released ? 1u : 0u;
        built.ownership.materialization_host_visible =
            materialization.allocation_snapshot.is_host_visible ? 1u : 0u;
        built.ownership.materialization_host_coherent =
            materialization.allocation_snapshot.is_host_coherent ? 1u : 0u;
        built.ownership.materialization_process_footprint_reflected =
            materialization.allocation_snapshot.is_process_footprint_reflected ? 1u : 0u;
        built.permit = std::move(*permit);
        return built;
    } catch (const vulkan_runtime_error&) {
        throw;
    }
}

/// GPU minimizer sketch with full memory admission, returning both the host
/// minimizers (for the arena slab) and the resident device buffer (for
/// seed-lookup reuse). Returns `std::nullopt` for pre-dispatch shapes the GPU
/// cannot run (oversized k/alphabet, no usable device, or admission
/// failure). Selected Vulkan pipeline callers convert that decline into a hard
/// error; runtime errors propagate directly.
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_result_resident_impl(
    const p_sequence& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr,
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible)
{
    using bare = std::remove_cvref_t<p_sequence>;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = vulkan_decline_bp_size(seq),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(vulkan_sketch_decline_reason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0
                      || traits::bits_per_char > 32) {
            decline(vulkan_sketch_decline_reason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == alphabet_kernel_kind::generic
                    ? traits::bits_per_char
                    : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(vulkan_sketch_decline_reason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(vulkan_sketch_decline_reason::window_size_unsupported);
                return std::nullopt;
            }
            const sequence_upload_view upload = make_sequence_upload_view(seq);
            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            return sketch_uploaded_window_result<p_minimizer_type>(
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

template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_batch_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_batch_result(
    std::span<const p_sequence> seqs,
    std::size_t window_size,
    std::size_t kmer_size) = delete;

[[nodiscard]] std::optional<
    vulkan_batch_sketch_result<::bioseq::minimizers::minimap_sketch_data>>
collect_minimap_sketch_data_vulkan_upload_batch_result(
    std::span<const sequence_upload_view> uploads,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_host_materialization materialization)
{
    using minimizer_type = ::bioseq::minimizers::minimap_sketch_data;
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::common::saturating_mul;

    if (materialization != vulkan_sketch_host_materialization::host_and_device
        && materialization != vulkan_sketch_host_materialization::device_only) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch has invalid host materialization mode"};
    }
    if (uploads.empty()) {
        return vulkan_batch_sketch_result<minimizer_type>{
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

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!hardware_device_available(runtime) || runtime.features().max_workgroup_count_x == 0u) {
        return std::nullopt;
    }

    using query_tile_range = gpu_sketch_query_layout_wire;

    // Pass 1 is scalar-only: validate every upload and compute exact vector
    // sizes without constructing any owned vector or Vulkan buffer.
    std::uint64_t packed_sequence_bytes = 0u;
    std::uint64_t total_tile_count = 0u;
    std::uint64_t max_minimizers = 0u;
    std::uint64_t max_bp = 0u;
    const sequence_upload_view& first = uploads.front();
    const std::uint32_t letters_per_unit = first.letters_per_unit;
    const std::uint32_t unit_bits = first.unit_bits;
    const std::uint32_t bits_per_char = first.bits_per_char;
    const std::uint32_t char_mask = first.char_mask;
    const std::uint32_t alphabet_size = first.alphabet_size;
    const std::uint32_t padding_letters = first.padding_letters;
    const std::uint32_t alphabet_kind = static_cast<std::uint32_t>(first.alphabet_kind);

    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const sequence_upload_view& upload = uploads[query_index];
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
        max_minimizers = bioseq::common::saturating_add(
            max_minimizers,
            estimate_vulkan_minimizer_capacity(upload.bp_size, window_size));
        if (max_minimizers
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return std::nullopt;
        }

        const std::uint64_t aligned_begin =
            bioseq::common::checked_align_up(
                packed_sequence_bytes,
                sizeof(std::uint32_t))
                .value_or((std::numeric_limits<std::uint64_t>::max)());
        if (aligned_begin
                == (std::numeric_limits<std::uint64_t>::max)()
            || aligned_begin / sizeof(std::uint32_t)
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return std::nullopt;
        }
        packed_sequence_bytes = bioseq::common::saturating_add(
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
            const gpu_sketch_tile_plan tile_plan = make_sketch_tile_plan(
                upload.bp_size,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
            if (!tile_plan.is_dispatchable || tile_plan.tile_count == 0u) {
                return std::nullopt;
            }
            total_tile_count = bioseq::common::saturating_add(
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
        // exists.  Do not return a result that could be mistaken for is_internally_consistent
        // zero-emission Vulkan execution.
        return std::nullopt;
    }

    const gpu_sketch_batch_allocation_requirements initial_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!initial_requirements.valid) {
        return std::nullopt;
    }
    const std::uint64_t pessimistic_host_minimizer_bytes =
        materialization == vulkan_sketch_host_materialization::host_and_device
        ? saturating_mul(max_minimizers, sizeof(minimizer_type))
        : 0u;
    const std::uint64_t peak_host_minimizer_bytes = saturating_mul(
        pessimistic_host_minimizer_bytes,
        materialization == vulkan_sketch_host_materialization::host_and_device
            ? 2u
            : 0u);
    const auto vector_capacity_bytes = []<class p_value_type>(
        const std::vector<p_value_type>& values) noexcept -> std::uint64_t {
        if (values.capacity()
            > (std::numeric_limits<std::uint64_t>::max)()) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            sizeof(p_value_type));
    };
    const auto release_vector_storage = []<class p_value_type>(
        std::vector<p_value_type>& values) noexcept {
        std::vector<p_value_type>{}.swap(values);
    };
    const std::uint64_t exact_scaffold_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            packed_sequence_bytes,
            saturating_mul(
                total_tile_count,
                sizeof(gpu_sketch_tile_wire))),
        bioseq::common::saturating_add(
            bioseq::common::saturating_add(
                saturating_mul(uploads.size(), sizeof(query_tile_range)),
                saturating_mul(
                    uploads.size(),
                    2u * sizeof(gpu_seed_query_record_wire)
                        + sizeof(std::uint32_t))),
            saturating_mul(
                total_tile_count,
                2u * sizeof(std::uint32_t))));
    const std::uint64_t initial_peak_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            initial_requirements.arena_allocation_bytes,
            initial_requirements.pessimistic_output_allocation_bytes),
        bioseq::common::saturating_add(peak_host_minimizer_bytes, exact_scaffold_bytes));
    if (initial_peak_bytes
        == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::nullopt;
    }
    auto initial_permit = memory_admission_controller::instance().try_reserve(
        initial_peak_bytes);
    if (!initial_permit) return std::nullopt;

    // Declaration order is a correctness invariant: the shared permit holder
    // precedes result/scaffold/raw ownership, so reverse destruction always
    // releases vectors and Vulkan Buffers before admission on every exit.
    auto memory_retention =
        std::make_shared<vulkan_batch_sketch_memory_retention>();
    memory_retention->permit = std::move(*initial_permit);
    vulkan_batch_sketch_result<minimizer_type> result{
        .memory_retention = memory_retention,
        .materialization = materialization,
    };
    std::vector<std::byte> packed_sequence;
    std::vector<gpu_sketch_tile_wire> tiles;
    std::vector<query_tile_range> tile_ranges;

    result.query_records.resize(uploads.size());
    packed_sequence.reserve(
        static_cast<std::size_t>(packed_sequence_bytes));
    tiles.reserve(static_cast<std::size_t>(total_tile_count));
    tile_ranges.resize(uploads.size());

    // Pass 2 fills the exact preflight shapes without capacity growth under
    // normal allocators.  Actual capacities are reconciled before dispatch.
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const sequence_upload_view& upload = uploads[query_index];
        const std::size_t aligned_begin = static_cast<std::size_t>(
            bioseq::common::checked_align_up(
                packed_sequence.size(),
                sizeof(std::uint32_t))
                .value_or((std::numeric_limits<std::size_t>::max)()));
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
            const gpu_sketch_tile_plan tile_plan = make_sketch_tile_plan(
                upload.bp_size,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
            const std::uint32_t bp =
                static_cast<std::uint32_t>(upload.bp_size);
            const std::uint32_t overlap = sketch_replay_overlap_bp(
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size));
            for (std::uint32_t tile = 0u; tile < tile_plan.tile_count; ++tile) {
                const std::uint32_t lo = tile * tile_plan.tile_bp;
                const std::uint32_t hi =
                    std::min(bp, (tile + 1u) * tile_plan.tile_bp);
                tiles.push_back(gpu_sketch_tile_wire{
                    .seq_word_offset = seq_word_offset,
                    .bp_count = bp,
                    .replay_lo = lo > overlap ? lo - overlap : 0u,
                    .tile_lo = lo,
                    .tile_hi = hi,
                    .out_base = 0u,
                });
            }
        }
        tile_ranges[query_index] = query_tile_range{
            .begin = tile_begin,
            .count = static_cast<std::uint32_t>(
                tiles.size() - tile_begin),
            .query_len = static_cast<std::uint32_t>(upload.bp_size),
        };
    }
    if (packed_sequence.size() != packed_sequence_bytes
        || tiles.size() != total_tile_count) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch preflight/fill shape mismatch"};
    }

    const gpu_sketch_batch_allocation_requirements pre_dispatch_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!pre_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t actual_scaffold_capacity_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(tile_ranges),
            bioseq::common::saturating_add(
                vector_capacity_bytes(result.query_records),
                vector_capacity_bytes(result.host_minimizers))));
    const std::uint64_t anticipated_raw_count_bytes = saturating_mul(
        total_tile_count,
        2u * sizeof(std::uint32_t));
    const std::uint64_t authority_query_record_headroom = saturating_mul(
        saturating_mul(
            uploads.size(),
            sizeof(gpu_seed_query_record_wire) + sizeof(std::uint32_t)),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t pre_dispatch_peak_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            pre_dispatch_requirements.arena_allocation_bytes,
            pre_dispatch_requirements.pessimistic_output_allocation_bytes),
        bioseq::common::saturating_add(
            peak_host_minimizer_bytes,
            bioseq::common::saturating_add(
                actual_scaffold_capacity_bytes,
                bioseq::common::saturating_add(
                    anticipated_raw_count_bytes,
                    authority_query_record_headroom))));
    bioseq::parallelism::memory_admission_failure growth_failure{};
    if (pre_dispatch_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            pre_dispatch_peak_bytes,
            &growth_failure)) {
        return std::nullopt;
    }

    const vulkan_sketch_params sketch_params{
        .bp_size = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                max_bp,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)()))),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = minimizer_wire_traits<minimizer_type>::k_hash_bits,
        .hash_shift = minimizer_wire_traits<minimizer_type>::k_hash_shift,
        .pos_bits = minimizer_wire_traits<minimizer_type>::k_position_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            minimizer_wire_traits<minimizer_type>::k_word_kind),
        .minimizer_words32 =
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
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
    // buffer/vector aggregate first, then scaffolds/result, then the permit.
    runtime_sketch_batch_result raw =
        runtime.run_sketch_tile_batch_roundtrip(
            std::span<const std::byte>{
                packed_sequence.data(),
                packed_sequence.size()},
            rank_lut,
            sketch_params,
            std::span<const gpu_sketch_tile_wire>{tiles.data(), tiles.size()},
            std::span<const gpu_sketch_query_layout_wire>{
                tile_ranges.data(), tile_ranges.size()},
            max_minimizers,
            materialization);
    const gpu_sketch_batch_allocation_requirements post_dispatch_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!post_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t post_dispatch_live_vector_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(tile_ranges),
            bioseq::common::saturating_add(
                bioseq::common::saturating_add(
                    vector_capacity_bytes(result.query_records),
                    vector_capacity_bytes(result.host_minimizers)),
                bioseq::common::saturating_add(
                    vector_capacity_bytes(raw.host_minimizer_bytes),
                    bioseq::common::saturating_add(
                        vector_capacity_bytes(raw.tile_counts),
                        bioseq::common::saturating_add(
                            vector_capacity_bytes(raw.tile_offsets),
                            raw.query_authority.owned_capacity_bytes()))))));
    const std::uint64_t post_dispatch_live_bytes = bioseq::common::saturating_add(
        raw.minimizer_words.allocation_bytes,
        bioseq::common::saturating_add(
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
    if (raw.dispatch_stats.materialization() != materialization
        || !raw.dispatch_stats.is_internally_consistent()) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch returned without matching is_internally_consistent "
            "count/scan/emit stats"};
    }
    if (materialization == vulkan_sketch_host_materialization::device_only
        && !raw.query_authority.does_cover_device_output()) {
        throw vulkan_runtime_error{
            "device-only Vulkan batch sketch returned without runtime-issued "
            "device-output coverage authority"};
    }

    if (materialization == vulkan_sketch_host_materialization::host_and_device) {
        result.host_minimizers = decode_host_minimizer_bytes<minimizer_type>(
            std::span<const std::byte>{
                raw.host_minimizer_bytes.data(),
                raw.host_minimizer_bytes.size()},
            raw.count);
    } else if (!raw.host_minimizer_bytes.empty()) {
        throw vulkan_runtime_error{
            "device-only Vulkan batch sketch unexpectedly materialized host hashes"};
    }
    std::uint64_t query_minimizer_cursor = 0u;
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const query_tile_range range = tile_ranges[query_index];
        std::uint64_t minimizer_count = 0u;
        for (std::uint32_t tile = 0u; tile < range.count; ++tile) {
            minimizer_count += raw.tile_counts[range.begin + tile];
        }
        const std::uint64_t minimizer_offset = range.count == 0u
            ? query_minimizer_cursor
            : static_cast<std::uint64_t>(raw.tile_offsets[range.begin]);
        if (minimizer_offset != query_minimizer_cursor) {
            throw vulkan_runtime_error{
                "Vulkan batch query minimizer slices are not contiguous"};
        }
        if (minimizer_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw vulkan_runtime_error{
                "Vulkan batch query minimizer count exceeds ABI"};
        }
        if (uploads[query_index].bp_size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw vulkan_runtime_error{"Vulkan batch query length exceeds ABI"};
        }
        result.query_records[query_index] = gpu_seed_query_record_wire{
            .minimizer_offset = static_cast<std::uint32_t>(minimizer_offset),
            .minimizer_count = static_cast<std::uint32_t>(minimizer_count),
            .query_len = static_cast<std::uint32_t>(uploads[query_index].bp_size),
            .flags = 0u,
        };
        query_minimizer_cursor = bioseq::common::saturating_add(
            query_minimizer_cursor,
            minimizer_count);
    }
    if (query_minimizer_cursor != raw.count) {
        throw vulkan_runtime_error{
            "Vulkan batch query minimizer slices do not cover device output"};
    }

    const std::uint64_t retained_host_capacity_bytes =
        vector_capacity_bytes(result.host_minimizers);
    const std::uint64_t retained_query_record_capacity_bytes =
        vector_capacity_bytes(result.query_records);
    const std::uint64_t retained_bytes = bioseq::common::saturating_add(
        raw.minimizer_words.allocation_bytes,
        bioseq::common::saturating_add(
            retained_host_capacity_bytes,
            bioseq::common::saturating_add(
                retained_query_record_capacity_bytes,
                raw.query_authority.owned_capacity_bytes())));
    const std::uint64_t transient_vector_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(raw.host_minimizer_bytes),
            vector_capacity_bytes(raw.tile_counts)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(raw.tile_offsets),
            bioseq::common::saturating_add(
                vector_capacity_bytes(packed_sequence),
                bioseq::common::saturating_add(
                    vector_capacity_bytes(tiles),
                    vector_capacity_bytes(tile_ranges)))));
    const std::uint64_t final_live_peak_bytes = bioseq::common::saturating_add(
        post_dispatch_requirements.arena_allocation_bytes,
        bioseq::common::saturating_add(retained_bytes, transient_vector_bytes));
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

    auto storage =
        std::make_shared<vulkan_device_minimizer_storage<minimizer_type>>();
    storage->words = std::move(raw.minimizer_words);
    storage->batch_memory_retention = memory_retention;
    storage->count = raw.count;
    storage->capacity =
        materialization == vulkan_sketch_host_materialization::host_and_device
        ? max_minimizers
        : raw.count;
    result.device = std::move(storage);
    result.dispatch_stats = raw.dispatch_stats;
    result.query_authority = std::move(raw.query_authority);
    result.device_minimizer_count = raw.count;
    return result;
}

/// Build the target minimizer index entirely on the GPU from the
/// target sequence (parallel sketch → device radix sort → race-free slot table).
/// Returns `std::nullopt` with `decline_out` populated for every typed shape,
/// admission, allocation, capacity, or invariant refusal. Vulkan API failures
/// still propagate. p_alphabet-generic; only the shader specialises.
/// The returned `gpu_built_index` owns the resident
/// slots/positions device buffers — the indexing layer adopts them.
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_index_on_device_resident_impl(
    const p_sequence& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    static const auto* index_build_region_description =
        bioseq::instrumentation::register_static_region("gpu_index_build");
    auto index_build_region_scope =
        bioseq::instrumentation::region(index_build_region_description);
    using bare = std::remove_cvref_t<p_sequence>;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = vulkan_decline_bp_size(seq),
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(vulkan_sketch_decline_reason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            decline(vulkan_sketch_decline_reason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                choose_alphabet_kernel<bare>() == alphabet_kernel_kind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(vulkan_sketch_decline_reason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(vulkan_sketch_decline_reason::window_size_unsupported);
                return std::nullopt;
            }

            const sequence_upload_view upload = make_sequence_upload_view(seq);
            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            return build_gpu_index_from_upload_on_device<p_minimizer_type>(
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
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN build_gpu_segment_index_on_device_resident_impl
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_segment_index_on_device_resident_impl(
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    static const auto* segment_index_build_region_description =
        bioseq::instrumentation::register_static_region(
            "gpu_segment_index_build");
    auto segment_index_build_region_scope = bioseq::instrumentation::region(
        segment_index_build_region_description);
    using bare = std::remove_cvref_t<p_sequence>;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = vulkan_decline_bp_size(seq),
            .query_gate_words = static_cast<std::uint64_t>(
                query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(vulkan_sketch_decline_reason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (traits::alphabet_size > 256 || traits::bits_per_char == 0 ||
                      traits::bits_per_char > 32) {
            decline(vulkan_sketch_decline_reason::alphabet_unsupported);
            return std::nullopt;
        } else {
            constexpr std::size_t step_bits =
                alphabet_kernel_kind_of_traits<traits>() == alphabet_kernel_kind::generic
                    ? traits::bits_per_char : std::size_t{2};
            if (kmer_size == 0 || kmer_size > 64 / step_bits || kmer_size > 32) {
                decline(vulkan_sketch_decline_reason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (window_size == 0 || window_size > 256) {
                decline(vulkan_sketch_decline_reason::window_size_unsupported);
                return std::nullopt;
            }

            const segment_upload_descriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            if (decline_out != nullptr) {
                decline_out->bp_size = descriptor.window_bp;
            }
            if (descriptor.window_bp == 0u) {
                decline(vulkan_sketch_decline_reason::empty_sequence);
                return std::nullopt;
            }
            if (descriptor.window_bp
                < static_cast<std::uint64_t>(window_size + kmer_size)) {
                decline(vulkan_sketch_decline_reason::segment_window_too_short);
                return std::nullopt;
            }

            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                descriptor.lo_unit);
            const auto* base = seq.read_cptr(begin_iter);
            const sequence_upload_view upload =
                make_upload_view_from_window<bare>(seq, base, descriptor);
            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            return build_gpu_index_from_upload_on_device<p_minimizer_type>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        }
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END build_gpu_segment_index_on_device_resident_impl

/// p_collector_type-compatible wrapper for existing minimizer call sites. Feeds the
/// GPU sketch into `collector`; returns false only for pre-dispatch ineligible
/// shapes. runtime failures propagate.
template <class p_minimizer_type, class p_collector_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_sketch_dispatch_token>
collect_minimizers_vulkan_resident_impl(
    p_collector_type& collector, const p_sequence& seq,
    std::size_t window_size, std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr)
{
    auto result = collect_minimizers_vulkan_result_resident_impl<p_minimizer_type>(
        seq, window_size, kmer_size, decline_out);
    if (!result) {
        return std::nullopt;
    }
    for (const p_minimizer_type& minimizer : result->host_minimizers) {
        collector.collect(minimizer);
    }
    if (!result->dispatch_token.is_internally_consistent()) {
        throw vulkan_runtime_error{
            "Vulkan minimizer sketch returned without a completed dispatch token"};
    }
    return result->dispatch_token;
}

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
        throw vulkan_runtime_error{"Vulkan sorted-pair count exceeds host address space"};
    }
    const std::size_t count = static_cast<std::size_t>(pair_count);
    const std::size_t expected = count * sizeof(std::uint64_t);
    if (key_bytes.size() < expected || value_bytes.size() < expected) {
        throw vulkan_runtime_error{"Vulkan sorted-pair download was truncated"};
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
/// storage, runs the fused tiled minimizer sketch on the GPU for every
/// supported alphabet, globalises/filters to the core range, then returns pairs
/// sorted by
/// (kmer, encoded_pos).
/// Throws `vulkan_runtime_error` on every selected-GPU decline — no scalar fallback.
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw
template <class p_sequence>
[[nodiscard]] vulkan_owned_segment_pairs
collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw(
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    bioseq::storage::sequence_size_t emit_base_bp,
    bioseq::storage::sequence_size_t core_lo_bp,
    bioseq::storage::sequence_size_t core_hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_segment_pair_execution_stats* execution_stats_out = nullptr)
{
    using bare = std::remove_cvref_t<p_sequence>;
    using minimizer_type = bioseq::minimizers::minimap_sketch_data;
    vulkan_segment_pair_execution_stats execution_stats{};
    if (execution_stats_out != nullptr) {
        *execution_stats_out = {};
    }
    vulkan_sketch_decline path_decline{
        .bp_size = vulkan_decline_bp_size(seq),
        .window_size = window_size,
        .kmer_size = kmer_size,
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        path_decline.reason =
            vulkan_sketch_decline_reason::fragmented_sequence_unsupported;
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer pair path", path_decline);
    } else {
        using traits = typename bare::traits;
        using storage_unit = typename traits::storage_unit;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            path_decline.reason = vulkan_sketch_decline_reason::alphabet_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer pair path", path_decline);
        } else {
            if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::kmer_size_unsupported;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (!vulkan_minimizer_window_size_supported(window_size)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::window_size_unsupported;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const std::uint64_t minimum_bases =
                static_cast<std::uint64_t>(window_size) + kmer_size - 1u;

            const segment_upload_descriptor descriptor =
                compute_segment_descriptor(seq, lo_unit, hi_unit);
            path_decline.bp_size = descriptor.window_bp;
            if (descriptor.window_bp == 0u) {
                path_decline.reason = vulkan_sketch_decline_reason::empty_sequence;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (descriptor.window_bp
                > static_cast<std::uint64_t>(
                    minimizer_type::k_position_field_mask)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::segment_position_field_overflow;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            if (descriptor.window_bp
                < minimum_bases) {
                if (execution_stats_out != nullptr) {
                    *execution_stats_out = execution_stats;
                }
                return {};
            }

            auto& runtime = bioseq::gpu::vulkan::runtime::instance();
            if (!hardware_device_available(runtime)) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::hardware_device_unavailable;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }

            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            const std::size_t lpu =
                std::max<std::size_t>(1u, traits::letters_per_unit);
            const std::size_t context_units =
                sketch_segment_context_units<traits>(window_size, kmer_size);
            const std::uint64_t collected_limit =
                admitted_sketch_collection_limit_bp<minimizer_type, traits>(
                    window_size,
                    kmer_size,
                    gpu_sketch_purpose::sorted_pairs);
            const std::uint64_t context_bp =
                sketch_segment_context_bp<traits>(window_size, kmer_size);
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
                    : static_cast<std::size_t>(bioseq::common::ceil_div(
                        static_cast<std::uint64_t>(descriptor_units),
                        static_cast<std::uint64_t>(core_units)));
            const std::size_t maximum_sub_units = std::min(
                descriptor_units,
                core_units + std::min(
                    descriptor_units,
                    context_units * 2u));
            const std::uint64_t maximum_sub_bp = std::min(
                descriptor.window_bp,
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(maximum_sub_units),
                    static_cast<std::uint64_t>(lpu)));
            const std::uint64_t maximum_dispatch_minimizers =
                estimate_vulkan_minimizer_capacity(
                    maximum_sub_bp, window_size);
            const std::uint64_t maximum_sequence_bytes = aligned_upload_bytes(
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(maximum_sub_units),
                    static_cast<std::uint64_t>(sizeof(storage_unit))));
            const std::uint64_t maximum_resident_requested_bytes =
                vulkan_sketch_resident_bytes<minimizer_type>(
                    maximum_dispatch_minimizers);
            const std::uint64_t maximum_host_bytes =
                bioseq::common::saturating_mul(
                    bioseq::common::saturating_mul(
                        maximum_dispatch_minimizers,
                        static_cast<std::uint64_t>(sizeof(minimizer_type))),
                    bioseq::parallelism::k_vector_preallocation_headroom_factor);
            const gpu_sketch_memory_plan maximum_dispatch_plan =
                make_gpu_sketch_memory_plan(
                    maximum_sub_bp,
                    maximum_sequence_bytes,
                    sizeof(vulkan_rank_lut),
                    maximum_dispatch_minimizers,
                    maximum_resident_requested_bytes,
                    gpu_sketch_purpose::minimizer_words,
                    static_cast<std::uint32_t>(kmer_size),
                    static_cast<std::uint32_t>(window_size),
                    static_cast<std::uint32_t>(lpu),
                    runtime.features().max_workgroup_count_x);
            if (!maximum_dispatch_plan.is_valid) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const gpu_sketch_batch_allocation_requirements
                maximum_allocation_requirements =
                    runtime.sketch_batch_allocation_requirements(
                        maximum_sequence_bytes,
                        maximum_dispatch_plan.tiles.tile_count,
                            static_cast<std::uint32_t>(
                            minimizer_wire_traits<minimizer_type>::k_word_kind),
                        minimizer_wire_traits<
                            minimizer_type>::k_words32_per_minimizer,
                        maximum_dispatch_minimizers);
            if (!maximum_allocation_requirements.valid) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            const std::uint64_t maximum_physical_arena_bytes =
                runtime.gpu_arena_allocation_requirement(
                    maximum_dispatch_plan.arena_scaffold_bytes);
            const std::uint64_t maximum_host_transient_headroom_bytes =
                vulkan_exact_host_allocation_admission_bytes(
                    maximum_dispatch_plan.count_submit_host_transient_bytes);
            const std::uint64_t maximum_dispatch_bytes =
                vulkan_sketch_dispatch_admission_bytes(
                    maximum_physical_arena_bytes,
                    maximum_host_transient_headroom_bytes,
                    0u,
                    maximum_allocation_requirements
                        .pessimistic_output_allocation_bytes);
            const std::uint64_t maximum_host_materialization_bytes =
                vulkan_sketch_host_materialization_admission_bytes(
                    maximum_allocation_requirements
                        .pessimistic_output_allocation_bytes,
                    maximum_host_bytes);
            // The borrowed slice covers the larger of the mutually exclusive
            // dispatch and exact-host materialization. It must use the same physical
            // arena/output requirements as `sketch_uploaded_window_result`,
            // including a reusable arena's high-water allocation.
            const std::uint64_t dispatch_budget = std::max(
                maximum_dispatch_bytes,
                maximum_host_materialization_bytes);
            const std::uint64_t initial_filtered_count =
                estimate_vulkan_minimizer_capacity(
                    descriptor.window_bp, window_size);
            const auto headroom_bytes =
                []<class p_value>(std::uint64_t count) {
                return bioseq::common::saturating_mul(
                    bioseq::common::saturating_mul(
                        count, static_cast<std::uint64_t>(sizeof(p_value))),
                    bioseq::parallelism::k_vector_preallocation_headroom_factor);
            };
            const std::uint64_t initial_owner_bytes = bioseq::common::saturating_add(
                dispatch_budget,
                bioseq::common::saturating_add(
                    headroom_bytes.template operator()<gpu_sketch_segment_window>(
                        initial_window_count),
                    bioseq::common::saturating_add(
                        headroom_bytes.template operator()<storage_unit>(
                            maximum_sub_units),
                        headroom_bytes.template operator()<
                            std::pair<std::uint64_t, std::uint64_t>>(
                                initial_filtered_count))));
            if (dispatch_budget == (std::numeric_limits<std::uint64_t>::max)()
                || initial_owner_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_plan_invalid;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(initial_owner_bytes);
            if (!permit) {
                path_decline.reason =
                    vulkan_sketch_decline_reason::memory_admission_denied;
                throw_vulkan_strict_decline(
                    "Vulkan segment minimizer pair path", path_decline);
            }
            struct segment_pair_owner {
                bioseq::parallelism::memory_permit permit;
                std::vector<gpu_sketch_segment_window> windows;
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
            auto windows = sketch_segment_windows<minimizer_type>(
                seq,
                descriptor,
                window_size,
                kmer_size,
                gpu_sketch_purpose::sorted_pairs,
                collected_limit);
            owner.windows.swap(windows);

            const auto retained_owner_bytes = [&]() noexcept {
                std::uint64_t bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.windows.capacity()),
                    static_cast<std::uint64_t>(sizeof(gpu_sketch_segment_window)));
                bytes = bioseq::common::saturating_add(bytes, bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.scratch.capacity()),
                    static_cast<std::uint64_t>(sizeof(storage_unit))));
                return bioseq::common::saturating_add(bytes, bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.filtered.capacity()),
                    static_cast<std::uint64_t>(
                        sizeof(std::pair<std::uint64_t, std::uint64_t>))));
            };
            const auto reconcile_owner = [&]() {
                const std::uint64_t exact = retained_owner_bytes();
                const std::uint64_t live = bioseq::common::saturating_add(exact, dispatch_budget);
                if (exact == (std::numeric_limits<std::uint64_t>::max)()
                    || live == (std::numeric_limits<std::uint64_t>::max)()
                    || live > owner.permit.bytes()) {
                    throw vulkan_runtime_error{
                        "Vulkan segment pair owner exceeded admitted capacity"};
                }
                owner.permit.shrink_to(live);
            };
            const auto ensure_owner_capacity = [&]<class p_value>(
                    std::vector<p_value>& values,
                    std::size_t required) {
                if (values.capacity() >= required) {
                    return;
                }
                const std::uint64_t peak = bioseq::common::saturating_add(
                    bioseq::common::saturating_add(retained_owner_bytes(), dispatch_budget),
                    headroom_bytes.template operator()<p_value>(required));
                if (peak == (std::numeric_limits<std::uint64_t>::max)()
                    || !owner.permit.grow_to(peak)) {
                    throw vulkan_runtime_error{
                        "Vulkan segment pair owner growth admission failed"};
                }
                values.reserve(required);
                reconcile_owner();
            };
            reconcile_owner();
            for (std::size_t window_index = 0u;
                 window_index < owner.windows.size();
                 ++window_index) {
                const gpu_sketch_segment_window window = owner.windows[window_index];
                const segment_upload_descriptor sub_descriptor =
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
                const auto* base = seq.read_cptr(begin_iter);
                const sequence_upload_view upload =
                    make_upload_view_from_window<bare>(seq, base, sub_descriptor);
                const std::uint64_t max_minimizers =
                    estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
                bool capacity_overflowed = false;
                vulkan_sketch_decline decline{};
                auto result = sketch_uploaded_window_result<minimizer_type>(
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
                            == vulkan_sketch_decline_reason::
                                output_capacity_overflow)) {
                        throw vulkan_runtime_error{
                            "Vulkan segment minimizer pair decline/capacity "
                            "identity mismatch"};
                    }
                    gpu_sketch_segment_window left{};
                    gpu_sketch_segment_window right{};
                    if (vulkan_sketch_decline_allows_segment_split(
                            decline.reason)
                        && split_sketch_segment_window(
                            seq,
                            descriptor,
                            window,
                            context_units,
                            left,
                            right)) {
                        execution_stats.record_gpu_window_split();
                        if (owner.windows.size()
                                > (std::numeric_limits<std::size_t>::max)() - 2u) {
                            throw vulkan_runtime_error{
                                "Vulkan segment window split count overflow"};
                        }
                        ensure_owner_capacity(
                            owner.windows, owner.windows.size() + 2u);
                        const auto insert_at =
                            static_cast<std::vector<gpu_sketch_segment_window>::difference_type>(
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

                if (!result->dispatch_token.is_internally_consistent()) {
                    throw vulkan_runtime_error{
                        "Vulkan segment minimizer pair path returned without "
                        "an is_internally_consistent operation-originated dispatch token"};
                }
                execution_stats.record_completed_roundtrip(
                    result->host_minimizers.size());

                const std::uint64_t sub_emit_base_bp =
                    static_cast<std::uint64_t>(emit_base_bp)
                    + static_cast<std::uint64_t>(
                        sub_descriptor.lo_unit - descriptor.lo_unit)
                        * static_cast<std::uint64_t>(lpu);
                if (result->host_minimizers.size()
                        > (std::numeric_limits<std::size_t>::max)()
                            - owner.filtered.size()) {
                    throw vulkan_runtime_error{
                        "Vulkan segment filtered pair count overflow"};
                }
                ensure_owner_capacity(
                    owner.filtered,
                    owner.filtered.size() + result->host_minimizers.size());
                for (const minimizer_type& minimizer : result->host_minimizers) {
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
                    std::vector<minimizer_type> empty;
                    result->host_minimizers.swap(empty);
                }
                result->host_permit.reset();
                result->device.reset();
            }
            // std::pair's default ordering sorts by (kmer, encoded_pos), exactly
            // the order the minimizer-sequence and minimizer-fragment builders
            // expect from a "sorted pair run".
            std::sort(owner.filtered.begin(), owner.filtered.end());
            owner.filtered.erase(
                std::unique(owner.filtered.begin(), owner.filtered.end()),
                owner.filtered.end());
            if (execution_stats_out != nullptr) {
                *execution_stats_out = execution_stats;
            }
            {
                std::vector<gpu_sketch_segment_window> empty_windows;
                std::vector<storage_unit> empty_scratch;
                owner.windows.swap(empty_windows);
                owner.scratch.swap(empty_scratch);
            }
            const std::uint64_t exact_pair_bytes =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(owner.filtered.capacity()),
                    static_cast<std::uint64_t>(
                        sizeof(std::pair<std::uint64_t, std::uint64_t>)));
            if (exact_pair_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || exact_pair_bytes > owner.permit.bytes()) {
                throw vulkan_runtime_error{
                    "Vulkan segment returned pairs exceeded admission"};
            }
            owner.permit.shrink_to(exact_pair_bytes);
            vulkan_owned_segment_pairs output;
            output.pairs.swap(owner.filtered);
            output.permit = std::move(owner.permit);
            return output;
        }
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw

/// p_collector_type-compatible segment sketch kept for tests and target alignment. It
/// is checked and throws when used as a selected Vulkan path.
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_minimizers_vulkan_segment_resident_impl_or_throw
template <class p_minimizer_type, class p_collector_type, class p_sequence>
void collect_minimizers_vulkan_segment_resident_impl_or_throw(
    p_collector_type& collector,
    const p_sequence& seq,
    std::size_t lo_unit,
    std::size_t hi_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {})
{
    using bare = std::remove_cvref_t<p_sequence>;
    vulkan_sketch_decline path_decline{
        .bp_size = vulkan_decline_bp_size(seq),
        .query_gate_words = static_cast<std::uint64_t>(
            query_gate.words.size()),
        .window_size = window_size,
        .kmer_size = kmer_size,
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        path_decline.reason =
            vulkan_sketch_decline_reason::fragmented_sequence_unsupported;
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer sketch", path_decline);
    } else {
        using traits = typename bare::traits;
        using minimizer_limit = bioseq::minimizers::minimap_sketch_data;

        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            path_decline.reason = vulkan_sketch_decline_reason::alphabet_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        } else if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::kmer_size_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        } else if (!vulkan_minimizer_window_size_supported(window_size)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::window_size_unsupported;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }

        const segment_upload_descriptor descriptor =
            compute_segment_descriptor(seq, lo_unit, hi_unit);
        path_decline.bp_size = descriptor.window_bp;
        if (descriptor.window_bp == 0u) {
            path_decline.reason = vulkan_sketch_decline_reason::empty_sequence;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        if (descriptor.window_bp
            > static_cast<std::uint64_t>(minimizer_limit::k_position_field_mask)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::segment_position_field_overflow;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        if (descriptor.window_bp
            < static_cast<std::uint64_t>(window_size + kmer_size)) {
            return;
        }

        auto& runtime = bioseq::gpu::vulkan::runtime::instance();
        if (!hardware_device_available(runtime)) {
            path_decline.reason =
                vulkan_sketch_decline_reason::hardware_device_unavailable;
            throw_vulkan_strict_decline(
                "Vulkan segment minimizer sketch", path_decline);
        }
        const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
        const std::size_t lpu = std::max<std::size_t>(1u, traits::letters_per_unit);
        const std::uint64_t outer_base_bp =
            static_cast<std::uint64_t>(descriptor.lo_unit) * lpu;
        std::vector<gpu_sketch_segment_window> windows =
            sketch_segment_windows<p_minimizer_type>(
                seq,
                descriptor,
                window_size,
                kmer_size,
                gpu_sketch_purpose::minimizer_words);
        if (query_gate.enabled() && windows.size() != 1u) {
            path_decline.reason = vulkan_sketch_decline_reason::
                query_gate_requires_single_window;
            throw_vulkan_strict_decline(
                "query-gated Vulkan segment minimizer sketch", path_decline);
        }
        const std::size_t context_units =
            sketch_segment_context_units<traits>(window_size, kmer_size);
        for (std::size_t window_index = 0u;
             window_index < windows.size();
             ++window_index) {
            const gpu_sketch_segment_window window = windows[window_index];
            const segment_upload_descriptor sub_descriptor =
                compute_segment_descriptor(seq, window.sub_lo_unit, window.sub_hi_unit);
            if (sub_descriptor.window_bp == 0u
                || sub_descriptor.window_bp
                    < static_cast<std::uint64_t>(window_size + kmer_size)) {
                continue;
            }
            auto begin_iter = seq.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(
                sub_descriptor.lo_unit);
            const auto* base = seq.read_cptr(begin_iter);
            const sequence_upload_view upload =
                make_upload_view_from_window<bare>(seq, base, sub_descriptor);

            const std::uint64_t max_minimizers =
                estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);
            bool capacity_overflowed = false;
            vulkan_sketch_decline decline{};
            auto result = sketch_uploaded_window_result<p_minimizer_type>(
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
                        == vulkan_sketch_decline_reason::
                            output_capacity_overflow)) {
                    throw vulkan_runtime_error{
                        "Vulkan segment minimizer sketch decline/capacity "
                        "identity mismatch"};
                }
                if (query_gate.enabled()) {
                    throw_vulkan_strict_decline(
                        "query-gated Vulkan segment minimizer sketch", decline);
                }
                gpu_sketch_segment_window left{};
                gpu_sketch_segment_window right{};
                if (vulkan_sketch_decline_allows_segment_split(decline.reason)
                    && split_sketch_segment_window(
                        seq,
                        descriptor,
                        window,
                        context_units,
                        left,
                        right)) {
                    const auto insert_at =
                        static_cast<std::vector<gpu_sketch_segment_window>::difference_type>(
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
            for (const p_minimizer_type& minimizer : result->host_minimizers) {
                const std::uint64_t abs_pos =
                    sub_base_bp + minimizer.last_pos();
                if (abs_pos < core_lo_bp || abs_pos >= core_hi_bp) {
                    continue;
                }
                collector.collect(p_minimizer_type{
                    minimizer.kmer(),
                    abs_pos - outer_base_bp,
                    minimizer.strand()});
            }
        }
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_minimizers_vulkan_segment_resident_impl_or_throw

template <class p_minimizer_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<vulkan_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_result(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr,
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible)
{
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, decline_out)) {
        return std::nullopt;
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, 0u, sequence.bp_size(), decline_out);
    if (!window) {
        return std::nullopt;
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [window_size, kmer_size, decline_out, host_ownership_route]
        <class p_range>(p_range& resident) {
            return collect_minimizers_vulkan_result_resident_impl<p_minimizer_type>(
                resident,
                window_size,
                kmer_size,
                decline_out,
                host_ownership_route);
        });
}

template <class p_minimizer_type, class p_collector_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<vulkan_sketch_dispatch_token>
collect_minimizers_vulkan(
    p_collector_type& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr)
{
    auto result = collect_minimizers_vulkan_result<p_minimizer_type>(
        sequence, window_size, kmer_size, decline_out);
    if (!result) {
        return std::nullopt;
    }
    for (const p_minimizer_type& minimizer : result->host_minimizers) {
        collector.collect(minimizer);
    }
    if (!result->dispatch_token.is_internally_consistent()) {
        throw vulkan_runtime_error{
            "Vulkan minimizer sketch returned without a completed dispatch token"};
    }
    return result->dispatch_token;
}

template <class p_minimizer_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_index_on_device(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, decline_out)) {
        return std::nullopt;
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, 0u, sequence.bp_size(), decline_out);
    if (!window) {
        return std::nullopt;
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [window_size, kmer_size, query_gate, decline_out]
        <class p_range>(p_range& resident) {
            return build_gpu_index_on_device_resident_impl<p_minimizer_type>(
                resident,
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        });
}

template <class p_minimizer_type, sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<gpu_built_index> build_gpu_segment_index_on_device(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr)
{
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, decline_out)) {
        return std::nullopt;
    }
    if (lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        if (decline_out != nullptr) {
            decline_out->reason =
                vulkan_sketch_decline_reason::sequence_type_unsupported;
        }
        return std::nullopt;
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, lo_bp, hi_bp, decline_out);
    if (!window) {
        return std::nullopt;
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [window_size, kmer_size, query_gate, decline_out]
        <class p_range>(p_range& resident) {
            return build_gpu_segment_index_on_device_resident_impl<p_minimizer_type>(
                resident,
                0u,
                static_cast<std::size_t>(resident.size()),
                window_size,
                kmer_size,
                query_gate,
                decline_out);
        });
}

template <class p_minimizer_type, class p_collector_type, sequence_alphabet p_alphabet>
void collect_minimizers_vulkan_segment_or_throw(
    p_collector_type& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_query_gate query_gate = {})
{
    vulkan_sketch_decline decline{};
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, &decline)
        || lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        if (!decline.declined()) {
            decline.reason = vulkan_sketch_decline_reason::sequence_type_unsupported;
        }
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer sketch", decline);
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, lo_bp, hi_bp, &decline);
    if (!window) {
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer sketch", decline);
    }
    admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [&collector, window_size, kmer_size, query_gate]
        <class p_range>(p_range& resident) {
            collect_minimizers_vulkan_segment_resident_impl_or_throw<p_minimizer_type>(
                collector,
                resident,
                0u,
                static_cast<std::size_t>(resident.size()),
                window_size,
                kmer_size,
                query_gate);
        });
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] vulkan_owned_segment_pairs
collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    bioseq::storage::sequence_size_t emit_base_bp,
    bioseq::storage::sequence_size_t core_lo_bp,
    bioseq::storage::sequence_size_t core_hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_segment_pair_execution_stats* execution_stats_out = nullptr)
{
    vulkan_sketch_decline decline{};
    if (!admitted_sequence_detail::validate_facade_sketch_shape(
            sequence, window_size, kmer_size, &decline)
        || lo_bp > hi_bp || hi_bp > sequence.bp_size()) {
        if (!decline.declined()) {
            decline.reason = vulkan_sketch_decline_reason::sequence_type_unsupported;
        }
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer pair path", decline);
    }
    auto window = admitted_sequence_detail::prepare_canonical_u32_window(
        sequence, lo_bp, hi_bp, &decline);
    if (!window) {
        throw_vulkan_strict_decline(
            "Vulkan segment minimizer pair path", decline);
    }
    return admitted_sequence_detail::with_canonical_u32_upload_source<p_alphabet>(
        *window,
        [emit_base_bp, core_lo_bp, core_hi_bp, window_size, kmer_size,
         execution_stats_out]
        <class p_range>(p_range& resident) {
            return collect_sorted_minimizer_pairs_vulkan_segment_resident_impl_or_throw(
                resident,
                0u,
                static_cast<std::size_t>(resident.size()),
                emit_base_bp,
                core_lo_bp,
                core_hi_bp,
                window_size,
                kmer_size,
                execution_stats_out);
        });
}

} // namespace bioseq::gpu::vulkan
