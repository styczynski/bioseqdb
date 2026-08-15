/**
 * @file pipeline_cache.cppm
 * @brief Owns compute-pipeline construction, cache admission, and descriptor binding.
 */
module;

#include <cstdint>
#include <libassert/assert.hpp>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.vulkan.runtime:pipeline_cache;

import :frontend_types;
import :support;
import :metal_archive;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.instrumentation;
import std;

namespace bioseq::gpu::vulkan {

void runtime::owned_pipeline_handles::reset() noexcept {
    if (device != VK_NULL_HANDLE) {
        if (pipeline != VK_NULL_HANDLE && vkDestroyPipeline != nullptr) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (module != VK_NULL_HANDLE && vkDestroyShaderModule != nullptr) {
            vkDestroyShaderModule(device, module, nullptr);
        }
        if (descriptor_pool != VK_NULL_HANDLE && vkDestroyDescriptorPool != nullptr) {
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE && vkDestroyPipelineLayout != nullptr) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (set_layout != VK_NULL_HANDLE && vkDestroyDescriptorSetLayout != nullptr) {
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        }
    }
    device = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
    module = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
}

std::uint64_t runtime::pipeline_cache_metadata_bytes_locked() const noexcept {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t owned = saturating_mul(static_cast<std::uint64_t>(pipeline_cache_.capacity()),
                                         sizeof(pipeline_entry));
    owned = saturating_add(owned,
                           saturating_mul(static_cast<std::uint64_t>(compute_pipelines_.capacity()),
                                          sizeof(owned_compute_pipeline)));
    for (const pipeline_entry& entry : pipeline_cache_) {
        owned = saturating_add(owned, entry.entry_name.capacity());
        if (entry.metal_archive_receipt) {
            owned = saturating_add(owned, entry.metal_archive_receipt->owned_capacity_bytes());
        }
    }
    for (const owned_compute_pipeline& entry : compute_pipelines_) {
        owned = saturating_add(owned, entry.entry_name.capacity());
        if (entry.metal_archive_receipt) {
            owned = saturating_add(owned, entry.metal_archive_receipt->owned_capacity_bytes());
        }
    }
    return owned;
}

void runtime::admit_pipeline_cache_growth_locked(std::size_t normal_capacity,
                                                 std::size_t compute_pipeline_capacity) {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    std::uint64_t projected = pipeline_cache_metadata_bytes_locked();
    if (normal_capacity > pipeline_cache_.capacity()) {
        projected = saturating_add(
            projected,
            saturating_mul(static_cast<std::uint64_t>(normal_capacity - pipeline_cache_.capacity()),
                           sizeof(pipeline_entry)));
    }
    if (compute_pipeline_capacity > compute_pipelines_.capacity()) {
        projected =
            saturating_add(projected,
                           saturating_mul(static_cast<std::uint64_t>(compute_pipeline_capacity -
                                                                     compute_pipelines_.capacity()),
                                          sizeof(owned_compute_pipeline)));
    }
    // Covers one pending entry, receipt allocation/control metadata, all five
    // bounded audit strings, and vector/string allocator headroom.
    projected = saturating_add(projected,
                               sizeof(runtime_metal_archive_pipeline_receipt) +
                                   sizeof(runtime_metal_archive_pipeline_audit) + 16u * 1024u);
    const std::uint64_t peak =
        saturating_mul(projected, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (peak == (std::numeric_limits<std::uint64_t>::max)()) {
        throw vulkan_runtime_error("pipeline cache metadata admission estimate saturated");
    }
    bioseq::parallelism::memory_admission_failure failure{};
    if (!pipeline_cache_metadata_permit_) {
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "pipeline cache metadata admission denied before Vulkan creation: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                " requested=",
                failure.requested_bytes));
        }
        pipeline_cache_metadata_permit_ = std::move(*permit);
    } else if (!pipeline_cache_metadata_permit_.grow_to(peak, &failure)) {
        throw vulkan_runtime_error(shader_cache_message(
            "pipeline cache metadata growth denied before Vulkan creation: reason=",
            bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
            " requested=",
            failure.requested_bytes));
    }
    try {
        pipeline_cache_.reserve(normal_capacity);
        compute_pipelines_.reserve(compute_pipeline_capacity);
    } catch (...) {
        reconcile_pipeline_cache_metadata_locked();
        throw;
    }
}

