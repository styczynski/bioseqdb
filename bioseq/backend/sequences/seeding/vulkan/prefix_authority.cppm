/**
 * @file prefix_authority.cppm
 * @brief Validated mapped and host prefix-count authorities for seed lookup.
 */
module;

#include <fmt/format.h>
#include <libassert/assert.hpp>

#include <cstring>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <vulkan/vulkan_core.h>

export module bioseq.seeding.vulkan:prefix_authority;

export import :diagnostics;
import bioseq.common.checked_arithmetic;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::seeding {

// ===========================================================================
// Non-allocating prefix prepass
// ===========================================================================

/// Allocation-free count-pass result for the exclusive seed prefix.
struct seed_prefix_plan {
    std::uint64_t prefix_len = 0;
    std::uint64_t exact_anchor_count = 0;
    std::uint64_t mapped_lookup_count = 0;
    std::uint64_t mapped_comparison_count = 0;
};

/// Materialized prefix plus mapped-index validation work counters.
struct seed_prefix_build_result {
    std::vector<std::uint32_t> prefix;
    std::uint64_t mapped_lookup_count = 0u;
    std::uint64_t mapped_comparison_count = 0u;
};

/// Raised when mapped sorted-dense authority changes or fails validation.
struct mapped_prefix_invalid_error final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "mapped sorted-dense prefix authority is invalid";
    }
};

/// Validated, non-owning mapped view used by both prefix passes.
struct mapped_sorted_dense_prefix_view {
    std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index>
        resident;
    const bioseq::indexing::gpu_minimizer_slot* controls = nullptr;
    std::uint32_t bucket_count = 0u;
    std::uint32_t position_count = 0u;
    std::uint64_t lookup_count = 0u;
    std::uint64_t comparison_count = 0u;
};

/// Full mapped-payload validation result and checksum.
struct mapped_sorted_dense_payload_validation {
    std::uint64_t checksum =
        bioseq::indexing::k_gpu_source_direct_checksum_seed;
    std::uint64_t covered_position_count = 0u;
    bool canonical_order_checked = false;
    bool is_valid = false;
};

/// Allocation-free authority check used on every mapped-prefix pass.  Stored
/// metadata is not sufficient: this re-reads the complete mapped control and
/// position payload so mutation between count/build passes fails closed.
[[nodiscard]] inline mapped_sorted_dense_payload_validation
validate_mapped_sorted_dense_payload(
    std::span<const bioseq::indexing::gpu_minimizer_slot> controls,
    std::span<const std::uint64_t> positions,
    std::uint64_t target_len,
    std::uint64_t expected_checksum) noexcept
{
    mapped_sorted_dense_payload_validation validation{
        .canonical_order_checked = true,
    };
    if (controls.empty() || positions.empty() || target_len == 0u
        || expected_checksum == 0u) {
        return validation;
    }
    std::uint64_t previous_hash = 0u;
    bool have_previous_hash = false;
    for (const auto& control : controls) {
        const std::uint64_t offset = control.pos_offset;
        const std::uint64_t count = control.pos_count;
        if (count == 0u || offset != validation.covered_position_count
            || offset > positions.size()
            || count > positions.size() - offset
            || (have_previous_hash && control.hash <= previous_hash)) {
            return validation;
        }
        validation.checksum = bioseq::indexing::gpu_source_direct_checksum_word(
            validation.checksum, control.hash);
        validation.checksum = bioseq::indexing::gpu_source_direct_checksum_word(
            validation.checksum, offset);
        for (std::uint64_t i = offset; i < offset + count; ++i) {
            const std::uint64_t encoded =
                positions[static_cast<std::size_t>(i)];
            if ((encoded >> 1u) >= target_len) return validation;
            validation.checksum =
                bioseq::indexing::gpu_source_direct_checksum_word(
                    validation.checksum, encoded);
        }
        validation.checksum = bioseq::indexing::gpu_source_direct_checksum_word(
            validation.checksum, count);
        validation.covered_position_count += count;
        previous_hash = control.hash;
        have_previous_hash = true;
    }
    validation.is_valid =
        validation.covered_position_count == positions.size()
        && validation.checksum == expected_checksum;
    return validation;
}

/// Host-only validation seam shared by the runtime-resident path and bounded
/// prefix tests.  It owns no memory: callers must keep both spans alive through
/// the complete count/build pass.
[[nodiscard]] inline mapped_sorted_dense_prefix_view
validated_mapped_sorted_dense_prefix_view(
    std::span<const bioseq::indexing::gpu_minimizer_slot> controls,
    std::span<const std::uint64_t> positions,
    std::uint64_t target_len,
    std::uint64_t expected_checksum)
{
    if (controls.size()
            > (std::numeric_limits<std::uint32_t>::max)()
        || positions.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
        throw mapped_prefix_invalid_error{};
    }
    const auto payload_validation = validate_mapped_sorted_dense_payload(
        controls, positions, target_len, expected_checksum);
    if (!payload_validation.canonical_order_checked
        || !payload_validation.is_valid) {
        throw mapped_prefix_invalid_error{};
    }
    return mapped_sorted_dense_prefix_view{
        .controls = controls.data(),
        .bucket_count = static_cast<std::uint32_t>(controls.size()),
        .position_count = static_cast<std::uint32_t>(positions.size()),
    };
}

