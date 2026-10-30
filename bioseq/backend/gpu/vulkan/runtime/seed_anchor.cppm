/**
 * @file seed_anchor.cppm
 * @brief Finds seed matches in device indexes and orders the resulting anchors for chaining.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:seed_anchor;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/**
 * Verify native k-spans on the device, prefix-scan validity and emit dense anchors.
 * Borrowed windows and optional provider state remain alive until each fence.
 * Temporary and retained allocations use the existing memory admission owner.
 */
gpu_verified_seed_buffer runtime::verify_seed_anchors_locked(
    const buffer& candidates, std::uint64_t candidate_byte_offset, std::uint32_t candidate_count,
    const gpu_native_sequence_upload& query, const gpu_native_sequence_upload& target,
    const vulkan_rank_lut& rank_policy, std::uint32_t query_length,
    std::uint32_t kmer_size, std::uint64_t target_position_bias,
    const gpu_native_sequence_provider& target_provider,
    runtime_pipeline_use_collector* collector, std::size_t job_index) {
    gpu_verified_seed_buffer result{};
    result.candidate_count = candidate_count;
    if (candidate_count == 0u) return result;
    const std::uint64_t candidate_bytes = std::uint64_t{candidate_count} * sizeof(gpu_anchor_wire);
    if (kmer_size == 0u || kmer_size > 31u || query_length < kmer_size ||
        query.symbol_count != query_length || candidate_byte_offset > candidates.bytes ||
        candidate_bytes > candidates.bytes - candidate_byte_offset ||
        candidate_bytes > features_.max_storage_buffer_range ||
        features_.max_workgroup_count_x == 0u || rank_policy.alphabet_size == 0u ||
        rank_policy.alphabet_size > rank_policy.rank_to_code.size() ||
        rank_policy.canonical_reverse_complement > 1u || rank_policy.seed_policy_id == 0u) {
        throw vulkan_runtime_error("native seed verification input contract is invalid");
    }
    for (std::size_t rank = 0u; rank < rank_policy.alphabet_size; ++rank) {
        if (rank_policy.rank_to_code[rank] != rank ||
            (rank_policy.canonical_reverse_complement != 0u &&
             rank_policy.rank_to_rc_code[rank] >= rank_policy.alphabet_size))
            throw vulkan_runtime_error("native seed rank policy is malformed");
    }
    const auto binding_alignment = std::max<std::uint64_t>(features_.min_storage_buffer_offset_alignment, 1u);
    std::uint64_t candidate_binding_offset = candidate_byte_offset / binding_alignment * binding_alignment;
    while ((candidate_byte_offset - candidate_binding_offset) % sizeof(gpu_anchor_wire) != 0u) {
        if (candidate_binding_offset < binding_alignment)
            throw vulkan_runtime_error("native candidate interval has incompatible descriptor alignment");
        candidate_binding_offset -= binding_alignment;
    }
    const auto candidate_index_bias = (candidate_byte_offset - candidate_binding_offset) / sizeof(gpu_anchor_wire);
    const auto candidate_binding_bytes = candidate_bytes + candidate_byte_offset - candidate_binding_offset;
    if (candidate_index_bias > std::numeric_limits<std::uint32_t>::max() ||
        candidate_binding_bytes > features_.max_storage_buffer_range)
        throw vulkan_runtime_error("native candidate binding exceeds device address range");
    const std::uint32_t groups = std::min(
        bioseq::common::ceil_div(candidate_count, 128u), features_.max_workgroup_count_x);
    const auto prepare = [collector](const compute_pipeline& pipeline) {
        if (collector != nullptr)
            collector->prepare(make_shader_key(pipeline.kind, 384u, pipeline.local_size,
                                               pipeline.max_span), pipeline.metal_archive_receipt);
    };
    const auto begin_submission = [this, collector](const char* label) {
        reset_dispatch_descriptor_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0u), label);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), label);
        if (collector != nullptr) collector->begin(command_buffer_);
        record_host_to_shader(command_buffer_);
        record_compute_barrier(command_buffer_);
    };
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    gpu_native_sequence_upload selected_target = target;
    if (target_provider.prepare != nullptr) {
        if (!target.windows.empty() || target_provider.symbol_count < kmer_size)
            throw vulkan_runtime_error("native seed target provider conflicts with supplied windows");
        const std::uint64_t coverage_bytes = std::uint64_t{candidate_count} * sizeof(sequence_upload_interval);
        if (coverage_bytes > features_.max_storage_buffer_range)
            throw vulkan_runtime_error("native seed coverage exceeds storage buffer range");
        const auto coverage_requirement = bioseq::common::saturating_add(
            buffer_memory_requirement_locked(coverage_bytes, k_compute_buffer_usage),
            buffer_memory_requirement_locked(sizeof(std::uint32_t), k_compute_buffer_usage));
        auto coverage_permit = admission.try_reserve(coverage_requirement);
        if (!coverage_permit) throw vulkan_runtime_error("native seed coverage admission denied");
        buffer coverage = create_device_buffer(coverage_bytes, k_compute_buffer_usage);
        buffer status = create_device_buffer(sizeof(std::uint32_t), k_compute_buffer_usage);
        if (coverage.mapped == nullptr || status.mapped == nullptr)
            throw vulkan_runtime_error("native seed coverage requires mapped device memory");
        std::memset(status.mapped, 0, sizeof(std::uint32_t));
        const auto pipeline = get_compute_pipeline_locked(shader_kind::seed_native_coverage, 128u, 1u);
        prepare(pipeline);
        struct coverage_push {
            std::uint32_t candidate_count, kmer_size;
            std::uint64_t target_position_bias, symbol_count;
            std::uint32_t candidate_index_bias, reserved;
        };
        static_assert(sizeof(coverage_push) == 32u);
        const coverage_push push{candidate_count, kmer_size, target_position_bias,
                                 target_provider.symbol_count, static_cast<std::uint32_t>(candidate_index_bias), 0u};
        const std::array<VkDescriptorBufferInfo, 3> infos{{
            {candidates.buffer_handle, candidate_binding_offset, candidate_binding_bytes},
            {coverage.buffer_handle, 0u, coverage.bytes}, {status.buffer_handle, 0u, status.bytes}}};
        begin_submission("begin(native_seed_coverage)");
        record_compute_dispatch_locked(command_buffer_, pipeline, infos, &push, sizeof(push),
                                       groups, collector, job_index);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(native_seed_coverage)", collector);
        result.coverage_submit_wait_identity = next_operation_identity_locked();
        std::uint32_t status_value{};
        std::memcpy(&status_value, status.mapped, sizeof(status_value));
        if (status_value != 0u) throw vulkan_runtime_error("native seed coverage rejected candidate coordinates");
        result.coverage_interval_count = candidate_count;
        result.vulkan_allocation_bytes = coverage.allocation_bytes + status.allocation_bytes;
        selected_target = target_provider.prepare(target_provider.context,
            std::span{static_cast<const sequence_upload_interval*>(coverage.mapped), candidate_count});
        if (selected_target.symbol_count != target_provider.symbol_count)
            throw vulkan_runtime_error("native seed provider changed source length");
    }
    std::uint64_t packed_bytes = 0u;
    const auto validate_windows = [&rank_policy, &packed_bytes](const gpu_native_sequence_upload& source) {
        std::uint64_t previous_end = 0u;
        if (source.windows.empty() || source.windows.size() > std::numeric_limits<std::uint32_t>::max())
            throw vulkan_runtime_error("native seed source has no bounded windows");
        for (const auto& upload : source.windows) {
            const auto& descriptor = upload.sequence;
            if (!packed_sequence_upload_is_valid(upload) || descriptor.bp_count == 0u ||
                descriptor.bp_count > std::numeric_limits<std::uint32_t>::max() ||
                descriptor.bp_offset < previous_end || descriptor.bp_offset > source.symbol_count ||
                descriptor.bp_count > source.symbol_count - descriptor.bp_offset ||
                descriptor.alphabet_size != rank_policy.alphabet_size ||
                descriptor.alphabet_code != rank_policy.alphabet_code ||
                descriptor.seed_policy_id != rank_policy.seed_policy_id)
                throw vulkan_runtime_error("native seed windows have invalid geometry, ranks or identity");
            previous_end = descriptor.bp_offset + descriptor.bp_count;
            packed_bytes = bioseq::common::saturating_add(packed_bytes, descriptor.padded_byte_count);
        }
    };
    validate_windows(query);
    validate_windows(selected_target);
    const auto record_count = query.windows.size() + selected_target.windows.size();
    if (record_count > std::numeric_limits<std::uint32_t>::max())
        throw vulkan_runtime_error("native seed descriptor count exceeds uint32");
    const std::uint64_t record_bytes = record_count * sizeof(gpu_sequence_view_wire);
    const std::uint64_t flag_bytes = std::uint64_t{candidate_count} * sizeof(std::uint32_t);
    const auto block_count = bioseq::common::ceil_div(candidate_count, k_prefix_scan_block_size);
    const std::uint64_t block_bytes = std::uint64_t{block_count} * sizeof(std::uint32_t);
    const std::array<std::uint64_t, 8> logical_sizes{
        packed_bytes, record_bytes, sizeof(vulkan_rank_lut), flag_bytes, flag_bytes,
        block_bytes, 2u * sizeof(std::uint32_t), candidate_bytes};
    std::uint64_t required_bytes = 0u;
    for (const auto bytes : logical_sizes) {
        if (bytes == 0u || bytes > features_.max_storage_buffer_range)
            throw vulkan_runtime_error("native seed verification exceeds storage buffer range");
        required_bytes = bioseq::common::saturating_add(required_bytes,
            buffer_memory_requirement_locked(bytes, k_compute_buffer_usage));
    }
    auto reserved = admission.try_reserve(required_bytes);
    if (!reserved) throw vulkan_runtime_error("native seed verification admission denied");
    auto permit = std::move(*reserved);
    buffer packed = create_device_buffer(packed_bytes, k_compute_buffer_usage);
    buffer records = create_device_buffer(record_bytes, k_compute_buffer_usage);
    buffer policy = create_device_buffer(sizeof(vulkan_rank_lut), k_compute_buffer_usage);
    buffer validity = create_device_buffer(flag_bytes, k_compute_buffer_usage);
    buffer prefix = create_device_buffer(flag_bytes, k_compute_buffer_usage);
    buffer blocks = create_device_buffer(block_bytes, k_compute_buffer_usage);
    buffer counters = create_device_buffer(2u * sizeof(std::uint32_t), k_compute_buffer_usage);
    for (const auto* allocation : {&packed, &records, &policy, &validity, &prefix, &blocks, &counters})
        if (allocation->mapped == nullptr) throw vulkan_runtime_error("native seed verification requires mapped memory");
    std::size_t packed_offset = 0u;
    std::size_t record_offset = 0u;
    const auto copy_windows = [&packed, &records, &packed_offset, &record_offset](auto windows) {
        for (const auto& upload : windows) {
            const auto placed = place_packed_sequence_upload(upload, packed_offset,
                std::span{static_cast<std::byte*>(packed.mapped), static_cast<std::size_t>(packed.bytes)}, packed_sequence_upload_is_valid);
            if (!placed) throw vulkan_runtime_error("native seed packed placement failed");
            std::memcpy(static_cast<std::byte*>(records.mapped) + record_offset, &*placed, sizeof(*placed));
            packed_offset += upload.sequence.padded_byte_count;
            record_offset += sizeof(*placed);
        }
    };
    copy_windows(query.windows);
    copy_windows(selected_target.windows);
    std::memcpy(policy.mapped, &rank_policy, sizeof(rank_policy));
    std::memset(counters.mapped, 0, 2u * sizeof(std::uint32_t));
    const auto verify_count = get_compute_pipeline_locked(shader_kind::seed_verify_count, 128u, 1u);
    const auto verify_emit = get_compute_pipeline_locked(shader_kind::seed_verify_emit, 128u, 1u);
    const auto scan_block = get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const auto scan_add = get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
    for (const auto& pipeline : {verify_count, verify_emit, scan_block, scan_add}) prepare(pipeline);
    struct verify_push {
        std::uint32_t candidate_count, mode, kmer_size, query_length;
        std::uint64_t target_position_bias;
        std::uint32_t verified_count, query_window_count, target_window_count, reserved;
    };
    static_assert(sizeof(verify_push) == 40u);
    verify_push push{candidate_count, 0u, kmer_size, query_length, target_position_bias, 0u,
        static_cast<std::uint32_t>(query.windows.size()),
        static_cast<std::uint32_t>(selected_target.windows.size()), static_cast<std::uint32_t>(candidate_index_bias)};
    std::array<VkDescriptorBufferInfo, 8> infos{{
        {candidates.buffer_handle, candidate_binding_offset, candidate_binding_bytes},
        {packed.buffer_handle, 0u, packed.bytes}, {records.buffer_handle, 0u, records.bytes},
        {policy.buffer_handle, 0u, policy.bytes}, {validity.buffer_handle, 0u, validity.bytes},
        {prefix.buffer_handle, 0u, prefix.bytes},
        {candidates.buffer_handle, candidate_binding_offset, candidate_binding_bytes},
        {counters.buffer_handle, 0u, counters.bytes}}};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {validity.buffer_handle, 0u, validity.bytes}, {prefix.buffer_handle, 0u, prefix.bytes},
        {blocks.buffer_handle, 0u, blocks.bytes}}};
    const scan_push_constants scan0{candidate_count, k_prefix_scan_block_size, block_count, 0u};
    const scan_push_constants scan1{candidate_count, k_prefix_scan_block_size, block_count, 1u};
    begin_submission("begin(native_seed_verify_count)");
    record_compute_dispatch_locked(command_buffer_, verify_count, infos, &push, sizeof(push), groups, collector, job_index);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, scan_block, scan_infos, &scan0, sizeof(scan0),
        std::min(block_count, features_.max_workgroup_count_x), collector, job_index);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, scan_add, scan_infos, &scan0, sizeof(scan0), 1u, collector, job_index);
    record_compute_barrier(command_buffer_);
    record_compute_dispatch_locked(command_buffer_, scan_add, scan_infos, &scan1, sizeof(scan1), groups, collector, job_index);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(native_seed_verify_count)", collector);
    result.verify_count_submit_wait_identity = next_operation_identity_locked();
    std::array<std::uint32_t, 2> counts{};
    std::memcpy(counts.data(), counters.mapped, sizeof(counts));
    if (counts[0] != 0u) throw vulkan_runtime_error("native seed verification rejected invalid input or missing coverage");
    std::uint32_t final_prefix{}, final_validity{};
    std::memcpy(&final_prefix, static_cast<const std::byte*>(prefix.mapped) + flag_bytes - 4u, sizeof(final_prefix));
    std::memcpy(&final_validity, static_cast<const std::byte*>(validity.mapped) + flag_bytes - 4u, sizeof(final_validity));
    result.verified_count = std::uint64_t{final_prefix} + final_validity;
    if (final_validity > 1u || result.verified_count > candidate_count)
        throw vulkan_runtime_error("native seed validity prefix exceeds candidate count");
    const auto output_bytes = std::max<std::uint64_t>(sizeof(gpu_anchor_wire),
        result.verified_count * sizeof(gpu_anchor_wire));
    buffer output = create_device_buffer(output_bytes, k_compute_buffer_usage);
    if (output.mapped == nullptr) throw vulkan_runtime_error("native seed output requires mapped memory");
    infos[6] = VkDescriptorBufferInfo{output.buffer_handle, 0u, output.bytes};
    push.mode = 1u;
    push.verified_count = static_cast<std::uint32_t>(result.verified_count);
    begin_submission("begin(native_seed_verify_emit)");
    record_compute_dispatch_locked(command_buffer_, verify_emit, infos, &push, sizeof(push), groups, collector, job_index);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(native_seed_verify_emit)", collector);
    result.verify_emit_submit_wait_identity = next_operation_identity_locked();
    std::memcpy(counts.data(), counters.mapped, sizeof(counts));
    result.emitted_count = counts[1];
    if (counts[0] != 0u || result.emitted_count != result.verified_count)
        throw vulkan_runtime_error("native seed emission count disagrees with verified prefix");
    std::uint64_t actual_bytes = output.allocation_bytes;
    for (const auto* allocation : {&packed, &records, &policy, &validity, &prefix, &blocks, &counters})
        actual_bytes = bioseq::common::saturating_add(actual_bytes, allocation->allocation_bytes);
    if (actual_bytes > permit.bytes()) throw vulkan_runtime_error("native seed allocation exceeded admission");
    permit.shrink_to(actual_bytes);
    result.vulkan_allocation_bytes += actual_bytes;
    result.permit = permit.split_off(output.allocation_bytes);
    result.anchors = std::move(output);
    if (collector != nullptr)
        collector->set_seed_native_verification(job_index, result.candidate_count, result.verified_count,
                                                result.emitted_count, result.coverage_interval_count);
    return result;
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/// @brief Checks that a query slice matches the completed GPU sketch and its devicn buffer.
/// @return Identifier of the matching sketch record.
std::uint64_t runtime::validate_seed_query_authorization_locked(
    const device_query_sketch_authorization& authorization,
    const buffer* device_words,
    const gpu_seed_query_record_wire& submitted_record,
    std::uint32_t query_index,
    std::uint32_t kmer_size,
    std::uint32_t word_kind,
    std::uint32_t words32_per_record,
    std::uint32_t hash_shift,
    std::uint32_t pos_bits) const {
    if (device_words == nullptr || query_index >= authorization.query_records_.size() ||
        kmer_size != authorization.kmer_size_) {
        throw vulkan_runtime_error("MPF query slice has no matching device-sketch authority");
    }
    (void)validate_query_sketch_authorization_locked(
        authorization, *device_words, authorization.minimizer_count_, word_kind,
        words32_per_record, hash_shift, pos_bits, authorization.query_records_);
    const gpu_seed_query_record_wire& sealed = authorization.query_records_[query_index];
    if (submitted_record.minimizer_offset != sealed.minimizer_offset ||
        submitted_record.minimizer_count != sealed.minimizer_count ||
        submitted_record.query_len != sealed.query_len ||
        submitted_record.flags != sealed.flags) {
        throw vulkan_runtime_error("MPF submitted device slice differs from its sealed query");
    }
    return authorization.identity_;
}
#endif

