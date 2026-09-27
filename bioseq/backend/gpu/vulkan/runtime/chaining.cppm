/**
 * @file chaining.cppm
 * @brief Runs GPU chaining and reads back scores, predecessors, or one selected chain.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:chaining;

import :frontend_types;
import :support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

/**
 * @brief Runs tiled GPU chaining and copies scores and predecessors back to the caller.
 *
 * The caller supplies anchor ranges and the initial score and predecessor arrays.
 * This call uploads them, waits for the dispatch, and replaces the output arrays
 * with the computed values.
 */
void runtime::run_chaining_tiled_dp_roundtrip(const chaining_pipeline_key& key,
                                              buffer& transfer_scratch,
                                              buffer& anchors,
                                              buffer& ranges,
                                              buffer& best_scores,
                                              buffer& predecessors,
                                              std::span<const std::byte> anchors_bytes,
                                              std::span<const std::byte> ranges_bytes,
                                              std::span<std::byte> best_scores_bytes,
                                              std::span<std::byte> predecessors_bytes,
                                              std::uint32_t anchor_count,
                                              std::uint32_t range_count,
                                              std::int32_t max_dist_x,
                                              std::int32_t max_dist_y,
                                              std::int32_t bandwidth,
                                              std::int32_t max_iter,
                                              float gap_penalty,
                                              float skip_penalty,
                                              std::int32_t max_chain_skip
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                              , runtime_pipeline_operation_token* completed_operation
#endif
                                              ) {
    std::lock_guard<std::mutex> guard(mu_);

    if (!features_.available) {
        throw vulkan_runtime_error("run_chaining_tiled_dp_roundtrip on unavailable device");
    }
    // Each workgroup processes a range, then advances by the total workgroup
    // count. This lets the kernel process all ranges when their count exceeds
    // the device limit for a single dispatch.
    const std::uint32_t chain_groups = std::min(range_count, features_.max_workgroup_count_x);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const chaining_pipeline selected_pipeline = create_pipeline_locked(key);
    const VkPipeline pipeline = selected_pipeline.pipeline;
    std::unique_ptr<runtime_pipeline_use_collector> collector;
    if (completed_operation != nullptr) {
        if (completed_operation->audit().kind != runtime_pipeline_operation_kind::invalid ||
            anchor_count == 0u || range_count == 0u || range_count > anchor_count) {
            throw vulkan_runtime_error("chaining receipt output or input shape is invalid");
        }
        collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::chaining_tiled_dp, 1u,
            next_operation_identity_locked());
        collector->prepare(selected_pipeline.shader_key,
                           selected_pipeline.metal_archive_receipt);
        // Record anchor and range counts for this chaining-only operation.
        collector->set_shape_input(0u, 0u, 0u, 0u);
        collector->set_shape_outcome(0u, anchor_count, range_count);
    }
#else
    const VkPipeline pipeline = create_pipeline_locked(key).pipeline;
#endif

    // Upload anchors, ranges, and the caller's initial scores and predecessors.
    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch,
                  best_scores,
                  best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch,
                  predecessors,
                  predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // Point the reusable descriptor set at this call's four buffers.
    const std::array<VkDescriptorBufferInfo, 4> buffer_infos{{
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {ranges.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t binding_index = 0; binding_index < 4; ++binding_index) {
        writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = descriptor_set_;
        writes[binding_index].dstBinding = binding_index;
        writes[binding_index].descriptorCount = 1;
        writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding_index].pBufferInfo = &buffer_infos[binding_index];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    chaining_push_constants push{};
    push.anchor_count = anchor_count;
    push.range_count = range_count;
    push.max_dist_x = max_dist_x;
    push.max_dist_y = max_dist_y;
    push.bandwidth = bandwidth;
    push.max_iter = max_iter;
    push.gap_penalty = gap_penalty;
    push.skip_penalty = skip_penalty;
    push.max_chain_skip = max_chain_skip;

    // Make host writes visible to the shader, then make its results visible to the host.
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer");
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

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_,
                            0,
                            1,
                            &descriptor_set_,
                            0,
                            nullptr);
    vkCmdPushConstants(
        command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr)
        collector->use(command_buffer_, 0u, selected_pipeline.shader_key,
                       selected_pipeline.metal_archive_receipt, chain_groups);
#endif

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

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "vkWaitForFences");
    require_immutable_metal_policy_live("chaining operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences");
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr)
        collector->commit(command_buffer_);
