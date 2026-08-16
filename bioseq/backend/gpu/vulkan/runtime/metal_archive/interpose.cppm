/**
 * @file interpose.cppm
 * @brief Owns the single Apple Objective-C and MoltenVK archive interposition boundary.
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

export module bioseq.gpu.vulkan.runtime:metal_archive.interpose;

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
moltenvk_command_library_interpose& moltenvk_command_interpose_state() noexcept {
    static moltenvk_command_library_interpose state;
    return state;
}

/// @brief Casts the dynamically resolved Objective-C message entry point.
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

struct command_interpose_device_binding {
    std::uint64_t registry_id = 0u;
    std::uint64_t methods_present = 0u;
    std::uint64_t wrapper_methods = 0u;
};

/// @brief Rare PSO-creation receipt: bind the receipt to the exact intercepted device
/// and its live method table. Submit/wait gates consume cached scalar facts.
/// this two-selector reflection runs only while creating a pipeline receipt.
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

struct active_metal_archive_pipeline_scope;

/// @brief Process state for immutable Metal archives and pipeline interception.
struct metal_pipeline_archive_interpose {
    objc_runtime_functions objc{};
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration_image;
    std::array<std::uint8_t, VK_UUID_SIZE> runtime_vulkan_device_uuid{};
    runtime_vulkan_device_identity runtime_vulkan_device_identity{};
    std::uint64_t runtime_metal_registry_id = 0u;
    runtime_metal_archive_set_identity configured_identity{};
    runtime_metal_archive_set_identity loaded_identity{};
    // Declared before retained Objective-C archive ownership so teardown
    // releases the NSArray/archives before returning their residency budget.
    bioseq::parallelism::memory_permit archive_residency_permit;
    std::uint64_t archive_residency_peak_bytes = 0u;
    std::uint64_t archive_residency_retained_bytes = 0u;
    void* loader_device = nullptr;
    void* archive_array = nullptr;
    runtime_metal_archive_process_qualification process_qualification{};
    void* interposed_new_pipeline_async_method = nullptr;
    void* interposed_new_pipeline_sync_method = nullptr;
    void* original_new_pipeline_async = nullptr;
    void* original_new_pipeline_sync = nullptr;
    void* sel_new_pipeline_sync = nullptr;
    void* sel_set_binary_archives = nullptr;
    void* sel_compute_function = nullptr;
    void* sel_name = nullptr;
    void* sel_utf8_string = nullptr;
    void* sel_error_domain = nullptr;
    void* sel_error_code = nullptr;
    void* sel_count = nullptr;
    void* sel_registry_id = nullptr;
    void* sel_release = nullptr;
    void* sel_max_total_threads = nullptr;
    void* sel_threadgroup_multiple = nullptr;
    std::uint64_t loader_registry_id = 0u;
    std::uint64_t loaded_pre_post_identity_checks = 0u;
    bool configuration_validated = false;
    bool installed = false;
    std::atomic<bool> sticky_failure{false};
    std::mutex failure_mu;
    std::string failure_reason;
    std::atomic<std::uint64_t> next_scope_identity{1u};
    std::atomic<std::uint64_t> next_receipt_identity{1u};
    std::mutex active_scope_mu;
    std::shared_ptr<active_metal_archive_pipeline_scope> active_scope;
    std::atomic<std::uint64_t> outside_scope_interceptions{0u};

    /// @brief Releases the retained archive array and loader device in ownership order.
    ~metal_pipeline_archive_interpose() {
        if (sel_release != nullptr && objc.msg_send != nullptr) {
            using release_function = void (*)(void*, void*);
            if (archive_array != nullptr) {
                objc_msg_cast<release_function>(objc.msg_send)(archive_array, sel_release);
                archive_array = nullptr;
            }
            if (loader_device != nullptr) {
                objc_msg_cast<release_function>(objc.msg_send)(loader_device, sel_release);
                loader_device = nullptr;
            }
        }
    }
};

/// @brief Returns the process-owned pipeline interpose state.
metal_pipeline_archive_interpose& metal_pipeline_archive_interpose_state() noexcept {
    static metal_pipeline_archive_interpose state;
    return state;
}

/// @brief Releases resident archives while retaining sticky failure state.
void rollback_metal_archive_loaded_state(metal_pipeline_archive_interpose& state) noexcept {
    if (state.archive_array != nullptr && state.sel_release != nullptr &&
        state.objc.msg_send != nullptr) {
        using release_function = void (*)(void*, void*);
        objc_msg_cast<release_function>(state.objc.msg_send)(state.archive_array,
                                                             state.sel_release);
        state.archive_array = nullptr;
    }
    state.archive_residency_permit.release();
    state.archive_residency_peak_bytes = 0u;
    state.archive_residency_retained_bytes = 0u;
    state.loaded_identity = {};
    state.loaded_pre_post_identity_checks = 0u;
    state.process_qualification = {};
}

/// @brief Restores original Objective-C methods and clears a partial installation.
void rollback_metal_pipeline_archive_interpose_install(
    metal_pipeline_archive_interpose& state) noexcept {
    if (state.objc.method_set_implementation != nullptr) {
        if (state.interposed_new_pipeline_async_method != nullptr &&
            state.original_new_pipeline_async != nullptr) {
            (void)state.objc.method_set_implementation(state.interposed_new_pipeline_async_method,
                                                       state.original_new_pipeline_async);
        }
        if (state.interposed_new_pipeline_sync_method != nullptr &&
            state.original_new_pipeline_sync != nullptr) {
            (void)state.objc.method_set_implementation(state.interposed_new_pipeline_sync_method,
                                                       state.original_new_pipeline_sync);
        }
    }
    state.installed = false;
    state.interposed_new_pipeline_async_method = nullptr;
    state.interposed_new_pipeline_sync_method = nullptr;
    state.original_new_pipeline_async = nullptr;
    state.original_new_pipeline_sync = nullptr;
    rollback_metal_archive_loaded_state(state);
    if (state.loader_device != nullptr && state.sel_release != nullptr &&
        state.objc.msg_send != nullptr) {
        using release_function = void (*)(void*, void*);
        objc_msg_cast<release_function>(state.objc.msg_send)(state.loader_device,
                                                             state.sel_release);
    }
    state.loader_device = nullptr;
    state.configuration_image.reset();
    state.configured_identity = {};
    state.configuration_validated = false;
    state.runtime_vulkan_device_uuid = {};
    state.runtime_vulkan_device_identity = {};
    state.runtime_metal_registry_id = 0u;
    state.loader_registry_id = 0u;
    state.sel_new_pipeline_sync = nullptr;
    state.sel_set_binary_archives = nullptr;
    state.sel_compute_function = nullptr;
    state.sel_name = nullptr;
    state.sel_utf8_string = nullptr;
    state.sel_error_domain = nullptr;
    state.sel_error_code = nullptr;
    state.sel_count = nullptr;
    state.sel_registry_id = nullptr;
    state.sel_release = nullptr;
    state.sel_max_total_threads = nullptr;
    state.sel_threadgroup_multiple = nullptr;
    state.objc = {};
}

/// @brief Copies a borrowed NSString into process-owned UTF-8 storage.
[[nodiscard]] std::string
bioseq_ns_string_utf8(objc_runtime_functions& objc, void* ns_string, void* sel_utf8_string) {
    if (ns_string == nullptr || sel_utf8_string == nullptr) {
        return {};
    }
    using utf8_function = const char* (*)(void*, void*);
    const char* text = objc_msg_cast<utf8_function>(objc.msg_send)(ns_string, sel_utf8_string);
    return text == nullptr ? std::string{} : std::string(text);
}

/// @brief Creates an autoreleased NSString from one owned UTF-8 view.
[[nodiscard]] void* bioseq_make_ns_string(objc_runtime_functions& objc, std::string_view value) {
    void* ns_string_cls = objc.get_class("NSString");
    if (ns_string_cls == nullptr) {
        return nullptr;
    }
    std::string owned(value);
    using string_function = void* (*)(void*, void*, const char*);
    return objc_msg_cast<string_function>(objc.msg_send)(
        ns_string_cls, objc.sel_register_name("stringWithUTF8String:"), owned.c_str());
}

/// @brief Creates a file URL for one canonical private archive path.
[[nodiscard]] void* bioseq_make_file_url(objc_runtime_functions& objc,
                                         const std::filesystem::path& path) {
    void* url_cls = objc.get_class("NSURL");
    void* ns_path = bioseq_make_ns_string(objc, path.string());
    if (url_cls == nullptr || ns_path == nullptr) {
        return nullptr;
    }
    using url_function = void* (*)(void*, void*, void*);
    return objc_msg_cast<url_function>(objc.msg_send)(
        url_cls, objc.sel_register_name("fileURLWithPath:"), ns_path);
}

/// @brief Copies a Metal error description without retaining the error object.
[[nodiscard]] std::string bioseq_ns_error_description(objc_runtime_functions& objc, void* error) {
    if (error == nullptr) {
        return {};
    }
    using get_object_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<get_object_function>(objc.msg_send)(
        error, objc.sel_register_name("localizedDescription"));
    return bioseq_ns_string_utf8(objc, desc, objc.sel_register_name("UTF8String"));
}

/// @brief Records the first owned diagnostic and permanently closes archive use.
void mark_metal_archive_sticky_failure(metal_pipeline_archive_interpose& state,
                                       std::string reason) noexcept {
    state.sticky_failure.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> guard(state.failure_mu);
        if (state.failure_reason.empty())
            state.failure_reason = std::move(reason);
    } catch (...) {
        // The atomic verdict is authoritative even if diagnostics cannot be
        // allocated on this already-failing path.
    }
}

/// @brief Records a literal diagnostic without allowing allocation failure to escape.
void mark_metal_archive_sticky_failure_literal(metal_pipeline_archive_interpose& state,
                                               const char* reason) noexcept {
    state.sticky_failure.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> guard(state.failure_mu);
        if (state.failure_reason.empty())
            state.failure_reason.assign(reason);
    } catch (...) {
    }
}

/// @brief Captures the first sticky failure diagnostic under its mutex.
[[nodiscard]] std::string metal_archive_failure_reason(metal_pipeline_archive_interpose& state) {
    std::lock_guard<std::mutex> guard(state.failure_mu);
    return state.failure_reason.empty() ? std::string{"immutable Metal archive receipt failed"}
                                        : state.failure_reason;
}

/// @brief Stable file identity and streamed-byte audit for one private archive.
struct metal_archive_file_snapshot {
    runtime_metal_archive_spec actual{};
    std::uint64_t device = 0u;
    std::uint64_t inode = 0u;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;
    std::uint64_t streamed_bytes = 0u;
    std::uint64_t scratch_capacity_bytes = 0u;
    bool exact_seed_compared = false;

    [[nodiscard]] bool operator==(const metal_archive_file_snapshot&) const noexcept = default;
};

/// @brief Validates one archive path and streams its exact retained bytes.
[[nodiscard]] std::expected<metal_archive_file_snapshot, std::string>
capture_metal_archive_file_snapshot(const std::filesystem::path& physical_path,
                                    const runtime_metal_archive_spec& expected,
                                    std::span<const std::byte> exact_seed_bytes = {}) {
    const std::filesystem::path path = physical_path;
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec || canonical != path) {
        return std::unexpected(
            fmt::format("private Metal archive snapshot path is not canonical for {}",
                        expected.artifact_filename));
    }

    struct stat before{};
    if (::lstat(path.c_str(), &before) != 0 || !S_ISREG(before.st_mode)) {
        return std::unexpected(fmt::format(
            "sealed Metal archive is missing, non-regular, or a symlink: {}", path.string()));
    }
    if (before.st_size <= 0) {
        return std::unexpected(fmt::format("sealed Metal archive is empty: {}", path.string()));
    }
    if (static_cast<std::uint64_t>(before.st_size) != expected.size ||
        (!exact_seed_bytes.empty() && exact_seed_bytes.size() != expected.size)) {
        return std::unexpected(
            fmt::format("private Metal archive size differs from retained seed for {}",
                        expected.artifact_filename));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(fmt::format("cannot open private Metal archive {}", path.string()));
    }
    constexpr std::size_t scratch_bytes = 64u * 1024u;
    std::array<std::byte, scratch_bytes> scratch{};
    sealed_shader_cache_detail::sha256_hasher hasher;
    std::uint64_t streamed_bytes = 0u;
    bool seed_matches = true;
    for (;;) {
        input.read(reinterpret_cast<char*>(scratch.data()),
                   static_cast<std::streamsize>(scratch.size()));
        const std::streamsize count = input.gcount();
        if (count < 0) {
            return std::unexpected(
                fmt::format("cannot stream private Metal archive {}", path.string()));
        }
        if (count == 0)
            break;
        const std::size_t chunk_bytes = static_cast<std::size_t>(count);
        const std::span<const std::byte> chunk{scratch.data(), chunk_bytes};
        hasher.update(chunk);
        if (!exact_seed_bytes.empty()) {
            const std::size_t offset = static_cast<std::size_t>(streamed_bytes);
            if (offset > exact_seed_bytes.size() ||
                chunk_bytes > exact_seed_bytes.size() - offset ||
                !std::equal(chunk.begin(),
                            chunk.end(),
                            exact_seed_bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
                seed_matches = false;
            }
        }
        streamed_bytes += static_cast<std::uint64_t>(chunk_bytes);
        if (!input && !input.eof()) {
            return std::unexpected(
                fmt::format("cannot stream private Metal archive {}", path.string()));
        }
    }
    if (streamed_bytes != expected.size || !seed_matches) {
        return std::unexpected(
            fmt::format("private Metal archive bytes differ from retained seed for {}",
                        expected.artifact_filename));
    }
    struct stat after{};
    if (::lstat(path.c_str(), &after) != 0 || !S_ISREG(after.st_mode) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
        before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
        before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) {
        return std::unexpected(
            fmt::format("sealed Metal archive changed while hashing: {}", path.string()));
    }
    runtime_metal_archive_spec actual{
        .artifact_ordinal = expected.artifact_ordinal,
        .artifact_filename = expected.artifact_filename,
        .size = static_cast<std::uint64_t>(after.st_size),
        .sha256 = hasher.finalize(),
        .negative_error_domain = expected.negative_error_domain,
        .negative_error_code = expected.negative_error_code,
    };
    if (actual != expected) {
        return std::unexpected(fmt::format("private Metal archive size or SHA-256 mismatch for {}",
                                           expected.artifact_filename));
    }
    return metal_archive_file_snapshot{
        .actual = std::move(actual),
        .device = static_cast<std::uint64_t>(after.st_dev),
        .inode = static_cast<std::uint64_t>(after.st_ino),
        .modified_seconds = after.st_mtimespec.tv_sec,
        .modified_nanoseconds = after.st_mtimespec.tv_nsec,
        .changed_seconds = after.st_ctimespec.tv_sec,
        .changed_nanoseconds = after.st_ctimespec.tv_nsec,
        .streamed_bytes = streamed_bytes,
        .scratch_capacity_bytes = scratch.size(),
        .exact_seed_compared = !exact_seed_bytes.empty(),
    };
}

/// @brief Owns and unlinks one private archive snapshot path.
class private_metal_archive_snapshot {
  public:
    /// @brief Creates an empty snapshot-path owner.
    private_metal_archive_snapshot() = default;

    /// @brief Adopts one private snapshot path.
    /// @param[in] path Snapshot path to unlink unless transferred.
    explicit private_metal_archive_snapshot(std::filesystem::path path) : path_(std::move(path)) {}

    /// @brief Prevents copying private snapshot ownership.
    private_metal_archive_snapshot(const private_metal_archive_snapshot&) = delete;

    /// @brief Prevents copy assignment of private snapshot ownership.
    private_metal_archive_snapshot& operator=(const private_metal_archive_snapshot&) = delete;

    /// @brief Transfers private snapshot-path ownership.
    /// @param[in,out] other Snapshot owner whose path is consumed.
    private_metal_archive_snapshot(private_metal_archive_snapshot&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}

    /// @brief Replaces this snapshot after unlinking it and transfers another path.
    /// @param[in,out] other Snapshot owner whose path is consumed.
    /// @return This snapshot owner.
    private_metal_archive_snapshot& operator=(private_metal_archive_snapshot&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }

    /// @brief Unlinks the owned private snapshot path.
    ~private_metal_archive_snapshot() {
        cleanup();
    }

    /// @brief Returns the owned private snapshot path.
    /// @return Borrowed path view.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief Transfers the private snapshot path out of this owner.
    /// @return Owned snapshot path.
    [[nodiscard]] std::filesystem::path release_path() noexcept {
        return std::exchange(path_, {});
    }

  private:
    /// @brief Best-effort unlinks and clears the owned snapshot path.
    void cleanup() noexcept {
        if (!path_.empty()) {
            (void)::unlink(path_.c_str());
            path_.clear();
        }
    }

    std::filesystem::path path_;
};
static_assert(std::is_nothrow_move_constructible_v<private_metal_archive_snapshot>);

/// @brief Owns one private snapshot descriptor until transfer or close.
class private_snapshot_file_descriptor {
  public:
    /// @brief Adopts one private snapshot file descriptor.
    /// @param[in] fd Descriptor to close unless transferred.
    explicit private_snapshot_file_descriptor(int fd) noexcept : fd_(fd) {}

    /// @brief Prevents copying descriptor ownership.
    private_snapshot_file_descriptor(const private_snapshot_file_descriptor&) = delete;

    /// @brief Prevents copy assignment of descriptor ownership.
    private_snapshot_file_descriptor& operator=(const private_snapshot_file_descriptor&) = delete;

    /// @brief Closes the owned descriptor.
    ~private_snapshot_file_descriptor() noexcept {
        if (fd_ >= 0)
            (void)::close(fd_);
    }

    /// @brief Returns the owned descriptor without transferring it.
    /// @return Borrowed file descriptor.
    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    /// @brief Transfers the descriptor out of this owner.
    /// @return Owned file descriptor.
    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    /// @brief Closes the descriptor immediately and preserves the close error.
    /// @return Zero on success, otherwise the captured `errno` value.
    [[nodiscard]] int close_now() noexcept {
        const int fd = std::exchange(fd_, -1);
        if (fd < 0 || ::close(fd) == 0)
            return 0;
        return errno;
    }

  private:
    int fd_ = -1;
};

/// @brief Removes an incomplete snapshot path unless ownership is released.
class private_snapshot_unlink_guard {
  public:
    /// @brief Adopts one incomplete snapshot pathname.
    /// @param[in] path Pathname to unlink unless released.
    explicit private_snapshot_unlink_guard(const char* path) noexcept : path_(path) {}

    /// @brief Prevents copying unlink ownership.
    private_snapshot_unlink_guard(const private_snapshot_unlink_guard&) = delete;

    /// @brief Prevents copy assignment of unlink ownership.
    private_snapshot_unlink_guard& operator=(const private_snapshot_unlink_guard&) = delete;

    /// @brief Unlinks the incomplete snapshot when ownership remains.
    ~private_snapshot_unlink_guard() noexcept {
        if (path_ != nullptr)
            (void)::unlink(path_);
    }

    /// @brief Relinquishes unlink responsibility after snapshot ownership transfers.
    void release() noexcept {
        path_ = nullptr;
    }

  private:
    const char* path_ = nullptr;
};

/// @brief Writes one exact seed artifact into a private snapshot.
[[nodiscard]] std::expected<private_metal_archive_snapshot, std::string>
write_private_metal_archive_snapshot(std::string_view artifact_filename,
                                     std::span<const std::byte> bytes,
                                     const std::filesystem::path& temp_parent_override = {}) {
    std::error_code temp_error;
    const std::filesystem::path temp_dir = temp_parent_override.empty()
                                               ? std::filesystem::temp_directory_path(temp_error)
                                               : temp_parent_override;
    if (temp_error || !temp_dir.is_absolute()) {
        return std::unexpected(
            fmt::format("cannot resolve private Metal archive snapshot directory for {}: {}",
                        artifact_filename,
                        temp_error ? temp_error.message() : "path is not absolute"));
    }
    std::error_code canonical_error;
    const std::filesystem::path canonical_temp_dir =
        std::filesystem::canonical(temp_dir, canonical_error);
    if (canonical_error || !canonical_temp_dir.is_absolute()) {
        return std::unexpected(
            fmt::format("cannot canonicalize private Metal archive snapshot directory for {}: {}",
                        artifact_filename,
                        canonical_error ? canonical_error.message() : "path is not absolute"));
    }
    std::filesystem::path pattern =
        canonical_temp_dir /
        (".bioseq-metalar-snapshot-" + std::string(artifact_filename) + "-XXXXXX");
    std::string owned_pattern = pattern.string();
    std::vector<char> writable(owned_pattern.begin(), owned_pattern.end());
    writable.push_back('\0');
    const int fd = ::mkstemp(writable.data());
    if (fd < 0) {
        return std::unexpected(
            fmt::format("cannot create private Metal archive snapshot for {}: {}",
                        artifact_filename,
                        std::strerror(errno)));
    }
    private_snapshot_file_descriptor fd_owner{fd};
    private_snapshot_unlink_guard unlink_owner{writable.data()};
    std::filesystem::path snapshot_path(writable.data());
    if (::fchmod(fd_owner.get(), S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected(fmt::format("cannot restrict private Metal archive snapshot {}: {}",
                                           snapshot_path.string(),
                                           std::strerror(errno)));
    }
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written =
            ::write(fd_owner.get(), bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            return std::unexpected(fmt::format("cannot write private Metal archive snapshot {}: {}",
                                               snapshot_path.string(),
                                               std::strerror(errno)));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd_owner.get()) != 0) {
        return std::unexpected(fmt::format("cannot fsync private Metal archive snapshot {}: {}",
                                           snapshot_path.string(),
                                           std::strerror(errno)));
    }
    const int close_error = fd_owner.close_now();
    if (close_error != 0) {
        return std::unexpected(fmt::format("cannot close private Metal archive snapshot {}: {}",
                                           snapshot_path.string(),
                                           std::strerror(close_error)));
    }
    private_metal_archive_snapshot result{std::move(snapshot_path)};
    unlink_owner.release();
    return std::expected<private_metal_archive_snapshot, std::string>{std::in_place,
                                                                      std::move(result)};
}

/// @brief Owns the mode-restricted process directory for open archive files.
class private_metal_archive_process_directory {
  public:
    /// @brief Creates an empty private-directory owner.
    private_metal_archive_process_directory() = default;

    /// @brief Adopts one mode-restricted private process directory.
    /// @param[in] path Directory path to remove unless transferred.
    explicit private_metal_archive_process_directory(std::filesystem::path path)
        : path_(std::move(path)) {}

    /// @brief Prevents copying private-directory ownership.
    private_metal_archive_process_directory(const private_metal_archive_process_directory&) =
        delete;

    /// @brief Prevents copy assignment of private-directory ownership.
    private_metal_archive_process_directory&
    operator=(const private_metal_archive_process_directory&) = delete;

    /// @brief Transfers private-directory ownership.
    /// @param[in,out] other Directory owner whose path is consumed.
    private_metal_archive_process_directory(
        private_metal_archive_process_directory&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}

    /// @brief Removes the owned private directory when it is still present.
    ~private_metal_archive_process_directory() noexcept {
        if (!path_.empty())
            (void)::rmdir(path_.c_str());
    }

    /// @brief Returns the owned private process directory.
    /// @return Borrowed directory path.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief Removes the directory immediately and captures any failure.
    /// @return Zero on success, otherwise the captured `errno` value.
    [[nodiscard]] int remove_now() noexcept {
        if (path_.empty())
            return 0;
        if (::rmdir(path_.c_str()) == 0) {
            path_.clear();
            return 0;
        }
        return errno;
    }

  private:
    std::filesystem::path path_;
};

/// @brief Creates the private process directory used during qualification.
[[nodiscard]] std::expected<private_metal_archive_process_directory, std::string>
make_private_metal_archive_process_directory() {
    std::error_code temp_error;
    const std::filesystem::path temp_parent = std::filesystem::temp_directory_path(temp_error);
    std::error_code canonical_error;
    const std::filesystem::path canonical_parent =
        temp_error ? std::filesystem::path{}
                   : std::filesystem::canonical(temp_parent, canonical_error);
    if (temp_error || canonical_error || !canonical_parent.is_absolute()) {
        return std::unexpected("cannot resolve private Metal archive process directory");
    }
    std::string pattern = (canonical_parent / ".bioseq-metalar-process-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
        return std::unexpected(fmt::format(
            "cannot create private Metal archive process directory: {}", std::strerror(errno)));
    }
    private_metal_archive_process_directory owner{std::filesystem::path(created)};
    if (::chmod(owner.path().c_str(), S_IRWXU) != 0) {
        return std::unexpected(fmt::format(
            "cannot restrict private Metal archive process directory: {}", std::strerror(errno)));
    }
    struct stat status{};
    if (::lstat(owner.path().c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected("private Metal archive process directory identity is invalid");
    }
    return owner;
}

/// @brief Owns one still-open archive file and its immutable file identity.
class open_private_metal_archive {
  public:
    /// @brief Creates an empty open-archive owner.
    open_private_metal_archive() = default;

    /// @brief Adopts one linked private archive descriptor and stable identity.
    /// @param[in] fd Open archive descriptor.
    /// @param[in] path Linked private archive path.
    /// @param[in] identity Expected regular-file identity shared by path and descriptor.
    open_private_metal_archive(int fd,
                               std::filesystem::path path,
                               sealed_shader_cache_detail::regular_file_identity identity) noexcept
        : fd_(fd), path_(std::move(path)), identity_(identity) {}

    /// @brief Prevents copying open archive ownership.
    open_private_metal_archive(const open_private_metal_archive&) = delete;

    /// @brief Prevents copy assignment of open archive ownership.
    open_private_metal_archive& operator=(const open_private_metal_archive&) = delete;

    /// @brief Transfers the descriptor, linked path, and immutable identity.
    /// @param[in,out] other Archive owner whose resources are consumed.
    open_private_metal_archive(open_private_metal_archive&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), path_(std::exchange(other.path_, {})),
          identity_(other.identity_), linked_(std::exchange(other.linked_, false)) {}

    /// @brief Unlinks any retained name and closes the descriptor.
    ~open_private_metal_archive() noexcept {
        if (linked_ && !path_.empty())
            (void)::unlink(path_.c_str());
        if (fd_ >= 0)
            (void)::close(fd_);
    }

    /// @brief Returns the open archive descriptor.
    /// @return Borrowed file descriptor.
    [[nodiscard]] int fd() const noexcept {
        return fd_;
    }

    /// @brief Returns the linked private archive path.
    /// @return Borrowed path view.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief Returns the immutable regular-file identity.
    /// @return Borrowed identity view.
    [[nodiscard]] const sealed_shader_cache_detail::regular_file_identity&
    identity() const noexcept {
        return identity_;
    }

    /// @brief Checks that the descriptor and linked pathname still name the same file.
    /// @return Whether both live identities equal the sealed identity.
    [[nodiscard]] bool do_path_and_fd_identities_match() const noexcept {
        if (fd_ < 0 || path_.empty() || !linked_)
            return false;
        struct stat fd_status{};
        struct stat path_status{};
        return ::fstat(fd_, &fd_status) == 0 && ::lstat(path_.c_str(), &path_status) == 0 &&
               S_ISREG(fd_status.st_mode) && S_ISREG(path_status.st_mode) &&
               !S_ISLNK(path_status.st_mode) &&
               sealed_shader_cache_detail::regular_file_identity_from_stat(fd_status) ==
                   identity_ &&
               sealed_shader_cache_detail::regular_file_identity_from_stat(path_status) ==
                   identity_;
    }

    /// @brief Verifies identity, unlinks the pathname, closes the descriptor, and proves absence.
    /// @return Success or the first identity, unlink, absence, or close diagnostic.
    [[nodiscard]] std::expected<void, std::string> unlink_close_and_require_absent() {
        if (!do_path_and_fd_identities_match()) {
            return std::unexpected("private Metal archive identity changed before unlink");
        }
        if (::unlink(path_.c_str()) != 0) {
            return std::unexpected(fmt::format(
                "cannot unlink private Metal archive immediately: {}", std::strerror(errno)));
        }
        linked_ = false;
        struct stat recreated{};
        errno = 0;
        if (::lstat(path_.c_str(), &recreated) == 0 || errno != ENOENT) {
            return std::unexpected(
                "private Metal archive name remained or was recreated after unlink");
        }
        struct stat fd_status{};
        if (::fstat(fd_, &fd_status) != 0 ||
            sealed_shader_cache_detail::regular_file_identity_from_stat(fd_status) != identity_) {
            return std::unexpected("unlinked private Metal archive descriptor identity changed");
        }
        const int fd = std::exchange(fd_, -1);
        if (::close(fd) != 0) {
            return std::unexpected(
                fmt::format("cannot close unlinked private Metal archive descriptor: {}",
                            std::strerror(errno)));
        }
        path_.clear();
        return {};
    }

  private:
    int fd_ = -1;
    std::filesystem::path path_;
    sealed_shader_cache_detail::regular_file_identity identity_{};
    bool linked_ = true;
};

/// @brief Writes and fsyncs one exact archive image without following links.
[[nodiscard]] std::expected<open_private_metal_archive, std::string>
write_open_private_metal_archive(const std::filesystem::path& directory,
                                 std::size_t ordinal,
                                 std::span<const std::byte> bytes) {
    const std::filesystem::path path =
        directory / ("archive-" + std::to_string(ordinal) + ".metalar");
    const int fd =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return std::unexpected(
            fmt::format("cannot create open private Metal archive: {}", std::strerror(errno)));
    }
    private_snapshot_file_descriptor fd_owner{fd};
    private_snapshot_unlink_guard unlink_owner{path.c_str()};
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected("cannot restrict open private Metal archive");
    }
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            return std::unexpected("cannot write open private Metal archive");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        return std::unexpected("cannot fsync open private Metal archive");
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) != bytes.size()) {
        return std::unexpected("open private Metal archive identity is invalid");
    }
    open_private_metal_archive result{
        fd_owner.get(), path, sealed_shader_cache_detail::regular_file_identity_from_stat(status)};
    (void)fd_owner.release();
    unlink_owner.release();
    return result;
}

/// @brief Hashes an open private archive once and compares retained seed bytes.
[[nodiscard]] std::expected<std::uint64_t, std::string>
hash_open_private_metal_archive_once(open_private_metal_archive& image,
                                     const runtime_metal_archive_spec& expected,
                                     std::span<const std::byte> exact_seed_bytes) {
    if (!image.do_path_and_fd_identities_match() || exact_seed_bytes.size() != expected.size ||
        ::lseek(image.fd(), 0, SEEK_SET) < 0) {
        return std::unexpected("private Metal archive is not stable before qualification hash");
    }
    sealed_shader_cache_detail::sha256_hasher hasher;
    std::array<std::byte, 64u << 10u> scratch{};
    std::uint64_t streamed = 0u;
    for (;;) {
        const ssize_t count = ::read(image.fd(), scratch.data(), scratch.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            return std::unexpected("private Metal archive qualification hash read failed");
        }
        if (count == 0)
            break;
        const std::size_t chunk = static_cast<std::size_t>(count);
        if (streamed > exact_seed_bytes.size() || chunk > exact_seed_bytes.size() - streamed ||
            !std::equal(scratch.begin(),
                        scratch.begin() + static_cast<std::ptrdiff_t>(chunk),
                        exact_seed_bytes.begin() + static_cast<std::ptrdiff_t>(streamed))) {
            return std::unexpected("private Metal archive differs from retained seed bytes");
        }
        hasher.update(std::span<const std::byte>{scratch.data(), chunk});
        streamed += chunk;
    }
    if (streamed != expected.size || hasher.finalize() != expected.sha256 ||
        !image.do_path_and_fd_identities_match()) {
        return std::unexpected("private Metal archive qualification hash mismatch");
    }
    return streamed;
}

/// @brief Loads one private Metal binary archive object from its open-file URL.
[[nodiscard]] void* load_metal_binary_archive(metal_pipeline_archive_interpose& state,
                                              void* loader_device,
                                              const std::filesystem::path& path) {
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    if (desc_cls == nullptr || loader_device == nullptr) {
        return nullptr;
    }
    using new_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<new_function>(state.objc.msg_send)(
        desc_cls, state.objc.sel_register_name("new"));
    scoped_objc_release desc_owner{&state.objc, state.sel_release, desc};
    void* url = bioseq_make_file_url(state.objc, path);
    if (desc == nullptr || url == nullptr) {
        return nullptr;
    }
    using set_object_function = void (*)(void*, void*, void*);
    objc_msg_cast<set_object_function>(state.objc.msg_send)(
        desc, state.objc.sel_register_name("setUrl:"), url);
    void* error = nullptr;
    using new_archive_function = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<new_archive_function>(state.objc.msg_send)(
        loader_device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    if (archive == nullptr) {
        bioseq_log<logging_level::debug>(
            "[vulkan_runtime] failed to load Metal binary archive {}: {}",
            path.string(),
            bioseq_ns_error_description(state.objc, error));
    }
    return archive;
}

/// @brief Loads the ordered archive set and seals process qualification.
[[nodiscard]] bool load_exact_metal_binary_archives(metal_pipeline_archive_interpose& state,
                                                    void* loader_device) {
    state.loaded_pre_post_identity_checks = 0u;
    if (!state.configuration_image || !state.configuration_image->is_valid()) {
        mark_metal_archive_sticky_failure(state,
                                          "immutable Metal archive configuration image is absent");
        return false;
    }
    const runtime_metal_archive_configuration& configuration =
        state.configuration_image->configuration();
    const runtime_metal_archive_set_identity expected_identity =
        make_runtime_metal_archive_set_identity(configuration.archives);
    const std::uint64_t residency_metadata = bioseq::common::saturating_add(
        bioseq::common::saturating_mul(expected_identity.archive_count,
                                       sizeof(runtime_metal_archive_spec) + 2u * sizeof(void*)),
        2u * 1024u);
    const std::uint64_t residency_peak = bioseq::common::saturating_mul(
        bioseq::common::saturating_add(expected_identity.total_size, residency_metadata),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (!expected_identity.is_valid() ||
        residency_peak == (std::numeric_limits<std::uint64_t>::max)()) {
        mark_metal_archive_sticky_failure(state,
                                          "immutable Metal archive residency admission saturated");
        return false;
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto residency_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            residency_peak, &admission_failure);
    if (!residency_permit) {
        mark_metal_archive_sticky_failure(
            state,
            shader_cache_message(
                "immutable Metal archive residency admission denied before load: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(admission_failure.reason),
                " requested=",
                admission_failure.requested_bytes));
        return false;
    }
    void* array_cls = state.objc.get_class("NSMutableArray");
    if (array_cls == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "NSMutableArray is unavailable for immutable Metal archives");
        return false;
    }
    using allocate_function = void* (*)(void*, void*);
    using initialize_capacity_function = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<allocate_function>(state.objc.msg_send)(
        array_cls, state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<initialize_capacity_function>(state.objc.msg_send)(
        allocated,
        state.objc.sel_register_name("initWithCapacity:"),
        static_cast<unsigned long>(configuration.archives.size()));
    scoped_objc_release array_owner{&state.objc, state.sel_release, array};
    if (array == nullptr) {
        mark_metal_archive_sticky_failure(state,
                                          "cannot allocate exact immutable Metal archive array");
        return false;
    }

    auto private_directory = make_private_metal_archive_process_directory();
    if (!private_directory) {
        mark_metal_archive_sticky_failure(state, std::move(private_directory.error()));
        return false;
    }
    using add_object_function = void (*)(void*, void*, void*);
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    for (std::size_t archive_index = 0u; archive_index < configuration.archives.size();
         ++archive_index) {
        const runtime_metal_archive_spec& expected = configuration.archives[archive_index];
        const sealed_shader_cache_artifact_image* seed_artifact =
            configuration.seed_image->artifact(expected.artifact_ordinal);
        if (seed_artifact == nullptr ||
            seed_artifact->descriptor.kind !=
                sealed_shader_cache_artifact_kind::metal_binary_archive ||
            seed_artifact->descriptor.filename != expected.artifact_filename ||
            seed_artifact->descriptor.size != expected.size ||
            seed_artifact->descriptor.sha256 != expected.sha256) {
            mark_metal_archive_sticky_failure(
                state,
                fmt::format(
                    "runtime Metal archive spec no longer matches retained seed bytes for {}",
                    expected.artifact_filename));
            return false;
        }
        auto private_image = write_open_private_metal_archive(
            private_directory->path(), archive_index, seed_artifact->bytes);
        if (!private_image) {
            mark_metal_archive_sticky_failure(state, std::move(private_image.error()));
            return false;
        }
        ++private_files_created;
        auto hashed =
            hash_open_private_metal_archive_once(*private_image, expected, seed_artifact->bytes);
        if (!hashed ||
            hashed_bytes > (std::numeric_limits<std::uint64_t>::max)() - (hashed ? *hashed : 0u)) {
            mark_metal_archive_sticky_failure(
                state,
                hashed ? "private Metal archive hashed byte count overflow"
                       : std::move(hashed.error()));
            return false;
        }
        hashed_bytes += *hashed;
        if (!private_image->do_path_and_fd_identities_match()) {
            mark_metal_archive_sticky_failure(
                state,
                "private Metal archive path/descriptor identity changed before synchronous load");
            return false;
        }
        if (void* archive = load_metal_binary_archive(state, loader_device, private_image->path());
            archive != nullptr) {
            scoped_objc_release archive_owner{&state.objc, state.sel_release, archive};
            if (!private_image->do_path_and_fd_identities_match()) {
                mark_metal_archive_sticky_failure(
                    state,
                    fmt::format(
                        "private Metal archive identity changed across synchronous load: {}",
                        expected.artifact_filename));
                return false;
            }
            objc_msg_cast<add_object_function>(state.objc.msg_send)(
                array, state.objc.sel_register_name("addObject:"), archive);
            auto unlinked = private_image->unlink_close_and_require_absent();
            if (!unlinked) {
                mark_metal_archive_sticky_failure(state, std::move(unlinked.error()));
                return false;
            }
            ++private_files_unlinked;
            ++state.loaded_pre_post_identity_checks;
        } else {
            mark_metal_archive_sticky_failure(
                state,
                fmt::format("Metal rejected private exact-byte archive snapshot for {}",
                            expected.artifact_filename));
            return false;
        }
    }
    if (const int remove_error = private_directory->remove_now(); remove_error != 0) {
        mark_metal_archive_sticky_failure(
            state,
            fmt::format("private Metal archive process directory did not disappear: {}",
                        std::strerror(remove_error)));
        return false;
    }
    state.loaded_identity = expected_identity;
    if (state.loaded_pre_post_identity_checks != expected_identity.archive_count) {
        mark_metal_archive_sticky_failure(
            state, "loaded Metal archive count differs from sealed expected set");
        return false;
    }
    using copy_function = void* (*)(void*, void*);
    void* immutable_array = objc_msg_cast<copy_function>(state.objc.msg_send)(
        array, state.objc.sel_register_name("copy"));
    scoped_objc_release immutable_array_owner{&state.objc, state.sel_release, immutable_array};
    using count_function = unsigned long (*)(void*, void*);
    const std::uint64_t immutable_count =
        immutable_array == nullptr ? 0u
                                   : static_cast<std::uint64_t>(objc_msg_cast<count_function>(
                                         state.objc.msg_send)(immutable_array, state.sel_count));
    if (immutable_array == nullptr || immutable_count != expected_identity.archive_count) {
        mark_metal_archive_sticky_failure(state, "cannot retain immutable Metal archive NSArray");
        return false;
    }
    const sealed_shader_cache_manifest& manifest = configuration.seed_image->manifest();
    const sealed_shader_cache_seed_receipt& seed_receipt = configuration.seed_image->receipt();
    runtime_metal_archive_process_qualification qualification{
        .wire_revision = 2u,
        .archive_set = expected_identity,
        .receipt_count = configuration.qualification_receipt_count,
        .hash_passes = 1u,
        .hashed_bytes = hashed_bytes,
        .private_files_created = private_files_created,
        .private_files_unlinked = private_files_unlinked,
        .live_private_paths = 0u,
        .pipeline_filesystem_hash_passes = 0u,
        .pipeline_filesystem_hashed_bytes = 0u,
        .helper_wire_revision = configuration.helper_wire_revision,
        .helper_build_identity = configuration.helper_build_identity,
        .helper_source = configuration.helper_source,
        .helper_executable_size = configuration.helper_executable_size,
        .helper_executable_sha256 = configuration.helper_executable_sha256,
        .helper_metal_registry_id = configuration.helper_metal_registry_id,
        .vulkan_device_uuid = state.runtime_vulkan_device_identity.device_uuid,
        .api_version = state.runtime_vulkan_device_identity.api_version,
        .driver_version = state.runtime_vulkan_device_identity.driver_version,
        .vendor_id = state.runtime_vulkan_device_identity.vendor_id,
        .device_id = state.runtime_vulkan_device_identity.device_id,
        .pipeline_cache_uuid = state.runtime_vulkan_device_identity.pipeline_cache_uuid,
        .seed_receipt_identity = seed_receipt.identity(),
        .seed_sha256 = manifest.seed_sha256,
        .seed_size = manifest.seed_size,
        .manifest_checksum = manifest.manifest_checksum,
        .saturated = false,
    };
    qualification.identity = runtime_metal_archive_process_qualification_identity(qualification);
    if (!qualification.is_internally_consistent()) {
        mark_metal_archive_sticky_failure(state,
                                          "process Metal archive qualification is incomplete");
        return false;
    }
    state.archive_residency_peak_bytes = residency_peak;
    state.archive_residency_retained_bytes =
        bioseq::common::saturating_add(expected_identity.total_size, residency_metadata);
    if (state.archive_residency_retained_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        state.archive_residency_retained_bytes > residency_permit->bytes()) {
        mark_metal_archive_sticky_failure(state,
                                          "Metal archive residency reconciliation saturated");
        state.archive_residency_retained_bytes = 0u;
        return false;
    }
    residency_permit->shrink_to(state.archive_residency_retained_bytes);
    state.archive_residency_permit = std::move(*residency_permit);
    state.archive_array = immutable_array_owner.release();
    state.process_qualification = std::move(qualification);
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] pso-interpose: loaded exact sealed archive set count=%llu "
                 "bytes=%llu checksum=%llu\n",
                 static_cast<unsigned long long>(state.loaded_identity.archive_count),
                 static_cast<unsigned long long>(state.loaded_identity.total_size),
                 static_cast<unsigned long long>(state.loaded_identity.ordered_checksum));
    bioseq_log<logging_level::debug>(
        "[vulkan_runtime] loaded exact sealed Metal archive set count={} bytes={} checksum={}",
        state.loaded_identity.archive_count,
        state.loaded_identity.total_size,
        state.loaded_identity.ordered_checksum);
    return true;
}

/// @brief Verifies in-memory qualification without reopening archive paths.
[[nodiscard]] std::expected<std::uint64_t, std::string>
verify_resident_metal_archive_snapshots(metal_pipeline_archive_interpose& state,
                                        std::string_view operation_label) {
    if (!state.configuration_image || state.archive_array == nullptr ||
        !state.process_qualification.is_internally_consistent()) {
        return std::unexpected(
            fmt::format("{} process Metal archive qualification is absent", operation_label));
    }
    if (state.loaded_identity != state.process_qualification.archive_set ||
        state.process_qualification.live_private_paths != 0u ||
        state.process_qualification.pipeline_filesystem_hash_passes != 0u ||
        state.process_qualification.pipeline_filesystem_hashed_bytes != 0u) {
        return std::unexpected(fmt::format(
            "{} process Metal archive qualification identity mismatch", operation_label));
    }
    return 0u;
}

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

/// @brief Binds one Vulkan pipeline creation to an immutable Metal audit.
class metal_archive_pipeline_create_scope {
  public:
    /// @brief Opens one immutable Metal audit scope for a Vulkan pipeline creation.
    /// @param[in] required Whether a provider-backed archive receipt is mandatory.
    /// @param[in] runtime_device_uuid Selected Vulkan device UUID.
    /// @param[in] shader_key Shader variant entering pipeline creation.
    /// @param[in] expected_entry_name Expected Metal entry-point name.
    metal_archive_pipeline_create_scope(bool required,
                                        std::array<std::uint8_t, VK_UUID_SIZE> runtime_device_uuid,
                                        shader_variant_key shader_key,
                                        std::string expected_entry_name)
        : required_(required) {
        auto& state = metal_pipeline_archive_interpose_state();
        if (!is_runtime_metal_archive_configuration_installed() || !state.configuration_image) {
            if (required_) {
                failure_ =
                    "provider-backed Vulkan pipeline has no sealed Metal archive configuration";
            }
            return;
        }
        const runtime_metal_archive_configuration& configuration =
            state.configuration_image->configuration();
        const sealed_shader_cache_manifest& manifest = configuration.seed_image->manifest();
        const sealed_shader_cache_seed_receipt& seed_receipt = configuration.seed_image->receipt();
        audit_.cache_digest = manifest.cache_digest;
        audit_.seed_sha256 = manifest.seed_sha256;
        audit_.seed_size = manifest.seed_size;
        audit_.manifest_checksum = manifest.manifest_checksum;
        audit_.seed_receipt_identity = seed_receipt.identity();
        audit_.seed_load_count = seed_receipt.load_count();
        audit_.seed_manifest_read_count = seed_receipt.manifest_read_count();
        audit_.seed_artifact_read_count = seed_receipt.artifact_read_count();
        audit_.seed_manifest_bytes = seed_receipt.manifest_bytes();
        audit_.seed_artifact_bytes = seed_receipt.artifact_bytes();
        audit_.seed_load_ns = seed_receipt.load_ns();
        audit_.seed_admission_count = seed_receipt.admission_count();
        audit_.seed_peak_admitted_bytes = seed_receipt.peak_admitted_bytes();
        audit_.seed_retained_capacity_bytes = seed_receipt.retained_capacity_bytes();
        audit_.seed_admission_saturated = seed_receipt.admission_saturated();
        audit_.cache_qualification = state.process_qualification;
        audit_.shader_key = shader_key;
        audit_.expected_entry_name = std::move(expected_entry_name);
        audit_.expected_archives = make_runtime_metal_archive_set_identity(configuration.archives);
        audit_.loaded_archives = state.loaded_identity;
        audit_.expected_negative_error_domain =
            configuration.archives.front().negative_error_domain;
        audit_.expected_negative_error_code = configuration.archives.front().negative_error_code;
        audit_.archive_pre_post_identity_checks = state.loaded_pre_post_identity_checks;
        auto& command_state = moltenvk_command_interpose_state();
        audit_.command_interpose_installed = command_state.installed;
        audit_.command_interpose_immutable_fail_closed = command_state.immutable_fail_closed;
        audit_.command_loader_metal_registry_id = command_state.loader_registry_id;
        audit_.unsealed_metal_source_requests =
            command_state.recognized_source_requests.load(std::memory_order_relaxed);
        audit_.metal_source_fallback_calls =
            command_state.source_fallback_calls.load(std::memory_order_relaxed);
        audit_.configured_vulkan_device_uuid = state.runtime_vulkan_device_uuid;
        audit_.runtime_vulkan_device_uuid = runtime_device_uuid;
        audit_.expected_metal_registry_id = state.runtime_metal_registry_id;
        audit_.loader_metal_registry_id = state.loader_registry_id;
        audit_.scope_identity = next_nonzero_identity(state.next_scope_identity);
        if (audit_.scope_identity == 0u) {
            failure_ = "Metal archive pipeline scope identity space exhausted";
            return;
        }
        if (!state.installed || state.archive_array == nullptr ||
            !audit_.cache_qualification.is_internally_consistent()) {
            failure_ = "sealed Metal archive interpose is not installed";
            return;
        }
        if (state.sticky_failure.load(std::memory_order_acquire)) {
            audit_.sticky_failure = true;
            failure_ = metal_archive_failure_reason(state);
            return;
        }
        if (!audit_.command_interpose_installed ||
            !audit_.command_interpose_immutable_fail_closed) {
            failure_ = "immutable MoltenVK command-source interpose is not installed fail-closed";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        if (command_state.sticky_failure.load(std::memory_order_acquire) ||
            audit_.unsealed_metal_source_requests != 0u ||
            audit_.metal_source_fallback_calls != 0u) {
            failure_ = "immutable GPU runtime observed an unsealed Metal source-library request";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        if (audit_.configured_vulkan_device_uuid != audit_.runtime_vulkan_device_uuid) {
            failure_ = "runtime Vulkan UUID differs from sealed archive UUID";
            mark_metal_archive_sticky_failure(state, failure_);
            return;
        }
        active_ = std::make_shared<active_metal_archive_pipeline_scope>();
        active_->audit = audit_;
        active_->required = required_;
        active_->active = true;
        {
            std::lock_guard<std::mutex> guard(state.active_scope_mu);
            if (state.active_scope != nullptr) {
                audit_.nested_scope = true;
                failure_ = "nested Metal archive pipeline-create scope rejected";
                mark_metal_archive_sticky_failure(state, failure_);
                active_.reset();
                return;
            }
            active_->outside_scope_interceptions_at_begin =
                state.outside_scope_interceptions.load(std::memory_order_relaxed);
            state.active_scope = active_;
        }
        active_scope_ = true;
    }

    /// @brief Prevents copying active audit-scope ownership.
    metal_archive_pipeline_create_scope(const metal_archive_pipeline_create_scope&) = delete;

    /// @brief Prevents copy assignment of active audit-scope ownership.
    metal_archive_pipeline_create_scope&
    operator=(const metal_archive_pipeline_create_scope&) = delete;

    /// @brief Fails closed when an active scope leaves without a consumed verdict.
    ~metal_archive_pipeline_create_scope() noexcept {
        if (!active_scope_ || active_ == nullptr)
            return;
        auto& state = metal_pipeline_archive_interpose_state();
        try {
            std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
            try {
                std::unique_lock<std::mutex> scope_guard(active_->mu);
                if (state.active_scope == active_)
                    state.active_scope.reset();
                active_->active = false;
                active_->finished = true;
                if (!finished_) {
                    mark_metal_archive_sticky_failure_literal(
                        state, "Metal archive pipeline-create scope abandoned before verdict");
                }
            } catch (...) {
                if (state.active_scope == active_)
                    state.active_scope.reset();
                mark_metal_archive_sticky_failure_literal(
                    state, "Metal archive pipeline-create scope cleanup failed");
            }
        } catch (...) {
            mark_metal_archive_sticky_failure_literal(
                state, "Metal archive pipeline-create scope cleanup failed");
        }
    }

    /// @brief Checks whether setup admitted the next pipeline call.
    /// @return Whether pipeline creation may proceed under this scope.
    [[nodiscard]] bool is_ready_for_pipeline_call() const noexcept {
        return failure_.empty() && (active_scope_ || !required_);
    }

    /// @brief Closes the scope and validates the complete immutable archive audit.
    /// @param[in] outer_result Vulkan pipeline-creation result.
    /// @return Optional accepted audit, or the exact fail-closed diagnostic.
    [[nodiscard]] std::expected<std::optional<runtime_metal_archive_pipeline_audit>, std::string>
    finish(VkResult outer_result) {
        if (finished_) {
            return std::unexpected("Metal archive pipeline-create scope verdict consumed twice");
        }
        finished_ = true;
        if (!failure_.empty())
            return std::unexpected(failure_);
        if (!active_scope_) {
            if (required_) {
                return std::unexpected(
                    "provider-backed Vulkan pipeline did not enter immutable Metal archive scope");
            }
            return std::optional<runtime_metal_archive_pipeline_audit>{};
        }
        auto& state = metal_pipeline_archive_interpose_state();
        {
            std::unique_lock<std::mutex> active_guard(state.active_scope_mu);
            std::unique_lock<std::mutex> scope_guard(active_->mu);
            if (state.active_scope != active_) {
                return std::unexpected(
                    "Metal archive pipeline-create scope ownership changed before verdict");
            }
            const std::uint64_t outside_end =
                state.outside_scope_interceptions.load(std::memory_order_relaxed);
            if (outside_end < active_->outside_scope_interceptions_at_begin) {
                active_->audit.outside_scope_interceptions =
                    (std::numeric_limits<std::uint64_t>::max)();
            } else {
                active_->audit.outside_scope_interceptions =
                    outside_end - active_->outside_scope_interceptions_at_begin;
            }
            active_->active = false;
            active_->finished = true;
            state.active_scope.reset();
            audit_ = std::move(active_->audit);
            if (!active_->failure.empty())
                failure_ = active_->failure;
        }
        if (!failure_.empty())
            return std::unexpected(failure_);
        if (!state.process_qualification.is_internally_consistent() ||
            state.process_qualification != audit_.cache_qualification) {
            failure_ = "process Metal archive qualification changed after pipeline";
            mark_metal_archive_sticky_failure(state, failure_);
            return std::unexpected(failure_);
        }
        audit_.pipeline_post_identity_checks = 0u;
        auto& command_state = moltenvk_command_interpose_state();
        audit_.unsealed_metal_source_requests =
            command_state.recognized_source_requests.load(std::memory_order_relaxed);
        audit_.metal_source_fallback_calls =
            command_state.source_fallback_calls.load(std::memory_order_relaxed);
        audit_.command_interpose_installed = command_state.installed;
        audit_.command_interpose_immutable_fail_closed = command_state.immutable_fail_closed;
        audit_.sticky_failure =
            audit_.sticky_failure || state.sticky_failure.load(std::memory_order_acquire);
        audit_.sticky_failure =
            audit_.sticky_failure || command_state.sticky_failure.load(std::memory_order_acquire);
        audit_.outer_vulkan_pipeline_succeeded = outer_result == VK_SUCCESS;
        audit_.receipt_identity = next_nonzero_identity(state.next_receipt_identity);
        if (!audit_.is_internally_consistent()) {
            return std::unexpected(
                fmt::format("immutable Metal archive receipt rejected: vk={} calls={} replace={} "
                            "enforce={} negative={}/{} sticky={}",
                            static_cast<int>(outer_result),
                            audit_.scoped_interceptions,
                            audit_.descriptor_replacements,
                            audit_.fail_on_miss_enforcements,
                            audit_.negative_control_misses,
                            audit_.same_descriptor_negative_controls,
                            audit_.sticky_failure));
        }
        return std::optional<runtime_metal_archive_pipeline_audit>{std::move(audit_)};
    }

  private:
    bool required_ = false;
    bool active_scope_ = false;
    bool finished_ = false;
    std::string failure_;
    runtime_metal_archive_pipeline_audit audit_{};
    std::shared_ptr<active_metal_archive_pipeline_scope> active_{};
};
#else
/// @brief Leaves command-library interposition inactive on non-Apple runtimes.
void install_moltenvk_command_library_interpose() noexcept {}
/// @brief Leaves pipeline interposition inactive on non-Apple runtimes.
void install_metal_pipeline_archive_interpose(runtime_vulkan_device_identity) noexcept {}

/// @brief Rejects immutable archive requirements on non-Apple runtimes.
class metal_archive_pipeline_create_scope {
  public:
    /// @brief Records whether immutable archive proof is required on a non-Apple runtime.
    /// @param[in] required Whether a provider-backed archive receipt is mandatory.
    /// The remaining arguments preserve the cross-platform constructor contract
    /// but are intentionally unnamed and ignored on this platform.
    metal_archive_pipeline_create_scope(bool required,
                                        std::array<std::uint8_t, VK_UUID_SIZE>,
                                        shader_variant_key,
                                        std::string) noexcept
        : required_(required || is_runtime_metal_archive_configuration_installed()) {}

    /// @brief Checks whether no immutable Metal proof is required.
    /// @return Whether pipeline creation may proceed on this non-Apple runtime.
    [[nodiscard]] bool is_ready_for_pipeline_call() const noexcept {
        return !required_;
    }

    /// @brief Produces the non-Apple fail-closed verdict.
    /// The Vulkan result is intentionally unnamed because the platform verdict
    /// depends only on whether immutable Metal proof was required.
    /// @return Empty audit when optional, otherwise the platform diagnostic.
    [[nodiscard]] std::expected<std::optional<runtime_metal_archive_pipeline_audit>, std::string>
    finish(VkResult) const {
        if (required_) {
            return std::unexpected(
                "immutable Metal archive receipt requested on a non-Apple runtime");
        }
        return std::optional<runtime_metal_archive_pipeline_audit>{};
    }

  private:
    bool required_ = false;
};
#endif

} // namespace bioseq::gpu::vulkan