/// @brief Writes counted seed hits and sorts anchors for chaining.
/// The GPU orders strand and target position. The host resolves equal-position ties.
/// @param[in] inputs Device index and resident or uploaded minimizers.
/// @param[in] prefix_counts Output offsets followed by the total.
/// @param[in] query_len Query length.
/// @param[in] kmer_size Anchor span.
/// @param[in] exact_anchor_count Expected count.
/// @param[in] sort_plan Device sort plan.
/// @param[in] expected_vulkan_allocation_bytes Reserved, allocation bytes.
/// @return Host anchors and measurements.
/// @throws vulkan_runtime_error If validation  or execution fails.
seed_lookup_result
runtime::run_seed_lookup_roundtrip(runtime_seed_lookup_inputs inputs,
                                   std::span<const std::uint32_t> prefix_counts,
                                   std::uint64_t query_len,
                                   std::uint32_t kmer_size,
                                   std::uint64_t exact_anchor_count,
                                   const seed_anchor_sort_plan& sort_plan,
                                   std::uint64_t expected_vulkan_allocation_bytes
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                   , const device_query_sketch_authorization* query_authorization,
                                   std::uint32_t query_index
#endif
                                   ) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw vulkan_runtime_error("run_seed_lookup_roundtrip on unavailable device");
    }
    if (prefix_counts.size() < 2 || exact_anchor_count == 0) {
        if (expected_vulkan_allocation_bytes != 0u) {
            throw vulkan_runtime_error("zero-anchor seed lookup carried a Vulkan allocation plan");
        }
        seed_lookup_result empty_result{};
        empty_result.anchor_order_audit = audit_seed_anchor_canonical_order({}, 0u);
        return empty_result;
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("seed lookup missing resident index buffers");
    }
    const std::uint64_t minimizer_count = prefix_counts.size() - 1u;
    if (inputs.device_minimizer_words == nullptr) {
        if (inputs.query_minimizer_record_offset != 0u) {
            throw vulkan_runtime_error("host-uploaded seed lookup carried a device record base");
        }
    } else {
        const std::uint64_t record_end =
            static_cast<std::uint64_t>(inputs.query_minimizer_record_offset) + minimizer_count;
        if (record_end > (std::numeric_limits<std::uint32_t>::max)() ||
            minimizer_device_bytes(inputs.minimizer_word_kind,
                                   inputs.minimizer_words32_per_record,
                                   record_end) > inputs.device_minimizer_words->bytes) {
            throw vulkan_runtime_error("seed lookup device minimizer subrange is malformed");
        }
    }
    if (static_cast<std::uint64_t>(inputs.position_count) * 8u > inputs.positions->bytes) {
        throw vulkan_runtime_error("seed lookup resident position range is malformed");
    }
    const std::uint64_t slot_count_from_bytes = inputs.slots->bytes / 16u;
    if (inputs.bucket_count != 0u) {
        if (inputs.slot_mask != 0u || inputs.bucket_count > slot_count_from_bytes) {
            throw vulkan_runtime_error(
                "seed lookup sorted-dense index representation is malformed");
        }
    } else {
        const std::uint64_t open_capacity = static_cast<std::uint64_t>(inputs.slot_mask) + 1u;
        if (inputs.slot_mask == 0u || !std::has_single_bit(open_capacity) ||
            open_capacity > slot_count_from_bytes) {
            throw vulkan_runtime_error(
                "seed lookup open-addressed index representation is malformed");
        }
    }
    const seed_anchor_sort_plan expected_sort_plan =
        make_seed_anchor_sort_plan(exact_anchor_count,
                                   inputs.target_len,
                                   inputs.target_position_offset,
                                   gpu_anchor_sort_capacity_,
                                   features_.max_workgroup_count_x,
                                   features_.max_workgroup_invocations,
                                   features_.max_workgroup_size_x,
                                   features_.max_storage_buffer_range);
    if (!sort_plan.is_valid() || sort_plan != expected_sort_plan ||
        sort_plan.backend == seed_anchor_sort_backend::none) {
        throw vulkan_runtime_error(
            "seed lookup sort plan does not match the runtime/device contract");
    }
    if (prefix_counts.back() != exact_anchor_count) {
        throw vulkan_runtime_error("seed lookup prefix total disagrees with anchor count");
    }
    constexpr std::uint32_t seed_local = 128u;
    const std::uint64_t seed_groups = (minimizer_count + seed_local - 1u) / seed_local;
    if (seed_groups > features_.max_workgroup_count_x) {
        throw vulkan_runtime_error("seed lookup dispatch exceeds maxComputeWorkGroupCount[0]");
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    std::unique_ptr<runtime_pipeline_use_collector> collector;
    std::uint64_t query_authority_identity = 0u;
    if (query_authorization != nullptr) {
        if (minimizer_count > (std::numeric_limits<std::uint32_t>::max)() ||
            query_len > (std::numeric_limits<std::uint32_t>::max)()) {
            throw vulkan_runtime_error("MPF seed receipt query shape exceeds its wire domain");
        }
        const gpu_seed_query_record_wire submitted_record{
            inputs.query_minimizer_record_offset, static_cast<std::uint32_t>(minimizer_count),
            static_cast<std::uint32_t>(query_len), 0u};
        query_authority_identity = validate_seed_query_authorization_locked(
            *query_authorization, inputs.device_minimizer_words, submitted_record, query_index,
            kmer_size, inputs.minimizer_word_kind, inputs.minimizer_words32_per_record,
            inputs.hash_shift, inputs.pos_bits);
        collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::mpf_seed_anchor_roundtrip, 1u,
            next_operation_identity_locked());
        collector->set_shape_input(0u, minimizer_count, inputs.target_len,
                                   inputs.target_position_offset);
    }