#endif

    // Copy scores and predecessors back after the GPU has finished.
    download_locked(transfer_scratch, best_scores, best_scores_bytes);
    download_locked(transfer_scratch, predecessors, predecessors_bytes);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (collector != nullptr) {
        require_immutable_metal_policy_live("chaining receipt sealing");
        auto token = collector->seal();
        if (!token.is_valid_for_chaining_tiled_dp(features_.max_workgroup_count_x)) {
            throw vulkan_runtime_error("chaining causal completion receipt is invalid");
        }
        *completed_operation = std::move(token);
    }
#endif
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
#if defined(BIOSEQ_TEST_INDEXES)
namespace {

struct selected_pipeline_finalizer_test_rejection {};

struct selected_pipeline_finalizer_test_probe {
    runtime_gpu_execution_environment_snapshot observed_environment{};
    runtime_gpu_cache_metrics_audit observed_audit{};
    std::size_t publication_calls = 0u;
    std::size_t accepted_publications = 0u;
    std::size_t live_calls = 0u;
    bool reject_publication = false;
    bool reject_live = false;
    bool order_valid = true;

    static void validate_publication(
        void* context,
        const runtime_gpu_execution_environment_snapshot& environment,
        const runtime_gpu_cache_metrics_audit& audit) {
        auto& probe = *static_cast<selected_pipeline_finalizer_test_probe*>(context);
        ++probe.publication_calls;
        probe.observed_environment = environment;
        probe.observed_audit = audit;
        if (probe.reject_publication) {
            throw selected_pipeline_finalizer_test_rejection{};
        }
        ++probe.accepted_publications;
    }

    static void validate_live(void* context) {
        auto& probe = *static_cast<selected_pipeline_finalizer_test_probe*>(context);
        ++probe.live_calls;
        probe.order_valid =
            probe.order_valid && probe.accepted_publications == probe.live_calls;
        if (probe.reject_live) {
            throw selected_pipeline_finalizer_test_rejection{};
        }
    }
};

} // namespace
#endif