void runtime::reconcile_pipeline_cache_metadata_locked() noexcept {
    if (!pipeline_cache_metadata_permit_)
        return;
    const std::uint64_t retained = pipeline_cache_metadata_bytes_locked();
    if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
        retained > pipeline_cache_metadata_permit_.bytes()) {
        // The conservative preallocation must make this unreachable. Retain
        // the larger permit and fail closed at the next live policy gate.
#if defined(__APPLE__)
        mark_metal_archive_sticky_failure_literal(
            metal_pipeline_archive_interpose_state(),
            "pipeline cache metadata exceeded admitted ownership");
#endif
        return;
    }
    pipeline_cache_metadata_permit_.shrink_to(retained);
    if (retained == 0u)
        pipeline_cache_metadata_permit_.release();
}

runtime::chaining_pipeline runtime::create_pipeline_locked(const chaining_pipeline_key& key) {
    const bool provider_used = should_use_shader_provider_locked();
    const bool archive_receipt_required =
        provider_used || is_runtime_metal_archive_configuration_installed();
    for (const auto& entry : pipeline_cache_) {
        if (entry.key == key) {
            if (archive_receipt_required) {
                require_immutable_metal_policy_live("cached chaining pipeline retrieval");
            }
            if (archive_receipt_required && (entry.metal_archive_receipt == nullptr ||
                                             !entry.metal_archive_receipt->is_valid())) {
                throw vulkan_runtime_error(
                    "cached chaining pipeline lacks its accepted immutable Metal archive receipt");
            }
            return chaining_pipeline{
                .pipeline = entry.handles.pipeline,
                .shader_key = make_shader_key(
                    shader_kind::chain_tiled_dp_u64, key.tile_size, key.local_size, 0u),
                .metal_archive_receipt = entry.metal_archive_receipt.get(),
            };
        }
    }

    if (key.local_size == 0 || key.local_size > features_.max_workgroup_invocations) {
        throw vulkan_runtime_error(
            fmt::format("chaining local_size {} exceeds maxComputeWorkGroupInvocations {}",
                        key.local_size,
                        features_.max_workgroup_invocations));
    }

    const std::size_t next_capacity =
        pipeline_cache_.size() < pipeline_cache_.capacity()
            ? pipeline_cache_.capacity()
            : std::max<std::size_t>(
                  pipeline_cache_.size() + 1u,
                  pipeline_cache_.capacity() == 0u ? 1u : pipeline_cache_.capacity() * 2u);
    admit_pipeline_cache_growth_locked(next_capacity, compute_pipelines_.capacity());
    pipeline_cache_admission_scope metadata_scope{*this};

    pipeline_entry candidate{};
    candidate.key = key;
    candidate.handles = owned_pipeline_handles{device_};
    VkShaderModule& module = candidate.handles.module;
    std::string& entry_name = candidate.entry_name;
    std::uint32_t provider_wg_x = 0;
    std::uint32_t provider_wg_y = 0;
    std::uint32_t provider_wg_z = 0;
    const shader_variant_key shader_key =
        make_shader_key(shader_kind::chain_tiled_dp_u64, key.tile_size, key.local_size, 0u);
    if (should_use_shader_provider_locked()) {
        if (!metallib_provider_capable_) {
            throw vulkan_runtime_error(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        runtime_shader_provider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            throw vulkan_runtime_error(fmt::format(
                "shader provider failed for {}: {}", cache_file_stem(shader_key), loaded.error()));
        }
        module = loaded->module;
        entry_name = loaded->entry_name.empty() ? std::string{"main"} : loaded->entry_name;
        provider_wg_x = loaded->wg_x;
        provider_wg_y = loaded->wg_y;
        provider_wg_z = loaded->wg_z;
        set_workgroup_size_mvk_(module, loaded->wg_x, loaded->wg_y, loaded->wg_z);
    } else {
        std::vector<std::uint32_t> spirv;
        try {
            spirv = compile_shader_spirv(shader_compile_request{
                shader_kind::chain_tiled_dp_u64, key.tile_size, key.local_size});
        } catch (const std::exception& e) {
            throw vulkan_runtime_error(fmt::format("shader compile failed: {}", e.what()));
        }

        // Validate the direct-anchor ABI in the emitted SPIR-V before trusting the
        // pipeline. A stride/offset mismatch would silently corrupt the DP read of
        // every anchor; reject rather than hide it behind a conversion buffer.
        const spirv_layout_scan scan = scan_spirv_layout(spirv);
        if (!scan.has_array_stride(24)) {
            throw vulkan_runtime_error("SPIR-V Anchors array stride != 24");
        }
        if (!scan.has_struct_offsets({0u, 8u, 16u, 20u})) {
            throw vulkan_runtime_error("SPIR-V AnchorWire member offsets != {0,8,16,20}");
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv.data();
        check_vk(vkCreateShaderModule(device_, &module_info, nullptr, &module),
                 "vkCreateShaderModule");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = entry_name.c_str();

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout_;

    VkPipeline& pipeline = candidate.handles.pipeline;
    metal_archive_pipeline_create_scope archive_scope{
        archive_receipt_required, features_.device_uuid, shader_key, entry_name};
    if (!archive_scope.is_ready_for_pipeline_call()) {
        auto rejected = archive_scope.finish(VK_ERROR_INITIALIZATION_FAILED);
        throw vulkan_runtime_error(fmt::format(
            "immutable Metal archive scope rejected chaining pipeline before creation: {}",
            rejected ? "missing required receipt" : rejected.error()));
    }
    const auto create_begin = std::chrono::steady_clock::now();
    const VkResult pr =
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    const auto create_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - create_begin)
                                    .count();
    candidate.pipeline_create_ns = std::max<std::uint64_t>(
        1u, create_elapsed > 0 ? static_cast<std::uint64_t>(create_elapsed) : 1u);
    auto archive_verdict = archive_scope.finish(pr);
    if (!archive_verdict) {
        throw vulkan_runtime_error(
            fmt::format("immutable Metal archive receipt failed for chaining pipeline: {}",
                        archive_verdict.error()));
    }
    if (pr != VK_SUCCESS) {
        throw vulkan_runtime_error(fmt::format(
            "vkCreateComputePipelines: VkResult {} provider={} entry={} wg={},{},{} vk={} mvk={}",
            static_cast<int>(pr),
            provider_used ? "metallib" : "spirv",
            entry_name,
            provider_wg_x,
            provider_wg_y,
            provider_wg_z,
            symbol_image(reinterpret_cast<const void*>(vkCreateComputePipelines)),
            symbol_image(reinterpret_cast<const void*>(set_workgroup_size_mvk_))));
    }

    if (archive_verdict->has_value()) {
        runtime_metal_archive_pipeline_audit& audit = **archive_verdict;
        audit.api_version = features_.api_version;
        audit.driver_version = features_.driver_version;
        audit.vendor_id = features_.vendor_id;
        audit.device_id = features_.device_id;
        audit.pipeline_cache_uuid = features_.pipeline_cache_uuid;
        audit.pipeline_create_ns = candidate.pipeline_create_ns;
        candidate.metal_archive_receipt.reset(
            new runtime_metal_archive_pipeline_receipt(std::move(audit)));
    }
    pipeline_cache_.push_back(std::move(candidate));
    try {
        bioseq_log<logging_level::debug>(
            "[vulkan_runtime] built chaining pipeline tile={} local={}",
            key.tile_size,
            key.local_size);
    } catch (...) {
    }
    const pipeline_entry& retained = pipeline_cache_.back();
    return chaining_pipeline{
        .pipeline = retained.handles.pipeline,
        .shader_key = shader_key,
        .metal_archive_receipt = retained.metal_archive_receipt.get(),
    };
}

// ---------------------------------------------------------------------------
// Upload / download helpers (called under mu_, command buffer already open)
// ---------------------------------------------------------------------------

runtime::compute_pipeline runtime::get_compute_pipeline_locked(shader_kind kind,
                                                               std::uint32_t local_size,
                                                               std::uint32_t max_span) {
    const bool provider_used = should_use_shader_provider_locked();
    const bool archive_receipt_required =
        provider_used || is_runtime_metal_archive_configuration_installed();
    for (auto& entry : compute_pipelines_) {
        if (entry.kind == kind && entry.local_size == local_size && entry.max_span == max_span) {
            if (archive_receipt_required) {
                require_immutable_metal_policy_live("cached compute pipeline retrieval");
            }
            if (archive_receipt_required && (entry.metal_archive_receipt == nullptr ||
                                             !entry.metal_archive_receipt->is_valid())) {
                throw vulkan_runtime_error(
                    "cached compute pipeline lacks its accepted immutable Metal archive receipt");
            }
            return entry.view();
        }
    }
    const shader_variant_key shader_key = make_shader_key(kind, 384u, local_size, max_span);
    shader_compile_request geometry_request{};
    geometry_request.kind = kind;
    geometry_request.tile_size = 384u;
    geometry_request.local_size = local_size;
    geometry_request.max_span = max_span;
    const shader_workgroup_size workgroup = expected_shader_workgroup_size(geometry_request);
    const std::uint64_t workgroup_invocations = static_cast<std::uint64_t>(workgroup.x) *
                                                static_cast<std::uint64_t>(workgroup.y) *
                                                static_cast<std::uint64_t>(workgroup.z);
    if (workgroup.x == 0 || workgroup.y == 0 || workgroup.z == 0 ||
        workgroup.x > features_.max_workgroup_size_x ||
        workgroup.y > features_.max_workgroup_size_y ||
        workgroup.z > features_.max_workgroup_size_z ||
        workgroup_invocations > features_.max_workgroup_invocations) {
        throw vulkan_runtime_error(
            fmt::format("seed lookup workgroup {}x{}x{} (key local_size {}) exceeds device "
                        "limits maxComputeWorkGroupSize={}x{}x{} or "
                        "maxComputeWorkGroupInvocations={}",
                        workgroup.x,
                        workgroup.y,
                        workgroup.z,
                        local_size,
                        features_.max_workgroup_size_x,
                        features_.max_workgroup_size_y,
                        features_.max_workgroup_size_z,
                        features_.max_workgroup_invocations));
    }

    const descriptor_layout_kind layout_kind = descriptor_layout_for(kind);
    const std::uint32_t binding_count = descriptor_binding_count(layout_kind);
    const std::uint32_t push_size = push_constant_size(kind);

    const std::size_t next_capacity =
        compute_pipelines_.size() < compute_pipelines_.capacity()
            ? compute_pipelines_.capacity()
            : std::max<std::size_t>(
                  compute_pipelines_.size() + 1u,
                  compute_pipelines_.capacity() == 0u ? 1u : compute_pipelines_.capacity() * 2u);
    admit_pipeline_cache_growth_locked(pipeline_cache_.capacity(), next_capacity);
    pipeline_cache_admission_scope metadata_scope{*this};

    owned_compute_pipeline entry;
    entry.handles = owned_pipeline_handles{device_};
    entry.kind = kind;
    entry.local_size = local_size;
    entry.max_span = max_span;
    entry.binding_count = binding_count;
    entry.push_size = push_size;

    std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
    for (std::uint32_t b = 0; b < binding_count; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = binding_count;
    set_layout_info.pBindings = bindings.data();
    check_vk(
        vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &entry.handles.set_layout),
        "vkCreateDescriptorSetLayout(compute)");

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_size;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &entry.handles.set_layout;
    layout_info.pushConstantRangeCount = push_size > 0 ? 1u : 0u;
    layout_info.pPushConstantRanges = push_size > 0 ? &push_range : nullptr;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &entry.handles.pipeline_layout),
             "vkCreatePipelineLayout(compute)");

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = binding_count;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &entry.handles.descriptor_pool),
             "vkCreateDescriptorPool(compute)");

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = entry.handles.descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &entry.handles.set_layout;
    check_vk(vkAllocateDescriptorSets(device_, &set_alloc, &entry.descriptor_set),
             "vkAllocateDescriptorSets(compute)");

    if (provider_used) {
        if (!metallib_provider_capable_) {
            throw vulkan_runtime_error(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        runtime_shader_provider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            throw vulkan_runtime_error(fmt::format(
                "shader provider failed for {}: {}", cache_file_stem(shader_key), loaded.error()));
        }
        entry.handles.module = loaded->module;
        entry.entry_name = loaded->entry_name.empty() ? std::string{"main"} : loaded->entry_name;
        set_workgroup_size_mvk_(entry.handles.module, loaded->wg_x, loaded->wg_y, loaded->wg_z);
    } else {
        std::vector<std::uint32_t> spirv;
        try {
            shader_compile_request request{};
            request.kind = kind;
            request.tile_size = 384;
            request.local_size = local_size;
            request.max_span = max_span;
            spirv = compile_shader_spirv(request);
        } catch (const std::exception& compile_error) {
            throw vulkan_runtime_error(
                fmt::format("seed lookup shader compile failed: {}", compile_error.what()));
        }

        // Pin the SSBO wire ABI: a sort-range-stride mismatch would silently
        // corrupt direct sort_range_wire reads.
        const spirv_layout_scan scan = scan_spirv_layout(spirv);
        if (kind == shader_kind::seed_lookup_anchors &&
            (!scan.has_binding(0) || !scan.has_binding(1) || !scan.has_binding(2) ||
             !scan.has_binding(3) || !scan.has_binding(4) || !scan.has_binding(5) ||
             !scan.has_array_stride(24) || !scan.has_array_stride(8) ||
             !scan.has_struct_offsets({0u, 8u, 16u, 20u}))) {
            throw vulkan_runtime_error("SPIR-V seed lookup anchor/key ABI mismatch");
        }
        if (kind == shader_kind::sort_anchors_bitonic && !scan.has_array_stride(8)) {
            throw vulkan_runtime_error("SPIR-V SortRange array stride != 8");
        }
        if (kind == shader_kind::generate_units && (!scan.has_binding(0) || !scan.has_binding(1) ||
                                                    !scan.has_array_stride(max_span / 8u))) {
            throw vulkan_runtime_error("SPIR-V generate_units layout mismatch");
        }
        if (kind == shader_kind::align_extend_banded_affine &&
            (!scan.has_binding(0) || !scan.has_binding(1) || !scan.has_binding(2) ||
             !scan.has_binding(3) || !scan.has_array_stride(4) || !scan.has_array_stride(16) ||
             !scan.has_array_stride(32))) {
            throw vulkan_runtime_error("SPIR-V alignment extension layout mismatch");
        }
        if (kind == shader_kind::align_extend_simple_cigar &&
            (!scan.has_binding(0) || !scan.has_binding(1) || !scan.has_binding(2) ||
             !scan.has_binding(3) || !scan.has_binding(4) || !scan.has_binding(5) ||
             !scan.has_array_stride(4) || !scan.has_array_stride(40) ||
             !scan.has_array_stride(48))) {
            throw vulkan_runtime_error("SPIR-V alignment CIGAR extension layout mismatch");
        }
        if ((kind == shader_kind::sneaky_snake_rank_batch || is_gatekeeper_rank_batch_kind(kind)) &&
            (!scan.has_binding_array_stride(0u, 0u, 4u) ||
             !scan.has_binding_array_stride(0u, 1u, 4u) ||
             !scan.has_binding_array_stride(0u, 2u, 20u) ||
             !scan.has_binding_struct_offsets(
                 0u, 2u, std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}) ||
             !scan.has_binding_array_stride(0u, 3u, 16u) ||
             !scan.has_binding_struct_offsets(
                 0u, 3u, std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}) ||
             !scan.has_push_constant_offsets(std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}))) {
            throw vulkan_runtime_error("SPIR-V rank-prefilter batch layout mismatch");
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv.data();
        check_vk(vkCreateShaderModule(device_, &module_info, nullptr, &entry.handles.module),
                 "vkCreateShaderModule(compute)");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = entry.handles.module;
    stage.pName = entry.entry_name.c_str();
    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = entry.handles.pipeline_layout;
    metal_archive_pipeline_create_scope archive_scope{
        archive_receipt_required, features_.device_uuid, shader_key, entry.entry_name};
    if (!archive_scope.is_ready_for_pipeline_call()) {
        auto rejected = archive_scope.finish(VK_ERROR_INITIALIZATION_FAILED);
        throw vulkan_runtime_error(fmt::format(
            "immutable Metal archive scope rejected compute pipeline before creation: {}",
            rejected ? "missing required receipt" : rejected.error()));
    }
    const auto create_begin = std::chrono::steady_clock::now();
    const VkResult pr = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &entry.handles.pipeline);
    const auto create_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - create_begin)
                                    .count();
    entry.pipeline_create_ns = std::max<std::uint64_t>(
        1u, create_elapsed > 0 ? static_cast<std::uint64_t>(create_elapsed) : 1u);
    auto archive_verdict = archive_scope.finish(pr);
    if (!archive_verdict) {
        throw vulkan_runtime_error(
            fmt::format("immutable Metal archive receipt failed for compute pipeline: {}",
                        archive_verdict.error()));
    }
    if (pr != VK_SUCCESS) {
        throw vulkan_runtime_error(
            fmt::format("vkCreateComputePipelines(compute): VkResult {}", static_cast<int>(pr)));
    }
    if (archive_verdict->has_value()) {
        runtime_metal_archive_pipeline_audit& audit = **archive_verdict;
        audit.api_version = features_.api_version;
        audit.driver_version = features_.driver_version;
        audit.vendor_id = features_.vendor_id;
        audit.device_id = features_.device_id;
        audit.pipeline_cache_uuid = features_.pipeline_cache_uuid;
        audit.pipeline_create_ns = entry.pipeline_create_ns;
        entry.metal_archive_receipt.reset(
            new runtime_metal_archive_pipeline_receipt(std::move(audit)));
    }

    compute_pipelines_.push_back(std::move(entry));
    try {
        bioseq_log<logging_level::debug>(
            "[vulkan_runtime] built compute pipeline kind={} local={} max_span={}",
            static_cast<int>(kind),
            local_size,
            max_span);
    } catch (...) {
    }
    return compute_pipelines_.back().view();
}

void runtime::bind_compute_pipeline_descriptors_locked(const compute_pipeline& entry,
                                                       std::span<const VkBuffer> buffers) {
    DEBUG_ASSERT(buffers.size() == entry.binding_count,
                 "seed lookup descriptor binding count mismatch");
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t binding_index = 0; binding_index < buffers.size(); ++binding_index) {
        infos[binding_index] = VkDescriptorBufferInfo{buffers[binding_index], 0, VK_WHOLE_SIZE};
        writes[binding_index] = VkWriteDescriptorSet{};
        writes[binding_index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding_index].dstSet = entry.descriptor_set;
        writes[binding_index].dstBinding = static_cast<std::uint32_t>(binding_index);
        writes[binding_index].descriptorCount = 1;
        writes[binding_index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding_index].pBufferInfo = &infos[binding_index];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

} // namespace bioseq::gpu::vulkan