#endif

    const compute_pipeline seed_pipe =
        get_compute_pipeline_locked(shader_kind::seed_lookup_anchors, seed_local, 256u);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr)
        collector->prepare(make_shader_key(seed_pipe.kind, 384u, seed_pipe.local_size,
                                           seed_pipe.max_span), seed_pipe.metal_archive_receipt);
#endif

    buffer scratch = create_host_buffer(4u, k_compute_buffer_usage);

    buffer uploaded_min_words;
    const buffer* min_words = inputs.device_minimizer_words;
    if (min_words == nullptr) {
        const std::uint64_t min_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(
                inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, minimizer_count),
            4u);
        uploaded_min_words = create_device_buffer(min_bytes, k_compute_buffer_usage);
        if (uploaded_min_words.mapped != nullptr) {
            std::memset(
                uploaded_min_words.mapped, 0, static_cast<std::size_t>(uploaded_min_words.bytes));
        }
        if (!inputs.host_minimizer_bytes.empty()) {
            upload_locked(scratch,
                          uploaded_min_words,
                          inputs.host_minimizer_bytes,
                          VK_ACCESS_SHADER_READ_BIT);
        }
        min_words = &uploaded_min_words;
    }

    buffer prefix_buf = create_device_buffer(
        std::max<std::uint64_t>(prefix_counts.size() * sizeof(std::uint32_t), 4u),
        k_compute_buffer_usage);
    upload_locked(scratch, prefix_buf, std::as_bytes(prefix_counts), VK_ACCESS_SHADER_READ_BIT);

    const std::uint64_t anchor_bytes = sort_plan.anchor_bytes;
    buffer anchors_buf = create_device_buffer(anchor_bytes, k_compute_buffer_usage);
    buffer anchor_keys_buf = create_device_buffer(sort_plan.key_bytes, k_compute_buffer_usage);
    buffer ranges_buf;
    std::optional<compute_pipeline> sort_pipe;
    buffer anchors_scratch_buf;
    buffer anchor_keys_scratch_buf;
    buffer histogram_buf;
    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        ranges_buf = create_device_buffer(sizeof(sort_range_wire), k_compute_buffer_usage);
        const sort_range_wire range{0u, static_cast<std::uint32_t>(exact_anchor_count)};
        upload_locked(
            scratch,
            ranges_buf,
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(&range), sizeof(range)},
            VK_ACCESS_SHADER_READ_BIT);
        sort_pipe = get_compute_pipeline_locked(
            shader_kind::sort_anchors_bitonic, gpu_anchor_sort_capacity_, 256u);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (collector != nullptr)
            collector->prepare(make_shader_key(sort_pipe->kind, 384u, sort_pipe->local_size,
                                               sort_pipe->max_span), sort_pipe->metal_archive_receipt);