[[nodiscard]] inline mapped_sorted_dense_prefix_view
validated_mapped_sorted_dense_prefix_view(
    const bioseq::gpu::vulkan::runtime& vulkan_runtime,
    const std::shared_ptr<const bioseq::indexing::
        gpu_resident_minimizer_index>& resident)
{
    using bioseq::indexing::gpu_index_representation;
    using bioseq::indexing::gpu_resident_accounting_authority;
    using bioseq::common::saturating_mul;
    if (!resident || !resident->slots.is_valid()
        || !resident->positions.is_valid()
        || resident->slots.mapped == nullptr
        || resident->positions.mapped == nullptr
        || resident->slots.owner != &vulkan_runtime
        || resident->positions.owner != &vulkan_runtime
        || !resident->slots.is_host_visible_coherent()
        || !resident->positions.is_host_visible_coherent()
        || resident->representation
            != gpu_index_representation::source_direct_sorted_dense
        || !resident->key.source_direct_sorted_dense
        || resident->key.representation
            != gpu_index_representation::source_direct_sorted_dense
        || resident->key.sorted_dense_abi_revision
            != bioseq::indexing::k_minimizer_sorted_dense_gpu_abi_revision
        || resident->bucket_count == 0u
        || resident->bucket_count != resident->unique_key_count
        || resident->bucket_count != resident->slot_capacity
        || resident->bucket_count != resident->key.table_size
        || resident->position_count == 0u
        || resident->position_count != resident->key.position_count
        || resident->slot_mask != 0u
        || resident->target_position_offset != 0u
        || resident->gate_identity.enabled
        || resident->key.gate_enabled
        || resident->key.gate_word_count != 0u
        || resident->key.gate_mask != 0u
        || resident->key.gate_digest_lo != 0u
        || resident->key.gate_digest_hi != 0u
        || resident->key.gate_canonical_words
        || resident->key.device_uuid != vulkan_runtime.features().device_uuid
        || resident->source_stream_checksum == 0u
        || resident->source_stream_checksum
            != resident->mapped_stream_checksum
        || resident->complete_source_traversals != 1u
        || resident->complete_mapped_traversals != 1u) {
        throw mapped_prefix_invalid_error{};
    }
    const std::uint64_t control_bytes = saturating_mul(
        resident->bucket_count,
        sizeof(bioseq::indexing::gpu_minimizer_slot));
    const std::uint64_t position_bytes = saturating_mul(
        resident->position_count, sizeof(std::uint64_t));
    const std::uint64_t actual_allocation_sum =
        bioseq::common::saturating_add(
            resident->slots.allocation_bytes,
            resident->positions.allocation_bytes);
    if (control_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || position_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || control_bytes > resident->slots.bytes
        || position_bytes > resident->positions.bytes
        || control_bytes > resident->slots.allocation_bytes
        || position_bytes > resident->positions.allocation_bytes
        || !bioseq::indexing::gpu_exact_resident_authority_valid(
            resident->accounting_authority,
            resident->materialization,
            static_cast<bool>(resident->permit),
            resident->permit.bytes(),
            actual_allocation_sum)) {
        throw mapped_prefix_invalid_error{};
    }
    const auto controls = std::span<const bioseq::indexing::gpu_minimizer_slot>{
        static_cast<const bioseq::indexing::gpu_minimizer_slot*>(
            resident->slots.mapped),
        resident->bucket_count};
    const auto positions = std::span<const std::uint64_t>{
        static_cast<const std::uint64_t*>(resident->positions.mapped),
        resident->position_count};
    auto view = validated_mapped_sorted_dense_prefix_view(
        controls,
        positions,
        static_cast<std::uint64_t>(resident->target_len),
        resident->source_stream_checksum);
    view.resident = resident;
    return view;
}

