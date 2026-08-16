/**
 * @file pipeline_interpose.cppm
 * @brief Owns Metal pipeline interception, callbacks, and scoped audit state.
 */
module;

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <libassert/assert.hpp>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:metal_archive.pipeline_interpose;

import :metal_archive.archive_storage;
import :metal_archive.command_interpose;
import :receipts;
import :metal_archive.configuration;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

#if defined(__APPLE__)
/// @brief Reads the exact Metal function name bound by one pipeline descriptor.
[[nodiscard]] std::string descriptor_function_name(metal_pipeline_archive_interpose& state,
                                                   void* desc) {
    if (desc == nullptr || state.sel_compute_function == nullptr || state.sel_name == nullptr) {
        return {};
    }
    using get_object_function = void* (*)(void*, void*);
    void* function =
        objc_msg_cast<get_object_function>(state.objc.msg_send)(desc, state.sel_compute_function);
    void* name =
        function == nullptr
            ? nullptr
            : objc_msg_cast<get_object_function>(state.objc.msg_send)(function, state.sel_name);
    return bioseq_ns_string_utf8(state.objc, name, state.sel_utf8_string);
}

/// @brief Exact domain and code returned by a negative-control Metal call.
struct metal_error_identity {
    std::string domain;
    std::int64_t code = 0;
};

/// @brief Captures a borrowed NSError domain and code into owned storage.
[[nodiscard]] metal_error_identity
metal_error_identity_from_error(metal_pipeline_archive_interpose& state, void* error) {
    if (error == nullptr || state.sel_error_domain == nullptr || state.sel_error_code == nullptr) {
        return {};
    }
    using get_object_function = void* (*)(void*, void*);
    using code_function = std::intptr_t (*)(void*, void*);
    void* domain =
        objc_msg_cast<get_object_function>(state.objc.msg_send)(error, state.sel_error_domain);
    return metal_error_identity{
        .domain = bioseq_ns_string_utf8(state.objc, domain, state.sel_utf8_string),
        .code = static_cast<std::int64_t>(
            objc_msg_cast<code_function>(state.objc.msg_send)(error, state.sel_error_code)),
    };
}

constexpr unsigned long k_metal_pipeline_fail_on_binary_archive_miss = 1ul << 2u;

/// @brief Mutable state for one explicitly scoped Vulkan pipeline creation.
struct active_metal_archive_pipeline_scope {
    std::mutex mu;
    runtime_metal_archive_pipeline_audit audit{};
    bool required = false;
    bool active = false;
    bool finished = false;
    std::string failure;
    std::uint64_t outside_scope_interceptions_at_begin = 0u;
};

