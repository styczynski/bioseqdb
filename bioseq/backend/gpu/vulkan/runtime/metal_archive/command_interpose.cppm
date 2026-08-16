/**
 * @file command_interpose.cppm
 * @brief Owns MoltenVK command-library interposition and Objective-C dispatch state.
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

export module bioseq.gpu.vulkan.runtime:metal_archive.command_interpose;

import :receipts;
import :metal_archive.configuration;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

#if defined(__APPLE__)
/// @brief Dynamically resolved Objective-C runtime calls used by both interposes.
struct objc_runtime_functions {
    void* msg_send = nullptr;
    void* (*get_class)(const char*) = nullptr;
    void* (*sel_register_name)(const char*) = nullptr;
    void* (*object_get_class)(void*) = nullptr;
    void* (*class_get_superclass)(void*) = nullptr;
    void* (*class_get_instance_method)(void*, void*) = nullptr;
    void* (*method_get_implementation)(void*) = nullptr;
    void* (*method_set_implementation)(void*, void*) = nullptr;
};

/// @brief Process state for replacing MoltenVK command-library source compilation.
struct moltenvk_command_library_interpose {
    objc_runtime_functions objc{};
    std::vector<std::byte> metallib;
    void* original_new_library_with_source = nullptr;
    void* original_new_library_with_source_async = nullptr;
    void* sel_utf8_string = nullptr;
    void* sel_new_library_with_data_error = nullptr;
    void* sel_new_library_with_source = nullptr;
    void* sel_new_library_with_source_async = nullptr;
    std::uint64_t loader_registry_id = 0u;
    std::uint64_t cached_methods_present = 0u;
    std::uint64_t cached_wrapper_methods = 0u;
    bool cached_binding_validated = false;
    bool installed = false;
    bool immutable_fail_closed = false;
    std::atomic<bool> sticky_failure{false};
    std::atomic<std::uint64_t> recognized_source_requests{0u};
    std::atomic<std::uint64_t> source_fallback_calls{0u};
};

/// @brief Returns the process-owned command-library interpose state.
/// @return Singleton retained through process teardown.
moltenvk_command_library_interpose& moltenvk_command_interpose_state() noexcept {
    static moltenvk_command_library_interpose state;
    return state;
}

/// @brief Casts the dynamically resolved Objective-C message entry point.
/// @tparam p_function_type Exact ABI-compatible function-pointer type.
/// @param[in] function Raw Objective-C runtime entry point.
/// @return Reinterpreted typed function pointer.
template <class p_function_type>
[[nodiscard]] p_function_type objc_msg_cast(void* function) noexcept {
    return reinterpret_cast<p_function_type>(function);
}

/// @brief Releases one retained Objective-C object unless ownership is transferred.
class scoped_objc_release {
  public:
    /// @brief Creates an empty Objective-C release guard.
    scoped_objc_release() = default;

    /// @brief Adopts one retained Objective-C object.
    /// @param[in] objc Resolved Objective-C runtime functions.
    /// @param[in] release_selector Objective-C `release` selector.
    /// @param[in] object Retained object to release unless transferred.
    scoped_objc_release(objc_runtime_functions* objc, void* release_selector, void* object) noexcept
        : objc_(objc), release_selector_(release_selector), object_(object) {}

    /// @brief Prevents copying retained-object ownership.
    scoped_objc_release(const scoped_objc_release&) = delete;

    /// @brief Prevents copy assignment of retained-object ownership.
    scoped_objc_release& operator=(const scoped_objc_release&) = delete;

    /// @brief Transfers the retained object and runtime dispatch state.
    /// @param[in,out] other Guard whose ownership is consumed.
    scoped_objc_release(scoped_objc_release&& other) noexcept
        : objc_(std::exchange(other.objc_, nullptr)),
          release_selector_(std::exchange(other.release_selector_, nullptr)),
          object_(std::exchange(other.object_, nullptr)) {}

    /// @brief Prevents replacing an established release guard by assignment.
    scoped_objc_release& operator=(scoped_objc_release&&) = delete;

    /// @brief Releases the retained object if ownership remains.
    ~scoped_objc_release() {
        reset();
    }

    /// @brief Returns the retained object without transferring ownership.
    /// @return Borrowed Objective-C object pointer.
    [[nodiscard]] void* get() const noexcept {
        return object_;
    }

    /// @brief Transfers the retained object out of this guard.
    /// @return Owned Objective-C object pointer.
    [[nodiscard]] void* release() noexcept {
        return std::exchange(object_, nullptr);
    }

    /// @brief Releases the retained object through the resolved Objective-C runtime.
    void reset() noexcept {
        if (objc_ != nullptr && objc_->msg_send != nullptr && release_selector_ != nullptr &&
            object_ != nullptr) {
            using release_function = void (*)(void*, void*);
            objc_msg_cast<release_function>(objc_->msg_send)(object_, release_selector_);
        }
        object_ = nullptr;
    }

  private:
    objc_runtime_functions* objc_ = nullptr;
    void* release_selector_ = nullptr;
    void* object_ = nullptr;
};

/// @brief Intercepts synchronous MoltenVK source-library creation.
/// @param[in] self Objective-C receiver.
/// @param[in] cmd Objective-C selector.
/// @param[in] source Metal source string.
/// @param[in] options Metal compile options.
/// @param[out] error Receives the original Metal error object.
/// @return Original or sealed replacement Metal library object.
extern "C" void* bioseq_moltenvk_new_library_with_source(
    void* self, void* cmd, void* source, void* options, void** error) {
    auto& state = moltenvk_command_interpose_state();
    using utf8_function = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<utf8_function>(state.objc.msg_send)(source, state.sel_utf8_string);
    }
    const bool is_moltenvk_command_source = text != nullptr &&
                                            std::strstr(text, "cmdCopyBufferBytes") != nullptr &&
                                            std::strstr(text, "vtxCmdBlitImage") != nullptr;
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] newLibraryWithSource intercepted: text=%d len=%zu "
                 "is_cmd_source=%d metallib_empty=%d\n",
                 text != nullptr ? 1 : 0,
                 text != nullptr ? std::strlen(text) : 0u,
                 is_moltenvk_command_source ? 1 : 0,
                 state.metallib.empty() ? 1 : 0);
    if (state.immutable_fail_closed) {
        state.recognized_source_requests.fetch_add(1u, std::memory_order_relaxed);
        state.sticky_failure.store(true, std::memory_order_release);
        std::fprintf(stderr,
                     "[bioseq-gpu-diag] cmd-interpose: immutable mode rejects every unsealed Metal "
                     "source-library request\n");
        return nullptr;
    }
    if (is_moltenvk_command_source && !state.metallib.empty()) {
        using library_data_function = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(state.metallib.data(),
                                                        state.metallib.size(),
                                                        nullptr,
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            if (void* library = objc_msg_cast<library_data_function>(state.objc.msg_send)(
                    self,
                    state.sel_new_library_with_data_error,
                    static_cast<void*>(mtl_data),
                    error);
                library != nullptr) {
                dispatch_release(mtl_data);
                std::fprintf(stderr,
                             "[bioseq-gpu-diag] cmd-interpose: returned cached command library "
                             "(newLibraryWithData ok)\n");
                return library;
            }
            dispatch_release(mtl_data);
            std::fprintf(stderr,
                         "[bioseq-gpu-diag] cmd-interpose: newLibraryWithData FAILED, falling "
                         "through to source compile\n");
        }
    }

    std::fprintf(stderr,
                 "[bioseq-gpu-diag] cmd-interpose: FALL-THROUGH to real newLibraryWithSource (XPC "
                 "compile)\n");
    if (is_moltenvk_command_source) {
        state.source_fallback_calls.fetch_add(1u, std::memory_order_relaxed);
    }
    using source_function = void* (*)(void*, void*, void*, void*, void**);
    return objc_msg_cast<source_function>(state.original_new_library_with_source)(
        self, cmd, source, options, error);
}

/// @brief ABI-compatible Objective-C block literal used by asynchronous wrappers.
struct objc_block_literal {
    void* isa = nullptr;
    int flags = 0;
    int reserved = 0;
    void* invoke = nullptr;
    void* descriptor = nullptr;
};

/// @brief Completes a wrapped Metal library callback through the original block.
/// @param[in] completion Borrowed Objective-C completion block.
/// @param[in] library Borrowed library result forwarded to the block.
/// @param[in] error Borrowed error result forwarded to the block.
void invoke_metal_library_completion(void* completion, void* library, void* error) noexcept {
    if (completion == nullptr) {
        return;
    }
    auto* block = static_cast<objc_block_literal*>(completion);
    if (block->invoke == nullptr) {
        return;
    }
    using invoke_function = void (*)(void*, void*, void*);
    objc_msg_cast<invoke_function>(block->invoke)(completion, library, error);
}

/// @brief Completes a wrapped Metal pipeline callback through the original block.
/// @param[in] completion Borrowed Objective-C completion block.
/// @param[in] pipeline Borrowed pipeline result forwarded to the block.
/// @param[in] reflection Borrowed reflection result forwarded to the block.
/// @param[in] error Borrowed error result forwarded to the block.
void invoke_metal_pipeline_completion(void* completion,
                                      void* pipeline,
                                      void* reflection,
                                      void* error) noexcept {
    if (completion == nullptr)
        return;
    auto* block = static_cast<objc_block_literal*>(completion);
    if (block->invoke == nullptr)
        return;
    using invoke_function = void (*)(void*, void*, void*, void*);
    objc_msg_cast<invoke_function>(block->invoke)(completion, pipeline, reflection, error);
}

/// @brief Intercepts asynchronous MoltenVK source-library creation.
/// @param[in] self Objective-C receiver.
/// @param[in] cmd Objective-C selector.
/// @param[in] source Metal source string.
/// @param[in] options Metal compile options.
/// @param[in] completion Original completion block.
extern "C" void bioseq_moltenvk_new_library_with_source_async(
    void* self, void* cmd, void* source, void* options, void* completion) {
    auto& state = moltenvk_command_interpose_state();
    using utf8_function = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<utf8_function>(state.objc.msg_send)(source, state.sel_utf8_string);
    }
    const bool is_moltenvk_command_source = text != nullptr &&
                                            std::strstr(text, "cmdCopyBufferBytes") != nullptr &&
                                            std::strstr(text, "vtxCmdBlitImage") != nullptr;
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] newLibraryWithSource async intercepted: text=%d len=%zu "
                 "is_cmd_source=%d metallib_empty=%d\n",
                 text != nullptr ? 1 : 0,
                 text != nullptr ? std::strlen(text) : 0u,
                 is_moltenvk_command_source ? 1 : 0,
                 state.metallib.empty() ? 1 : 0);
    if (state.immutable_fail_closed) {
        state.recognized_source_requests.fetch_add(1u, std::memory_order_relaxed);
        state.sticky_failure.store(true, std::memory_order_release);
        std::fprintf(stderr,
                     "[bioseq-gpu-diag] cmd-interpose: immutable mode rejects every unsealed Metal "
                     "source-library request async\n");
        invoke_metal_library_completion(completion, nullptr, nullptr);
        return;
    }
    if (is_moltenvk_command_source && !state.metallib.empty()) {
        using library_data_function = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(state.metallib.data(),
                                                        state.metallib.size(),
                                                        nullptr,
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            void* error = nullptr;
            void* library = objc_msg_cast<library_data_function>(state.objc.msg_send)(
                self, state.sel_new_library_with_data_error, static_cast<void*>(mtl_data), &error);
            dispatch_release(mtl_data);
            if (library != nullptr) {
                std::fprintf(stderr,
                             "[bioseq-gpu-diag] cmd-interpose: returned cached command library "
                             "async (newLibraryWithData ok)\n");
                invoke_metal_library_completion(completion, library, nullptr);
                return;
            }
            std::fprintf(stderr,
                         "[bioseq-gpu-diag] cmd-interpose: async newLibraryWithData FAILED, "
                         "falling through to source compile\n");
        }
    }

    std::fprintf(stderr,
                 "[bioseq-gpu-diag] cmd-interpose: async FALL-THROUGH to real newLibraryWithSource "
                 "(XPC compile)\n");
    if (is_moltenvk_command_source) {
        state.source_fallback_calls.fetch_add(1u, std::memory_order_relaxed);
    }
    if (state.original_new_library_with_source_async == nullptr) {
        std::fprintf(stderr,
                     "[bioseq-gpu-diag] cmd-interpose: async original IMP missing; completing with "
                     "null library\n");
        invoke_metal_library_completion(completion, nullptr, nullptr);
        return;
    }
    using source_async_function = void (*)(void*, void*, void*, void*, void*);
    objc_msg_cast<source_async_function>(state.original_new_library_with_source_async)(
        self, cmd, source, options, completion);
}

/// @brief Reads one metallib candidate into an owned byte vector.
/// @param[in] path Null-terminated candidate path.
/// @param[out] out Vector replaced with the complete nonempty file contents.
/// @return `true` when the full file was read.
[[nodiscard]] bool read_file_bytes(const char* path, std::vector<std::byte>* out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0) {
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(out->data()), size);
    return static_cast<bool>(input);
}

/// @brief Installs fail-closed command-library wrappers before MoltenVK device setup.
void install_moltenvk_command_library_interpose() noexcept {
    auto& state = moltenvk_command_interpose_state();
    void* installed_method = nullptr;
    void* installed_method_async = nullptr;
    // Restores any methods replaced during this partial installation attempt.
    const auto rollback_swizzles = [&state, &installed_method, &installed_method_async]() noexcept {
        if (state.objc.method_set_implementation != nullptr) {
            if (installed_method != nullptr && state.original_new_library_with_source != nullptr) {
                (void)state.objc.method_set_implementation(installed_method,
                                                           state.original_new_library_with_source);
            }
            if (installed_method_async != nullptr &&
                state.original_new_library_with_source_async != nullptr) {
                (void)state.objc.method_set_implementation(
                    installed_method_async, state.original_new_library_with_source_async);
            }
        }
        state.installed = false;
        state.cached_binding_validated = false;
        state.cached_methods_present = 0u;
        state.cached_wrapper_methods = 0u;
        state.loader_registry_id = 0u;
        state.original_new_library_with_source = nullptr;
        state.original_new_library_with_source_async = nullptr;
        state.sel_utf8_string = nullptr;
        state.sel_new_library_with_data_error = nullptr;
        state.sel_new_library_with_source = nullptr;
        state.sel_new_library_with_source_async = nullptr;
        std::vector<std::byte>{}.swap(state.metallib);
        state.objc = {};
    };
    try {
        const bool immutable_fail_closed = is_runtime_metal_archive_configuration_installed();
        const bool previously_immutable_fail_closed = state.immutable_fail_closed;
        // Latches immutable-mode installation failures without throwing.
        const auto reject_immutable_install = [&state, immutable_fail_closed]() noexcept {
            if (immutable_fail_closed) {
                state.sticky_failure.store(true, std::memory_order_release);
            }
        };
        if (state.installed) {
            std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: already installed\n");
            if (immutable_fail_closed && !previously_immutable_fail_closed) {
                state.sticky_failure.store(true, std::memory_order_release);
            }
            return;
        }
        state.immutable_fail_closed = immutable_fail_closed;
        const char* path = std::getenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB");
        std::fprintf(stderr,
                     "[bioseq-gpu-diag] cmd-interpose: BIOSEQ_MOLTENVK_COMMAND_METALLIB=%s\n",
                     path != nullptr ? path : "(null)");
        if (!immutable_fail_closed && (path == nullptr || *path == '\0')) {
            return;
        }
        std::vector<std::byte> metallib;
        if (!immutable_fail_closed && !read_file_bytes(path, &metallib)) {
            std::fprintf(
                stderr, "[bioseq-gpu-diag] cmd-interpose: read_file_bytes FAILED for %s\n", path);
            bioseq_log<logging_level::debug>(
                "[vulkan_runtime] MoltenVK command metallib unavailable at {}", path);
            return;
        }
        if (immutable_fail_closed) {
            std::fprintf(stderr,
                         "[bioseq-gpu-diag] cmd-interpose: immutable mode ignores unsealed "
                         "placeholder command metallib and fails closed on recognized source\n");
        } else {
            std::fprintf(stderr,
                         "[bioseq-gpu-diag] cmd-interpose: read command metallib ok bytes=%zu\n",
                         metallib.size());
        }

        void* objc_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
        void* metal_handle =
            ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
        if (objc_handle == nullptr || metal_handle == nullptr) {
            reject_immutable_install();
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
            reject_immutable_install();
            return;
        }

        using create_device_function = void* (*)();
        auto create_device = reinterpret_cast<create_device_function>(
            ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
        if (create_device == nullptr) {
            reject_immutable_install();
            return;
        }
        void* device = create_device();
        if (device == nullptr) {
            std::fprintf(
                stderr,
                "[bioseq-gpu-diag] cmd-interpose: MTLCreateSystemDefaultDevice returned NULL\n");
            reject_immutable_install();
            return;
        }
        void* release_selector = objc.sel_register_name("release");
        scoped_objc_release device_owner{&objc, release_selector, device};

        void* sel_source = objc.sel_register_name("newLibraryWithSource:options:error:");
        void* sel_source_async =
            objc.sel_register_name("newLibraryWithSource:options:completionHandler:");
        state.sel_new_library_with_source = sel_source;
        state.sel_new_library_with_source_async = sel_source_async;
        state.sel_utf8_string = objc.sel_register_name("UTF8String");
        state.sel_new_library_with_data_error = objc.sel_register_name("newLibraryWithData:error:");
        using registry_function = unsigned long long (*)(void*, void*);
        state.loader_registry_id = static_cast<std::uint64_t>(objc_msg_cast<registry_function>(
            objc.msg_send)(device, objc.sel_register_name("registryID")));
        if (immutable_fail_closed && state.loader_registry_id == 0u) {
            rollback_swizzles();
            reject_immutable_install();
            return;
        }
        void* cls = objc.object_get_class(device);
        while (cls != nullptr &&
               (installed_method == nullptr || installed_method_async == nullptr)) {
            if (installed_method == nullptr) {
                installed_method = objc.class_get_instance_method(cls, sel_source);
            }
            if (installed_method_async == nullptr) {
                installed_method_async = objc.class_get_instance_method(cls, sel_source_async);
            }
            cls = objc.class_get_superclass(cls);
        }
        if (installed_method == nullptr && installed_method_async == nullptr) {
            std::fprintf(stderr,
                         "[bioseq-gpu-diag] cmd-interpose: newLibraryWithSource methods NOT FOUND "
                         "on device class\n");
            rollback_swizzles();
            reject_immutable_install();
            return;
        }
        if (immutable_fail_closed &&
            (installed_method == nullptr || installed_method_async == nullptr)) {
            rollback_swizzles();
            reject_immutable_install();
            return;
        }

        state.objc = objc;
        state.metallib = std::move(metallib);
        if (installed_method != nullptr) {
            state.original_new_library_with_source = objc.method_set_implementation(
                installed_method,
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source));
        }
        if (installed_method_async != nullptr) {
            state.original_new_library_with_source_async = objc.method_set_implementation(
                installed_method_async,
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source_async));
        }
        const bool any_installed = state.original_new_library_with_source != nullptr ||
                                   state.original_new_library_with_source_async != nullptr;
        const bool every_present_method_installed =
            (installed_method == nullptr || state.original_new_library_with_source != nullptr) &&
            (installed_method_async == nullptr ||
             state.original_new_library_with_source_async != nullptr);
        state.installed =
            any_installed &&
            (!immutable_fail_closed ||
             (every_present_method_installed && state.original_new_library_with_source != nullptr &&
              state.original_new_library_with_source_async != nullptr));
        state.cached_methods_present =
            static_cast<std::uint64_t>(installed_method != nullptr) +
            static_cast<std::uint64_t>(installed_method_async != nullptr);
        state.cached_wrapper_methods = 0u;
        if (installed_method != nullptr &&
            objc.method_get_implementation(installed_method) ==
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source)) {
            ++state.cached_wrapper_methods;
        }
        if (installed_method_async != nullptr &&
            objc.method_get_implementation(installed_method_async) ==
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source_async)) {
            ++state.cached_wrapper_methods;
        }
        state.cached_binding_validated =
            state.installed && state.loader_registry_id != 0u &&
            state.cached_methods_present == 2u &&
            state.cached_wrapper_methods == state.cached_methods_present;
        if (immutable_fail_closed && !state.cached_binding_validated) {
            rollback_swizzles();
            reject_immutable_install();
            return;
        }
        if (!state.installed) {
            rollback_swizzles();
            reject_immutable_install();
            return;
        }
        std::fprintf(stderr,
                     "[bioseq-gpu-diag] cmd-interpose: swizzle installed=%d sync_original=%p "
                     "async_original=%p\n",
                     state.installed ? 1 : 0,
                     state.original_new_library_with_source,
                     state.original_new_library_with_source_async);
        if (state.installed) {
            try {
                bioseq_log<logging_level::debug>(
                    "[vulkan_runtime] installed MoltenVK command-source interpose "
                    "immutable_fail_closed={} path={}",
                    immutable_fail_closed,
                    path != nullptr ? path : "(none)");
            } catch (...) {
            }
        }
    } catch (...) {
        rollback_swizzles();
        state.sticky_failure.store(true, std::memory_order_release);
    }
}

/// @brief Cached registry identity and wrapper coverage for one Metal device.
struct command_interpose_device_binding {
    std::uint64_t registry_id = 0u;
    std::uint64_t methods_present = 0u;
    std::uint64_t wrapper_methods = 0u;
};

/// @brief Rare PSO-creation receipt: bind the receipt to the exact intercepted device
/// and its live method table. Submit/wait gates consume cached scalar facts.
/// This two-selector reflection runs only while creating a pipeline receipt.
/// @param[in] device Objective-C Metal device to inspect.
/// @return Intercepted device registry identity and available method mask.
[[nodiscard]] command_interpose_device_binding
capture_command_interpose_device_binding(void* device) noexcept {
    auto& state = moltenvk_command_interpose_state();
    command_interpose_device_binding result{};
    if (device == nullptr || !state.installed || state.objc.msg_send == nullptr ||
        state.objc.object_get_class == nullptr || state.objc.class_get_instance_method == nullptr ||
        state.objc.class_get_superclass == nullptr ||
        state.objc.method_get_implementation == nullptr) {
        return result;
    }
    using registry_function = unsigned long long (*)(void*, void*);
    result.registry_id = static_cast<std::uint64_t>(objc_msg_cast<registry_function>(
        state.objc.msg_send)(device, state.objc.sel_register_name("registryID")));
    const std::array selectors{
        std::pair{state.sel_new_library_with_source,
                  reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source)},
        std::pair{state.sel_new_library_with_source_async,
                  reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source_async)},
    };
    void* root = state.objc.object_get_class(device);
    for (const auto& [selector, expected_imp] : selectors) {
        void* method = nullptr;
        for (void* cursor = root; cursor != nullptr && method == nullptr;
             cursor = state.objc.class_get_superclass(cursor)) {
            method = state.objc.class_get_instance_method(cursor, selector);
        }
        if (method == nullptr)
            continue;
        ++result.methods_present;
        if (state.objc.method_get_implementation(method) == expected_imp) {
            ++result.wrapper_methods;
        }
    }
    return result;
}
#endif

} // namespace bioseq::gpu::vulkan
