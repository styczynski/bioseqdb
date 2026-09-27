/**
 * @file lookup.cppm
 * @brief Vulkan seed lookup and conversion of GPU results into chaining anchors.
 */
module;

#include <fmt/format.h>
#include <libassert/assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
import bioseq.pipeline.minimap_port;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

export namespace bioseq::seeding {

// Seed lookup and result ownership.

/// Lookup completion with any memory reservation kept for output anchors.
struct vulkan_seed_lookup_completion {
    /** Whether lookup and result checks succeeded. */
    bool is_successful = false;
    /** Output-buffer reservation for callers without context allocation hooks. */
    bioseq::parallelism::memory_permit retained_anchor_permit{};
    /** Bytes held by the output-buffer reservation. */
    std::uint64_t retained_anchor_bytes = 0u;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Keep the runtime operation records for benchmark checks in later host
    // stages. Move the original records with the completed result.
    bioseq::gpu::vulkan::runtime_pipeline_operation_token query_operation_receipt{};
    bioseq::gpu::vulkan::runtime_pipeline_operation_token seed_operation_receipt{};
#endif

    vulkan_seed_lookup_completion() = default;
    /** @param completed Initial success flag. */
    vulkan_seed_lookup_completion(bool completed) noexcept
        : is_successful(completed) {}
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    vulkan_seed_lookup_completion(vulkan_seed_lookup_completion&& other) noexcept
        : is_successful(std::exchange(other.is_successful, false)),
          retained_anchor_permit(std::move(other.retained_anchor_permit)),
          retained_anchor_bytes(std::exchange(other.retained_anchor_bytes, 0u)),
          query_operation_receipt(std::move(other.query_operation_receipt)),
          seed_operation_receipt(std::move(other.seed_operation_receipt)) {}
    vulkan_seed_lookup_completion& operator=(vulkan_seed_lookup_completion&& other) noexcept {
        if (this != &other) {
            is_successful = std::exchange(other.is_successful, false);
            retained_anchor_permit = std::move(other.retained_anchor_permit);
            retained_anchor_bytes = std::exchange(other.retained_anchor_bytes, 0u);
            query_operation_receipt = std::move(other.query_operation_receipt);
            seed_operation_receipt = std::move(other.seed_operation_receipt);
        }
        return *this;
    }
#else
    vulkan_seed_lookup_completion(vulkan_seed_lookup_completion&&) noexcept = default;
    vulkan_seed_lookup_completion& operator=(
        vulkan_seed_lookup_completion&&) noexcept = default;
#endif
    vulkan_seed_lookup_completion(const vulkan_seed_lookup_completion&) = delete;
    vulkan_seed_lookup_completion& operator=(
        const vulkan_seed_lookup_completion&) = delete;

