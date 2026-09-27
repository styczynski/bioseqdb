/**
 * @file metal_archive/command_capture.cppm
 * @brief Captures MoltenVK command-library source inside a separate helper process.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

module bioseq.gpu.metal_archive;

namespace bioseq::gpu::metal_archive {
namespace {

using objc_object = void*;
using objc_selector = void*;
using objc_class = void*;

/// @brief Objective-C functions loaded, by the command-source capture helper.
struct capture_objc_runtime {
    void* handle = nullptr;
    void* message_send = nullptr;
    objc_selector (*register_selector)(const char*) = nullptr;
    objc_class (*object_get_class)(objc_object) = nullptr;
    objc_class (*class_get_superclass)(objc_class) = nullptr;
    void* (*class_get_instance_method)(objc_class, objc_selector) = nullptr;
    void* (*method_get_implementation)(void*) = nullptr;
    void* (*method_set_implementation)(void*, void*) = nullptr;

    [[nodiscard]] bool load() noexcept {
        handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
            return false;
        message_send = ::dlsym(handle, "objc_msgSend");
        register_selector = reinterpret_cast<decltype(register_selector)>(
            ::dlsym(handle, "sel_registerName"));
        object_get_class = reinterpret_cast<decltype(object_get_class)>(
            ::dlsym(handle, "object_getClass"));
        class_get_superclass = reinterpret_cast<decltype(class_get_superclass)>(
            ::dlsym(handle, "class_getSuperclass"));
        class_get_instance_method = reinterpret_cast<decltype(class_get_instance_method)>(
            ::dlsym(handle, "class_getInstanceMethod"));
        method_get_implementation = reinterpret_cast<decltype(method_get_implementation)>(
            ::dlsym(handle, "method_getImplementation"));
        method_set_implementation = reinterpret_cast<decltype(method_set_implementation)>(
            ::dlsym(handle, "method_setImplementation"));
        return message_send != nullptr && register_selector != nullptr &&
               object_get_class != nullptr && class_get_superclass != nullptr &&
               class_get_instance_method != nullptr && method_get_implementation != nullptr &&
               method_set_implementation != nullptr;
    }

    template <class p_function_type>
    [[nodiscard]] p_function_type message() const noexcept {
        return reinterpret_cast<p_function_type>(message_send);
    }

    [[nodiscard]] bool responds_to(objc_object object, const char* name) const noexcept {
        if (object == nullptr)
            return false;
        using function_type = signed char (*)(objc_object, objc_selector, objc_selector);
        return message<function_type>()(
                   object,
                   register_selector("respondsToSelector:"),
                   register_selector(name)) != 0;
    }

    [[nodiscard]] std::uint64_t unsigned_property(objc_object object,
                                                  const char* name) const noexcept {
        using function_type = unsigned long (*)(objc_object, objc_selector);
        return static_cast<std::uint64_t>(
            message<function_type>()(object, register_selector(name)));
    }

    [[nodiscard]] bool boolean_property(objc_object object, const char* name) const noexcept {
        using function_type = signed char (*)(objc_object, objc_selector);
        return message<function_type>()(object, register_selector(name)) != 0;
    }

    [[nodiscard]] objc_object object_property(objc_object object,
                                              const char* name) const noexcept {
        using function_type = objc_object (*)(objc_object, objc_selector);
        return message<function_type>()(object, register_selector(name));
    }
};

/// @brief Compiler option values and availability flags saved with the captured source
struct captured_compile_options {
    std::uint64_t language_version = 0u;
    std::uint64_t fast_math_enabled = 0u;
    std::uint64_t math_mode_available = 0u;
    std::uint64_t math_mode = 0u;
    std::uint64_t math_fp_functions_available = 0u;
    std::uint64_t math_fp_functions = 0u;
    std::uint64_t optimization_level_available = 0u;
    std::uint64_t optimization_level = 0u;
    std::uint64_t preserve_invariance = 0u;
    std::uint64_t library_type_available = 0u;
    std::uint64_t library_type = 0u;
    std::uint64_t compile_symbol_visibility_available = 0u;
    std::uint64_t compile_symbol_visibility = 0u;
    std::uint64_t allow_undefined_available = 0u;
    std::uint64_t allow_undefined = 0u;
    std::uint64_t max_total_threads_available = 0u;
    std::uint64_t max_total_threads = 0u;
    std::uint64_t enable_logging_available = 0u;
    std::uint64_t enable_logging = 0u;
    std::uint64_t preprocessor_macro_count = 0u;
    std::uint64_t linked_library_count = 0u;
    std::uint64_t install_name_bytes = 0u;

    [[nodiscard]] bool operator==(const captured_compile_options&) const noexcept = default;
};

/// @brief Returns the collection size, or zero  for a null NSArray or NSDictionary.
[[nodiscard]] std::uint64_t collection_count(const capture_objc_runtime& objc,
                                             objc_object collection) noexcept {
    return collection == nullptr ? 0u : objc.unsigned_property(collection, "count");
}

/// @brief Returns the UTF-8 byte count, or zero for a null string or text pointer.
[[nodiscard]] std::uint64_t string_byte_count(const capture_objc_runtime& objc,
                                              objc_object string) noexcept {
    if (string == nullptr)
        return 0u;
    using function_type = const char* (*)(objc_object, objc_selector);
    const char* text = objc.message<function_type>()(
        string, objc.register_selector("UTF8String"));
    return text == nullptr ? 0u : static_cast<std::uint64_t>(std::strlen(text));
}

/// @brief Reads the compiler options supported by this capture format.
[[nodiscard]] captured_compile_options capture_options(const capture_objc_runtime& objc,
                                                       objc_object options) noexcept {
    captured_compile_options result{};
    if (options == nullptr)
        return result;
    result.language_version = objc.unsigned_property(options, "languageVersion");
    result.fast_math_enabled = objc.boolean_property(options, "fastMathEnabled") ? 1u : 0u;
    result.math_mode_available = objc.responds_to(options, "mathMode") ? 1u : 0u;
    if (result.math_mode_available)
        result.math_mode = objc.unsigned_property(options, "mathMode");
    result.math_fp_functions_available =
        objc.responds_to(options, "mathFloatingPointFunctions") ? 1u : 0u;
    if (result.math_fp_functions_available) {
        result.math_fp_functions =
            objc.unsigned_property(options, "mathFloatingPointFunctions");
    }
    result.optimization_level_available =
        objc.responds_to(options, "optimizationLevel") ? 1u : 0u;
    if (result.optimization_level_available)
        result.optimization_level = objc.unsigned_property(options, "optimizationLevel");
    result.preserve_invariance =
        objc.boolean_property(options, "preserveInvariance") ? 1u : 0u;
    result.library_type_available = objc.responds_to(options, "libraryType") ? 1u : 0u;
    if (result.library_type_available)
        result.library_type = objc.unsigned_property(options, "libraryType");
    result.compile_symbol_visibility_available =
        objc.responds_to(options, "compileSymbolVisibility") ? 1u : 0u;
    if (result.compile_symbol_visibility_available) {
        result.compile_symbol_visibility =
            objc.unsigned_property(options, "compileSymbolVisibility");
    }
    result.allow_undefined_available =
        objc.responds_to(options, "allowReferencingUndefinedSymbols") ? 1u : 0u;
    if (result.allow_undefined_available) {
        result.allow_undefined =
            objc.boolean_property(options, "allowReferencingUndefinedSymbols") ? 1u : 0u;
    }
    result.max_total_threads_available =
        objc.responds_to(options, "maxTotalThreadsPerThreadgroup") ? 1u : 0u;
    if (result.max_total_threads_available) {
        result.max_total_threads =
            objc.unsigned_property(options, "maxTotalThreadsPerThreadgroup");
    }
    result.enable_logging_available = objc.responds_to(options, "enableLogging") ? 1u : 0u;
    if (result.enable_logging_available)
        result.enable_logging = objc.boolean_property(options, "enableLogging") ? 1u : 0u;
    result.preprocessor_macro_count =
        collection_count(objc, objc.object_property(options, "preprocessorMacros"));
    result.linked_library_count =
        collection_count(objc, objc.object_property(options, "libraries"));
    result.install_name_bytes =
        string_byte_count(objc, objc.object_property(options, "installName"));
    return result;
}

/// @brief Shared state while wrappers intercept Metal's source-compilation methods.
struct command_capture_state {
    capture_objc_runtime objc{};
    void* original_sync = nullptr;
    void* original_async = nullptr;
    objc_selector utf8_selector = nullptr;
    objc_selector registry_selector = nullptr;
    std::mutex lifecycle_mutex;
    std::mutex mutex;
    std::atomic<std::uint64_t> in_flight{0u};
    std::string source;
    captured_compile_options options{};
    std::string moltenvk_image_path;
    std::uint64_t metal_registry_id = 0u;
    std::uint64_t capture_count = 0u;
    bool divergent = false;
    bool lifecycle_consumed = false; ///< Protected by lifecycle_mutex. Prevents reusing this state for a second capture.
};

/// @brief Returns capture storage that stays alive until the helper exits.
[[nodiscard]] command_capture_state& command_capture_process_state() noexcept {
    static command_capture_state state;
    return state;
}

/// @brief State currently available to the source-compilation wrappers.
std::atomic<command_capture_state*> g_command_capture_state{nullptr};

/// @brief Keeps an active-wrapper count until this guard is destroyed.
class command_capture_access {
  public:
    command_capture_access() = default;
    explicit command_capture_access(command_capture_state* state) noexcept : state_(state) {}
    command_capture_access(const command_capture_access&) = delete;
    command_capture_access& operator=(const command_capture_access&) = delete;
    command_capture_access(command_capture_access&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
    command_capture_access& operator=(command_capture_access&&) = delete;
    ~command_capture_access() {
        if (state_ != nullptr &&
            state_->in_flight.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
            state_->in_flight.notify_all();
        }
    }
    [[nodiscard]] command_capture_state* get() const noexcept { return state_; }

  private:
    command_capture_state* state_ = nullptr;
};

/// @brief Acquires the current capture stte, or an empty guard if capture is unavailable.
[[nodiscard]] command_capture_access acquire_command_capture_state() noexcept {
    command_capture_state* state = g_command_capture_state.load(std::memory_order_acquire);
    if (state == nullptr)
        return {};
    state->in_flight.fetch_add(1u, std::memory_order_acq_rel);
    if (g_command_capture_state.load(std::memory_order_acquire) == state)
        return command_capture_access{state};
    if (state->in_flight.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
        state->in_flight.notify_all();
    return {};
}

/// @brief Waits until every wrapper that acquired the state has returned.
void wait_for_command_capture_quiescence(command_capture_state& state) noexcept {
    std::uint64_t in_flight = state.in_flight.load(std::memory_order_acquire);
    while (in_flight != 0u) {
        state.in_flight.wait(in_flight, std::memory_order_acquire);
        in_flight = state.in_flight.load(std::memory_order_acquire);
    }
}

/// @brief Encodes arbitrary bytes as lowercase hexadecimal transcript text.
[[nodiscard]] std::string encode_hex(std::string_view bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes)
        out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

template <std::size_t p_size>
[[nodiscard]] std::string encode_hex(const std::array<std::uint8_t, p_size>& bytes) {
    return encode_hex(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

/// @brief Saves a matching MoltenVK source  request and detects changes across repeated requests.
void record_command_request(command_capture_state& state,
                            objc_object receiver,
                            objc_object source,
                            objc_object options,
                            const void* caller) noexcept {
    if (source == nullptr || options == nullptr)
        return;
    using utf8_function = const char* (*)(objc_object, objc_selector);
    const char* text = state.objc.message<utf8_function>()(source, state.utf8_selector);
    if (text == nullptr || std::strstr(text, "cmdCopyBufferBytes") == nullptr ||
        std::strstr(text, "vtxCmdBlitImage") == nullptr) {
        return;
    }
    using registry_function = unsigned long long (*)(objc_object, objc_selector);
    const std::uint64_t registry_id = static_cast<std::uint64_t>(
        state.objc.message<registry_function>()(receiver, state.registry_selector));
    const captured_compile_options captured = capture_options(state.objc, options);
    Dl_info image_info{};
    const std::string image_path =
        caller != nullptr && ::dladdr(caller, &image_info) != 0 && image_info.dli_fname != nullptr
            ? std::string(image_info.dli_fname)
            : std::string{};
    std::lock_guard<std::mutex> guard(state.mutex);
    ++state.capture_count;
    const std::string candidate(text);
    if (state.capture_count == 1u) {
        state.source = candidate;
        state.options = captured;
        state.metal_registry_id = registry_id;
        state.moltenvk_image_path = image_path;
    } else if (state.source != candidate || state.options != captured ||
               state.metal_registry_id != registry_id ||
               state.moltenvk_image_path != image_path) {
        state.divergent = true;
    }
}

extern "C" objc_object bioseq_capture_new_library_with_source(
    objc_object self,
    objc_selector selector,
    objc_object source,
    objc_object options,
    objc_object* error) {
    command_capture_access access = acquire_command_capture_state();
    command_capture_state* state = access.get();
    if (state == nullptr || state->original_sync == nullptr)
        return nullptr;
    record_command_request(*state, self, source, options, __builtin_return_address(0));
    using function_type = objc_object (*)(
        objc_object, objc_selector, objc_object, objc_object, objc_object*);
    return reinterpret_cast<function_type>(state->original_sync)(
        self, selector, source, options, error);
}

extern "C" void bioseq_capture_new_library_with_source_async(
    objc_object self,
    objc_selector selector,
    objc_object source,
    objc_object options,
    objc_object completion) {
    command_capture_access access = acquire_command_capture_state();
    command_capture_state* state = access.get();
    if (state == nullptr || state->original_async == nullptr)
        return;
    record_command_request(*state, self, source, options, __builtin_return_address(0));
    using function_type = void (*)(
        objc_object, objc_selector, objc_object, objc_object, objc_object);
    reinterpret_cast<function_type>(state->original_async)(
        self, selector, source, options, completion);
}

/// @brief Searches the device class and its parent classes for an instance method.
[[nodiscard]] void* find_instance_method(const capture_objc_runtime& objc,
                                         objc_object device,
                                         objc_selector selector) noexcept {
    for (objc_class cls = objc.object_get_class(device); cls != nullptr;
         cls = objc.class_get_superclass(cls)) {
        if (void* method = objc.class_get_instance_method(cls, selector); method != nullptr)
            return method;
    }
    return nullptr;
}

/**
 * @brief Creates a private source file and syncs its bytes to disk.
 *
 * The path must be absolute and unused, and the source must be non-empty.
 * Returns false on failure and removes any incomplete file it created.
 */