std::pair<runtime_gpu_execution_environment_snapshot, runtime_gpu_cache_metrics_delta>
runtime::finalize_selected_pipeline_operation_transaction(
    runtime_pipeline_operation_token&& token,
    selected_pipeline_publication_validator validate_publication,
    void* publication_context,
    std::span<runtime_pipeline_operation_token> additional_operations,
    std::span<device_query_sketch_authorization> additional_query_authorizations,
    const vulkan_feature_set& live_features,
    bool& seed_metrics_claimed,
    selected_pipeline_live_validator validate_live,
    void* live_context) {
    const auto& audit = token.audit();
    std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
        distinct_receipts{};
    std::size_t receipt_count = 0u;
    // Validate every operation before claiming its metrics. Count a shared
    // pipeline once even when several jobs used it.
    const auto collect_operation = [&audit, &distinct_receipts, &receipt_count, &live_features](
                                       const runtime_pipeline_operation_token& operation) {
        bool accepted_kind = false;
        switch (operation.audit().kind) {
        case runtime_pipeline_operation_kind::query_sketch:
            accepted_kind = operation.is_valid_for_query_sketch();
            break;
        case runtime_pipeline_operation_kind::grouped_soa_seed_anchor_roundtrip:
            accepted_kind = operation.is_valid_for_grouped_soa_seed_anchor_roundtrip();
            break;
        case runtime_pipeline_operation_kind::chaining_tiled_dp:
            accepted_kind = operation.is_valid_for_chaining_tiled_dp(
                live_features.max_workgroup_count_x);
            break;
        case runtime_pipeline_operation_kind::mpf_seed_anchor_roundtrip:
            accepted_kind = operation.is_valid_for_mpf_seed_anchor_roundtrip(
                live_features.max_workgroup_count_x);
            break;
        default:
            break;
        }
        const auto& current = operation.audit();
        if (!accepted_kind || current.native_vulkan != audit.native_vulkan ||
            !live_features.available || live_features.software_device ||
            current.device_uuid != live_features.device_uuid ||
            current.api_version != live_features.api_version ||
            current.driver_version != live_features.driver_version ||
            current.vendor_id != live_features.vendor_id ||
            current.device_id != live_features.device_id ||
            current.pipeline_cache_uuid != live_features.pipeline_cache_uuid ||
            current.seed_sha256 != audit.seed_sha256 || current.seed_size != audit.seed_size ||
            current.manifest_checksum != audit.manifest_checksum ||
            current.seed_receipt_identity != audit.seed_receipt_identity) {
            throw vulkan_runtime_error("selected pipeline completion differs from the live GPU");
        }
        for (std::size_t index = 0u; index < current.prepared_count; ++index) {
            const auto* receipt = operation.receipts_[index];
            bool already_present = false;
            for (std::size_t prior = 0u; prior < receipt_count; ++prior) {
                already_present = already_present || distinct_receipts[prior] == receipt;
            }
            if (already_present)
                continue;
            if (receipt_count == distinct_receipts.size()) {
                throw vulkan_runtime_error("selected pipeline claim exceeds its fixed receipt set");
            }
            distinct_receipts[receipt_count++] = receipt;
        }
    };
    collect_operation(token);
    for (auto& operation : additional_operations) {
        if (std::addressof(operation) == std::addressof(token)) {
            throw vulkan_runtime_error("selected pipeline completion aliases its primary token");
        }
        collect_operation(operation);
    }
    for (const auto& authorization : additional_query_authorizations) {
        if (!authorization.is_valid() ||
            authorization.runtime_ == nullptr ||
            std::addressof(authorization.pipeline_token_) == std::addressof(token)) {
            throw vulkan_runtime_error(
                "selected query completion has an invalid additional authority");
        }
        collect_operation(authorization.pipeline_token_);
    }
    const auto receipts = std::span<const runtime_metal_archive_pipeline_receipt* const>{
        distinct_receipts.data(), receipt_count};
    runtime_gpu_cache_metrics_audit maximum{
        .token_identity = audit.operation_identity,
        .gpu_pipeline_cache_loads = audit.native_vulkan ? 0u : 1u,
        .gpu_pipeline_cache_seed_bytes = audit.seed_size,
        .gpu_pipeline_cache_load_ns = receipts.front()->audit().seed_load_ns,
    };
    for (const auto* receipt : receipts) {
        const auto& pipeline = receipt->audit();
        const auto creates = bioseq::common::checked_add(maximum.gpu_pipeline_creates,
                                                        std::uint64_t{1u});
        const auto create_ns = bioseq::common::checked_add(maximum.gpu_pipeline_creation_ns,
                                                          pipeline.pipeline_create_ns);
        if (!creates || !create_ns || pipeline.seed_load_ns != maximum.gpu_pipeline_cache_load_ns) {
            throw vulkan_runtime_error("selected pipeline cache maximum is inconsistent");
        }
        maximum.gpu_pipeline_creates = *creates;
        maximum.gpu_pipeline_creation_ns = *create_ns;
    }
    auto claim = runtime_detail::runtime_gpu_cache_metrics_claim_plan::prepare(
        audit.operation_identity, maximum, seed_metrics_claimed, receipts);
    if (!claim.is_valid()) {
        throw vulkan_runtime_error("selected pipeline cache claim is invalid");
    }
    std::pair<runtime_gpu_execution_environment_snapshot, runtime_gpu_cache_metrics_delta> result;
    result.first = runtime_gpu_execution_environment_snapshot{
        .gpu_environment_present = true,
        .gpu_device_uuid = audit.device_uuid,
        .gpu_driver_identity = canonical_vulkan_driver_identity(live_features),
        .cache_policy = audit.native_vulkan ? runtime_pipeline_cache_policy::native_vulkan
                                           : runtime_pipeline_cache_policy::immutable_preseed_all_hit,
        .seed_sha256 = audit.seed_sha256,
        .seed_size = audit.seed_size,
        .manifest_checksum = audit.manifest_checksum,
        .pipeline_cache_all_hit = !audit.native_vulkan,
    };
    if (!result.first.is_internally_consistent()) {
        throw vulkan_runtime_error("selected pipeline environment is incomplete");
    }
    if (validate_publication != nullptr) {
        validate_publication(publication_context, result.first, claim.audit());
    }
    if (validate_live == nullptr) {
        throw vulkan_runtime_error("selected pipeline final publication has no live gate");
    }
    validate_live(live_context);
    // No allocation, dispatch, or fallible check follows this report-once commit.
    result.second.audit_ = claim.audit();
    claim.commit(seed_metrics_claimed);
    result.second.issued_ = true;
    for (auto& operation : additional_operations) {
        operation = runtime_pipeline_operation_token{};
    }
    for (auto& authorization : additional_query_authorizations) {
        authorization.clear();
    }
    token = runtime_pipeline_operation_token{};
    return result;
}