    /// Reports whether lookup and result checks succeeded.
    [[nodiscard]] operator bool() const noexcept { return is_successful; }
};

/**
 * @brief Returns one empty query job without allocating an anchor buffer.
 *
 * job_storage must be empty. Existing capacity can be reused.
 * @return True when the empty job matches submitted_ordinal.
 */
[[nodiscard]] inline bool publish_empty_canonical_seed_batch(
    std::uint32_t submitted_ordinal,
    bioseq::simd::canonical_seed_batch& batch_out,
    std::vector<bioseq::simd::canonical_seed_job_span> job_storage = {}) {
    batch_out = bioseq::simd::canonical_seed_batch{};
    bioseq::simd::canonical_seed_batch pending{};
    pending.jobs = std::move(job_storage);
    pending.jobs.push_back(bioseq::simd::canonical_seed_job_span{
        .ordinal = submitted_ordinal,
        .offset = 0u,
        .count = 0u,
    });
    const std::array<std::uint32_t, 1u> submitted_ordinals{submitted_ordinal};
    if (!bioseq::simd::canonical_seed_jobs_match_submission(
            pending, submitted_ordinals)) {
        return false;
    }
    batch_out = std::move(pending);
    return true;
}

/**
 * @brief Checks one MPF seed download and converts it to chaining anchors.
 *
 * MPF uses seed_anchor_wire records. Rechecks byte size, anchor order, and the
 * producer digest before validating each coordinate and constructing anchors field
 * by field. batch_out remains empty if validation fails.
 * @param readback Downloaded records and producer checks.
 * @param target_position_offset Absolute start of the target interval.
 * @param target_length Target interval length.
 * @param query_length Query length in symbols.
 * @param decoded_query_span Expected span of each seed.
 * @param submitted_ordinal Original query ordinal.
 * @param batch_out Receives one complete query job on success.
 * @param job_storage Empty job vector whose capacity can be reused.
 * @return True when all record and job checks pass.
 */
[[nodiscard]] inline bool authenticate_mpf_seed_anchor_readback(
    const bioseq::gpu::vulkan::seed_lookup_result& readback,
    std::uint64_t target_position_offset,
    std::uint64_t target_length,
    std::uint64_t query_length,
    std::uint32_t decoded_query_span,
    std::uint32_t submitted_ordinal,
    bioseq::simd::canonical_seed_batch& batch_out,
    std::vector<bioseq::simd::canonical_seed_job_span> job_storage = {}) {
    using bioseq::gpu::vulkan::seed_anchor_wire;
    batch_out = bioseq::simd::canonical_seed_batch{};

    constexpr std::uint64_t maximum_u64 =
        (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::uint64_t first_invalid_reference = std::uint64_t{1} << 63u;
    if (decoded_query_span == 0u
        || decoded_query_span > (std::numeric_limits<std::uint8_t>::max)()
        || query_length > (std::numeric_limits<std::uint32_t>::max)()
        || readback.count > (std::numeric_limits<std::uint32_t>::max)()
        || readback.count > maximum_u64 / sizeof(seed_anchor_wire)) {
        return false;
    }

    const std::uint64_t wire_bytes = readback.count * sizeof(seed_anchor_wire);
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (wire_bytes > (std::numeric_limits<std::size_t>::max)()
            || readback.count > (std::numeric_limits<std::size_t>::max)()) {
            return false;
        }
    }
    if (readback.host_bytes.size() != static_cast<std::size_t>(wire_bytes)) {
        return false;
    }

    const std::span<const std::byte> payload{
        readback.host_bytes.data(), readback.host_bytes.size()};
    const auto authenticated_audit =
        bioseq::gpu::vulkan::audit_seed_anchor_canonical_order(
            payload, readback.count);
    const auto& producer_audit = readback.anchor_order_audit;
    if (!producer_audit.canonical_order_checked
        || !producer_audit.canonical_order_accepted
        || !authenticated_audit.canonical_order_checked
        || !authenticated_audit.canonical_order_accepted
        || producer_audit.anchor_count != readback.count
        || authenticated_audit.anchor_count != readback.count
        || producer_audit.digest_lo != authenticated_audit.digest_lo
        || producer_audit.digest_hi != authenticated_audit.digest_hi) {
        return false;
    }

    if (readback.count != 0u
        && (target_length == 0u
            || target_position_offset >= first_invalid_reference
            || target_length > first_invalid_reference - target_position_offset
            || query_length < decoded_query_span)) {
        return false;
    }
    const std::uint64_t target_end = target_position_offset + target_length;
    const std::uint64_t minimum_query_end = decoded_query_span - 1u;

    bioseq::simd::canonical_seed_batch pending{};
    pending.jobs = std::move(job_storage);
    if (readback.count != 0u) {
        pending.anchors =
            bioseq::chaining::make_arena_slab<bioseq::chaining::anchor>();
        pending.anchors.reserve(static_cast<std::size_t>(readback.count));
    }
    for (std::uint64_t index = 0u; index < readback.count; ++index) {
        const seed_anchor_wire wire =
            bioseq::gpu::vulkan::load_seed_anchor_wire(payload, index);
        if (wire.ref_position < target_position_offset
            || wire.ref_position >= target_end
            || wire.query_position < minimum_query_end
            || wire.query_position >= query_length
            || wire.is_reverse > 1u
            || wire.query_span != decoded_query_span
            || wire.flags != 0u
            || wire.segment_id != 0u) {
            return false;
        }
        pending.anchors.push_back(bioseq::chaining::anchor{
            .ref_position = wire.ref_position,
            .query_position = wire.query_position,
            .is_reverse = wire.is_reverse,
            .query_span = wire.query_span,
            .flags = wire.flags,
            .segment_id = wire.segment_id,
        });
    }

    pending.jobs.push_back(bioseq::simd::canonical_seed_job_span{
        .ordinal = submitted_ordinal,
        .offset = 0u,
        .count = static_cast<std::size_t>(readback.count),
    });
    const std::array<std::uint32_t, 1u> submitted_ordinals{submitted_ordinal};
    if (!bioseq::simd::canonical_seed_jobs_match_submission(
            pending, submitted_ordinals)) {
        return false;
    }
    batch_out = std::move(pending);
    return true;
}

/**
 * @brief Checks an MPS batch download and converts its jobs to chaining anchors.
 *
 * Consumes gpu_anchor_wire records with their runtime completion record. Checks
 * query partitions, memory accounting, and each job's anchor digest and order,
 * then validates coordinates while constructing host anchors field by field.
 *
 * job_storage must be empty with enough reserved capacity for every query.
 * batch_out receives the full batch only after all checks pass and remains empty
 * on failure.
 * @return Successful completion, or false when validation fails.
 */
[[nodiscard]] inline vulkan_seed_lookup_completion authenticate_mps_seed_anchor_readback(
    bioseq::gpu::vulkan::grouped_soa_seed_anchor_result&& readback,
    std::span<const bioseq::gpu::vulkan::gpu_seed_query_record_wire>
        expected_query_records,
    std::span<const std::uint32_t> submitted_ordinals,
    std::uint64_t target_position_offset,
    std::uint64_t target_length,
    std::uint32_t decoded_query_span,
    std::uint64_t expected_query_authority_identity,
    bioseq::simd::canonical_seed_batch& batch_out,
    std::vector<bioseq::simd::canonical_seed_job_span> job_storage) {
    using bioseq::gpu::vulkan::gpu_anchor_wire;
    batch_out = bioseq::simd::canonical_seed_batch{};
    auto completed = std::move(readback);

    constexpr std::uint64_t maximum_u64 =
        (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::uint64_t first_invalid_reference = std::uint64_t{1} << 63u;
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (completed.anchor_wires.size() > maximum_u64 ||
            completed.jobs.size() > maximum_u64) {
            return false;
        }
    }
    const std::uint64_t wire_count =
        static_cast<std::uint64_t>(completed.anchor_wires.size());
    if (wire_count > maximum_u64 / sizeof(gpu_anchor_wire)) {
        return false;
    }
    const std::uint64_t wire_bytes = wire_count * sizeof(gpu_anchor_wire);
    const std::uint64_t retained_anchor_capacity =
        completed.owned_anchor_capacity_bytes();
    const std::uint64_t retained_job_capacity = completed.owned_job_capacity_bytes();
    const auto has_capacity_proof = [](
        std::uint64_t capacity_bytes,
        const bioseq::parallelism::memory_permit& permit,
        bool process_footprint_reflected) noexcept {
        if (capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        if (capacity_bytes == 0u) {
            return !permit && !process_footprint_reflected;
        }
        return process_footprint_reflected
            ? !permit
            : static_cast<bool>(permit) && permit.bytes() == capacity_bytes;
    };
    if (decoded_query_span == 0u ||
        decoded_query_span > (std::numeric_limits<std::uint8_t>::max)() ||
        expected_query_authority_identity == 0u || target_length == 0u ||
        target_position_offset >= first_invalid_reference ||
        target_length > first_invalid_reference - target_position_offset ||
        expected_query_records.size() != submitted_ordinals.size() ||
        expected_query_records.size() >
            (std::numeric_limits<std::uint32_t>::max)() ||
        !job_storage.empty() ||
        job_storage.capacity() < expected_query_records.size() ||
        completed.jobs.size() != expected_query_records.size() ||
        completed.seed_operation_receipt.audit().jobs.size() !=
            expected_query_records.size() ||
        wire_bytes > retained_anchor_capacity ||
        !has_capacity_proof(retained_anchor_capacity,
                            completed.host_anchor_permit,
                            completed.host_anchor_capacity_reflected) ||
        !has_capacity_proof(retained_job_capacity,
                            completed.host_job_permit,
                            completed.host_job_capacity_reflected) ||
        !completed.seed_operation_receipt
             .is_valid_for_grouped_soa_seed_anchor_roundtrip() ||
        !bioseq::gpu::vulkan::grouped_soa_seed_wire_jobs_cover(
            completed.jobs, completed.anchor_wires.size())) {
        return false;
    }

    std::uint64_t minimizer_prefix = 0u;
    for (std::size_t index = 0u; index < expected_query_records.size(); ++index) {
        const auto& record = expected_query_records[index];
        if (record.flags != 0u || record.query_len == 0u ||
            record.minimizer_offset != minimizer_prefix ||
            (index != 0u && submitted_ordinals[index] <= submitted_ordinals[index - 1u]) ||
            record.minimizer_count >
                (std::numeric_limits<std::uint64_t>::max)() - minimizer_prefix) {
            return false;
        }
        minimizer_prefix += record.minimizer_count;
    }

    const std::uint64_t target_end = target_position_offset + target_length;
    const std::uint64_t minimum_query_end = decoded_query_span - 1u;
    const std::span<const gpu_anchor_wire> all_job_wires{
        completed.anchor_wires};
    bioseq::simd::canonical_seed_batch pending{};
    pending.jobs = std::move(job_storage);
    if (!completed.anchor_wires.empty()) {
        pending.anchors =
            bioseq::chaining::make_arena_slab<bioseq::chaining::anchor>();
        pending.anchors.reserve(completed.anchor_wires.size());
    }
    const auto& operation_audit = completed.seed_operation_receipt.audit();
    for (std::size_t job_index = 0u; job_index < completed.jobs.size(); ++job_index) {
        const auto& wire_job = completed.jobs[job_index];
        const auto& query_record = expected_query_records[job_index];
        const auto& job_audit = operation_audit.jobs[job_index];
        const std::span<const gpu_anchor_wire> job_wires =
            all_job_wires.subspan(wire_job.offset, wire_job.count);
        const auto authenticated_order =
            bioseq::gpu::vulkan::audit_gpu_anchor_canonical_order(job_wires);
        if (wire_job.ordinal != job_index ||
            job_audit.minimizer_count != query_record.minimizer_count ||
            job_audit.target_position_offset != target_position_offset ||
            job_audit.target_len != target_length ||
            job_audit.anchor_count != wire_job.count || job_audit.range_count != 0u ||
            job_audit.query_authority_identity !=
                expected_query_authority_identity ||
            !job_audit.anchor_order_checked || !job_audit.anchor_order_accepted ||
            !authenticated_order.canonical_order_checked ||
            !authenticated_order.canonical_order_accepted ||
            authenticated_order.anchor_count != wire_job.count ||
            authenticated_order.digest_lo != job_audit.anchor_digest_lo ||
            authenticated_order.digest_hi != job_audit.anchor_digest_hi ||
            (wire_job.count != 0u && query_record.query_len < decoded_query_span)) {
            return false;
        }

        const std::size_t canonical_offset = pending.anchors.size();
        for (const gpu_anchor_wire& wire : job_wires) {
            if (wire.ref_position < target_position_offset ||
                wire.ref_position >= target_end ||
                wire.query_position < minimum_query_end ||
                wire.query_position >= query_record.query_len ||
                wire.is_reverse > 1u || wire.query_span != decoded_query_span ||
                wire.flags != 0u || wire.segment_id != 0u) {
                return false;
            }
            pending.anchors.push_back(bioseq::chaining::anchor{
                .ref_position = wire.ref_position,
                .query_position = wire.query_position,
                .is_reverse = wire.is_reverse,
                .query_span = wire.query_span,
                .flags = wire.flags,
                .segment_id = wire.segment_id,
            });
        }
        pending.jobs.push_back(bioseq::simd::canonical_seed_job_span{
            .ordinal = submitted_ordinals[job_index],
            .offset = canonical_offset,
            .count = wire_job.count,
        });
    }
    if (!bioseq::simd::canonical_seed_jobs_match_submission(
            pending, submitted_ordinals)) {
        return false;
    }
    batch_out = std::move(pending);
    vulkan_seed_lookup_completion completion{true};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    completion.query_operation_receipt = std::move(completed.query_operation_receipt);
    completion.seed_operation_receipt = std::move(completed.seed_operation_receipt);
#endif
    return completion;
}

/** Validates completed MPS GPU results and returns their chaining-anchor batch. */
[[nodiscard]] inline vulkan_seed_lookup_completion collect_seed_hits_vulkan(
    bioseq::gpu::vulkan::grouped_soa_seed_anchor_result&& readback,
    std::span<const bioseq::gpu::vulkan::gpu_seed_query_record_wire>
        expected_query_records,
    std::span<const std::uint32_t> submitted_ordinals,
    std::uint64_t target_position_offset,
    std::uint64_t target_length,
    std::uint32_t decoded_query_span,
    std::uint64_t expected_query_authority_identity,
    bioseq::simd::canonical_seed_batch& anchors_out,
    std::vector<bioseq::simd::canonical_seed_job_span> job_storage) {
    return authenticate_mps_seed_anchor_readback(
        std::move(readback),
        expected_query_records,
        submitted_ordinals,
        target_position_offset,
        target_length,
        decoded_query_span,
        expected_query_authority_identity,
        anchors_out,
        std::move(job_storage));
}

/**
 * Looks up target matches for query minimizers and returns sorted chaining anchors.
 *
 * Uses the selected GPU path. The fused path counts and sorts on the GPU, while
 * the other path builds output offsets from a mapped index or host hash map
 * before submitting seed lookup. Results are checked before anchors_out receives
 * a complete query job.
 * @param query_minimizers Host records used for counts and optional upload.
 * @param device_minimizers Optional device records with the same count.
 * @param target_index Host index used when prefix counting needs it.
 * @param resident_index Target index in device memory.
 * @param vulkan_enabled Whether Vulkan lookup is enabled.
 * @param query_len Query length in symbols.
 * @param max_occ Skip hashes with more occurrences. Negative values produce no anchors
 * on the prefix path and are rejected by the fused path.
 * @param kmer_size Seed span to validate.
 * @param submitted_ordinal Original query ordinal.
 * @param anchors_out Receives the completed query job.
 * @param context_backing_is_admitted Context allocation hooks already reserve backing memory.
 * @param fused_frontend Select GPU counting and sorting.
 * @param decline_out Optional rejection details.
 * @return Completion state, possibly holding an output-memory reservation.
 *
 * Input or planning failures can return false. Runtime and result-validation
 * failures can throw. Keep the completion reservation until the output anchors are freed.
 */
template <class p_minimizer_type>
[[nodiscard]] vulkan_seed_lookup_completion collect_seed_hits_vulkan(
    std::span<const p_minimizer_type> query_minimizers,
    std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer_type>>
        device_minimizers,
    const bioseq::indexing::minimizer_hashmap_fragment_index* target_index,
    std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index> resident_index,
    bool vulkan_enabled,
    bioseq::storage::sequence_size_t query_len,
    std::int32_t max_occ,
    std::int32_t kmer_size,
    std::uint32_t submitted_ordinal,
    bioseq::simd::canonical_seed_batch& anchors_out,
    bool context_backing_is_admitted,
    bool fused_frontend = false,
    vulkan_seed_lookup_decline* decline_out = nullptr
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    , const bioseq::gpu::vulkan::device_query_sketch_authorization* query_authorization = nullptr,
    std::uint32_t query_index = 0u
#endif
    )
{
    auto job_storage = std::move(anchors_out.jobs);
    anchors_out = bioseq::simd::canonical_seed_batch{};
    if (context_backing_is_admitted && job_storage.capacity() < 1u) {
        throw std::logic_error{"campaign MPF seed jobs require admitted physical storage"};
    }
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
    // Source-direct indexes still validate an empty query through the prefix path.
    const bool source_direct_empty_minimizer_query =
        query_minimizers.empty()
        && resident_index != nullptr
        && resident_index->key.source_direct_sorted_dense;
    if (query_minimizers.empty()
        && !source_direct_empty_minimizer_query) {
        record_vulkan_seed_anchor_audit(
            decline_out, bioseq::gpu::vulkan::audit_seed_anchor_canonical_order({}, 0u));
        return publish_empty_canonical_seed_batch(
            submitted_ordinal, anchors_out, std::move(job_storage));
    }
    if (!vulkan_enabled) {
        decline(vulkan_seed_lookup_decline_reason::vulkan_disabled);
        return false;
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_seed_lookup_decline_reason::query_length_exceeds_u32);
        return false;
    }
    if (kmer_size <= 0
        || kmer_size > (std::numeric_limits<std::uint8_t>::max)()
        || (query_len < static_cast<bioseq::storage::sequence_size_t>(kmer_size)
            && !source_direct_empty_minimizer_query)) {
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
        decline(vulkan_seed_lookup_decline_reason::mapped_prefix_authority_invalid);
        return false;
    }
    const bioseq::gpu::vulkan::buffer* fused_device_words =
        (device_minimizers && device_minimizers->words() != nullptr &&
         device_minimizers->count() == query_minimizers.size())
            ? device_minimizers->words()
            : nullptr;
    const std::uint64_t fused_device_record_offset =
        fused_device_words != nullptr ? device_minimizers->offset() : 0u;
    if (fused_device_record_offset >
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        decline(vulkan_seed_lookup_decline_reason::query_minimizer_count_exceeds_u32);
        return false;
    }

    // The fused path counts, emits, and sorts anchors on the GPU. Planning
    // failures are reported before dispatch, and runtime failures propagate.
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
            .query_minimizer_record_offset =
                static_cast<std::uint32_t>(fused_device_record_offset),
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
        auto readback = vulkan_runtime.run_seed_anchor_roundtrip(
            inputs, query_len, static_cast<std::uint32_t>(kmer_size), max_occ
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            , query_authorization, query_index
#endif
            );

        if (readback.count > (std::numeric_limits<std::uint32_t>::max)()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan fused seed anchor count exceeds the MPF domain");
        }
        const std::uint64_t canonical_anchor_bytes =
            bioseq::common::saturating_mul(
                readback.count, sizeof(bioseq::chaining::anchor));
        if (canonical_anchor_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan fused canonical anchor extent overflowed");
        }
        const std::uint64_t planned_anchor_growth =
            context_backing_is_admitted ? 0u : canonical_anchor_bytes;
        if (decline_out != nullptr) {
            decline_out->reserved_bytes = bioseq::common::saturating_add(
                reserve, planned_anchor_growth);
            decline_out->anchors_out_growth_bytes = planned_anchor_growth;
        }

        std::optional<bioseq::parallelism::memory_permit>
            legacy_anchor_permit{};
        if (!context_backing_is_admitted && canonical_anchor_bytes != 0u) {
            bioseq::parallelism::memory_admission_failure output_failure{};
            legacy_anchor_permit = admission.try_reserve(
                canonical_anchor_bytes, &output_failure);
            if (!legacy_anchor_permit) {
                if (decline_out != nullptr) {
                    decline_out->reason = vulkan_seed_lookup_decline_reason::
                        seed_transient_memory_admission_denied;
                    decline_out->admission = output_failure;
                }
                const vulkan_seed_lookup_decline fallback_decline{
                    .reason = vulkan_seed_lookup_decline_reason::
                        seed_transient_memory_admission_denied,
                    .query_length = static_cast<std::uint64_t>(query_len),
                    .query_minimizer_count = static_cast<std::uint64_t>(
                        query_minimizers.size()),
                    .kmer_size = kmer_size,
                    .max_occ = max_occ,
                    .fused_frontend = true,
                    .admission = output_failure,
                };
                const auto& reported_decline = decline_out != nullptr
                    ? *decline_out
                    : fallback_decline;
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "Vulkan fused canonical anchor admission failed ({})",
                    format_vulkan_seed_lookup_decline(reported_decline)));
            }
        }

        bioseq::simd::canonical_seed_batch pending_batch{};
        if (!authenticate_mpf_seed_anchor_readback(
                readback,
                resident_index->target_position_offset,
                resident_index->target_len,
                static_cast<std::uint64_t>(query_len),
                static_cast<std::uint32_t>(kmer_size),
                submitted_ordinal,
                pending_batch, std::move(job_storage))) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan fused seed anchor authentication failed");
        }
        const std::uint64_t actual_slab_bytes =
            bioseq::common::saturating_mul(
                pending_batch.anchors.capacity(),
                sizeof(bioseq::chaining::anchor));
        if (actual_slab_bytes != canonical_anchor_bytes) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan fused canonical anchor capacity is invalid");
        }
        if (decline_out != nullptr) {
            decline_out->seed_lookup_dispatches =
                readback.seed_lookup_dispatches;
            decline_out->open_addressed_lookup_dispatches =
                readback.open_addressed_lookup_dispatches;
            decline_out->sorted_dense_lookup_dispatches =
                readback.sorted_dense_lookup_dispatches;
            decline_out->sort_stats = readback.sort_stats;
            decline_out->exact_anchor_count = readback.count;
            decline_out->anchors_out_actual_growth_bytes = actual_slab_bytes;
            record_vulkan_seed_anchor_audit(decline_out, readback);
        }

        vulkan_seed_lookup_completion completion{true};
        if (legacy_anchor_permit) {
            legacy_anchor_permit->shrink_to(actual_slab_bytes);
            if (legacy_anchor_permit->bytes() != actual_slab_bytes) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan fused canonical anchor permit is invalid");
            }
            completion.retained_anchor_bytes = actual_slab_bytes;
            completion.retained_anchor_permit =
                std::move(*legacy_anchor_permit);
            if (decline_out != nullptr) {
                decline_out->anchors_out_retained_permit_bytes =
                    actual_slab_bytes;
            }
        }
        anchors_out = std::move(pending_batch);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        completion.seed_operation_receipt = std::move(readback.seed_operation_receipt);
