/**
 * @file metal_archive/implementation.cppm
 * @brief Private Objective-C/Metal implementation of archive construction.
 */
module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

module bioseq.gpu.metal_archive;

namespace bioseq::gpu::metal_archive {
namespace {

using objective_c_object = void*;
using objective_c_selector = void*;
using objective_c_class = void*;

/**
 * @brief Owns the dynamically resolved Objective-C and Metal process
 * interfaces.
 *
 * Library handles remain open for the lifetime of the exec-clean helper
 * process. Instances are confined to the calling thread and all callable fields
 * are valid only after load_frameworks() succeeds.
 */
struct objective_c_runtime {
    void* objective_c_library_handle = nullptr;
    void* metal_library_handle = nullptr;
    void* message_send_symbol = nullptr;
    objective_c_class (*get_class_function)(const char*) = nullptr;
    objective_c_selector (*register_selector_name_function)(const char*) = nullptr;
    objective_c_object (*create_system_default_device)() = nullptr;

    /**
     * @brief Loads the Objective-C runtime and Metal framework entry points.
     *
     * @param[out] transcript Receives the exact load-status transcript.
     * @return `true` when every required library and symbol is available.
     */
    [[nodiscard]] bool load_frameworks(std::ostringstream& transcript) {
        objective_c_library_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
        if (objective_c_library_handle == nullptr) {
            transcript << "load objc: failed: " << ::dlerror() << '\n';
            return false;
        }
        message_send_symbol = ::dlsym(objective_c_library_handle, "objc_msgSend");
        get_class_function = reinterpret_cast<objective_c_class (*)(const char*)>(
            ::dlsym(objective_c_library_handle, "objc_getClass"));
        register_selector_name_function = reinterpret_cast<objective_c_selector (*)(const char*)>(
            ::dlsym(objective_c_library_handle, "sel_registerName"));
        if (message_send_symbol == nullptr || get_class_function == nullptr ||
            register_selector_name_function == nullptr) {
            transcript << "load objc: missing runtime symbol\n";
            return false;
        }

        metal_library_handle =
            ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
        if (metal_library_handle == nullptr) {
            transcript << "load Metal.framework: failed: " << ::dlerror() << '\n';
            return false;
        }
        create_system_default_device = reinterpret_cast<objective_c_object (*)()>(
            ::dlsym(metal_library_handle, "MTLCreateSystemDefaultDevice"));
        if (create_system_default_device == nullptr) {
            transcript << "load Metal.framework: missing MTLCreateSystemDefaultDevice\n";
            return false;
        }
        transcript << "load: objc+Metal ok\n";
        return true;
    }

    /**
     * @brief Converts the resolved Objective-C message sender to a typed
     * callable.
     *
     * @tparam p_function_type Objective-C message signature used at the call
     * site.
     * @return Typed view of `objc_msgSend`.
     * @pre load_frameworks() succeeded.
     */
    template <class p_function_type>
    [[nodiscard]] p_function_type resolve_message_function() const noexcept {
        return reinterpret_cast<p_function_type>(message_send_symbol);
    }

    /**
     * @brief Registers an Objective-C selector name.
     *
     * @param[in] selector_name Null-terminated external selector spelling.
     * @return Objective-C selector handle.
     */
    [[nodiscard]] objective_c_selector register_selector(const char* selector_name) const noexcept {
        return register_selector_name_function(selector_name);
    }