#endif
    } else {
        anchors_scratch_buf = create_device_buffer(sort_plan.anchor_bytes, k_compute_buffer_usage);
        anchor_keys_scratch_buf = create_device_buffer(sort_plan.key_bytes, k_compute_buffer_usage);
        histogram_buf = create_device_buffer(sort_plan.histogram_bytes, k_compute_buffer_usage);
    }

    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    // Count the physical bytes allocated for this lookup and sort.
    const auto add_allocation = [&actual_vulkan_allocation_bytes](const buffer& buffer) noexcept {
        actual_vulkan_allocation_bytes =
            bioseq::common::saturating_add(actual_vulkan_allocation_bytes, buffer.allocation_bytes);
    };
    add_allocation(scratch);
    add_allocation(uploaded_min_words);
    add_allocation(prefix_buf);
    add_allocation(anchors_buf);
    add_allocation(anchor_keys_buf);
    add_allocation(ranges_buf);
    add_allocation(anchors_scratch_buf);
    add_allocation(anchor_keys_scratch_buf);
    add_allocation(histogram_buf);
    if (actual_vulkan_allocation_bytes != expected_vulkan_allocation_bytes) {
        throw vulkan_runtime_error(fmt::format("seed lookup Vulkan allocation requirement mismatch "
                                               "(planned={} actual={})",
                                               expected_vulkan_allocation_bytes,
                                               actual_vulkan_allocation_bytes));
    }

    {
        const std::array<VkBuffer, 6> seed_bufs{min_words->buffer_handle,
                                                inputs.slots->buffer_handle,
                                                inputs.positions->buffer_handle,
                                                prefix_buf.buffer_handle,
                                                anchors_buf.buffer_handle,
                                                anchor_keys_buf.buffer_handle};
        bind_compute_pipeline_descriptors_locked(seed_pipe, seed_bufs);
        if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
            const std::array<VkBuffer, 2> sort_bufs{anchors_buf.buffer_handle,
                                                    ranges_buf.buffer_handle};
            bind_compute_pipeline_descriptors_locked(*sort_pipe, sort_bufs);
        }
    }

    if (sort_plan.backend == seed_anchor_sort_backend::radix) {
        reset_dispatch_descriptor_pool_locked();
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(seed)");
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr)
        collector->begin(command_buffer_);
