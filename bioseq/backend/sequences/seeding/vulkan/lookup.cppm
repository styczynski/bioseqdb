/**
 * @file lookup.cppm
 * @brief Selected-route Vulkan seed lookup and completion ownership.
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

export module bioseq.seeding.vulkan:lookup;

export import :diagnostics;
export import :prefix_authority;
export import :reservation;
import bioseq.common.checked_arithmetic;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.chaining.core;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

export namespace bioseq::seeding {

// ===========================================================================
// Seed-hit lookup entry point
// ===========================================================================

/// Move-only completion state and retained output-memory reservation.
struct vulkan_seed_lookup_completion {
    /** Whether lookup finished without a recorded decline. */
    bool is_successful = false;
    /** Transferred permit covering retained anchor slab growth. */
    bioseq::parallelism::memory_permit retained_anchor_permit{};
    /** Bytes covered by @ref retained_anchor_permit after shrink reconciliation. */
    std::uint64_t retained_anchor_bytes = 0u;

    vulkan_seed_lookup_completion() = default;
    /** @param completed Initial success flag for a mapped-prefix completion handoff. */
    vulkan_seed_lookup_completion(bool completed) noexcept
        : is_successful(completed) {}
    vulkan_seed_lookup_completion(vulkan_seed_lookup_completion&&) noexcept = default;
    vulkan_seed_lookup_completion& operator=(
        vulkan_seed_lookup_completion&&) noexcept = default;
    vulkan_seed_lookup_completion(const vulkan_seed_lookup_completion&) = delete;
    vulkan_seed_lookup_completion& operator=(
        const vulkan_seed_lookup_completion&) = delete;

    /// Return whether lookup completed without a recorded decline.
    [[nodiscard]] operator bool() const noexcept { return is_successful; }
};

/**
 * @brief Collect canonical anchors through the selected Vulkan seed route.
 *
 * The function never silently selects a CPU lookup. It returns a false
 * completion only for a structured, caller-visible decline and propagates
 * runtime failures once GPU execution begins.
 *
 * @tparam p_minimizer_type Query minimizer record and device wire representation.
 * @param[in] query_minimizers Host minimizers when no device span is supplied.
 * @param[in] device_minimizers Optional device-resident minimizer words.
 * @param[in] target_index Host hashmap used for non-mapped prefix counting.
 * @param[in] resident_index GPU-resident target index for lookup dispatch.
 * @param[in] vulkan_enabled Whether the selected execution policy permits Vulkan.
 * @param[in] query_len Query length in logical symbols.
 * @param[in] max_occ Per-minimizer occurrence cap, or negative for count-only mode.
 * @param[in] kmer_size Selected k-mer size for lookup validation.
 * @param[out] anchors_out Chain arena receiving emitted anchors.
 * @param[in] fused_frontend Whether the fused count-and-sort frontend is selected.
 * @param[out] decline_out Optional structured decline and telemetry sink.
 * @return Success completion, or a declined completion when @p decline_out is set.
 */