    /**
     * @brief Resolves an Objective-C class by its external runtime name.
     *
     * @param[in] class_name Null-terminated external class spelling.
     * @return Objective-C class handle, or `nullptr` when unavailable.
     */
    [[nodiscard]] objective_c_class find_class(const char* class_name) const noexcept {
        return get_class_function(class_name);
    }
};

/**
 * @brief Reads the complete Metal-library byte image.
 *
 * @param[in] metallib_path Path to the Metal-library input.
 * @param[out] transcript Receives the exact read-status transcript.
 * @return File bytes, or an empty vector after an open, size, or read failure.
 */
[[nodiscard]] std::vector<unsigned char> read_metallib_bytes(std::string_view metallib_path,
                                                             std::ostringstream& transcript) {
    std::ifstream input(std::string(metallib_path), std::ios::binary);
    if (!input) {
        transcript << "read metallib: failed path=" << metallib_path << " errno=" << errno << '\n';
        return {};
    }
    input.seekg(0, std::ios::end);
    const auto file_size = input.tellg();
    if (file_size <= 0) {
        transcript << "read metallib: empty path=" << metallib_path << '\n';
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> file_bytes(static_cast<std::size_t>(file_size));
    input.read(reinterpret_cast<char*>(file_bytes.data()),
               static_cast<std::streamsize>(file_bytes.size()));
    if (!input) {
        transcript << "read metallib: short read path=" << metallib_path << '\n';
        return {};
    }
    transcript << "read metallib: ok bytes=" << file_bytes.size() << " path=" << metallib_path
               << '\n';
    return file_bytes;
}

/**
 * @brief Converts an NSString-compatible object to an owned UTF-8 string.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] ns_string NSString-compatible object, or `nullptr`.
 * @return UTF-8 bytes, or an empty string for a null object or null character
 * view.
 */
[[nodiscard]] std::string convert_ns_string_to_utf8(objective_c_runtime& objective_c,
                                                    objective_c_object ns_string) {
    if (ns_string == nullptr) {
        return {};
    }
    auto get_utf8_characters =
        objective_c
            .resolve_message_function<const char* (*)(objective_c_object, objective_c_selector)>();
    const char* utf8_characters =
        get_utf8_characters(ns_string, objective_c.register_selector("UTF8String"));
    return utf8_characters == nullptr ? std::string{} : std::string(utf8_characters);
}

/**
 * @brief Formats the localized description and reason of an NSError-compatible
 * object.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] ns_error NSError-compatible object, or `nullptr`.
 * @return Human-readable error text, or an empty string for a null object.
 */
[[nodiscard]] std::string format_ns_error_description(objective_c_runtime& objective_c,
                                                      objective_c_object ns_error) {
    if (ns_error == nullptr) {
        return {};
    }
    auto read_object_property = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_object, objective_c_selector)>();
    objective_c_object description_object =
        read_object_property(ns_error, objective_c.register_selector("localizedDescription"));
    objective_c_object reason_object =
        read_object_property(ns_error, objective_c.register_selector("localizedFailureReason"));
    std::string description = convert_ns_string_to_utf8(objective_c, description_object);
    const std::string reason_text = convert_ns_string_to_utf8(objective_c, reason_object);
    if (!reason_text.empty()) {
        description += " reason=";
        description += reason_text;
    }
    return description;
}

/** @brief Stable NSError domain and numeric code captured for the negative
 * control. */
struct ns_error_identity {
    std::string domain;
    std::int64_t code = 0;
};

/**
 * @brief Captures the stable domain and code of an NSError-compatible object.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] ns_error NSError-compatible object, or `nullptr`.
 * @return Error identity when a nonempty domain is available.
 */
[[nodiscard]] std::optional<ns_error_identity>
capture_ns_error_identity(objective_c_runtime& objective_c, objective_c_object ns_error) {
    if (ns_error == nullptr)
        return std::nullopt;
    auto read_object_property = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_object, objective_c_selector)>();
    auto read_code =
        objective_c.resolve_message_function<long (*)(objective_c_object, objective_c_selector)>();
    const std::string domain = convert_ns_string_to_utf8(
        objective_c, read_object_property(ns_error, objective_c.register_selector("domain")));
    if (domain.empty())
        return std::nullopt;
    return ns_error_identity{
        .domain = domain,
        .code =
            static_cast<std::int64_t>(read_code(ns_error, objective_c.register_selector("code"))),
    };
}

/**
 * @brief Encodes an arbitrary byte string as lower-case hexadecimal text.
 *
 * @param[in] bytes Byte sequence to encode.
 * @return Two hexadecimal characters per input byte.
 */
[[nodiscard]] std::string encode_hex(std::string_view bytes) {
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes) {
        encoded << std::setw(2) << static_cast<unsigned>(byte);
    }
    return encoded.str();
}

/**
 * @brief Creates an autoreleased NSString-compatible object from UTF-8 bytes.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] value UTF-8 value copied by the Objective-C runtime.
 * @return NSString-compatible object, or `nullptr` on failure.
 */