#endif

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &host_to_shader,
                         0,
                         nullptr,
                         0,
                         nullptr);

    const seed_lookup_push_constants seed_push{static_cast<std::uint32_t>(minimizer_count),
                                               inputs.minimizer_words32_per_record,
                                               inputs.minimizer_word_kind,
                                               inputs.hash_shift,
                                               inputs.pos_bits,
                                               static_cast<std::uint32_t>(query_len),
                                               kmer_size,
                                               inputs.slot_mask,
                                               inputs.bucket_count,
                                               static_cast<std::uint32_t>(exact_anchor_count),
                                               sort_plan.target_ref_bits,
                                               inputs.query_minimizer_record_offset};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, seed_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            seed_pipe.pipeline_layout,
                            0,
                            1,
                            &seed_pipe.descriptor_set,
                            0,
                            nullptr);
    vkCmdPushConstants(command_buffer_,
                       seed_pipe.pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(seed_push),
                       &seed_push);
    vkCmdDispatch(command_buffer_, static_cast<std::uint32_t>(seed_groups), 1, 1);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr)
        collector->use(command_buffer_, 0u,
                       make_shader_key(seed_pipe.kind, 384u, seed_pipe.local_size,
                                       seed_pipe.max_span),
                       seed_pipe.metal_archive_receipt, seed_groups);
#endif

    VkMemoryBarrier shader_to_shader{};
    shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         1,
                         &shader_to_shader,
                         0,
                         nullptr,
                         0,
                         nullptr);

    bool radix_result_in_scratch = false;
    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, sort_pipe->pipeline);
        vkCmdBindDescriptorSets(command_buffer_,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                sort_pipe->pipeline_layout,
                                0,
                                1,
                                &sort_pipe->descriptor_set,
                                0,
                                nullptr);
        vkCmdDispatch(command_buffer_, 1, 1, 1);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (collector != nullptr)
            collector->use(command_buffer_, 0u,
                           make_shader_key(sort_pipe->kind, 384u, sort_pipe->local_size,
                                           sort_pipe->max_span),
                           sort_pipe->metal_archive_receipt, 1u);
#endif
    } else {
        const gpu_arena_slice anchors{&anchors_buf, 0u, sort_plan.anchor_bytes};
        const gpu_arena_slice keys{&anchor_keys_buf, 0u, sort_plan.key_bytes};
        const gpu_arena_slice anchors_scratch{&anchors_scratch_buf, 0u, sort_plan.anchor_bytes};
        const gpu_arena_slice keys_scratch{&anchor_keys_scratch_buf, 0u, sort_plan.key_bytes};
        const gpu_arena_slice histogram{&histogram_buf, 0u, sort_plan.histogram_bytes};
        const gpu_sort_plan radix_plan{exact_anchor_count, 8u, 24u, 8u, sort_plan.radix_pass_count};
        record_radix_sort_locked(command_buffer_,
                                 keys,
                                 anchors,
                                 keys_scratch,
                                 anchors_scratch,
                                 histogram,
                                 radix_plan,
                                 sort_plan.num_workgroups,
                                 sort_plan.elements_per_workgroup,
                                 3u,
                                 radix_result_in_scratch
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                 , collector.get(), 0u
#endif
                                 );
        if (radix_result_in_scratch !=
            is_seed_radix_result_in_scratch(sort_plan.radix_pass_count)) {
            throw vulkan_runtime_error("seed radix sort result parity mismatch");
        }
    }

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &shader_to_host,
                         0,
                         nullptr,
                         0,
                         nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(seed)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(seed)");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences(seed)");
    require_immutable_metal_policy_live("seed operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(seed)");
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr)
        collector->commit(command_buffer_);