[[nodiscard]] bool write_exclusive_file(std::string_view path, std::string_view bytes) noexcept {
    if (path.empty() || path.front() != '/' || bytes.empty())
        return false;
    const std::string owned_path(path);
    const int fd = ::open(owned_path.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          S_IRUSR | S_IWUSR);
    if (fd < 0)
        return false;
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            (void)::close(fd);
            (void)::unlink(owned_path.c_str());
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    struct stat status{};
    const bool accepted = ::fsync(fd) == 0 && ::fstat(fd, &status) == 0 &&
                          S_ISREG(status.st_mode) &&
                          static_cast<std::uint64_t>(status.st_size) == bytes.size() &&
                          (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    const bool closed = ::close(fd) == 0;
    if (!accepted || !closed) {
        (void)::unlink(owned_path.c_str());
        return false;
    }
    return true;
}

/// @brief Vulkan functions loaded for the device-creation probe.
struct capture_vulkan_runtime {
    void* loader_handle = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceFeatures get_physical_device_features = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;

    [[nodiscard]] bool load(std::string_view required_loader_path,
                            std::string* loader_path) noexcept {
        try {
            if (required_loader_path.empty() ||
                required_loader_path.find('\0') != std::string_view::npos) {
                return false;
            }
            std::error_code error;
            const std::filesystem::path required = std::filesystem::canonical(
                std::filesystem::path{std::string{required_loader_path}}, error);
            if (error || !required.is_absolute())
                return false;
            loader_handle = ::dlopen(required.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (loader_handle == nullptr)
                return false;
            enumerate_instance_extensions =
                reinterpret_cast<decltype(enumerate_instance_extensions)>(
                    ::dlsym(loader_handle, "vkEnumerateInstanceExtensionProperties"));
            create_instance = reinterpret_cast<decltype(create_instance)>(
                ::dlsym(loader_handle, "vkCreateInstance"));
            destroy_instance = reinterpret_cast<decltype(destroy_instance)>(
                ::dlsym(loader_handle, "vkDestroyInstance"));
            enumerate_physical_devices =
                reinterpret_cast<decltype(enumerate_physical_devices)>(
                    ::dlsym(loader_handle, "vkEnumeratePhysicalDevices"));
            get_physical_device_features =
                reinterpret_cast<decltype(get_physical_device_features)>(
                    ::dlsym(loader_handle, "vkGetPhysicalDeviceFeatures"));
            get_queue_family_properties =
                reinterpret_cast<decltype(get_queue_family_properties)>(
                    ::dlsym(loader_handle, "vkGetPhysicalDeviceQueueFamilyProperties"));
            enumerate_device_extensions =
                reinterpret_cast<decltype(enumerate_device_extensions)>(
                    ::dlsym(loader_handle, "vkEnumerateDeviceExtensionProperties"));
            get_physical_device_properties2 =
                reinterpret_cast<decltype(get_physical_device_properties2)>(
                    ::dlsym(loader_handle, "vkGetPhysicalDeviceProperties2"));
            create_device = reinterpret_cast<decltype(create_device)>(
                ::dlsym(loader_handle, "vkCreateDevice"));
            destroy_device = reinterpret_cast<decltype(destroy_device)>(
                ::dlsym(loader_handle, "vkDestroyDevice"));
            if (enumerate_instance_extensions == nullptr || create_instance == nullptr ||
                destroy_instance == nullptr || enumerate_physical_devices == nullptr ||
                get_physical_device_features == nullptr ||
                get_queue_family_properties == nullptr ||
                enumerate_device_extensions == nullptr ||
                get_physical_device_properties2 == nullptr || create_device == nullptr ||
                destroy_device == nullptr) {
                return false;
            }
            Dl_info info{};
            if (::dladdr(reinterpret_cast<const void*>(create_instance), &info) == 0 ||
                info.dli_fname == nullptr) {
                return false;
            }
            error.clear();
            const std::filesystem::path observed =
                std::filesystem::canonical(info.dli_fname, error);
            if (error || observed != required)
                return false;
            *loader_path = observed.string();
            return !loader_path->empty();
        } catch (...) {
            return false;
        }
    }
};

[[nodiscard]] bool has_extension(std::span<const VkExtensionProperties> extensions,
                                 const char* name) noexcept {
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    }
    return false;
}

/// @brief Keeps the selected Vulkan ICD, or finds Homebrew's MoltenVK driver.
[[nodiscard]] bool configure_moltenvk_icd() noexcept {
    if (::setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0) != 0)
        return false;
    if (const char* existing = std::getenv("VK_ICD_FILENAMES");
        existing != nullptr && *existing != '\0') {
        return true;
    }
    constexpr std::array candidates{
        "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json",
    };
    for (const char* candidate : candidates) {
        if (::access(candidate, R_OK) == 0)
            return ::setenv("VK_ICD_FILENAMES", candidate, 1) == 0;
    }
    return false;
}