#endif
        return completion;
    }

    if (fused_frontend && max_occ < 0) {
        decline(vulkan_seed_lookup_decline_reason::negative_max_occ_unsupported);
        if (decline_out != nullptr) {
            return false;
        }
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "Vulkan fused seed lookup selected but negative max_occ is unsupported");
    }

    if (!mapped_prefix_authority && target_index == nullptr) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "non-mapped Vulkan seed lookup has no host prefix authority");
    }
    seed_prefix_plan prefix_plan;
    try {
        prefix_plan = mapped_prefix_authority
            ? count_seed_prefix_plan(
                query_minimizers, vulkan_runtime, resident_index, max_occ)
            : count_seed_prefix_plan(
                query_minimizers, *target_index, max_occ);
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
        record_vulkan_seed_anchor_audit(
            decline_out, bioseq::gpu::vulkan::audit_seed_anchor_canonical_order({}, 0u));
        return publish_empty_canonical_seed_batch(
            submitted_ordinal, anchors_out, std::move(job_storage));
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
    const std::uint64_t device_minimizer_record_offset =
        has_device_minimizers ? device_minimizers->offset() : 0u;
    if (device_minimizer_record_offset >
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        decline(vulkan_seed_lookup_decline_reason::query_minimizer_count_exceeds_u32);
        return false;
    }

    const auto reservation = plan_vulkan_seed_lookup_reservation<p_minimizer_type>(
        vulkan_runtime,
        query_minimizers.size(), sort_plan, has_device_minimizers,
        /*anchors_out_capacity=*/0u,
        context_backing_is_admitted);
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
    bioseq::simd::canonical_seed_batch pending_batch{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    bioseq::gpu::vulkan::runtime_pipeline_operation_token pending_seed_operation;
#endif
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
                    query_minimizers, *target_index, max_occ, prefix_plan);
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
                .query_minimizer_record_offset =
                    static_cast<std::uint32_t>(device_minimizer_record_offset),
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
            auto readback = vulkan_runtime.run_seed_lookup_roundtrip(
                inputs,
                std::span<const std::uint32_t>{
                    prefix.data(), prefix.size()},
                query_len,
                static_cast<std::uint32_t>(kmer_size),
                exact_anchors,
                sort_plan,
                reservation.vulkan_allocation_requirement_bytes
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                , query_authorization, query_index
#endif
                );

            if (readback.count != exact_anchors
                || readback.actual_vulkan_allocation_bytes
                    != reservation.vulkan_allocation_requirement_bytes
                || !readback.anchor_order_audit.canonical_order_checked
                || !readback.anchor_order_audit.canonical_order_accepted
                || readback.anchor_order_audit.anchor_count != readback.count) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan seed lookup result validation disagrees with admission");
            }

            if (!authenticate_mpf_seed_anchor_readback(
                    readback,
                    resident_index->target_position_offset,
                    resident_index->target_len,
                    static_cast<std::uint64_t>(query_len),
                    static_cast<std::uint32_t>(kmer_size),
                    submitted_ordinal,
                    pending_batch, std::move(job_storage))) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan seed lookup result authentication failed");
            }
            actual_slab_growth_bytes =
                bioseq::common::saturating_mul(
                    pending_batch.anchors.capacity(),
                    sizeof(bioseq::chaining::anchor));
            if ((!context_backing_is_admitted
                    && actual_slab_growth_bytes != anchors_out_growth_bytes)
                || (context_backing_is_admitted
                    && anchors_out_growth_bytes != 0u)) {
                post_allocation_invariant_failed = true;
            }
            if (decline_out != nullptr) {
                decline_out->seed_lookup_dispatches =
                    readback.seed_lookup_dispatches;
                decline_out->open_addressed_lookup_dispatches =
                    readback.open_addressed_lookup_dispatches;
                decline_out->sorted_dense_lookup_dispatches =
                    readback.sorted_dense_lookup_dispatches;
                decline_out->sort_stats = readback.sort_stats;
                decline_out->actual_vulkan_allocation_bytes =
                    readback.actual_vulkan_allocation_bytes;
                record_vulkan_seed_anchor_audit(decline_out, readback);
            }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            pending_seed_operation = std::move(readback.seed_operation_receipt);