#endif

    if (inputs.native_rank_policy == nullptr || query_len > std::numeric_limits<std::uint32_t>::max())
        throw vulkan_runtime_error("seed lookup lacks native source identity");
    const buffer& sorted_anchors = radix_result_in_scratch ? anchors_scratch_buf : anchors_buf;
    auto verified = verify_seed_anchors_locked(sorted_anchors, 0u,
        static_cast<std::uint32_t>(exact_anchor_count), inputs.native_query, inputs.native_target,
        *inputs.native_rank_policy, static_cast<std::uint32_t>(query_len), kmer_size,
        inputs.target_position_offset, {}
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        , collector.get(), 0u
#endif
        );
    seed_lookup_result result;
    result.count = verified.verified_count;
    result.candidate_count = exact_anchor_count;
    result.native_verification_vulkan_allocation_bytes = verified.vulkan_allocation_bytes;
    result.actual_vulkan_allocation_bytes = actual_vulkan_allocation_bytes;
    result.seed_lookup_dispatches = 1u;
    result.open_addressed_lookup_dispatches = inputs.bucket_count == 0u ? 1u : 0u;
    result.sorted_dense_lookup_dispatches = inputs.bucket_count != 0u ? 1u : 0u;
    result.host_bytes.resize(static_cast<std::size_t>(result.count * sizeof(gpu_anchor_wire)));
    if (!result.host_bytes.empty())
        std::memcpy(result.host_bytes.data(), verified.anchors.mapped, result.host_bytes.size());
    const auto tie_normalization = normalize_seed_anchor_equal_ref_ties(
        std::span<std::byte>{result.host_bytes.data(), result.host_bytes.size()}, result.count);
    if (!tie_normalization.gpu_ref_order_checked || !tie_normalization.gpu_ref_order_accepted) {
        throw vulkan_runtime_error("seed lookup GPU result is not ordered by Anchor ref_bits");
    }
    result.canonical_tie_normalization_runs = tie_normalization.canonical_tie_run_count;
    result.canonical_tie_normalization_anchors = tie_normalization.canonical_tie_anchor_count;
    result.anchor_order_audit = audit_seed_anchor_canonical_order(
        std::span<const std::byte>{result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!result.anchor_order_audit.canonical_order_checked ||
        !result.anchor_order_audit.canonical_order_accepted ||
        result.anchor_order_audit.anchor_count != result.count) {
        throw vulkan_runtime_error("seed lookup result is not in canonical Anchor order");
    }

    if (sort_plan.backend == seed_anchor_sort_backend::bitonic) {
        result.sort_stats.bitonic_sort_dispatches = 1u;
    } else {
        result.sort_stats.radix_sort_operations = 1u;
        result.sort_stats.radix_anchor_count = exact_anchor_count;
        result.sort_stats.radix_passes = sort_plan.radix_pass_count;
        result.sort_stats.radix_histogram_dispatches = sort_plan.radix_pass_count;
        result.sort_stats.radix_scan_dispatches = sort_plan.radix_pass_count;
        result.sort_stats.radix_scatter_dispatches = sort_plan.radix_pass_count;
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr) {
        collector->set_shape_outcome(0u, result.count, 0u);
        collector->set_seed_anchor_authentication(0u, query_authority_identity,
                                                  result.anchor_order_audit);
        require_immutable_metal_policy_live("MPF native seed receipt sealing");
        auto token = collector->seal();
        if (!token.is_valid_for_mpf_seed_anchor_roundtrip(features_.max_workgroup_count_x)) {
            throw vulkan_runtime_error("MPF native seed causal completion receipt is invalid");
        }
        result.seed_operation_receipt = std::move(token);
    }
#endif
    return result;
}

// Seed lookup with GPU counting and an exactly sized output buffer.