#if defined(BIOSEQ_TEST_INDEXES)
bool runtime::self_test_selected_pipeline_finalizer_transaction_contract(
    runtime_metal_archive_pipeline_audit base_audit) {
    if (!base_audit.is_internally_consistent() || base_audit.pipeline_create_ns == 0u) {
        return false;
    }
    const std::array<shader_variant_key, 9u> keys{
        make_shader_key(shader_kind::ordered_sketch_tile, 384u, 64u, 10u),
        make_shader_key(shader_kind::prefix_scan_block, 384u, 1u, 1u),
        make_shader_key(shader_kind::prefix_scan_add, 384u, 128u, 1u),
        make_shader_key(
            shader_kind::grouped_soa_seed_count, 384u, 128u,
            k_grouped_soa_seed_cache_abi_revision),
        make_shader_key(
            shader_kind::grouped_soa_seed_emit, 384u, 128u,
            k_grouped_soa_seed_cache_abi_revision),
        make_shader_key(shader_kind::radix_sort_histogram, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scan, 384u, 1u, 1u),
        make_shader_key(shader_kind::radix_sort_scatter, 384u, 1u, 1u),
        make_shader_key(shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u),
    };
    if (!std::all_of(keys.begin(), keys.end(), [](const shader_variant_key& key) {
            return is_shader_variant_key_valid(key);
        })) {
        return false;
    }

    std::array<runtime_metal_archive_pipeline_receipt, 9u> receipts{};
    for (std::size_t index = 0u; index < receipts.size(); ++index) {
        auto audit = base_audit;
        audit.receipt_identity = 12'000u + index;
        audit.scope_identity = 12'100u + index;
        audit.shader_key = keys[index];
        receipts[index] = runtime_metal_archive_pipeline_receipt{std::move(audit)};
        if (!receipts[index].is_valid()) {
            return false;
        }
    }

    const auto command = [](const std::uintptr_t identity) {
        return reinterpret_cast<VkCommandBuffer>(identity);
    };
    const auto make_query = [&keys, &receipts, command](const std::uint64_t identity) {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::query_sketch, 1u, identity);
        for (std::size_t index = 0u; index < 3u; ++index) {
            collector->prepare(keys[index], &receipts[index]);
        }
        collector->begin(command(identity));
        collector->use(command(identity), 0u, keys[0], &receipts[0], 3u);
        collector->use(command(identity), 0u, keys[1], &receipts[1], 2u);
        collector->use(command(identity), 0u, keys[2], &receipts[2], 2u);
        collector->commit(command(identity));
        collector->begin(command(identity + 1u));
        collector->use(command(identity + 1u), 0u, keys[0], &receipts[0], 3u);
        collector->use(command(identity + 1u), 0u, keys[2], &receipts[2], 1u);
        collector->commit(command(identity + 1u));
        return collector->seal();
    };
    const auto make_seed = [&keys, &receipts, command](const std::uint64_t identity) {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::grouped_soa_seed_anchor_roundtrip, 1u, identity);
        constexpr std::array<std::size_t, 7u> slots{3u, 4u, 1u, 2u, 5u, 6u, 7u};
        for (const std::size_t slot : slots) {
            collector->prepare(keys[slot], &receipts[slot]);
        }
        collector->set_shape_input(0u, 1u, 255u, 0u);
        collector->set_shape_outcome(0u, 1u, 0u);
        collector->set_seed_anchor_authentication(
            0u,
            12'200u,
            seed_anchor_canonical_order_audit{
                .anchor_count = 1u,
                .digest_lo = 0x1234u,
                .digest_hi = 0x5678u,
                .canonical_order_checked = true,
                .canonical_order_accepted = true,
            });
        collector->begin(command(identity));
        collector->use(command(identity), 0u, keys[3], &receipts[3], 1u);
        collector->use(command(identity), 0u, keys[1], &receipts[1], 1u);
        collector->use(command(identity), 0u, keys[2], &receipts[2], 1u);
        collector->use(command(identity), 0u, keys[2], &receipts[2], 1u);
        collector->commit(command(identity));
        collector->begin(command(identity + 1u));
        collector->use(command(identity + 1u), 0u, keys[4], &receipts[4], 1u);
        for (std::size_t pass = 0u; pass < 2u; ++pass) {
            for (std::size_t slot = 5u; slot < 8u; ++slot) {
                collector->use(command(identity + 1u), 0u, keys[slot], &receipts[slot], 1u);
            }
        }
        collector->commit(command(identity + 1u));
        return collector->seal();
    };
    const auto make_chain = [&keys, &receipts, command](const std::uint64_t identity) {
        auto collector = runtime_pipeline_use_collector::create(
            runtime_pipeline_operation_kind::chaining_tiled_dp, 1u, identity);
        collector->prepare(keys[8], &receipts[8]);
        collector->set_shape_input(0u, 0u, 0u, 0u);
        collector->set_shape_outcome(0u, 64u, 2u);
        collector->begin(command(identity));
        collector->use(command(identity), 0u, keys[8], &receipts[8], 2u);
        collector->commit(command(identity));
        return collector->seal();
    };

    runtime_pipeline_operation_token query{};
    std::array<runtime_pipeline_operation_token, 4u> additional{};
    try {
        query = make_query(12'300u);
        additional[0] = make_seed(12'400u);
        additional[1] = make_chain(12'500u);
        additional[2] = make_chain(12'600u);
        additional[3] = make_query(12'650u);
    } catch (...) {
        return false;
    }

    vulkan_feature_set live_features{};
    live_features.available = true;
    live_features.software_device = false;
    live_features.max_workgroup_count_x = 2u;
    live_features.device_uuid = query.audit().device_uuid;
    live_features.api_version = query.audit().api_version;
    live_features.driver_version = query.audit().driver_version;
    live_features.vendor_id = query.audit().vendor_id;
    live_features.device_id = query.audit().device_id;
    live_features.pipeline_cache_uuid = query.audit().pipeline_cache_uuid;

    const auto tokens_are_intact = [&query, &additional, &live_features]() {
        return query.is_valid_for_query_sketch() &&
               additional[0].is_valid_for_grouped_soa_seed_anchor_roundtrip() &&
               additional[1].is_valid_for_chaining_tiled_dp(
                   live_features.max_workgroup_count_x) &&
               additional[2].is_valid_for_chaining_tiled_dp(
                   live_features.max_workgroup_count_x) &&
               additional[3].is_valid_for_query_sketch();
    };
    bool seed_metrics_claimed = false;
    const auto claims_are_clear = [&receipts, &seed_metrics_claimed]() {
        return !seed_metrics_claimed &&
               std::none_of(receipts.begin(), receipts.end(), [](const auto& receipt) {
                   return receipt.metrics_claimed_;
               });
    };
    runtime_gpu_cache_metrics_audit expected{
        .token_identity = query.audit().operation_identity,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = query.audit().seed_size,
        .gpu_pipeline_cache_load_ns = receipts.front().audit().seed_load_ns,
        .gpu_pipeline_creates = static_cast<std::uint64_t>(receipts.size()),
    };
    for (const auto& receipt : receipts) {
        const auto total = bioseq::common::checked_add(
            expected.gpu_pipeline_creation_ns, receipt.audit().pipeline_create_ns);
        if (!total) {
            return false;
        }
        expected.gpu_pipeline_creation_ns = *total;
    }
    if (!tokens_are_intact() || !claims_are_clear() || !expected.is_internally_consistent()) {
        return false;
    }

    selected_pipeline_finalizer_test_probe probe{};
    probe.reject_publication = true;
    try {
        auto rejected = finalize_selected_pipeline_operation_transaction(
            std::move(query),
            &selected_pipeline_finalizer_test_probe::validate_publication,
            &probe,
            additional,
            {},
            live_features,
            seed_metrics_claimed,
            &selected_pipeline_finalizer_test_probe::validate_live,
            &probe);
        static_cast<void>(rejected);
        return false;
    } catch (const selected_pipeline_finalizer_test_rejection&) {
    } catch (...) {
        return false;
    }
    if (probe.publication_calls != 1u || probe.accepted_publications != 0u ||
        probe.live_calls != 0u || probe.observed_audit != expected ||
        !probe.observed_environment.is_internally_consistent() || !tokens_are_intact() ||
        !claims_are_clear()) {
        return false;
    }

    probe.reject_publication = false;
    probe.reject_live = true;
    try {
        auto rejected = finalize_selected_pipeline_operation_transaction(
            std::move(query),
            &selected_pipeline_finalizer_test_probe::validate_publication,
            &probe,
            additional,
            {},
            live_features,
            seed_metrics_claimed,
            &selected_pipeline_finalizer_test_probe::validate_live,
            &probe);
        static_cast<void>(rejected);
        return false;
    } catch (const selected_pipeline_finalizer_test_rejection&) {
    } catch (...) {
        return false;
    }
    if (probe.publication_calls != 2u || probe.accepted_publications != 1u ||
        probe.live_calls != 1u || !probe.order_valid || probe.observed_audit != expected ||
        !tokens_are_intact() || !claims_are_clear()) {
        return false;
    }

    probe.reject_live = false;
    std::pair<runtime_gpu_execution_environment_snapshot, runtime_gpu_cache_metrics_delta>
        accepted{};
    try {
        accepted = finalize_selected_pipeline_operation_transaction(
            std::move(query),
            &selected_pipeline_finalizer_test_probe::validate_publication,
            &probe,
            additional,
            {},
            live_features,
            seed_metrics_claimed,
            &selected_pipeline_finalizer_test_probe::validate_live,
            &probe);
    } catch (...) {
        return false;
    }
    const auto all_claims_set = [&receipts]() {
        return std::all_of(receipts.begin(), receipts.end(), [](const auto& receipt) {
            return receipt.metrics_claimed_;
        });
    };
    const bool tokens_consumed =
        query.audit().kind == runtime_pipeline_operation_kind::invalid &&
        std::all_of(additional.begin(), additional.end(), [](const auto& operation) {
            return operation.audit().kind == runtime_pipeline_operation_kind::invalid;
        });
    if (probe.publication_calls != 3u || probe.accepted_publications != 2u ||
        probe.live_calls != 2u || !probe.order_valid || !seed_metrics_claimed ||
        !all_claims_set() || !tokens_consumed || !accepted.first.is_internally_consistent() ||
        !accepted.second.is_valid() || accepted.second.audit() != expected ||
        probe.observed_environment != accepted.first || probe.observed_audit != expected) {
        return false;
    }

    runtime_pipeline_operation_token later{};
    try {
        later = make_query(12'700u);
    } catch (...) {
        return false;
    }
    const runtime_gpu_cache_metrics_audit zero_expected{
        .token_identity = later.audit().operation_identity,
    };
    std::pair<runtime_gpu_execution_environment_snapshot, runtime_gpu_cache_metrics_delta> zero{};
    try {
        zero = finalize_selected_pipeline_operation_transaction(
            std::move(later),
            &selected_pipeline_finalizer_test_probe::validate_publication,
            &probe,
            {},
            {},
            live_features,
            seed_metrics_claimed,
            &selected_pipeline_finalizer_test_probe::validate_live,
            &probe);
    } catch (...) {
        return false;
    }
    if (probe.publication_calls != 4u || probe.accepted_publications != 3u ||
        probe.live_calls != 3u || !probe.order_valid || probe.observed_audit != zero_expected ||
        !zero.second.is_valid() || zero.second.audit() != zero_expected ||
        later.audit().kind != runtime_pipeline_operation_kind::invalid || !all_claims_set()) {
        return false;
    }
    accepted.second.consume();
    zero.second.consume();
    return !accepted.second.is_valid() && !zero.second.is_valid();
}
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
std::pair<runtime_gpu_execution_environment_snapshot, runtime_gpu_cache_metrics_delta>
runtime::finalize_selected_pipeline_operation(
    runtime_pipeline_operation_token&& token,
    selected_pipeline_publication_validator validate_publication,
    void* publication_context,
    std::span<runtime_pipeline_operation_token> additional_operations) {
    std::scoped_lock lock(mu_);
    return finalize_selected_pipeline_operation_transaction(
        std::move(token), validate_publication, publication_context, additional_operations,
        {},
        features_, seed_metrics_claimed_,
        [](void*) {
            require_immutable_metal_policy_live(
                "selected pipeline final publication");
        },
        this);
}

std::pair<runtime_gpu_execution_environment_snapshot, runtime_gpu_cache_metrics_delta>
runtime::finalize_selected_query_sketch(
    device_query_sketch_authorization&& authorization,
    selected_pipeline_publication_validator validate_publication,
    void* publication_context,
    std::span<runtime_pipeline_operation_token> additional_operations) {
    return finalize_selected_query_sketches(
        std::span<device_query_sketch_authorization>{std::addressof(authorization), 1u},
        validate_publication,
        publication_context,
        additional_operations);
}

std::pair<runtime_gpu_execution_environment_snapshot, runtime_gpu_cache_metrics_delta>
runtime::finalize_selected_query_sketches(
    std::span<device_query_sketch_authorization> authorizations,
    selected_pipeline_publication_validator validate_publication,
    void* publication_context,
    std::span<runtime_pipeline_operation_token> additional_operations) {
    if (authorizations.empty()) {
        throw vulkan_runtime_error("selected query finalization has no issuing authority");
    }
    for (const auto& authorization : authorizations) {
        if (!authorization.is_valid() || authorization.runtime_ != this) {
            throw vulkan_runtime_error(
                "selected query finalization has a foreign or invalid issuing authority");
        }
    }
    std::scoped_lock lock(mu_);
    auto& primary = authorizations.front();
    auto result = finalize_selected_pipeline_operation_transaction(
        std::move(primary.pipeline_token_),
        validate_publication,
        publication_context,
        additional_operations,
        authorizations.subspan(1u),
        features_,
        seed_metrics_claimed_,
        [](void*) {
            require_immutable_metal_policy_live(
                "selected query final publication");
        },
        this);
    primary.clear();
    return result;
}
#endif
#endif

/**
 * @brief Chains anchors on the GPU and returns coordinates with the selected anchor trace.
 *
 * The coordinate reduction follows the chaining dispatch in the same submission.
 * Only the selected trace is copied back, along with its coordinate result.
 */
gpu_coordinate_chain_trace_result
runtime::run_chaining_coordinate_trace_roundtrip(const chaining_pipeline_key& key,
                                                 buffer& transfer_scratch,
                                                 buffer& anchors,
                                                 buffer& ranges,
                                                 buffer& best_scores,
                                                 buffer& predecessors,
                                                 std::span<const std::byte> anchors_bytes,
                                                 std::span<const std::byte> ranges_bytes,
                                                 std::span<const std::byte> best_scores_bytes,
                                                 std::span<const std::byte> predecessors_bytes,
                                                 std::uint32_t anchor_count,
                                                 std::uint32_t range_count,
                                                 std::uint64_t query_len,
                                                 std::int32_t max_dist_x,
                                                 std::int32_t max_dist_y,
                                                 std::int32_t bandwidth,
                                                 std::int32_t max_iter,
                                                 float gap_penalty,
                                                 float skip_penalty,
                                                 std::int32_t max_chain_skip,
                                                 std::int32_t min_chain_score,
                                                 std::int32_t min_seed_count,
                                                 std::uint32_t probe_limit) {
    std::lock_guard<std::mutex> guard(mu_);

    gpu_coordinate_chain_trace_result out{};
    if (!features_.available) {
        throw vulkan_runtime_error("run_chaining_coordinate_trace_roundtrip on unavailable device");
    }

    const std::uint32_t chain_groups = std::min(range_count, features_.max_workgroup_count_x);
    const VkPipeline pipeline = create_pipeline_locked(key).pipeline;
    const compute_pipeline coord_pipe =
        get_compute_pipeline_locked(shader_kind::coordinate_reduce, 128u, 1u);

    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch,
                  best_scores,
                  best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch,
                  predecessors,
                  predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    constexpr VkBufferUsageFlags coordinate_trace_buffer_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer visited = create_device_buffer(std::uint64_t{anchor_count} * sizeof(std::uint32_t),
                                          coordinate_trace_buffer_usage);
    buffer trace = create_device_buffer(std::uint64_t{anchor_count} * sizeof(std::uint32_t),
                                        coordinate_trace_buffer_usage);
    buffer result_buf =
        create_device_buffer(sizeof(gpu_coordinate_result_wire), coordinate_trace_buffer_usage);
    if (visited.mapped == nullptr || trace.mapped == nullptr || result_buf.mapped == nullptr) {
        throw vulkan_runtime_error("coordinate-only chaining requires host-visible trace buffers");
    }
    std::memset(visited.mapped, 0, static_cast<std::size_t>(visited.bytes));
    std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));

    const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {ranges.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t binding_index = 0; binding_index < 4; ++binding_index) {
        writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = descriptor_set_;
        writes[binding_index].dstBinding = binding_index;
        writes[binding_index].descriptorCount = 1;
        writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding_index].pBufferInfo = &chain_infos[binding_index];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    chaining_push_constants chain_push{};
    chain_push.anchor_count = anchor_count;
    chain_push.range_count = range_count;
    chain_push.max_dist_x = max_dist_x;
    chain_push.max_dist_y = max_dist_y;
    chain_push.bandwidth = bandwidth;
    chain_push.max_iter = max_iter;
    chain_push.gap_penalty = gap_penalty;
    chain_push.skip_penalty = skip_penalty;
    chain_push.max_chain_skip = max_chain_skip;

    reset_dispatch_descriptor_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(chain_coord)");

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

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_,
                            0,
                            1,
                            &descriptor_set_,
                            0,
                            nullptr);
    vkCmdPushConstants(command_buffer_,
                       pipeline_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(chain_push),
                       &chain_push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
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

    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
        {anchors.buffer_handle, 0, VK_WHOLE_SIZE},
        {best_scores.buffer_handle, 0, VK_WHOLE_SIZE},
        {predecessors.buffer_handle, 0, VK_WHOLE_SIZE},
        {visited.buffer_handle, 0, visited.bytes},
        {trace.buffer_handle, 0, trace.bytes},
        {result_buf.buffer_handle, 0, result_buf.bytes},
    }};
    gpu_coordinate_reduce_push coord_push{
        .anchor_count = anchor_count,
        .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
        .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
        .reserved = 0u,
    };
    record_compute_dispatch_locked(
        command_buffer_, coord_pipe, coord_infos, &coord_push, sizeof(coord_push), 1u);
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

    check_vk(vkEndCommandBuffer(command_buffer_), "end(chain_coord)");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "submit(chain_coord)");
    check_vk(vkWaitForFences(device_,
                             1,
                             &fence_,
                             VK_TRUE,
                             std::numeric_limits<std::uint64_t>::max()),
             "wait(chain_coord)");
    require_immutable_metal_policy_live("chaining coordinate operation completion");
    check_vk(vkResetFences(device_, 1, &fence_), "reset_fence(chain_coord)");

    std::memcpy(&out.result, result_buf.mapped, sizeof(out.result));
    if (out.result.is_valid != 0u) {
        if (out.result.seed_count < 0 ||
            static_cast<std::uint64_t>(out.result.seed_count) > anchor_count) {
            throw vulkan_runtime_error("coordinate-only chaining produced invalid trace length");
        }
        const auto trace_count = static_cast<std::size_t>(out.result.seed_count);
        out.trace.resize(trace_count);
        if (trace_count > 0) {
            std::memcpy(out.trace.data(), trace.mapped, trace_count * sizeof(std::uint32_t));
        }
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}



} // namespace bioseq::gpu::vulkan