[[nodiscard]] objective_c_object make_ns_string(objective_c_runtime& objective_c,
                                                std::string_view value) {
    objective_c_class ns_string = objective_c.find_class("NSString");
    if (ns_string == nullptr) {
        return nullptr;
    }
    std::string owned_value(value);
    auto create_string = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_class, objective_c_selector, const char*)>();
    return create_string(
        ns_string, objective_c.register_selector("stringWithUTF8String:"), owned_value.c_str());
}

/**
 * @brief Creates a file NSURL-compatible object for a filesystem path.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] path Filesystem path copied by the Objective-C runtime.
 * @return NSURL-compatible object, or `nullptr` on failure.
 */
[[nodiscard]] objective_c_object make_file_url(objective_c_runtime& objective_c,
                                               std::string_view path) {
    objective_c_class ns_url = objective_c.find_class("NSURL");
    objective_c_object ns_path = make_ns_string(objective_c, path);
    if (ns_url == nullptr || ns_path == nullptr) {
        return nullptr;
    }
    auto create_url = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_class, objective_c_selector, objective_c_object)>();
    return create_url(ns_url, objective_c.register_selector("fileURLWithPath:"), ns_path);
}

/**
 * @brief Creates an empty MTLBinaryArchive ready to receive compute pipeline
 * functions.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] device MTLDevice-compatible object.
 * @param[out] transcript Receives the exact archive-creation transcript.
 * @return MTLBinaryArchive-compatible object, or `nullptr` on failure.
 */
[[nodiscard]] objective_c_object make_empty_binary_archive(objective_c_runtime& objective_c,
                                                           objective_c_object device,
                                                           std::ostringstream& transcript) {
    objective_c_class descriptor_class = objective_c.find_class("MTLBinaryArchiveDescriptor");
    if (descriptor_class == nullptr) {
        transcript << "archive descriptor: class missing\n";
        return nullptr;
    }
    auto allocate_object = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_class, objective_c_selector)>();
    objective_c_object descriptor =
        allocate_object(descriptor_class, objective_c.register_selector("new"));
    if (descriptor == nullptr) {
        transcript << "archive descriptor: allocation failed\n";
        return nullptr;
    }
    transcript << "archive descriptor: url=nil (create empty archive)\n";

    objective_c_object ns_error = nullptr;
    auto create_archive = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_object, objective_c_selector, objective_c_object, objective_c_object*)>();
    objective_c_object archive =
        create_archive(device,
                       objective_c.register_selector("newBinaryArchiveWithDescriptor:error:"),
                       descriptor,
                       &ns_error);
    if (archive == nullptr) {
        transcript << "newBinaryArchiveWithDescriptor: failed error="
                   << format_ns_error_description(objective_c, ns_error) << '\n';
        return nullptr;
    }
    transcript << "newBinaryArchiveWithDescriptor: ok\n";
    return archive;
}

/**
 * @brief Opens the serialized archive bytes that are candidates for
 * publication.
 *
 * Qualification deliberately exercises the serialized file instead of the live
 * archive object that populated it.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] device MTLDevice-compatible object.
 * @param[in] archive_path Serialized archive path.
 * @param[out] transcript Receives the exact archive-open transcript.
 * @return MTLBinaryArchive-compatible object, or `nullptr` on failure.
 */
[[nodiscard]] objective_c_object open_serialized_binary_archive(objective_c_runtime& objective_c,
                                                                objective_c_object device,
                                                                std::string_view archive_path,
                                                                std::ostringstream& transcript) {
    objective_c_class descriptor_class = objective_c.find_class("MTLBinaryArchiveDescriptor");
    if (descriptor_class == nullptr) {
        transcript << "archive reopen descriptor: class missing\n";
        return nullptr;
    }
    auto allocate_object = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_class, objective_c_selector)>();
    objective_c_object descriptor =
        allocate_object(descriptor_class, objective_c.register_selector("new"));
    objective_c_object file_url = make_file_url(objective_c, archive_path);
    if (descriptor == nullptr || file_url == nullptr) {
        transcript << "archive reopen descriptor: allocation/URL failed path=" << archive_path
                   << '\n';
        return nullptr;
    }
    auto set_object_property = objective_c.resolve_message_function<void (*)(
        objective_c_object, objective_c_selector, objective_c_object)>();
    // MTLBinaryArchiveDescriptor declares the property as `url` (not `URL`),
    // so the Objective-C setter selector is exactly `setUrl:`.
    set_object_property(descriptor, objective_c.register_selector("setUrl:"), file_url);

    objective_c_object ns_error = nullptr;
    auto create_archive = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_object, objective_c_selector, objective_c_object, objective_c_object*)>();
    objective_c_object archive =
        create_archive(device,
                       objective_c.register_selector("newBinaryArchiveWithDescriptor:error:"),
                       descriptor,
                       &ns_error);
    if (archive == nullptr) {
        transcript << "archive reopen: failed path=" << archive_path
                   << " error=" << format_ns_error_description(objective_c, ns_error) << '\n';
        return nullptr;
    }
    transcript << "archive reopen: ok path=" << archive_path << '\n';
    return archive;
}