/// @brief Counts seed matches, allocates exact output, and sorts anchors for chaining.
/// The GPU orders strand and target position. The host resolves equal-position ties.
/// @param[in] inputs Device index and resident or uploaded minimizers.
/// @param[in] query_len Query length.
/// @param[in] kmer_size Anchor span.
/// @param[in] max_occ Skip minimizers whose target-occurrence count exceeds this limit.
/// @return Host anchors and measurements.
/// @throws vulkan_runtime_error If  validation, memory reservation, or execution fails.
seed_lookup_result runtime::run_seed_anchor_roundtrip(const gpu_seed_inputs& inputs,
                                                      std::uint64_t query_len,
                                                      std::uint32_t kmer_size,
                                                      std::int32_t max_occ
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                      , const device_query_sketch_authorization* query_authorization,
                                                      std::uint32_t query_index
#endif
                                                      ) {
    std::lock_guard<std::mutex> guard(mu_);
    seed_lookup_result result;
    result.anchor_order_audit = audit_seed_anchor_canonical_order({}, 0u);
    if (!features_.available)
        throw vulkan_runtime_error("run_seed_anchor_roundtrip on unavailable device");
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw vulkan_runtime_error("gpu seed missing resident index buffers");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    if (inputs.device_minimizer_words == nullptr) {
        if (inputs.query_minimizer_record_offset != 0u) {
            throw vulkan_runtime_error("host-uploaded fused seed carried a device record base");
        }
    } else {
        const std::uint64_t record_end =
            static_cast<std::uint64_t>(inputs.query_minimizer_record_offset) + mc;
        if (record_end > (std::numeric_limits<std::uint32_t>::max)() ||
            minimizer_device_bytes(inputs.minimizer_word_kind,
                                   inputs.minimizer_words32_per_record,
                                   record_end) > inputs.device_minimizer_words->bytes) {
            throw vulkan_runtime_error("fused seed device minimizer subrange is malformed");
        }
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    std::unique_ptr<runtime_pipeline_use_collector> collector;
    std::uint64_t query_authority_identity = 0u;
    if (query_authorization != nullptr) {
        if (query_len > (std::numeric_limits<std::uint32_t>::max)()) {
            throw vulkan_runtime_error("MPF fused seed receipt query length exceeds its wire domain");
        }
        const gpu_seed_query_record_wire submitted_record{
            inputs.query_minimizer_record_offset, mc, static_cast<std::uint32_t>(query_len), 0u};
        query_authority_identity = validate_seed_query_authorization_locked(
            *query_authorization, inputs.device_minimizer_words, submitted_record, query_index,
            kmer_size, inputs.minimizer_word_kind, inputs.minimizer_words32_per_record,
            inputs.hash_shift, inputs.pos_bits);
        if (mc != 0u && max_occ >= 0) {
            collector = runtime_pipeline_use_collector::create(
                runtime_pipeline_operation_kind::mpf_seed_anchor_roundtrip, 1u,
                next_operation_identity_locked());
            collector->set_shape_input(0u, mc, inputs.target_len, inputs.target_position_offset);
        }
    }
#endif
    gpu_arena& arena = reusable_gpu_arena();
    if (mc == 0u || max_occ < 0) {
        if (!arena.release_idle_capacity()) {
            throw vulkan_runtime_error("gpu seed idle arena release found live slices");
        }
        return result;
    }
    const std::uint32_t num_blocks = bioseq::common::ceil_div(mc, k_prefix_scan_block_size);
    const std::uint64_t min_words_bytes =
        inputs.device_minimizer_words == nullptr
            ? std::max<std::uint64_t>(minimizer_device_bytes(inputs.minimizer_word_kind,
                                                             inputs.minimizer_words32_per_record,
                                                             mc),
                                      4u)
            : 0u;
    // Include padding bezween arena slices, plus space to align a reused
    // arena cursor. The same request must fit both new and reused arenas.
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u + 16u +
                       min_words_bytes + 6u * std::uint64_t{k_gpu_buffer_offset_alignment};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        // A failed grow leaves allocation positions unchanged. Release any idle
        // backing buffers before reporting the failure so retries start clean
        if (!arena.release_idle_capacity()) {
            throw vulkan_runtime_error("gpu seed failed grow left live arena slices");
        }
        throw vulkan_runtime_error("gpu seed arena grow failed");
    }
    const gpu_arena_mark mark = arena.mark();
    // Return all per-query arena space and free its backing buffers on exit.
    struct seed_arena_lifetime {
        gpu_arena& arena;
        gpu_arena_mark mark;
        bool released = false;

        /// @brief Restores the arena mark and attempts to free backing buffers on exceptional exits.
        ~seed_arena_lifetime() noexcept {
            if (!released) {
                // Destruction  cannot report a cleanup error. Normal returns use
                // release_or_throw() so a remaining live slice is reported.
                arena.rewind(mark);
                (void)arena.release_idle_capacity();
            }
        }

        /// @brief Restores the arena mark and frees its buffers, throwing if a slice remains live.
        void release_or_throw() {
            arena.rewind(mark);
            if (!arena.release_idle_capacity()) {
                throw vulkan_runtime_error("gpu seed idle arena release found live slices");
            }
            released = true;
        }
    } seed_arena_lifetime{arena, mark};

    const gpu_arena_slice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const gpu_arena_slice blocks_s =
        arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const gpu_arena_slice qrec_s = arena.allocate_device(16u, 16u);
    gpu_arena_slice minw_s{};
    if (inputs.device_minimizer_words == nullptr)
        minw_s = arena.allocate_device(min_words_bytes, 16u);
    if (!counts_s.is_valid() || !prefix_s.is_valid() || !blocks_s.is_valid() ||
        !qrec_s.is_valid() || (inputs.device_minimizer_words == nullptr && !minw_s.is_valid())) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed arena oversubscribed");
    }
    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
                                          ? inputs.device_minimizer_words->buffer_handle
                                          : minw_s.buffer->buffer_handle;
    const std::uint64_t min_words_offset =
        inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
    const std::uint64_t min_words_range =
        inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
    if (inputs.device_minimizer_words == nullptr) {
        std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
        if (!inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minw_s.offset,
                        inputs.host_minimizer_bytes.data(),
                        inputs.host_minimizer_bytes.size());
        }
    }
    const host_query_record qrec{inputs.query_minimizer_record_offset,
                                 mc,
                                 static_cast<std::uint32_t>(query_len),
                                 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const compute_pipeline count_pipe =
        get_compute_pipeline_locked(shader_kind::seed_count, 128u, 1u);
    const compute_pipeline emit_pipe =
        get_compute_pipeline_locked(shader_kind::seed_emit, 128u, 1u);
    const compute_pipeline scanb =
        get_compute_pipeline_locked(shader_kind::prefix_scan_block, 1u, 1u);
    const compute_pipeline scana =
        get_compute_pipeline_locked(shader_kind::prefix_scan_add, 128u, 1u);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr) {
        const auto prepare = [collector_ptr = collector.get()](const compute_pipeline& pipeline) {
            collector_ptr->prepare(make_shader_key(pipeline.kind, 384u, pipeline.local_size,
                                                   pipeline.max_span),
                                   pipeline.metal_archive_receipt);
        };
        prepare(count_pipe);
        prepare(emit_pipe);
        prepare(scanb);
        prepare(scana);
    }
#endif

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const gpu_anchor_sort_key_plan anchor_sort_plan =
        make_anchor_sort_key_plan(inputs.target_len, inputs.target_position_offset);
    seed_push_constants cpush{inputs.minimizer_words32_per_record,
                              inputs.minimizer_word_kind,
                              inputs.hash_shift,
                              inputs.pos_bits,
                              kmer_size,
                              inputs.slot_mask,
                              static_cast<std::uint32_t>(max_occ),
                              0u,
                              1u,
                              inputs.bucket_count,
                              target_offset_lo,
                              target_offset_hi,
                              anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u, std::min(bioseq::common::ceil_div(mc, 128u), features_.max_workgroup_count_x));

    // First submission: count seed hits and find each minimizer's output offset.
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("seed_count");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer_handle,
             counts_s.offset,
             counts_s.size}, // Anchor output is unused while counting.
            {counts_s.buffer->buffer_handle,
             counts_s.offset,
             counts_s.size}, // Anchor-key output is unused while counting.
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer_handle, blocks_s.offset, blocks_s.size},
        }};
        scan_push_constants s0{mc, k_prefix_scan_block_size, num_blocks, 0u};
        scan_push_constants s1{mc, k_prefix_scan_block_size, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed1)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed1)");
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (collector != nullptr)
            collector->begin(command_buffer_);