/// @brief Creates a compute-capable Vulkan device to trigger MoltenVK command compilation.
[[nodiscard]] bool create_capture_device(capture_vulkan_runtime& vk,
                                         VkInstance* instance,
                                         VkDevice* device,
                                         std::array<std::uint8_t, VK_UUID_SIZE>* device_uuid,
                                         std::array<std::uint8_t, VK_UUID_SIZE>* pipeline_uuid,
                                         std::uint32_t* api_version,
                                         std::uint32_t* driver_version,
                                         std::uint32_t* vendor_id,
                                         std::uint32_t* device_id) {
    constexpr const char* k_portability_subset = "VK_KHR_portability_subset";
    std::uint32_t extension_count = 0u;
    if (vk.enumerate_instance_extensions(nullptr, &extension_count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> instance_extensions(extension_count);
    if (extension_count != 0u &&
        vk.enumerate_instance_extensions(nullptr, &extension_count, instance_extensions.data()) !=
            VK_SUCCESS) {
        return false;
    }
    std::vector<const char*> enabled_instance_extensions;
    VkInstanceCreateFlags instance_flags = 0u;
    if (has_extension(instance_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled_instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    if (has_extension(instance_extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        enabled_instance_extensions.push_back(
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    VkBool32 use_argument_buffers = VK_FALSE;
    VkLayerSettingEXT settings[]{{
        "MoltenVK",
        "useMetalArgumentBuffers",
        VK_LAYER_SETTING_TYPE_BOOL32_EXT,
        1u,
        &use_argument_buffers,
    }};
    VkLayerSettingsCreateInfoEXT settings_info{};
    settings_info.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    settings_info.settingCount = 1u;
    settings_info.pSettings = settings;
    const bool has_layer_settings =
        has_extension(instance_extensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    if (has_layer_settings)
        enabled_instance_extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "bioseq-command-capture";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "bioseq-gpu";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pNext = has_layer_settings ? &settings_info : nullptr;
    instance_info.flags = instance_flags;
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount =
        static_cast<std::uint32_t>(enabled_instance_extensions.size());
    instance_info.ppEnabledExtensionNames = enabled_instance_extensions.data();
    if (vk.create_instance(&instance_info, nullptr, instance) != VK_SUCCESS ||
        *instance == VK_NULL_HANDLE) {
        return false;
    }

    std::uint32_t physical_count = 0u;
    if (vk.enumerate_physical_devices(*instance, &physical_count, nullptr) != VK_SUCCESS ||
        physical_count == 0u) {
        return false;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    if (vk.enumerate_physical_devices(*instance, &physical_count, physical_devices.data()) !=
        VK_SUCCESS) {
        return false;
    }
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0u;
    bool portability_subset = false;
    for (VkPhysicalDevice candidate : physical_devices) {
        VkPhysicalDeviceFeatures features{};
        vk.get_physical_device_features(candidate, &features);
        if (features.shaderInt64 != VK_TRUE)
            continue;
        std::uint32_t family_count = 0u;
        vk.get_queue_family_properties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        if (family_count != 0u)
            vk.get_queue_family_properties(candidate, &family_count, families.data());
        for (std::uint32_t ordinal = 0u; ordinal < family_count; ++ordinal) {
            if ((families[ordinal].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0u)
                continue;
            selected = candidate;
            queue_family = ordinal;
            break;
        }
        if (selected == VK_NULL_HANDLE)
            continue;
        std::uint32_t device_extension_count = 0u;
        if (vk.enumerate_device_extensions(
                selected, nullptr, &device_extension_count, nullptr) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkExtensionProperties> device_extensions(device_extension_count);
        if (device_extension_count != 0u &&
            vk.enumerate_device_extensions(selected,
                                           nullptr,
                                           &device_extension_count,
                                           device_extensions.data()) != VK_SUCCESS) {
            return false;
        }
        portability_subset = has_extension(device_extensions, k_portability_subset);
        break;
    }
    if (selected == VK_NULL_HANDLE)
        return false;

    VkPhysicalDeviceIDProperties id_properties{};
    id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id_properties;
    vk.get_physical_device_properties2(selected, &properties);
    std::copy(std::begin(id_properties.deviceUUID),
              std::end(id_properties.deviceUUID),
              device_uuid->begin());
    std::copy(std::begin(properties.properties.pipelineCacheUUID),
              std::end(properties.properties.pipelineCacheUUID),
              pipeline_uuid->begin());
    *api_version = properties.properties.apiVersion;
    *driver_version = properties.properties.driverVersion;
    *vendor_id = properties.properties.vendorID;
    *device_id = properties.properties.deviceID;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1u;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.shaderInt64 = VK_TRUE;
    std::array<const char*, 1u> device_extensions{};
    std::uint32_t enabled_device_extension_count = 0u;
    if (portability_subset) {
        device_extensions[0] = k_portability_subset;
        enabled_device_extension_count = 1u;
    }
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1u;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = enabled_device_extension_count;
    device_info.ppEnabledExtensionNames =
        enabled_device_extension_count == 0u ? nullptr : device_extensions.data();
    device_info.pEnabledFeatures = &enabled_features;
    return vk.create_device(selected, &device_info, nullptr, device) == VK_SUCCESS &&
           *device != VK_NULL_HANDLE;
}

} // namespace

/** @copydoc bioseq::gpu::metal_archive::capture_moltenvk_command_library */
std::string capture_moltenvk_command_library(std::string_view source_output_path,
                                             std::string_view loader_image_path) {
    std::ostringstream transcript;
    transcript << "pid=" << static_cast<long>(::getpid()) << '\n';
    if (!configure_moltenvk_icd()) {
        transcript << "command capture: MoltenVK ICD unavailable\n";
        return transcript.str();
    }

    capture_objc_runtime objc;
    void* metal_handle =
        ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    using create_metal_device_function = objc_object (*)();
    auto create_metal_device = metal_handle == nullptr
                                   ? nullptr
                                   : reinterpret_cast<create_metal_device_function>(
                                         ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
    if (!objc.load() || create_metal_device == nullptr) {
        transcript << "command capture: Objective-C/Metal load failed\n";
        return transcript.str();
    }
    objc_object metal_device = create_metal_device();
    if (metal_device == nullptr) {
        transcript << "command capture: Metal device unavailable\n";
        return transcript.str();
    }
    const objc_selector release_selector = objc.register_selector("release");
    const objc_selector source_selector =
        objc.register_selector("newLibraryWithSource:options:error:");
    const objc_selector source_async_selector =
        objc.register_selector("newLibraryWithSource:options:completionHandler:");
    void* sync_method = find_instance_method(objc, metal_device, source_selector);
    void* async_method = find_instance_method(objc, metal_device, source_async_selector);
    if (sync_method == nullptr || async_method == nullptr) {
        using release_function = void (*)(objc_object, objc_selector);
        objc.message<release_function>()(metal_device, release_selector);
        transcript << "command capture: Metal source methods unavailable\n";
        return transcript.str();
    }

    command_capture_state& capture = command_capture_process_state();
    std::unique_lock<std::mutex> lifecycle_guard(capture.lifecycle_mutex);
    if (capture.lifecycle_consumed ||
        g_command_capture_state.load(std::memory_order_acquire) != nullptr ||
        capture.in_flight.load(std::memory_order_acquire) != 0u) {
        using release_function = void (*)(objc_object, objc_selector);
        objc.message<release_function>()(metal_device, release_selector);
        transcript << "command capture: process capture state is unavailable\n";
        return transcript.str();
    }
    capture.lifecycle_consumed = true;
    capture.objc = objc;
    capture.original_sync = nullptr;
    capture.original_async = nullptr;
    capture.utf8_selector = objc.register_selector("UTF8String");
    capture.registry_selector = objc.register_selector("registryID");
    capture.source.clear();
    capture.options = {};
    capture.moltenvk_image_path.clear();
    capture.metal_registry_id = 0u;
    capture.capture_count = 0u;
    capture.divergent = false;
    capture.original_sync = objc.method_set_implementation(
        sync_method, reinterpret_cast<void*>(&bioseq_capture_new_library_with_source));
    capture.original_async = objc.method_set_implementation(
        async_method, reinterpret_cast<void*>(&bioseq_capture_new_library_with_source_async));
    if (capture.original_sync == nullptr || capture.original_async == nullptr) {
        if (capture.original_sync != nullptr)
            (void)objc.method_set_implementation(sync_method, capture.original_sync);
        if (capture.original_async != nullptr)
            (void)objc.method_set_implementation(async_method, capture.original_async);
        using release_function = void (*)(objc_object, objc_selector);
        objc.message<release_function>()(metal_device, release_selector);
        transcript << "command capture: Metal source swizzle failed\n";
        return transcript.str();
    }
    command_capture_state* expected_unpublished = nullptr;
    if (!g_command_capture_state.compare_exchange_strong(expected_unpublished,
                                                         &capture,
                                                         std::memory_order_release,
                                                         std::memory_order_acquire)) {
        (void)objc.method_set_implementation(sync_method, capture.original_sync);
        (void)objc.method_set_implementation(async_method, capture.original_async);
        using release_function = void (*)(objc_object, objc_selector);
        objc.message<release_function>()(metal_device, release_selector);
        transcript << "command capture: state publication failed\n";
        return transcript.str();
    }

    capture_vulkan_runtime vk;
    std::string loader_path;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    const bool device_created =
        vk.load(loader_image_path, &loader_path) &&
        create_capture_device(vk,
                              &instance,
                              &device,
                              &device_uuid,
                              &pipeline_uuid,
                              &api_version,
                              &driver_version,
                              &vendor_id,
                              &device_id);
    if (device != VK_NULL_HANDLE)
        vk.destroy_device(device, nullptr);
    if (instance != VK_NULL_HANDLE)
        vk.destroy_instance(instance, nullptr);

    command_capture_state* published_state =
        g_command_capture_state.exchange(nullptr, std::memory_order_acq_rel);
    wait_for_command_capture_quiescence(capture);
    (void)objc.method_set_implementation(sync_method, capture.original_sync);
    (void)objc.method_set_implementation(async_method, capture.original_async);
    using release_function = void (*)(objc_object, objc_selector);
    objc.message<release_function>()(metal_device, release_selector);

    const bool methods_restored =
        objc.method_get_implementation(sync_method) == capture.original_sync &&
        objc.method_get_implementation(async_method) == capture.original_async;
    const bool publication_consistent = published_state == &capture;
    std::lock_guard<std::mutex> final_capture_guard(capture.mutex);
    const bool options_supported =
        capture.options.language_version != 0u &&
        capture.options.math_mode_available == capture.options.math_fp_functions_available &&
        capture.options.math_mode <= 2u && capture.options.math_fp_functions <= 1u &&
        capture.options.optimization_level <= 1u && capture.options.library_type == 0u &&
        capture.options.compile_symbol_visibility == 0u && capture.options.allow_undefined == 0u &&
        capture.options.max_total_threads == 0u && capture.options.enable_logging == 0u &&
        capture.options.preprocessor_macro_count == 0u &&
        capture.options.linked_library_count == 0u && capture.options.install_name_bytes == 0u;
    const bool source_valid =
        !capture.source.empty() && capture.source.find('\0') == std::string::npos &&
        capture.source.find("cmdCopyBufferBytes") != std::string::npos &&
        capture.source.find("vtxCmdBlitImage") != std::string::npos;
    const bool identities_valid =
        capture.metal_registry_id != 0u && !loader_path.empty() && loader_path.front() == '/' &&
        !capture.moltenvk_image_path.empty() && capture.moltenvk_image_path.front() == '/' &&
        api_version != 0u && driver_version != 0u &&
        std::any_of(device_uuid.begin(), device_uuid.end(), [](std::uint8_t value) {
            return value != 0u;
        }) &&
        std::any_of(pipeline_uuid.begin(), pipeline_uuid.end(), [](std::uint8_t value) {
            return value != 0u;
        });
    if (!device_created || !publication_consistent || !methods_restored ||
        capture.capture_count != 1u || capture.divergent || !options_supported || !source_valid ||
        !identities_valid ||
        !write_exclusive_file(source_output_path, capture.source)) {
        transcript << "command capture: qualification failed"
                   << " device_created=" << (device_created ? 1 : 0)
                   << " publication_consistent=" << (publication_consistent ? 1 : 0)
                   << " methods_restored=" << (methods_restored ? 1 : 0)
                   << " capture_count=" << capture.capture_count
                   << " divergent=" << (capture.divergent ? 1 : 0)
                   << " options_supported=" << (options_supported ? 1 : 0)
                   << " source_valid=" << (source_valid ? 1 : 0)
                   << " identities_valid=" << (identities_valid ? 1 : 0) << '\n';
        return transcript.str();
    }

    const captured_compile_options& o = capture.options;
    transcript << "COMMAND_CAPTURE"
               << " wire_revision=" << k_command_capture_wire_revision
               << " build=" << k_archive_helper_build_identity
               << " capture_count=" << capture.capture_count
               << " registry_id=" << capture.metal_registry_id
               << " language_version=" << o.language_version
               << " fast_math_enabled=" << o.fast_math_enabled
               << " math_mode_available=" << o.math_mode_available
               << " math_mode=" << o.math_mode
               << " math_fp_functions_available=" << o.math_fp_functions_available
               << " math_fp_functions=" << o.math_fp_functions
               << " optimization_level_available=" << o.optimization_level_available
               << " optimization_level=" << o.optimization_level
               << " preserve_invariance=" << o.preserve_invariance
               << " library_type_available=" << o.library_type_available
               << " library_type=" << o.library_type
               << " compile_symbol_visibility_available="
               << o.compile_symbol_visibility_available
               << " compile_symbol_visibility=" << o.compile_symbol_visibility
               << " allow_undefined_available=" << o.allow_undefined_available
               << " allow_undefined=" << o.allow_undefined
               << " max_total_threads_available=" << o.max_total_threads_available
               << " max_total_threads=" << o.max_total_threads
               << " enable_logging_available=" << o.enable_logging_available
               << " enable_logging=" << o.enable_logging
               << " preprocessor_macro_count=" << o.preprocessor_macro_count
               << " linked_library_count=" << o.linked_library_count
               << " install_name_bytes=" << o.install_name_bytes
               << " api_version=" << api_version
               << " driver_version=" << driver_version
               << " vendor_id=" << vendor_id
               << " device_id=" << device_id
               << " device_uuid_hex=" << encode_hex(device_uuid)
               << " pipeline_uuid_hex=" << encode_hex(pipeline_uuid)
               << " loader_path_hex=" << encode_hex(loader_path)
               << " moltenvk_path_hex=" << encode_hex(capture.moltenvk_image_path) << '\n';
    transcript << "RESULT: ok\n";
    return transcript.str();
}

} // namespace bioseq::gpu::metal_archive