[[nodiscard]] inline std::uint32_t mapped_sorted_dense_position_count(
    mapped_sorted_dense_prefix_view& view,
    std::uint64_t hash)
{
    ++view.lookup_count;
    std::uint32_t lo = 0u;
    std::uint32_t hi = view.bucket_count;
    while (lo < hi) {
        ++view.comparison_count;
        const std::uint32_t mid = lo + ((hi - lo) >> 1u);
        if (view.controls[mid].hash < hash) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == view.bucket_count || view.controls[lo].hash != hash) {
        return 0u;
    }
    const std::uint64_t offset = view.controls[lo].pos_offset;
    const std::uint64_t count = view.controls[lo].pos_count;
    if (count == 0u || offset > view.position_count
        || count > view.position_count - offset) {
        throw mapped_prefix_invalid_error{};
    }
    return view.controls[lo].pos_count;
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_plan count_seed_prefix_plan(
    std::span<const p_minimizer_type> query_minimizers,
    mapped_sorted_dense_prefix_view view,
    std::int32_t max_occ)
{
    const std::uint64_t prefix_len64 = checked_seed_prefix_length(
        static_cast<std::uint64_t>(query_minimizers.size()));
    if (max_occ < 0) {
        return seed_prefix_plan{
            .prefix_len = prefix_len64,
            .exact_anchor_count = 0u};
    }
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0u;
    for (const p_minimizer_type& minimizer : query_minimizers) {
        const std::uint32_t count = mapped_sorted_dense_position_count(
            view, minimizer.kmer());
        const std::uint32_t add = count <= max_occ_u ? count : 0u;
        total = checked_seed_anchor_total(total, add);
    }
    return seed_prefix_plan{
        .prefix_len = prefix_len64,
        .exact_anchor_count = total,
        .mapped_lookup_count = view.lookup_count,
        .mapped_comparison_count = view.comparison_count,
    };
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_plan count_seed_prefix_plan(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::gpu::vulkan::runtime& vulkan_runtime,
    const std::shared_ptr<const bioseq::indexing::
        gpu_resident_minimizer_index>& resident,
    std::int32_t max_occ)
{
    auto view = validated_mapped_sorted_dense_prefix_view(
        vulkan_runtime, resident);
    return count_seed_prefix_plan(
        query_minimizers, std::move(view), max_occ);
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_build_result
build_seed_prefix_counts_after_admission(
    std::span<const p_minimizer_type> query_minimizers,
    mapped_sorted_dense_prefix_view view,
    std::int32_t max_occ,
    seed_prefix_plan plan)
{
    seed_prefix_build_result result;
    result.prefix.resize(
        static_cast<std::size_t>(plan.prefix_len), 0u);
    if (max_occ < 0) return result;
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0u;
    for (std::size_t i = 0u; i < query_minimizers.size(); ++i) {
        const std::uint32_t count = mapped_sorted_dense_position_count(
            view, query_minimizers[i].kmer());
        const std::uint32_t add = count <= max_occ_u ? count : 0u;
        total = checked_seed_anchor_total(total, add);
        result.prefix[i + 1u] = static_cast<std::uint32_t>(total);
    }
    if (total != plan.exact_anchor_count) {
        throw mapped_prefix_invalid_error{};
    }
    result.mapped_lookup_count = view.lookup_count;
    result.mapped_comparison_count = view.comparison_count;
    return result;
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_build_result
build_seed_prefix_counts_after_admission(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::gpu::vulkan::runtime& vulkan_runtime,
    const std::shared_ptr<const bioseq::indexing::
        gpu_resident_minimizer_index>& resident,
    std::int32_t max_occ,
    seed_prefix_plan plan)
{
    auto view = validated_mapped_sorted_dense_prefix_view(
        vulkan_runtime, resident);
    return build_seed_prefix_counts_after_admission(
        query_minimizers, std::move(view), max_occ, plan);
}

template <class p_minimizer_type>
[[nodiscard]] seed_prefix_plan count_seed_prefix_plan(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::indexing::minimizer_hashmap_fragment_index& target_index,
    std::int32_t max_occ)
{
    const std::uint64_t prefix_len64 = checked_seed_prefix_length(
        static_cast<std::uint64_t>(query_minimizers.size()));
    if (max_occ < 0) {
        return seed_prefix_plan{.prefix_len = prefix_len64, .exact_anchor_count = 0u};
    }
    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0;
    for (const p_minimizer_type& minimizer : query_minimizers) {
        std::uint32_t add = 0;
        const auto hits = target_index.lookup(minimizer.kmer());
        if (!hits.empty()) {
            const auto count = static_cast<std::uint32_t>(hits.size());
            if (count <= max_occ_u) add = count;
        }
        total = checked_seed_anchor_total(total, add);
    }
    return seed_prefix_plan{.prefix_len = prefix_len64, .exact_anchor_count = total};
}

template <class p_minimizer_type>
[[nodiscard]] std::vector<std::uint32_t> build_seed_prefix_counts_after_admission(
    std::span<const p_minimizer_type> query_minimizers,
    const bioseq::indexing::minimizer_hashmap_fragment_index& target_index,
    std::int32_t max_occ,
    seed_prefix_plan plan)
{
    std::vector<std::uint32_t> prefix(static_cast<std::size_t>(plan.prefix_len), 0u);
    if (max_occ < 0) return prefix;

    const auto max_occ_u = static_cast<std::uint32_t>(max_occ);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < query_minimizers.size(); ++i) {
        std::uint32_t add = 0;
        const auto hits = target_index.lookup(query_minimizers[i].kmer());
        if (!hits.empty()) {
            const auto count = static_cast<std::uint32_t>(hits.size());
            if (count <= max_occ_u) add = count;
        }
        total = checked_seed_anchor_total(total, add);
        prefix[i + 1u] = static_cast<std::uint32_t>(total);
    }
    DEBUG_ASSERT(total == plan.exact_anchor_count,
        "seed prefix prepass and build pass diverged");
    return prefix;
}


}  // namespace bioseq::seeding