#endif
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            , collector.get(), 0u
#endif
            );
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            , collector.get(), 0u
#endif
            );
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                       , collector.get(), 0u
#endif
                                       );
        record_compute_barrier(command_buffer_);
        record_compute_dispatch_locked(command_buffer_,
                                       scana,
                                       scan_infos,
                                       &s1,
                                       sizeof(s1),
                                       bioseq::common::ceil_div(mc, 128u)
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                       , collector.get(), 0u
#endif
                                       );
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed1)"
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                        , collector.get()
#endif
                        );
    }
    result.seed_lookup_dispatches = 1u;
    result.open_addressed_lookup_dispatches = inputs.bucket_count == 0u ? 1u : 0u;
    result.sorted_dense_lookup_dispatches = inputs.bucket_count != 0u ? 1u : 0u;
    std::uint32_t plast = 0, clast = 0;
    std::memcpy(&plast, base + prefix_s.offset + (mc - 1u) * 4u, 4);
    std::memcpy(&clast, base + counts_s.offset + (mc - 1u) * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(plast) + clast;
    if (total == 0u) {
        seed_arena_lifetime.release_or_throw();
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (collector != nullptr) {
            collector->set_shape_outcome(0u, 0u, 0u);
            collector->set_seed_anchor_authentication(0u, query_authority_identity,
                                                      result.anchor_order_audit);
            require_immutable_metal_policy_live("MPF zero-hit seed receipt sealing");
            auto token = collector->seal();
            if (!token.is_valid_for_mpf_seed_anchor_roundtrip(features_.max_workgroup_count_x)) {
                throw vulkan_runtime_error("MPF zero-hit seed causal completion receipt is invalid");
            }
            result.seed_operation_receipt = std::move(token);
        }
#endif
        return result;
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed anchor count overflow");
    }

    // Second, submission: write anchors and sort by strand and target position.
    // The coun is now known, so allocate the output and  sort buffers at their
    // required sizes. Keep them separate from the arena, whose counts and
    // offsets must remain valid until emission finishes.
    const std::uint64_t anchor_u64 = total * 3u; // 24-byte anchors as 3 u64
    const workgroup_plan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    const std::uint64_t exact_reserve = seed_anchor_reserved_bytes(total, awg.num_workgroups);
    auto exact_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(exact_reserve);
    if (!exact_permit) {
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error(
            fmt::format("gpu seed exact anchor memory admission failed for {} bytes "
                        "(anchors={} workgroups={} reserved={} allocatable={})",
                        exact_reserve,
                        total,
                        awg.num_workgroups,
                        admission.reserved_bytes(),
                        admission.allocatable_after_reservations()));
    }
    buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, k_compute_buffer_usage);
    buffer akeys_scr_buf = create_device_buffer(total * 8u, k_compute_buffer_usage);
    buffer ahist_buf =
        create_device_buffer(std::uint64_t{awg.num_workgroups} * 256u * 4u, k_compute_buffer_usage);
    if (anchors_buf.mapped == nullptr) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed non-unified memory");
    }
    const gpu_arena_slice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const gpu_arena_slice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const gpu_arena_slice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const gpu_arena_slice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const gpu_arena_slice ahist_s{&ahist_buf, 0, ahist_buf.bytes};

    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instrumentation::register_static_region("seed_emit");
        auto _rs = bioseq::instrumentation::region(_rd);
        reset_dispatch_descriptor_pool_locked();
        seed_push_constants epush = cpush;
        epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer_handle, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer_handle, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer_handle, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer_handle, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer_handle, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer_handle, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer_handle, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed2)");
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (collector != nullptr)
            collector->begin(command_buffer_);
#endif
        record_host_to_shader(command_buffer_);
        record_compute_dispatch_locked(
            command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            , collector.get(), 0u
#endif
            );
        record_compute_barrier(command_buffer_);
        gpu_sort_plan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(command_buffer_,
                                 akeys_s,
                                 anchors_s,
                                 akeys_scr_s,
                                 anchors_scr_s,
                                 ahist_s,
                                 sort_plan,
                                 awg.num_workgroups,
                                 awg.elems_per_wg,
                                 3u,
                                 sorted_in_scratch
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                 , collector.get(), 0u
#endif
                                 );
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed2)"
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                        , collector.get()
#endif
                        );
    }
    const gpu_arena_slice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    if (inputs.native_rank_policy == nullptr || query_len > std::numeric_limits<std::uint32_t>::max())
        throw vulkan_runtime_error("fused seed lookup lacks native source identity");
    auto verified = verify_seed_anchors_locked(*sorted_anchors.buffer, sorted_anchors.offset,
        static_cast<std::uint32_t>(total), inputs.native_query, inputs.native_target,
        *inputs.native_rank_policy, static_cast<std::uint32_t>(query_len), kmer_size, 0u, {}
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        , collector.get(), 0u
#endif
        );
    result.count = verified.verified_count;
    result.candidate_count = total;
    result.native_verification_vulkan_allocation_bytes = verified.vulkan_allocation_bytes;
    result.host_bytes.resize(static_cast<std::size_t>(result.count * sizeof(gpu_anchor_wire)));
    if (!result.host_bytes.empty())
        std::memcpy(result.host_bytes.data(), verified.anchors.mapped, result.host_bytes.size());
    const auto tie_normalization = normalize_seed_anchor_equal_ref_ties(
        std::span<std::byte>{result.host_bytes.data(), result.host_bytes.size()}, result.count);
    if (!tie_normalization.gpu_ref_order_checked || !tie_normalization.gpu_ref_order_accepted) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed GPU result is not ordered by Anchor ref_bits");
    }
    result.canonical_tie_normalization_runs = tie_normalization.canonical_tie_run_count;
    result.canonical_tie_normalization_anchors = tie_normalization.canonical_tie_anchor_count;
    result.anchor_order_audit = audit_seed_anchor_canonical_order(
        std::span<const std::byte>{result.host_bytes.data(), result.host_bytes.size()},
        result.count);
    if (!result.anchor_order_audit.canonical_order_checked ||
        !result.anchor_order_audit.canonical_order_accepted ||
        result.anchor_order_audit.anchor_count != result.count) {
        seed_arena_lifetime.release_or_throw();
        throw vulkan_runtime_error("gpu seed result is not in canonical Anchor order");
    }
    result.sort_stats.radix_sort_operations = 1u;
    result.sort_stats.radix_anchor_count = total;
    result.sort_stats.radix_passes = anchor_sort_plan.pass_count;
    result.sort_stats.radix_histogram_dispatches = anchor_sort_plan.pass_count;
    result.sort_stats.radix_scan_dispatches = anchor_sort_plan.pass_count;
    result.sort_stats.radix_scatter_dispatches = anchor_sort_plan.pass_count;

    seed_arena_lifetime.release_or_throw();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr) {
        collector->set_shape_outcome(0u, result.count, 0u);
        collector->set_seed_anchor_authentication(0u, query_authority_identity,
                                                  result.anchor_order_audit);
        require_immutable_metal_policy_live("MPF fused seed receipt sealing");
        auto token = collector->seal();
        if (!token.is_valid_for_mpf_seed_anchor_roundtrip(features_.max_workgroup_count_x)) {
            throw vulkan_runtime_error("MPF fused seed causal completion receipt is invalid");
        }
        result.seed_operation_receipt = std::move(token);
    }
#endif
    return result;
}

} // namespace bioseq::gpu::vulkan