/**
 * @brief Creates a compute-pipeline descriptor for one function and optional
 * archive.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] function MTLFunction-compatible object.
 * @param[in] archive Optional MTLBinaryArchive-compatible object.
 * @param[in] maximum_total_threads_per_threadgroup Metal thread-group limit in
 * threads.
 * @param[in] is_threadgroup_size_multiple_of_thread_execution_width Whether Metal may assume an
 * exact thread-execution-width multiple.
 * @param[out] transcript Receives the exact descriptor-construction transcript.
 * @return MTLComputePipelineDescriptor-compatible object, or `nullptr` on
 * failure.
 */
[[nodiscard]] objective_c_object
make_compute_pipeline_descriptor(objective_c_runtime& objective_c,
                                 objective_c_object function,
                                 objective_c_object archive,
                                 std::uint64_t maximum_total_threads_per_threadgroup,
                                 bool is_threadgroup_size_multiple_of_thread_execution_width,
                                 std::ostringstream& transcript) {
    objective_c_class descriptor_class = objective_c.find_class("MTLComputePipelineDescriptor");
    if (descriptor_class == nullptr) {
        transcript << "pipeline descriptor: class missing\n";
        return nullptr;
    }
    auto allocate_object = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_class, objective_c_selector)>();
    objective_c_object descriptor =
        allocate_object(descriptor_class, objective_c.register_selector("new"));
    if (descriptor == nullptr) {
        transcript << "pipeline descriptor: allocation failed\n";
        return nullptr;
    }

    auto set_object_property = objective_c.resolve_message_function<void (*)(
        objective_c_object, objective_c_selector, objective_c_object)>();
    set_object_property(descriptor, objective_c.register_selector("setComputeFunction:"), function);
    transcript << "pipeline descriptor: computeFunction set\n";
    if (maximum_total_threads_per_threadgroup > 0) {
        auto set_unsigned_long_property = objective_c.resolve_message_function<void (*)(
            objective_c_object, objective_c_selector, unsigned long)>();
        set_unsigned_long_property(
            descriptor,
            objective_c.register_selector("setMaxTotalThreadsPerThreadgroup:"),
            static_cast<unsigned long>(maximum_total_threads_per_threadgroup));
        transcript << "pipeline descriptor: maxTotalThreadsPerThreadgroup="
                   << maximum_total_threads_per_threadgroup << '\n';
    }
    if (is_threadgroup_size_multiple_of_thread_execution_width) {
        auto set_boolean_property = objective_c.resolve_message_function<void (*)(
            objective_c_object, objective_c_selector, signed char)>();
        set_boolean_property(
            descriptor,
            objective_c.register_selector("setThreadGroupSizeIsMultipleOfThreadExecutionWidth:"),
            static_cast<signed char>(1));
        transcript << "pipeline descriptor: "
                      "threadGroupSizeIsMultipleOfThreadExecutionWidth=true\n";
    }

    if (archive != nullptr) {
        objective_c_class array_class = objective_c.find_class("NSArray");
        if (array_class == nullptr) {
            transcript << "pipeline descriptor: NSArray missing\n";
            return nullptr;
        }
        auto array_with_object = objective_c.resolve_message_function<objective_c_object (*)(
            objective_c_class, objective_c_selector, objective_c_object)>();
        objective_c_object archive_array = array_with_object(
            array_class, objective_c.register_selector("arrayWithObject:"), archive);
        if (archive_array == nullptr) {
            transcript << "pipeline descriptor: archive array failed\n";
            return nullptr;
        }
        set_object_property(
            descriptor, objective_c.register_selector("setBinaryArchives:"), archive_array);
        transcript << "pipeline descriptor: binaryArchives attached\n";
    }
    return descriptor;
}