template <class p_minimizer_type>
[[nodiscard]] vulkan_seed_lookup_completion collect_seed_hits_vulkan(
    std::span<const p_minimizer_type> query_minimizers,
    std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer_type>>
        device_minimizers,
    const bioseq::indexing::minimizer_hashmap_fragment_index& target_index,
    std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index> resident_index,
    bool vulkan_enabled,
    bioseq::storage::sequence_size_t query_len,
    std::int32_t max_occ,
    std::int32_t kmer_size,
    bioseq::chaining::arena_slab<bioseq::chaining::anchor>& anchors_out,
    bool fused_frontend = false,
    vulkan_seed_lookup_decline* decline_out = nullptr)
{
    if (decline_out != nullptr) {
        *decline_out = vulkan_seed_lookup_decline{
            .query_length = static_cast<std::uint64_t>(query_len),
            .query_minimizer_count =
                static_cast<std::uint64_t>(query_minimizers.size()),
            .kmer_size = kmer_size,
            .max_occ = max_occ,
            .fused_frontend = fused_frontend,
        };
    }
    // Record a terminal decline reason when the caller requested diagnostics.
    const auto decline = [decline_out](
            vulkan_seed_lookup_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    // Source-direct indices may run with zero query minimizers.
    const bool source_direct_empty_minimizer_query =
        query_minimizers.empty()
        && resident_index != nullptr
        && resident_index->key.source_direct_sorted_dense;
    if (query_minimizers.empty()
        && !source_direct_empty_minimizer_query) {
        record_vulkan_seed_anchor_audit(
            decline_out, bioseq::gpu::vulkan::audit_seed_anchor_canonical_order({}, 0u));
        return true;
    }
    if (!vulkan_enabled) {
        decline(vulkan_seed_lookup_decline_reason::vulkan_disabled);
        return false;
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_seed_lookup_decline_reason::query_length_exceeds_u32);
        return false;
    }
    if (kmer_size <= 0) {
        decline(vulkan_seed_lookup_decline_reason::invalid_kmer_size);
        return false;
    }
    if (!resident_index) {
        decline(vulkan_seed_lookup_decline_reason::resident_index_unavailable);
        return false;
    }
    if (query_minimizers.size() > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_seed_lookup_decline_reason::
            query_minimizer_count_exceeds_u32);
        return false;
    }
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime)) {
        decline(vulkan_seed_lookup_decline_reason::hardware_device_unavailable);
        return false;
    }
    if (decline_out != nullptr) {
        decline_out->max_workgroup_count_x =
            vulkan_runtime.features().max_workgroup_count_x;
    }

    const bool mapped_prefix_authority =
        resident_index->key.source_direct_sorted_dense;
    if (mapped_prefix_authority && fused_frontend) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    }

    const bioseq::gpu::vulkan::buffer* fused_device_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;

    // The fused frontend counts, emits, and sorts anchors on the GPU without a
    // CPU prefix prepass or CPU anchor sort. Once selected, any failure is a
    // hard error; the prefix-driven path is used only when it is not selected.
    if (fused_frontend && max_occ >= 0) {
        const std::uint64_t reserve =
            vulkan_fused_seed_lookup_reservation_bytes<p_minimizer_type>(
                query_minimizers.size(), fused_device_words != nullptr);
        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        bioseq::parallelism::memory_admission_failure admission_failure{};
        auto permit = admission.try_reserve(reserve, &admission_failure);
        if (!permit) {
            if (decline_out != nullptr) {
                decline_out->reason = vulkan_seed_lookup_decline_reason::
                    seed_transient_memory_admission_denied;
                decline_out->admission = admission_failure;
                return false;
            }
            throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                "Vulkan fused seed lookup count memory admission failed ({})",
                format_vulkan_seed_lookup_decline(vulkan_seed_lookup_decline{
                    .reason = vulkan_seed_lookup_decline_reason::
                        seed_transient_memory_admission_denied,
                    .query_length = static_cast<std::uint64_t>(query_len),
                    .query_minimizer_count = static_cast<std::uint64_t>(
                        query_minimizers.size()),
                    .max_workgroup_count_x =
                        vulkan_runtime.features().max_workgroup_count_x,
                    .kmer_size = kmer_size,
                    .max_occ = max_occ,
                    .fused_frontend = true,
                    .admission = admission_failure,
                })));
        }
        bioseq::gpu::vulkan::gpu_seed_inputs inputs{
            .host_minimizer_bytes = fused_device_words != nullptr
                ? std::span<const std::byte>{}
                : std::as_bytes(query_minimizers),
            .device_minimizer_words = fused_device_words,
            .slots = &resident_index->slots,
            .positions = &resident_index->positions,
            .target_len = resident_index->target_len,
            .slot_mask = resident_index->slot_mask,
            .position_count = resident_index->position_count,
            .minimizer_count = static_cast<std::uint32_t>(query_minimizers.size()),
            .minimizer_word_kind =
                static_cast<std::uint32_t>(bioseq::gpu::vulkan::
                    minimizer_wire_traits<p_minimizer_type>::k_word_kind),
            .minimizer_words32_per_record =
                bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::
                    k_words32_per_minimizer,
            .hash_shift = bioseq::gpu::vulkan::
                minimizer_wire_traits<p_minimizer_type>::k_hash_shift,
            .pos_bits = bioseq::gpu::vulkan::
                minimizer_wire_traits<p_minimizer_type>::k_position_bits,
            .bucket_count = resident_index->bucket_count,
            .target_position_offset = resident_index->target_position_offset,
        };
        auto anchors = vulkan_runtime.run_seed_anchor_roundtrip(
            inputs, query_len, static_cast<std::uint32_t>(kmer_size), max_occ);
        if (!anchors.anchor_order_audit.canonical_order_checked
            || !anchors.anchor_order_audit.canonical_order_accepted
            || anchors.anchor_order_audit.anchor_count != anchors.count) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan fused seed anchor validation is invalid");
        }
        anchors_out.reserve(anchors.count);
        anchors_out.resize(anchors.count);
        if (anchors.count > 0) {
            std::memcpy(anchors_out.data(), anchors.host_bytes.data(),
                        anchors.count * sizeof(bioseq::chaining::anchor));
        }
        if (decline_out != nullptr) {
            decline_out->seed_lookup_dispatches =
                anchors.seed_lookup_dispatches;
            decline_out->open_addressed_lookup_dispatches =
                anchors.open_addressed_lookup_dispatches;
            decline_out->sorted_dense_lookup_dispatches =
                anchors.sorted_dense_lookup_dispatches;
            decline_out->sort_stats = anchors.sort_stats;
            decline_out->exact_anchor_count = anchors.count;
            record_vulkan_seed_anchor_audit(decline_out, anchors);
        }
        return true;
    }

    if (fused_frontend && max_occ < 0) {
        decline(vulkan_seed_lookup_decline_reason::negative_max_occ_unsupported);
        if (decline_out != nullptr) {
            return false;
        }
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "Vulkan fused seed lookup selected but negative max_occ is unsupported");
    }

    if (mapped_prefix_authority && anchors_out.capacity() != 0u) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    }
    seed_prefix_plan prefix_plan;
    try {
        prefix_plan = mapped_prefix_authority
            ? count_seed_prefix_plan(
                query_minimizers, vulkan_runtime, resident_index, max_occ)
            : count_seed_prefix_plan(
                query_minimizers, target_index, max_occ);
    } catch (const seed_lookup_too_large& overflow) {
        if (decline_out != nullptr) {
            if (overflow.kind == seed_lookup_overflow_kind::query_prefix_length) {
                decline_out->reason = vulkan_seed_lookup_decline_reason::
                    query_prefix_length_exceeds_u32;
                decline_out->query_prefix_length = overflow.observed;
            } else {
                decline_out->reason = vulkan_seed_lookup_decline_reason::
                    exact_anchor_count_exceeds_u32;
                decline_out->exact_anchor_count = overflow.observed;
            }
        }
        return false;
    } catch (const mapped_prefix_invalid_error&) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    }
    const std::uint64_t exact_anchors = prefix_plan.exact_anchor_count;
    if (decline_out != nullptr) {
        decline_out->query_prefix_length = prefix_plan.prefix_len;
        decline_out->exact_anchor_count = exact_anchors;
        decline_out->mapped_prefix_authority_passes =
            mapped_prefix_authority ? 1u : 0u;
        decline_out->mapped_prefix_lookup_count =
            prefix_plan.mapped_lookup_count;
        decline_out->mapped_prefix_comparison_count =
            prefix_plan.mapped_comparison_count;
    }
    if (exact_anchors == 0u) {
        if (mapped_prefix_authority) {
            try {
                const auto second_zero = count_seed_prefix_plan(
                    query_minimizers, vulkan_runtime, resident_index, max_occ);
                if (second_zero.exact_anchor_count != 0u
                    || second_zero.prefix_len != prefix_plan.prefix_len) {
                    throw mapped_prefix_invalid_error{};
                }
                if (decline_out != nullptr) {
                    decline_out->mapped_prefix_authority_passes = 2u;
                    decline_out->mapped_prefix_lookup_count +=
                        second_zero.mapped_lookup_count;
                    decline_out->mapped_prefix_comparison_count +=
                        second_zero.mapped_comparison_count;
                }
            } catch (const mapped_prefix_invalid_error&) {
                decline(vulkan_seed_lookup_decline_reason::
                    mapped_prefix_authority_invalid);
                return false;
            }
        }
        anchors_out.resize(0);
        record_vulkan_seed_anchor_audit(
            decline_out, bioseq::gpu::vulkan::audit_seed_anchor_canonical_order({}, 0u));
        return true;
    }
    const bioseq::gpu::vulkan::seed_anchor_sort_plan sort_plan = bioseq::gpu::vulkan::make_seed_anchor_sort_plan(
        exact_anchors,
        resident_index->target_len,
        resident_index->target_position_offset,
        vulkan_runtime.gpu_anchor_sort_capacity(),
        vulkan_runtime.features().max_workgroup_count_x,
        vulkan_runtime.features().max_workgroup_invocations,
        vulkan_runtime.features().max_workgroup_size_x,
        vulkan_runtime.features().max_storage_buffer_range);
    if (decline_out != nullptr) {
        decline_out->sort_plan = sort_plan;
    }
    if (!sort_plan.is_valid()) {
        switch (sort_plan.failure) {
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::anchor_count_exceeds_u32:
                decline(vulkan_seed_lookup_decline_reason::
                    exact_anchor_count_exceeds_u32);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::target_coordinate_invalid:
                decline(vulkan_seed_lookup_decline_reason::
                    target_coordinate_invalid);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::dispatch_limit_unavailable:
                decline(vulkan_seed_lookup_decline_reason::
                    dispatch_workgroup_limit_exceeded);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::radix_workgroup_geometry_unsupported:
                decline(vulkan_seed_lookup_decline_reason::
                    radix_workgroup_geometry_unsupported);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::storage_buffer_range_exceeded:
                decline(vulkan_seed_lookup_decline_reason::
                    sort_storage_buffer_range_exceeded);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::arithmetic_overflow:
                decline(vulkan_seed_lookup_decline_reason::
                    sort_plan_arithmetic_overflow);
                break;
            case bioseq::gpu::vulkan::seed_anchor_sort_plan_failure::none:
                break;
        }
        return false;
    }
    constexpr std::uint32_t k_seed_lookup_local_size = 128u;
    const std::uint64_t required_workgroups =
        bioseq::common::ceil_div(
            query_minimizers.size(), k_seed_lookup_local_size);
    if (decline_out != nullptr) {
        decline_out->required_workgroups = required_workgroups;
    }
    if (required_workgroups > vulkan_runtime.features().max_workgroup_count_x) {
        decline(vulkan_seed_lookup_decline_reason::
            dispatch_workgroup_limit_exceeded);
        return false;
    }

    const bioseq::gpu::vulkan::buffer* device_minimizer_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;
    const bool has_device_minimizers = device_minimizer_words != nullptr;

    const auto reservation = plan_vulkan_seed_lookup_reservation<p_minimizer_type>(
        vulkan_runtime,
        query_minimizers.size(), sort_plan, has_device_minimizers,
        anchors_out.capacity());
    if (!reservation.is_arithmetic_valid) {
        decline(vulkan_seed_lookup_decline_reason::sort_plan_arithmetic_overflow);
        return false;
    }
    const std::uint64_t reserve = reservation.total_bytes;
    const std::uint64_t anchors_out_growth_bytes =
        reservation.anchors_out_growth_bytes;
    if (decline_out != nullptr) {
        decline_out->reserved_bytes = reserve;
        decline_out->planned_vulkan_allocation_bytes =
            reservation.vulkan_allocation_requirement_bytes;
        decline_out->host_prefix_bytes = reservation.host_prefix_bytes;
        decline_out->host_result_bytes = reservation.host_result_bytes;
        decline_out->anchors_out_growth_bytes = anchors_out_growth_bytes;
    }

    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = bioseq::parallelism::memory_admission_controller::instance()
        .try_reserve(reserve, &admission_failure);
    if (!permit) {
        if (decline_out != nullptr) {
            decline_out->reason = vulkan_seed_lookup_decline_reason::
                seed_transient_memory_admission_denied;
            decline_out->admission = admission_failure;
        }
        return false;
    }

    std::uint64_t actual_slab_growth_bytes = 0u;
    bool post_allocation_invariant_failed = false;
    try {
        {
            std::vector<std::uint32_t> prefix;
            if (mapped_prefix_authority) {
                auto built_prefix =
                    build_seed_prefix_counts_after_admission(
                        query_minimizers,
                        vulkan_runtime,
                        resident_index,
                        max_occ,
                        prefix_plan);
                if (decline_out != nullptr) {
                    decline_out->mapped_prefix_authority_passes = 2u;
                    decline_out->mapped_prefix_lookup_count +=
                        built_prefix.mapped_lookup_count;
                    decline_out->mapped_prefix_comparison_count +=
                        built_prefix.mapped_comparison_count;
                }
                prefix = std::move(built_prefix.prefix);
            } else {
                prefix = build_seed_prefix_counts_after_admission(
                    query_minimizers, target_index, max_occ, prefix_plan);
            }
            bioseq::gpu::vulkan::runtime_seed_lookup_inputs inputs{
                .host_minimizer_bytes = has_device_minimizers
                    ? std::span<const std::byte>{}
                    : std::as_bytes(query_minimizers),
                .device_minimizer_words = device_minimizer_words,
                .slots = &resident_index->slots,
                .positions = &resident_index->positions,
                .slot_mask = resident_index->slot_mask,
                .bucket_count = resident_index->bucket_count,
                .position_count = resident_index->position_count,
                .minimizer_word_kind = static_cast<std::uint32_t>(
                    bioseq::gpu::vulkan::minimizer_wire_traits<
                        p_minimizer_type>::k_word_kind),
                .minimizer_words32_per_record =
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer_type>::
                        k_words32_per_minimizer,
                .hash_shift =
                    bioseq::gpu::vulkan::minimizer_wire_traits<
                        p_minimizer_type>::k_hash_shift,
                .pos_bits = bioseq::gpu::vulkan::minimizer_wire_traits<
                    p_minimizer_type>::k_position_bits,
                .target_len = resident_index->target_len,
                .target_position_offset =
                    resident_index->target_position_offset,
            };
            auto anchors = vulkan_runtime.run_seed_lookup_roundtrip(
                inputs,
                std::span<const std::uint32_t>{
                    prefix.data(), prefix.size()},
                query_len,
                static_cast<std::uint32_t>(kmer_size),
                exact_anchors,
                sort_plan,
                reservation.vulkan_allocation_requirement_bytes);

            if (anchors.count != exact_anchors
                || anchors.actual_vulkan_allocation_bytes
                    != reservation.vulkan_allocation_requirement_bytes
                || !anchors.anchor_order_audit.canonical_order_checked
                || !anchors.anchor_order_audit.canonical_order_accepted
                || anchors.anchor_order_audit.anchor_count != anchors.count) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan seed lookup result validation disagrees with admission");
            }

            const std::uint64_t slab_capacity_before =
                anchors_out.capacity();
            anchors_out.reserve(anchors.count);
            anchors_out.resize(anchors.count);
            std::memcpy(
                anchors_out.data(),
                anchors.host_bytes.data(),
                anchors.count * sizeof(bioseq::chaining::anchor));
            actual_slab_growth_bytes =
                bioseq::common::saturating_mul(
                    anchors_out.capacity() - slab_capacity_before,
                    sizeof(bioseq::chaining::anchor));
            if (mapped_prefix_authority
                && actual_slab_growth_bytes
                    != anchors_out_growth_bytes) {
                post_allocation_invariant_failed = true;
            }
            if (decline_out != nullptr) {
                decline_out->seed_lookup_dispatches =
                    anchors.seed_lookup_dispatches;
                decline_out->open_addressed_lookup_dispatches =
                    anchors.open_addressed_lookup_dispatches;
                decline_out->sorted_dense_lookup_dispatches =
                    anchors.sorted_dense_lookup_dispatches;
                decline_out->sort_stats = anchors.sort_stats;
                decline_out->actual_vulkan_allocation_bytes =
                    anchors.actual_vulkan_allocation_bytes;
                record_vulkan_seed_anchor_audit(decline_out, anchors);
            }
        }

        // Prefix/result host vectors and runtime-owned seed temporaries have
        // left scope. Reconcile the aggregate permit to the exact capacity
        // retained by the per-query chain arena before transferring ownership.
        if (mapped_prefix_authority) {
            permit->shrink_to(actual_slab_growth_bytes);
            if (permit->bytes() != actual_slab_growth_bytes) {
                post_allocation_invariant_failed = true;
            }
            if (decline_out != nullptr) {
                decline_out->anchors_out_actual_growth_bytes =
                    actual_slab_growth_bytes;
                decline_out->anchors_out_retained_permit_bytes =
                    permit->bytes();
            }
            vulkan_seed_lookup_completion completion{
                !post_allocation_invariant_failed};
            completion.retained_anchor_bytes = permit->bytes();
            completion.retained_anchor_permit = std::move(*permit);
            if (post_allocation_invariant_failed) {
                decline(vulkan_seed_lookup_decline_reason::
                    mapped_prefix_authority_invalid);
            }
            return completion;
        }
        return true;
    } catch (const mapped_prefix_invalid_error&) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    } catch (const bioseq::gpu::vulkan::vulkan_runtime_error&) {
        throw;
    }
}

}  // namespace bioseq::seeding
