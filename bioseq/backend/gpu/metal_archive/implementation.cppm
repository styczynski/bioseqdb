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
 * @brief Objective-C and Metal functions loaded by the archive-builder process.
 *
 * Call `load_frameworks()` successfully before using the function pointers.
 * The handles remain open until the helper exits. Use an instance only on its
 * calling thread.
 */
struct objective_c_runtime {
    void* objective_c_library_handle = nullptr;
    void* metal_library_handle = nullptr;
    void* message_send_symbol = nullptr;
    objective_c_class (*get_class_function)(const char*) = nullptr;
    objective_c_selector (*register_selector_name_function)(const char*) = nullptr;
    objective_c_object (*create_system_default_device)() = nullptr;

    /**
     * @brief Loads the Objective-C and Metal functions needed by this helper.
     * @param[out] transcript Receives load results and errors.
     * @return True when every required function is available.
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
     * @brief Casts `objc_msgSend` to the signature needed for this call.
     * @tparam p_function_type Objective-C method signature.
     * @pre `load_frameworks()` succeeded.
     */
    template <class p_function_type>
    [[nodiscard]] p_function_type resolve_message_function() const noexcept {
        return reinterpret_cast<p_function_type>(message_send_symbol);
    }

    /**
     * @brief Looks up an Objective-C selector by its null-terminated name.
     */
    [[nodiscard]] objective_c_selector register_selector(const char* selector_name) const noexcept {
        return register_selector_name_function(selector_name);
    }

    /**
     * @brief Looks up an Objective-C class by its null-terminated name.
     * @return Class handle, or null if it is unavailable.
     */
    [[nodiscard]] objective_c_class find_class(const char* class_name) const noexcept {
        return get_class_function(class_name);
    }
};

/**
 * @brief Reads a complete Metal-library file into memory.
 * @param[out] transcript Receives read results and errors.
 * @return File bytes, or an empty vector if opening, sizing, or reading fails.
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
 * @brief Copies an NSString's UTF-8 text into a C++ string.
 *
 * Returns an empty string for a null object or a null `UTF8String` result.
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
 * @brief Reads an NSError description and appends its failure reason.
 *
 * Returns an empty string for a null error.
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

/** @brief NSError domain and code reported by the empty-archive check. */
struct ns_error_identity {
    std::string domain;
    std::int64_t code = 0;
};

/**
 * @brief Reads an NSError domain and code.
 * @return No value for a null error or empty domain.
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
 * @brief Creates an autoreleased NSString from UTF-8 text.
 * @return Null if the class is unavailable or string creation fails.
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
 * @brief Creates a file URL from a filesystem path.
 * @return Null if the URL class or path string is unavailable.
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
 * @brief Creates an empty Metal binary archive for compute pipelines.
 * @param[out] transcript Receives creation results and errors.
 * @return Archive object, or null on failure.
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
 * @brief Reopens the saved archive to test the file that will be published.
 * @param[out] transcript Receives open results and errors.
 * @return Archive object, or null on failure.
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
    // The property is named `url`, so its Objective-C setter is `setUrl:`.
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
 * @brief Creates a compute-pipeline descriptor with its function and thread-group settings.
 *
 * Attaches the supplied archive when it is non-null.
 * @param[in] maximum_total_threads_per_threadgroup Thread limit. Zero leaves the default.
 * @param[in] is_threadgroup_size_multiple_of_thread_execution_width Whether Metal
 *         may assume a multiple of its execution width.
 * @param[out] transcript Receives setup results and errors.
 * @return Descriptor object, or null on failure.
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
 * @brief Loads a compute function from a Metal-library file.
 * @param[in] entry_name Function name, or empty to select `main0`.
 * @param[out] transcript Receives load results and errors.
 * @return Function object, or null on failure.
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
 * @brief Adds the descriptor's compute function to the archive.
 * @param[out] transcript Receives results and Metal errors.
 * @return True when Metal accepts the descriptor.
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
 * @brief Saves the archive to a file.
 *
 * An empty path skips the write and returns true. Otherwise, failure to create
 * the URL or serialize the archive returns false. Errors go in `transcript`.
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
 * @brief Checks whether the archive satisfies a pipeline request.
 *
 * `FailOnBinaryArchiveMiss` prevents compilation from hiding a missing entry.
 * A positive check requires a pipeline. An empty-archive check requires
 * rejection with an NSError domain and code, which are written to the transcript.
 */
[[nodiscard]] bool validate_compute_pipeline_archive_hit(objective_c_runtime& objective_c,
                                                         objective_c_object device,
                                                         objective_c_object pipeline_descriptor,
                                                         std::string_view control_label,
                                                         bool is_hit_expected,
                                                         std::ostringstream& transcript) {
    // Numeric value of MTLPipelineOptionFailOnBinaryArchiveMiss, used here
    // to avoid importing Objective-C Metal headers.
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