/// @brief Produces a nonzero monotonically increasing receipt identity.
[[nodiscard]] std::uint64_t next_nonzero_identity(std::atomic<std::uint64_t>& counter) noexcept {
    constexpr std::uint64_t exhausted = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t value = counter.load(std::memory_order_relaxed);
    while (value != 0u && value != exhausted) {
        if (counter.compare_exchange_weak(
                value, value + 1u, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return value;
        }
    }
    return 0u;
}

/// @brief Reads the registry identity from one intercepted Metal device.
[[nodiscard]] std::uint64_t metal_device_registry_id(metal_pipeline_archive_interpose& state,
                                                     void* device) noexcept {
    if (device == nullptr || state.sel_registry_id == nullptr)
        return 0u;
    using registry_function = unsigned long long (*)(void*, void*);
    return static_cast<std::uint64_t>(
        objc_msg_cast<registry_function>(state.objc.msg_send)(device, state.sel_registry_id));
}

/// @brief Releases one Objective-C object through the cached selector.
void release_objc_object(metal_pipeline_archive_interpose& state, void* object) noexcept {
    if (object == nullptr || state.sel_release == nullptr)
        return;
    using release_function = void (*)(void*, void*);
    objc_msg_cast<release_function>(state.objc.msg_send)(object, state.sel_release);
}

/// @brief Creates an empty binary-archive array for the negative control.
[[nodiscard]] void* make_empty_metal_archive_array(metal_pipeline_archive_interpose& state,
                                                   void* device) noexcept {
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    void* array_cls = state.objc.get_class("NSMutableArray");
    if (desc_cls == nullptr || array_cls == nullptr || device == nullptr) {
        return nullptr;
    }
    using new_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<new_function>(state.objc.msg_send)(
        desc_cls, state.objc.sel_register_name("new"));
    if (desc == nullptr)
        return nullptr;
    void* error = nullptr;
    using new_archive_function = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<new_archive_function>(state.objc.msg_send)(
        device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    release_objc_object(state, desc);
    if (archive == nullptr)
        return nullptr;

    using allocate_function = void* (*)(void*, void*);
    using initialize_capacity_function = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<allocate_function>(state.objc.msg_send)(
        array_cls, state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<initialize_capacity_function>(state.objc.msg_send)(
        allocated, state.objc.sel_register_name("initWithCapacity:"), 1ul);
    if (array == nullptr) {
        release_objc_object(state, archive);
        return nullptr;
    }
    using add_object_function = void (*)(void*, void*, void*);
    objc_msg_cast<add_object_function>(state.objc.msg_send)(
        array, state.objc.sel_register_name("addObject:"), archive);
    release_objc_object(state, archive);
    return array;
}

/// @brief Marks both the active scope and process interpose as failed.
void fail_active_metal_archive_scope(metal_pipeline_archive_interpose& state,
                                     active_metal_archive_pipeline_scope& scope,
                                     std::string reason) noexcept {
    scope.audit.sticky_failure = true;
    try {
        if (scope.failure.empty())
            scope.failure = reason;
    } catch (...) {
    }
    mark_metal_archive_sticky_failure(state, std::move(reason));
}

enum class metal_pipeline_intercept_verdict : std::uint8_t {
    outside_scope,
    enforced,
    rejected,
};

/// @brief Replaces archive descriptors and runs the required negative control.
[[nodiscard]] metal_pipeline_intercept_verdict prepare_scoped_metal_pipeline_call(
    void* self, void* desc, unsigned long options, bool asynchronous) noexcept {
    auto& state = metal_pipeline_archive_interpose_state();
    try {
        std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
        const std::shared_ptr<active_metal_archive_pipeline_scope> scope = state.active_scope;
        if (scope == nullptr) {
            state.outside_scope_interceptions.fetch_add(1u, std::memory_order_relaxed);
            active_guard.unlock();
            if (state.installed) {
                mark_metal_archive_sticky_failure(
                    state, "immutable Metal runtime intercepted an unscoped pipeline creation");
                return metal_pipeline_intercept_verdict::rejected;
            }
            return metal_pipeline_intercept_verdict::outside_scope;
        }
        std::unique_lock<std::mutex> scope_guard(scope->mu);
        active_guard.unlock();
        if (!scope->active || scope->finished) {
            state.outside_scope_interceptions.fetch_add(1u, std::memory_order_relaxed);
            scope_guard.unlock();
            if (state.installed) {
                mark_metal_archive_sticky_failure(state,
                                                  "immutable Metal runtime intercepted a pipeline "
                                                  "outside the live receipt scope");
                return metal_pipeline_intercept_verdict::rejected;
            }
            return metal_pipeline_intercept_verdict::outside_scope;
        }
        try {
            ++scope->audit.scoped_interceptions;
            if (scope->audit.scoped_interceptions != 1u) {
                fail_active_metal_archive_scope(state,
                                                *scope,
                                                "one Vulkan createInfo unexpectedly produced more "
                                                "than one Metal PSO interception");
                return metal_pipeline_intercept_verdict::rejected;
            }
            const std::string observed_entry = descriptor_function_name(state, desc);
            if (observed_entry.empty() || observed_entry != scope->audit.expected_entry_name) {
                fail_active_metal_archive_scope(
                    state,
                    *scope,
                    fmt::format("intercepted Metal function '{}' differs from expected '{}'",
                                observed_entry,
                                scope->audit.expected_entry_name));
                return metal_pipeline_intercept_verdict::rejected;
            }
            scope->audit.observed_entry_name = observed_entry;
            ++scope->audit.matched_descriptor_functions;
            if (state.sticky_failure.load(std::memory_order_acquire)) {
                fail_active_metal_archive_scope(state, *scope, metal_archive_failure_reason(state));
                return metal_pipeline_intercept_verdict::rejected;
            }
            if (self == nullptr || desc == nullptr || state.archive_array == nullptr ||
                state.original_new_pipeline_sync == nullptr) {
                fail_active_metal_archive_scope(
                    state,
                    *scope,
                    "Metal archive interpose lacks descriptor, archive set, or original sync IMP");
                return metal_pipeline_intercept_verdict::rejected;
            }
            const std::uint64_t intercepted_registry = metal_device_registry_id(state, self);
            if (intercepted_registry == 0u ||
                intercepted_registry != state.runtime_metal_registry_id) {
                fail_active_metal_archive_scope(
                    state,
                    *scope,
                    fmt::format("intercepted Metal registry id {} differs from sealed {}",
                                intercepted_registry,
                                state.runtime_metal_registry_id));
                return metal_pipeline_intercept_verdict::rejected;
            }
            if (scope->audit.intercepted_metal_registry_id != 0u &&
                scope->audit.intercepted_metal_registry_id != intercepted_registry) {
                fail_active_metal_archive_scope(
                    state, *scope, "one pipeline scope intercepted more than one Metal device");
                return metal_pipeline_intercept_verdict::rejected;
            }
            scope->audit.intercepted_metal_registry_id = intercepted_registry;
            auto& command_state = moltenvk_command_interpose_state();
            const command_interpose_device_binding command_binding =
                capture_command_interpose_device_binding(self);
            scope->audit.command_source_methods_present = command_binding.methods_present;
            scope->audit.command_source_wrapper_methods = command_binding.wrapper_methods;
            if (!command_state.cached_binding_validated ||
                command_state.loader_registry_id != scope->audit.command_loader_metal_registry_id ||
                command_binding.registry_id != intercepted_registry ||
                command_binding.registry_id != command_state.loader_registry_id ||
                command_binding.methods_present != 2u ||
                command_binding.wrapper_methods != command_binding.methods_present) {
                fail_active_metal_archive_scope(state,
                                                *scope,
                                                "intercepted Metal device does not retain both "
                                                "immutable command-source wrappers");
                return metal_pipeline_intercept_verdict::rejected;
            }

            using count_function = unsigned long (*)(void*, void*);
            const unsigned long exact_count = objc_msg_cast<count_function>(state.objc.msg_send)(
                state.archive_array, state.sel_count);
            const std::size_t configured_archive_count =
                state.configuration_image
                    ? state.configuration_image->configuration().archives.size()
                    : 0u;
            if (exact_count != configured_archive_count) {
                fail_active_metal_archive_scope(
                    state, *scope, "resident Metal archive object count differs from sealed set");
                return metal_pipeline_intercept_verdict::rejected;
            }

            void* empty_array = make_empty_metal_archive_array(state, self);
            if (empty_array == nullptr) {
                fail_active_metal_archive_scope(
                    state, *scope, "cannot construct same-device empty archive negative control");
                return metal_pipeline_intercept_verdict::rejected;
            }
            using set_object_function = void (*)(void*, void*, void*);
            objc_msg_cast<set_object_function>(state.objc.msg_send)(
                desc, state.sel_set_binary_archives, empty_array);
            ++scope->audit.same_descriptor_negative_controls;

            void* negative_reflection = nullptr;
            void* negative_error = nullptr;
            using sync_function = void* (*)(void*, void*, void*, unsigned long, void**, void**);
            ++scope->audit.original_sync_negative_calls;
            void* unexpected_pipeline =
                objc_msg_cast<sync_function>(state.original_new_pipeline_sync)(
                    self,
                    state.sel_new_pipeline_sync,
                    desc,
                    options | k_metal_pipeline_fail_on_binary_archive_miss,
                    &negative_reflection,
                    &negative_error);
            if (unexpected_pipeline != nullptr || negative_error == nullptr) {
                release_objc_object(state, unexpected_pipeline);
                release_objc_object(state, empty_array);
                fail_active_metal_archive_scope(
                    state,
                    *scope,
                    "empty archive negative control did not produce a binary-archive miss");
                return metal_pipeline_intercept_verdict::rejected;
            }
            const metal_error_identity negative_identity =
                metal_error_identity_from_error(state, negative_error);
            scope->audit.observed_negative_error_domain = negative_identity.domain;
            scope->audit.observed_negative_error_code = negative_identity.code;
            if (negative_identity.domain != scope->audit.expected_negative_error_domain ||
                negative_identity.code != scope->audit.expected_negative_error_code) {
                release_objc_object(state, empty_array);
                fail_active_metal_archive_scope(
                    state,
                    *scope,
                    fmt::format("empty archive negative control returned NSError ({}, {}) instead "
                                "of sealed ({}, {})",
                                negative_identity.domain,
                                negative_identity.code,
                                scope->audit.expected_negative_error_domain,
                                scope->audit.expected_negative_error_code));
                return metal_pipeline_intercept_verdict::rejected;
            }
            ++scope->audit.negative_control_misses;

            // This replacement is unconditional: a descriptor-provided array can
            // neither augment nor override the exact manifest-owned set.
            objc_msg_cast<set_object_function>(state.objc.msg_send)(
                desc, state.sel_set_binary_archives, state.archive_array);
            release_objc_object(state, empty_array);
            if (!state.process_qualification.is_internally_consistent() ||
                state.process_qualification != scope->audit.cache_qualification) {
                fail_active_metal_archive_scope(
                    state, *scope, "process Metal archive qualification changed before pipeline");
                return metal_pipeline_intercept_verdict::rejected;
            }
            scope->audit.pipeline_pre_identity_checks = 0u;
            ++scope->audit.descriptor_replacements;
            ++scope->audit.fail_on_miss_enforcements;
            if (asynchronous) {
                ++scope->audit.actual_async_calls;
            } else {
                ++scope->audit.actual_sync_calls;
            }
            std::fprintf(stderr,
                         "[bioseq-gpu-diag] pso-interpose: enforced immutable archives count=%lu "
                         "function=%s registry=%llu\n",
                         exact_count,
                         descriptor_function_name(state, desc).c_str(),
                         static_cast<unsigned long long>(intercepted_registry));
            return metal_pipeline_intercept_verdict::enforced;
        } catch (const std::exception& error) {
            fail_active_metal_archive_scope(state, *scope, error.what());
            return metal_pipeline_intercept_verdict::rejected;
        } catch (...) {
            fail_active_metal_archive_scope(
                state, *scope, "unknown immutable Metal archive interpose failure");
            return metal_pipeline_intercept_verdict::rejected;
        }
    } catch (...) {
        mark_metal_archive_sticky_failure_literal(
            state, "immutable Metal archive interpose failed before scoped handling");
        return metal_pipeline_intercept_verdict::rejected;
    }
}

/// @brief Intercepts asynchronous Metal compute-pipeline creation inside an active scope.
/// @param[in] self Objective-C Metal device receiver.
/// @param[in] cmd Objective-C selector.
/// @param[in] desc Compute pipeline descriptor.
/// @param[in] options Original Metal pipeline options.
/// @param[in] completion_handler Original completion block.
extern "C" void bioseq_metal_new_compute_pipeline_state_async(
    void* self, void* cmd, void* desc, unsigned long options, void* completion_handler) {
    auto& state = metal_pipeline_archive_interpose_state();
    using async_function = void (*)(void*, void*, void*, unsigned long, void*);
    const metal_pipeline_intercept_verdict verdict =
        prepare_scoped_metal_pipeline_call(self, desc, options, true);
    if (verdict == metal_pipeline_intercept_verdict::outside_scope) {
        objc_msg_cast<async_function>(state.original_new_pipeline_async)(
            self, cmd, desc, options, completion_handler);
        return;
    }
    if (verdict == metal_pipeline_intercept_verdict::rejected) {
        invoke_metal_pipeline_completion(completion_handler, nullptr, nullptr, nullptr);
        return;
    }
    objc_msg_cast<async_function>(state.original_new_pipeline_async)(
        self,
        cmd,
        desc,
        options | k_metal_pipeline_fail_on_binary_archive_miss,
        completion_handler);
}

/// @brief Intercepts synchronous Metal compute-pipeline creation inside an active scope.
/// @param[in] self Objective-C Metal device receiver.
/// @param[in] cmd Objective-C selector.
/// @param[in] desc Compute pipeline descriptor.
/// @param[in] options Original Metal pipeline options.
/// @param[out] reflection Receives Metal reflection output.
/// @param[out] error Receives the original Metal error object.
/// @return Original pipeline object, or `nullptr` on fail-closed rejection.
extern "C" void* bioseq_metal_new_compute_pipeline_state_sync(
    void* self, void* cmd, void* desc, unsigned long options, void** reflection, void** error) {
    auto& state = metal_pipeline_archive_interpose_state();
    using sync_function = void* (*)(void*, void*, void*, unsigned long, void**, void**);
    const metal_pipeline_intercept_verdict verdict =
        prepare_scoped_metal_pipeline_call(self, desc, options, false);
    if (verdict == metal_pipeline_intercept_verdict::outside_scope) {
        return objc_msg_cast<sync_function>(state.original_new_pipeline_sync)(
            self, cmd, desc, options, reflection, error);
    }
    if (verdict == metal_pipeline_intercept_verdict::rejected) {
        return nullptr;
    }
    return objc_msg_cast<sync_function>(state.original_new_pipeline_sync)(
        self, cmd, desc, options | k_metal_pipeline_fail_on_binary_archive_miss, reflection, error);
}

/// @brief Installs pipeline wrappers for the selected Vulkan device.
void install_metal_pipeline_archive_interpose(
    runtime_vulkan_device_identity selected_vulkan_device) noexcept {
    auto& state = metal_pipeline_archive_interpose_state();
    void* async_method = nullptr;
    void* sync_method = nullptr;
    const auto rollback_install = [&state]() noexcept {
        rollback_metal_pipeline_archive_interpose_install(state);
    };
    try {
        if (state.sticky_failure.load(std::memory_order_acquire)) {
            return;
        }
        if (state.installed) {
            return;
        }
        const auto configuration = runtime_metal_archive_configuration_snapshot();
        if (!configuration) {
            std::fprintf(stderr,
                         "[bioseq-gpu-diag] pso-interpose: no sealed runtime archive "
                         "configuration; interpose inactive\n");
            return;
        }
        if (!configuration->is_valid()) {
            mark_metal_archive_sticky_failure(
                state, "installed runtime Metal archive configuration is invalid");
            return;
        }

        void* objc_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
        void* metal_handle =
            ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
        if (objc_handle == nullptr || metal_handle == nullptr) {
            mark_metal_archive_sticky_failure(state,
                                              "Objective-C or Metal runtime cannot be loaded");
            return;
        }

        objc_runtime_functions objc{};
        objc.msg_send = ::dlsym(objc_handle, "objc_msgSend");
        objc.get_class =
            reinterpret_cast<decltype(objc.get_class)>(::dlsym(objc_handle, "objc_getClass"));
        objc.sel_register_name = reinterpret_cast<decltype(objc.sel_register_name)>(
            ::dlsym(objc_handle, "sel_registerName"));
        objc.object_get_class = reinterpret_cast<decltype(objc.object_get_class)>(
            ::dlsym(objc_handle, "object_getClass"));
        objc.class_get_superclass = reinterpret_cast<decltype(objc.class_get_superclass)>(
            ::dlsym(objc_handle, "class_getSuperclass"));
        objc.class_get_instance_method = reinterpret_cast<decltype(objc.class_get_instance_method)>(
            ::dlsym(objc_handle, "class_getInstanceMethod"));
        objc.method_get_implementation = reinterpret_cast<decltype(objc.method_get_implementation)>(
            ::dlsym(objc_handle, "method_getImplementation"));
        objc.method_set_implementation = reinterpret_cast<decltype(objc.method_set_implementation)>(
            ::dlsym(objc_handle, "method_setImplementation"));
        if (objc.msg_send == nullptr || objc.get_class == nullptr ||
            objc.sel_register_name == nullptr || objc.object_get_class == nullptr ||
            objc.class_get_superclass == nullptr || objc.class_get_instance_method == nullptr ||
            objc.method_get_implementation == nullptr ||
            objc.method_set_implementation == nullptr) {
            mark_metal_archive_sticky_failure(
                state, "Objective-C runtime lacks required interpose symbols");
            return;
        }

        using create_device_function = void* (*)();
        auto create_device = reinterpret_cast<create_device_function>(
            ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
        if (create_device == nullptr) {
            mark_metal_archive_sticky_failure(state, "MTLCreateSystemDefaultDevice is unavailable");
            return;
        }
        void* device = create_device();
        if (device == nullptr) {
            mark_metal_archive_sticky_failure(state, "MTLCreateSystemDefaultDevice returned null");
            return;
        }
        void* release_selector = objc.sel_register_name("release");
        scoped_objc_release device_owner{&objc, release_selector, device};

        void* cls = objc.object_get_class(device);
        void* sel_async = objc.sel_register_name(
            "newComputePipelineStateWithDescriptor:options:completionHandler:");
        void* sel_sync = objc.sel_register_name(
            "newComputePipelineStateWithDescriptor:options:reflection:error:");
        for (void* cursor = cls;
             cursor != nullptr && (async_method == nullptr || sync_method == nullptr);
             cursor = objc.class_get_superclass(cursor)) {
            if (async_method == nullptr) {
                async_method = objc.class_get_instance_method(cursor, sel_async);
            }
            if (sync_method == nullptr) {
                sync_method = objc.class_get_instance_method(cursor, sel_sync);
            }
        }
        if (sync_method == nullptr) {
            mark_metal_archive_sticky_failure(
                state, "original synchronous Metal pipeline IMP is required for negative control");
            return;
        }
        std::fprintf(stderr,
                     "[bioseq-gpu-diag] pso-interpose: methods found async=%d sync=%d\n",
                     async_method != nullptr ? 1 : 0,
                     sync_method != nullptr ? 1 : 0);

        state.objc = objc;
        state.configuration_image = configuration;
        const runtime_metal_archive_configuration& installed_configuration =
            state.configuration_image->configuration();
        state.configured_identity =
            make_runtime_metal_archive_set_identity(installed_configuration.archives);
        state.configuration_validated = state.configured_identity.is_valid();
        if (!state.configuration_validated) {
            rollback_install();
            mark_metal_archive_sticky_failure(
                state, "runtime Metal archive configuration lost install-time validation");
            return;
        }
        state.sel_new_pipeline_sync = sel_sync;
        state.sel_set_binary_archives = objc.sel_register_name("setBinaryArchives:");
        state.sel_compute_function = objc.sel_register_name("computeFunction");
        state.sel_name = objc.sel_register_name("name");
        state.sel_utf8_string = objc.sel_register_name("UTF8String");
        state.sel_error_domain = objc.sel_register_name("domain");
        state.sel_error_code = objc.sel_register_name("code");
        state.sel_count = objc.sel_register_name("count");
        state.sel_registry_id = objc.sel_register_name("registryID");
        state.sel_release = release_selector;
        state.sel_max_total_threads = objc.sel_register_name("maxTotalThreadsPerThreadgroup");
        state.sel_threadgroup_multiple =
            objc.sel_register_name("threadGroupSizeIsMultipleOfThreadExecutionWidth");

        const bool selected_uuid_missing =
            std::all_of(selected_vulkan_device.device_uuid.begin(),
                        selected_vulkan_device.device_uuid.end(),
                        [](std::uint8_t byte) { return byte == 0u; });
        const bool configured_uuid_present =
            std::any_of(installed_configuration.vulkan_device_uuid.begin(),
                        installed_configuration.vulkan_device_uuid.end(),
                        [](std::uint8_t byte) { return byte != 0u; });
        if (!selected_vulkan_device.is_valid() || selected_uuid_missing ||
            (configured_uuid_present &&
             installed_configuration.vulkan_device_uuid != selected_vulkan_device.device_uuid)) {
            rollback_install();
            mark_metal_archive_sticky_failure(
                state, "selected Vulkan device UUID differs from optional sealed expectation");
            return;
        }
        state.runtime_vulkan_device_uuid = selected_vulkan_device.device_uuid;
        state.runtime_vulkan_device_identity = selected_vulkan_device;

        state.loader_registry_id = metal_device_registry_id(state, device);
        if (state.loader_registry_id == 0u ||
            state.loader_registry_id != installed_configuration.helper_metal_registry_id ||
            (installed_configuration.metal_device_registry_id != 0u &&
             state.loader_registry_id != installed_configuration.metal_device_registry_id)) {
            rollback_install();
            mark_metal_archive_sticky_failure(
                state,
                fmt::format("Metal archive loader registry id {} differs from helper-qualified {} "
                            "or optional sealed {}",
                            state.loader_registry_id,
                            installed_configuration.helper_metal_registry_id,
                            installed_configuration.metal_device_registry_id));
            return;
        }
        state.runtime_metal_registry_id = state.loader_registry_id;
        if (!load_exact_metal_binary_archives(state, device)) {
            rollback_install();
            return;
        }

        state.interposed_new_pipeline_async_method = async_method;
        state.interposed_new_pipeline_sync_method = sync_method;
        if (async_method != nullptr) {
            state.original_new_pipeline_async = objc.method_set_implementation(
                async_method,
                reinterpret_cast<void*>(&bioseq_metal_new_compute_pipeline_state_async));
        }
        if (sync_method != nullptr) {
            state.original_new_pipeline_sync = objc.method_set_implementation(
                sync_method,
                reinterpret_cast<void*>(&bioseq_metal_new_compute_pipeline_state_sync));
        }
        state.installed = state.original_new_pipeline_sync != nullptr &&
                          (async_method == nullptr || state.original_new_pipeline_async != nullptr);
        std::fprintf(
            stderr, "[bioseq-gpu-diag] pso-interpose: installed=%d\n", state.installed ? 1 : 0);
        if (!state.installed) {
            rollback_install();
            mark_metal_archive_sticky_failure(
                state, "Metal pipeline method interposition did not install exactly");
            return;
        }
        state.loader_device = device_owner.release();
        try {
            bioseq_log<logging_level::debug>(
                "[vulkan_runtime] installed immutable Metal archive interpose manifest={} "
                "registry={} archives={} async={} sync={}",
                installed_configuration.seed_image->manifest().manifest_checksum,
                state.loader_registry_id,
                state.loaded_identity.archive_count,
                state.original_new_pipeline_async != nullptr,
                state.original_new_pipeline_sync != nullptr);
        } catch (...) {
            // Diagnostics cannot invalidate a fully committed interpose.
        }
    } catch (const std::bad_alloc&) {
        rollback_install();
        mark_metal_archive_sticky_failure_literal(
            state, "immutable Metal archive install allocation failed");
    } catch (...) {
        rollback_install();
        mark_metal_archive_sticky_failure_literal(
            state, "immutable Metal archive install threw unexpectedly");
    }
}
#endif

} // namespace bioseq::gpu::vulkan