/**
 * @brief Loads one compute function from a serialized Metal library.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] device MTLDevice-compatible object.
 * @param[in] metallib_path Metal-library input path.
 * @param[in] entry_name Compute entry-point name; an empty value selects
 * `main0`.
 * @param[out] transcript Receives the exact library-load transcript.
 * @return MTLFunction-compatible object, or `nullptr` on failure.
 */
[[nodiscard]] objective_c_object load_function_from_metallib(objective_c_runtime& objective_c,
                                                             objective_c_object device,
                                                             std::string_view metallib_path,
                                                             std::string_view entry_name,
                                                             std::ostringstream& transcript) {
    auto metallib_bytes = read_metallib_bytes(metallib_path, transcript);
    if (metallib_bytes.empty()) {
        return nullptr;
    }

    dispatch_data_t metallib_dispatch_data = dispatch_data_create(
        metallib_bytes.data(), metallib_bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (metallib_dispatch_data == nullptr) {
        transcript << "dispatch_data_create: failed\n";
        return nullptr;
    }

    objective_c_object ns_error = nullptr;
    auto create_library = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_object, objective_c_selector, dispatch_data_t, objective_c_object*)>();
    objective_c_object library =
        create_library(device,
                       objective_c.register_selector("newLibraryWithData:error:"),
                       metallib_dispatch_data,
                       &ns_error);
    dispatch_release(metallib_dispatch_data);
    if (library == nullptr) {
        transcript << "newLibraryWithData: failed error="
                   << format_ns_error_description(objective_c, ns_error) << '\n';
        return nullptr;
    }
    transcript << "newLibraryWithData: ok\n";

    objective_c_object function_name =
        make_ns_string(objective_c, entry_name.empty() ? "main0" : entry_name);
    if (function_name == nullptr) {
        transcript << "newFunctionWithName: NSString failed entry=" << entry_name << '\n';
        return nullptr;
    }
    auto create_function = objective_c.resolve_message_function<objective_c_object (*)(
        objective_c_object, objective_c_selector, objective_c_object)>();
    objective_c_object function = create_function(
        library, objective_c.register_selector("newFunctionWithName:"), function_name);
    if (function == nullptr) {
        transcript << "newFunctionWithName(" << entry_name << "): failed\n";
        return nullptr;
    }
    transcript << "newFunctionWithName(" << entry_name << "): ok\n";
    return function;
}

/**
 * @brief Adds one compute-pipeline descriptor to a mutable binary archive.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] archive MTLBinaryArchive-compatible object.
 * @param[in] pipeline_descriptor MTLComputePipelineDescriptor-compatible
 * object.
 * @param[out] transcript Receives the exact archive-update transcript.
 * @return `true` when Metal accepts the descriptor.
 */
[[nodiscard]] bool add_compute_pipeline_to_archive(objective_c_runtime& objective_c,
                                                   objective_c_object archive,
                                                   objective_c_object pipeline_descriptor,
                                                   std::ostringstream& transcript) {
    objective_c_object ns_error = nullptr;
    auto add_pipeline_functions = objective_c.resolve_message_function<signed char (*)(
        objective_c_object, objective_c_selector, objective_c_object, objective_c_object*)>();
    const signed char did_add_pipeline = add_pipeline_functions(
        archive,
        objective_c.register_selector("addComputePipelineFunctionsWithDescriptor:error:"),
        pipeline_descriptor,
        &ns_error);
    if (!did_add_pipeline) {
        transcript << "addComputePipelineFunctionsWithDescriptor: failed error="
                   << format_ns_error_description(objective_c, ns_error) << '\n';
        return false;
    }
    transcript << "addComputePipelineFunctionsWithDescriptor: ok\n";
    return true;
}

/**
 * @brief Serializes a binary archive to the requested file URL.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] archive MTLBinaryArchive-compatible object.
 * @param[in] archive_path Output path; an empty path preserves the existing
 * skip behavior.
 * @param[out] transcript Receives the exact serialization transcript.
 * @return `true` when serialization succeeds or is skipped for an empty path.
 */