#endif
        }

        // Temporary prefix and download buffers have been released. Context hooks
        // keep any backing-memory reservation they created. Other callers keep
        // the output anchor reservation in the returned completion object.
        vulkan_seed_lookup_completion completion{
            !post_allocation_invariant_failed};
        if (!context_backing_is_admitted) {
            permit->shrink_to(actual_slab_growth_bytes);
            if (permit->bytes() != actual_slab_growth_bytes) {
                post_allocation_invariant_failed = true;
                completion.is_successful = false;
            }
            completion.retained_anchor_bytes = permit->bytes();
            completion.retained_anchor_permit = std::move(*permit);
        }
        if (decline_out != nullptr) {
            decline_out->anchors_out_actual_growth_bytes =
                actual_slab_growth_bytes;
            decline_out->anchors_out_retained_permit_bytes =
                completion.retained_anchor_permit.bytes();
        }
        if (post_allocation_invariant_failed) {
            decline(mapped_prefix_authority
                ? vulkan_seed_lookup_decline_reason::
                    mapped_prefix_authority_invalid
                : vulkan_seed_lookup_decline_reason::
                    seed_arena_permit_owner_unavailable);
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan canonical anchor ownership reconciliation failed");
        }
        anchors_out = std::move(pending_batch);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        completion.seed_operation_receipt = std::move(pending_seed_operation);
#endif
        return completion;
    } catch (const mapped_prefix_invalid_error&) {
        decline(vulkan_seed_lookup_decline_reason::
            mapped_prefix_authority_invalid);
        return false;
    } catch (const bioseq::gpu::vulkan::vulkan_runtime_error&) {
        throw;
    }
}


}  // namespace bioseq::seeding