[[nodiscard]] bool serialize_archive(objective_c_runtime& objective_c,
                                     objective_c_object archive,
                                     std::string_view archive_path,
                                     std::ostringstream& transcript) {
    if (archive_path.empty()) {
        transcript << "serializeToURL: skipped (no path)\n";
        return true;
    }
    objective_c_object file_url = make_file_url(objective_c, archive_path);
    if (file_url == nullptr) {
        transcript << "serializeToURL: file URL failed path=" << archive_path << '\n';
        return false;
    }
    objective_c_object ns_error = nullptr;
    auto serialize_to_url = objective_c.resolve_message_function<signed char (*)(
        objective_c_object, objective_c_selector, objective_c_object, objective_c_object*)>();
    const signed char did_serialize = serialize_to_url(
        archive, objective_c.register_selector("serializeToURL:error:"), file_url, &ns_error);
    if (!did_serialize) {
        transcript << "serializeToURL: failed error="
                   << format_ns_error_description(objective_c, ns_error) << '\n';
        return false;
    }
    transcript << "serializeToURL: ok path=" << archive_path << '\n';
    return true;
}

/**
 * @brief Validates the expected Metal archive hit or miss for one pipeline
 * descriptor.
 *
 * @param[in] objective_c Loaded Objective-C runtime interface.
 * @param[in] device MTLDevice-compatible object.
 * @param[in] pipeline_descriptor MTLComputePipelineDescriptor-compatible
 * object.
 * @param[in] control_label Stable transcript label for the qualification
 * control.
 * @param[in] is_hit_expected Whether a pipeline object must be returned.
 * @param[out] transcript Receives the exact control transcript.
 * @return `true` when Metal exhibits the expected hit or classified miss.
 */
[[nodiscard]] bool validate_compute_pipeline_archive_hit(objective_c_runtime& objective_c,
                                                         objective_c_object device,
                                                         objective_c_object pipeline_descriptor,
                                                         std::string_view control_label,
                                                         bool is_hit_expected,
                                                         std::ostringstream& transcript) {
    // MTLPipelineOptionFailOnBinaryArchiveMiss.  Keep the numeric ABI here so
    // this exec-clean helper remains independent of Objective-C Metal headers.
    constexpr unsigned long k_fail_on_binary_archive_miss = 1UL << 2u;
    objective_c_object ns_error = nullptr;
    objective_c_object pipeline_reflection = nullptr;
    auto create_pipeline =
        objective_c.resolve_message_function<objective_c_object (*)(objective_c_object,
                                                                    objective_c_selector,
                                                                    objective_c_object,
                                                                    unsigned long,
                                                                    objective_c_object*,
                                                                    objective_c_object*)>();
    objective_c_object pipeline =
        create_pipeline(device,
                        objective_c.register_selector(
                            "newComputePipelineStateWithDescriptor:options:reflection:error:"),
                        pipeline_descriptor,
                        k_fail_on_binary_archive_miss,
                        &pipeline_reflection,
                        &ns_error);
    if (is_hit_expected && pipeline == nullptr) {
        transcript << control_label << ": failed (archive miss) error="
                   << format_ns_error_description(objective_c, ns_error) << '\n';
        return false;
    }
    if (!is_hit_expected && pipeline != nullptr) {
        transcript << control_label
                   << ": failed (empty archive unexpectedly satisfied descriptor)\n";
        return false;
    }
    if (is_hit_expected) {
        transcript << control_label << ": ok (FailOnBinaryArchiveMiss hit)\n";
    } else {
        const auto error_identity = capture_ns_error_identity(objective_c, ns_error);
        if (!error_identity) {
            transcript << control_label
                       << ": failed (empty-archive rejection has no NSError domain/code)\n";
            return false;
        }
        transcript << control_label << ": ok (FailOnBinaryArchiveMiss rejected empty archive)"
                   << " error=" << format_ns_error_description(objective_c, ns_error) << '\n';
        transcript << "NEGATIVE_CONTROL_NSError domain_hex=" << encode_hex(error_identity->domain)
                   << " code=" << error_identity->code << '\n';
    }
    return true;
}

} // namespace

/** @copydoc bioseq::gpu::metal_archive::build_binary_archive */
std::string build_binary_archive(std::string_view metallib_path,
                                 std::string_view archive_path,
                                 std::string_view entry_name,
                                 std::uint64_t maximum_total_threads_per_threadgroup,
                                 bool is_threadgroup_size_multiple_of_thread_execution_width) {
    std::ostringstream transcript;
    transcript << "pid=" << static_cast<long>(::getpid()) << '\n';

    objective_c_runtime objective_c;
    if (!objective_c.load_frameworks(transcript)) {
        return transcript.str();
    }

    objective_c_object device = objective_c.create_system_default_device();
    if (device == nullptr) {
        transcript << "MTLCreateSystemDefaultDevice: failed\n";
        return transcript.str();
    }
    transcript << "MTLCreateSystemDefaultDevice: ok\n";
    using registry_id_function_type =
        unsigned long long (*)(objective_c_object, objective_c_selector);
    const std::uint64_t registry_id = static_cast<std::uint64_t>(
        objective_c.resolve_message_function<registry_id_function_type>()(
            device, objective_c.register_selector("registryID")));
    if (registry_id == 0u) {
        transcript << "MTLDevice registryID: failed\n";
        return transcript.str();
    }
    transcript << "HELPER_QUALIFICATION wire_revision=" << k_archive_helper_wire_revision
               << " build=" << k_archive_helper_build_identity << " registry_id=" << registry_id
               << '\n';

    objective_c_object function =
        load_function_from_metallib(objective_c, device, metallib_path, entry_name, transcript);
    if (function == nullptr) {
        return transcript.str();
    }

    if (!archive_path.empty()) {
        ::unlink(std::string(archive_path).c_str());
        transcript << "archive pre-unlink: path=" << archive_path << '\n';
    }

    objective_c_object archive = make_empty_binary_archive(objective_c, device, transcript);
    if (archive == nullptr) {
        return transcript.str();
    }

    objective_c_object pipeline_descriptor =
        make_compute_pipeline_descriptor(objective_c,
                                         function,
                                         archive,
                                         maximum_total_threads_per_threadgroup,
                                         is_threadgroup_size_multiple_of_thread_execution_width,
                                         transcript);
    if (pipeline_descriptor == nullptr) {
        return transcript.str();
    }

    if (!add_compute_pipeline_to_archive(objective_c, archive, pipeline_descriptor, transcript)) {
        return transcript.str();
    }
    if (!serialize_archive(objective_c, archive, archive_path, transcript)) {
        return transcript.str();
    }
    objective_c_object reopened_archive =
        open_serialized_binary_archive(objective_c, device, archive_path, transcript);
    if (reopened_archive == nullptr) {
        return transcript.str();
    }

    objective_c_object qualified_pipeline_descriptor =
        make_compute_pipeline_descriptor(objective_c,
                                         function,
                                         reopened_archive,
                                         maximum_total_threads_per_threadgroup,
                                         is_threadgroup_size_multiple_of_thread_execution_width,
                                         transcript);
    if (qualified_pipeline_descriptor == nullptr ||
        !validate_compute_pipeline_archive_hit(objective_c,
                                               device,
                                               qualified_pipeline_descriptor,
                                               "qualified archive positive control",
                                               true,
                                               transcript)) {
        return transcript.str();
    }

    objective_c_object empty_control_archive =
        make_empty_binary_archive(objective_c, device, transcript);
    if (empty_control_archive == nullptr) {
        return transcript.str();
    }
    objective_c_object empty_control_pipeline_descriptor =
        make_compute_pipeline_descriptor(objective_c,
                                         function,
                                         empty_control_archive,
                                         maximum_total_threads_per_threadgroup,
                                         is_threadgroup_size_multiple_of_thread_execution_width,
                                         transcript);
    if (empty_control_pipeline_descriptor == nullptr ||
        !validate_compute_pipeline_archive_hit(objective_c,
                                               device,
                                               empty_control_pipeline_descriptor,
                                               "same-descriptor empty-archive negative control",
                                               false,
                                               transcript)) {
        return transcript.str();
    }
    transcript << "RESULT: ok\n";
    return transcript.str();
}

} // namespace bioseq::gpu::metal_archive
