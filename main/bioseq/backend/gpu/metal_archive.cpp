#include "gpu/metal_archive.hpp"

#include <dispatch/dispatch.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace bioseq::gpu::metal_archive {
namespace {

using Obj = void*;
using Sel = void*;
using Class = void*;

struct ObjcRuntime {
    void* objc_handle = nullptr;
    void* metal_handle = nullptr;
    void* msg_send = nullptr;
    Class (*get_class)(const char*) = nullptr;
    Sel (*sel_register_name)(const char*) = nullptr;
    Obj (*mtl_create_system_default_device)() = nullptr;

    [[nodiscard]] bool load(std::ostringstream& out) {
        objc_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
        if (objc_handle == nullptr) {
            out << "load objc: failed: " << ::dlerror() << '\n';
            return false;
        }
        msg_send = ::dlsym(objc_handle, "objc_msgSend");
        get_class = reinterpret_cast<Class (*)(const char*)>(
            ::dlsym(objc_handle, "objc_getClass"));
        sel_register_name = reinterpret_cast<Sel (*)(const char*)>(
            ::dlsym(objc_handle, "sel_registerName"));
        if (msg_send == nullptr || get_class == nullptr || sel_register_name == nullptr) {
            out << "load objc: missing runtime symbol\n";
            return false;
        }

        metal_handle = ::dlopen(
            "/System/Library/Frameworks/Metal.framework/Metal",
            RTLD_NOW | RTLD_LOCAL);
        if (metal_handle == nullptr) {
            out << "load Metal.framework: failed: " << ::dlerror() << '\n';
            return false;
        }
        mtl_create_system_default_device = reinterpret_cast<Obj (*)()>(
            ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
        if (mtl_create_system_default_device == nullptr) {
            out << "load Metal.framework: missing MTLCreateSystemDefaultDevice\n";
            return false;
        }
        out << "load: objc+Metal ok\n";
        return true;
    }

    template <class Fn>
    [[nodiscard]] Fn msg() const noexcept {
        return reinterpret_cast<Fn>(msg_send);
    }

    [[nodiscard]] Sel sel(const char* name) const noexcept {
        return sel_register_name(name);
    }

    [[nodiscard]] Class cls(const char* name) const noexcept {
        return get_class(name);
    }
};

[[nodiscard]] std::vector<unsigned char> read_file(std::string_view path, std::ostringstream& out) {
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) {
        out << "read metallib: failed path=" << path << " errno=" << errno << '\n';
        return {};
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0) {
        out << "read metallib: empty path=" << path << '\n';
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        out << "read metallib: short read path=" << path << '\n';
        return {};
    }
    out << "read metallib: ok bytes=" << bytes.size() << " path=" << path << '\n';
    return bytes;
}

[[nodiscard]] std::string ns_string_utf8(ObjcRuntime& objc, Obj ns_string) {
    if (ns_string == nullptr) {
        return {};
    }
    auto utf8 = objc.msg<const char* (*)(Obj, Sel)>();
    const char* cstr = utf8(ns_string, objc.sel("UTF8String"));
    return cstr == nullptr ? std::string{} : std::string(cstr);
}

[[nodiscard]] std::string ns_error_description(ObjcRuntime& objc, Obj ns_error) {
    if (ns_error == nullptr) {
        return {};
    }
    auto get_obj = objc.msg<Obj (*)(Obj, Sel)>();
    Obj desc = get_obj(ns_error, objc.sel("localizedDescription"));
    Obj reason = get_obj(ns_error, objc.sel("localizedFailureReason"));
    std::string result = ns_string_utf8(objc, desc);
    const std::string reason_text = ns_string_utf8(objc, reason);
    if (!reason_text.empty()) {
        result += " reason=";
        result += reason_text;
    }
    return result;
}

struct NSErrorFingerprint {
    std::string domain;
    std::int64_t code = 0;
};

[[nodiscard]] std::optional<NSErrorFingerprint> ns_error_fingerprint(
    ObjcRuntime& objc,
    Obj ns_error)
{
    if (ns_error == nullptr) return std::nullopt;
    auto get_obj = objc.msg<Obj (*)(Obj, Sel)>();
    auto get_code = objc.msg<long (*)(Obj, Sel)>();
    const std::string domain = ns_string_utf8(
        objc, get_obj(ns_error, objc.sel("domain")));
    if (domain.empty()) return std::nullopt;
    return NSErrorFingerprint{
        .domain = domain,
        .code = static_cast<std::int64_t>(
            get_code(ns_error, objc.sel("code"))),
    };
}

[[nodiscard]] std::string hex_encode(std::string_view bytes)
{
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes) {
        encoded << std::setw(2) << static_cast<unsigned>(byte);
    }
    return encoded.str();
}

[[nodiscard]] Obj make_ns_string(ObjcRuntime& objc, std::string_view value) {
    Class ns_string = objc.cls("NSString");
    if (ns_string == nullptr) {
        return nullptr;
    }
    std::string owned(value);
    auto fn = objc.msg<Obj (*)(Class, Sel, const char*)>();
    return fn(ns_string, objc.sel("stringWithUTF8String:"), owned.c_str());
}

[[nodiscard]] Obj make_file_url(ObjcRuntime& objc, std::string_view path) {
    Class ns_url = objc.cls("NSURL");
    Obj ns_path = make_ns_string(objc, path);
    if (ns_url == nullptr || ns_path == nullptr) {
        return nullptr;
    }
    auto fn = objc.msg<Obj (*)(Class, Sel, Obj)>();
    return fn(ns_url, objc.sel("fileURLWithPath:"), ns_path);
}

// Create an empty MTLBinaryArchive ready to receive compute pipeline functions.
[[nodiscard]] Obj make_empty_archive(ObjcRuntime& objc, Obj device, std::ostringstream& out) {
    Class desc_cls = objc.cls("MTLBinaryArchiveDescriptor");
    if (desc_cls == nullptr) {
        out << "archive descriptor: class missing\n";
        return nullptr;
    }
    auto msg_new = objc.msg<Obj (*)(Class, Sel)>();
    Obj desc = msg_new(desc_cls, objc.sel("new"));
    if (desc == nullptr) {
        out << "archive descriptor: allocation failed\n";
        return nullptr;
    }
    out << "archive descriptor: url=nil (create empty archive)\n";

    Obj err = nullptr;
    auto new_archive = objc.msg<Obj (*)(Obj, Sel, Obj, Obj*)>();
    Obj archive = new_archive(device, objc.sel("newBinaryArchiveWithDescriptor:error:"), desc, &err);
    if (archive == nullptr) {
        out << "newBinaryArchiveWithDescriptor: failed error="
            << ns_error_description(objc, err) << '\n';
        return nullptr;
    }
    out << "newBinaryArchiveWithDescriptor: ok\n";
    return archive;
}

// Reopen the bytes that will be published.  Qualification must exercise the
// serialized file, not the still-live archive object that populated it.
[[nodiscard]] Obj reopen_archive(
    ObjcRuntime& objc,
    Obj device,
    std::string_view archive_path,
    std::ostringstream& out)
{
    Class desc_cls = objc.cls("MTLBinaryArchiveDescriptor");
    if (desc_cls == nullptr) {
        out << "archive reopen descriptor: class missing\n";
        return nullptr;
    }
    auto msg_new = objc.msg<Obj (*)(Class, Sel)>();
    Obj desc = msg_new(desc_cls, objc.sel("new"));
    Obj url = make_file_url(objc, archive_path);
    if (desc == nullptr || url == nullptr) {
        out << "archive reopen descriptor: allocation/URL failed path="
            << archive_path << '\n';
        return nullptr;
    }
    auto set_obj = objc.msg<void (*)(Obj, Sel, Obj)>();
    // MTLBinaryArchiveDescriptor declares the property as `url` (not `URL`),
    // so the Objective-C setter selector is exactly `setUrl:`.
    set_obj(desc, objc.sel("setUrl:"), url);

    Obj err = nullptr;
    auto new_archive = objc.msg<Obj (*)(Obj, Sel, Obj, Obj*)>();
    Obj archive = new_archive(
        device, objc.sel("newBinaryArchiveWithDescriptor:error:"), desc, &err);
    if (archive == nullptr) {
        out << "archive reopen: failed path=" << archive_path
            << " error=" << ns_error_description(objc, err) << '\n';
        return nullptr;
    }
    out << "archive reopen: ok path=" << archive_path << '\n';
    return archive;
}

[[nodiscard]] Obj make_pipeline_descriptor(ObjcRuntime& objc, Obj function, Obj archive,
                                           std::uint64_t max_total_threads_per_threadgroup,
                                           bool threadgroup_size_is_multiple,
                                           std::ostringstream& out) {
    Class desc_cls = objc.cls("MTLComputePipelineDescriptor");
    if (desc_cls == nullptr) {
        out << "pipeline descriptor: class missing\n";
        return nullptr;
    }
    auto msg_new = objc.msg<Obj (*)(Class, Sel)>();
    Obj desc = msg_new(desc_cls, objc.sel("new"));
    if (desc == nullptr) {
        out << "pipeline descriptor: allocation failed\n";
        return nullptr;
    }

    auto set_obj = objc.msg<void (*)(Obj, Sel, Obj)>();
    set_obj(desc, objc.sel("setComputeFunction:"), function);
    out << "pipeline descriptor: computeFunction set\n";
    if (max_total_threads_per_threadgroup > 0) {
        auto set_ulong = objc.msg<void (*)(Obj, Sel, unsigned long)>();
        set_ulong(
            desc,
            objc.sel("setMaxTotalThreadsPerThreadgroup:"),
            static_cast<unsigned long>(max_total_threads_per_threadgroup));
        out << "pipeline descriptor: maxTotalThreadsPerThreadgroup="
            << max_total_threads_per_threadgroup << '\n';
    }
    if (threadgroup_size_is_multiple) {
        auto set_bool = objc.msg<void (*)(Obj, Sel, signed char)>();
        set_bool(
            desc,
            objc.sel("setThreadGroupSizeIsMultipleOfThreadExecutionWidth:"),
            static_cast<signed char>(1));
        out << "pipeline descriptor: threadGroupSizeIsMultipleOfThreadExecutionWidth=true\n";
    }

    if (archive != nullptr) {
        Class array_cls = objc.cls("NSArray");
        if (array_cls == nullptr) {
            out << "pipeline descriptor: NSArray missing\n";
            return nullptr;
        }
        auto array_with_object = objc.msg<Obj (*)(Class, Sel, Obj)>();
        Obj archives = array_with_object(array_cls, objc.sel("arrayWithObject:"), archive);
        if (archives == nullptr) {
            out << "pipeline descriptor: archive array failed\n";
            return nullptr;
        }
        set_obj(desc, objc.sel("setBinaryArchives:"), archives);
        out << "pipeline descriptor: binaryArchives attached\n";
    }
    return desc;
}

[[nodiscard]] Obj load_function_from_metallib(ObjcRuntime& objc, Obj device,
                                              std::string_view metallib_path,
                                              std::string_view entry_name,
                                              std::ostringstream& out) {
    auto bytes = read_file(metallib_path, out);
    if (bytes.empty()) {
        return nullptr;
    }

    dispatch_data_t data = dispatch_data_create(
        bytes.data(),
        bytes.size(),
        nullptr,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == nullptr) {
        out << "dispatch_data_create: failed\n";
        return nullptr;
    }

    Obj err = nullptr;
    auto new_library = objc.msg<Obj (*)(Obj, Sel, dispatch_data_t, Obj*)>();
    Obj library = new_library(device, objc.sel("newLibraryWithData:error:"), data, &err);
    dispatch_release(data);
    if (library == nullptr) {
        out << "newLibraryWithData: failed error=" << ns_error_description(objc, err) << '\n';
        return nullptr;
    }
    out << "newLibraryWithData: ok\n";

    Obj name = make_ns_string(objc, entry_name.empty() ? "main0" : entry_name);
    if (name == nullptr) {
        out << "newFunctionWithName: NSString failed entry=" << entry_name << '\n';
        return nullptr;
    }
    auto new_function = objc.msg<Obj (*)(Obj, Sel, Obj)>();
    Obj function = new_function(library, objc.sel("newFunctionWithName:"), name);
    if (function == nullptr) {
        out << "newFunctionWithName(" << entry_name << "): failed\n";
        return nullptr;
    }
    out << "newFunctionWithName(" << entry_name << "): ok\n";
    return function;
}

[[nodiscard]] bool archive_add_compute_pipeline(ObjcRuntime& objc, Obj archive, Obj desc,
                                                std::ostringstream& out) {
    Obj err = nullptr;
    auto add = objc.msg<signed char (*)(Obj, Sel, Obj, Obj*)>();
    const signed char ok = add(
        archive,
        objc.sel("addComputePipelineFunctionsWithDescriptor:error:"),
        desc,
        &err);
    if (!ok) {
        out << "addComputePipelineFunctionsWithDescriptor: failed error="
            << ns_error_description(objc, err) << '\n';
        return false;
    }
    out << "addComputePipelineFunctionsWithDescriptor: ok\n";
    return true;
}

[[nodiscard]] bool archive_serialize(ObjcRuntime& objc, Obj archive, std::string_view archive_path,
                                     std::ostringstream& out) {
    if (archive_path.empty()) {
        out << "serializeToURL: skipped (no path)\n";
        return true;
    }
    Obj url = make_file_url(objc, archive_path);
    if (url == nullptr) {
        out << "serializeToURL: file URL failed path=" << archive_path << '\n';
        return false;
    }
    Obj err = nullptr;
    auto serialize = objc.msg<signed char (*)(Obj, Sel, Obj, Obj*)>();
    const signed char ok = serialize(archive, objc.sel("serializeToURL:error:"), url, &err);
    if (!ok) {
        out << "serializeToURL: failed error=" << ns_error_description(objc, err) << '\n';
        return false;
    }
    out << "serializeToURL: ok path=" << archive_path << '\n';
    return true;
}

[[nodiscard]] bool require_compute_pipeline_archive_hit(
    ObjcRuntime& objc,
    Obj device,
    Obj desc,
    std::string_view label,
    bool expect_hit,
    std::ostringstream& out)
{
    // MTLPipelineOptionFailOnBinaryArchiveMiss.  Keep the numeric ABI here so
    // this exec-clean helper remains independent of Objective-C Metal headers.
    constexpr unsigned long fail_on_binary_archive_miss = 1UL << 2u;
    Obj err = nullptr;
    Obj reflection = nullptr;
    auto new_pipeline = objc.msg<Obj (*)(Obj, Sel, Obj, unsigned long, Obj*, Obj*)>();
    Obj pipeline = new_pipeline(
        device,
        objc.sel("newComputePipelineStateWithDescriptor:options:reflection:error:"),
        desc,
        fail_on_binary_archive_miss,
        &reflection,
        &err);
    if (expect_hit && pipeline == nullptr) {
        out << label << ": failed (archive miss) error="
            << ns_error_description(objc, err) << '\n';
        return false;
    }
    if (!expect_hit && pipeline != nullptr) {
        out << label
            << ": failed (empty archive unexpectedly satisfied descriptor)\n";
        return false;
    }
    if (expect_hit) {
        out << label << ": ok (FailOnBinaryArchiveMiss hit)\n";
    } else {
        const auto fingerprint = ns_error_fingerprint(objc, err);
        if (!fingerprint) {
            out << label
                << ": failed (empty-archive rejection has no NSError domain/code)\n";
            return false;
        }
        out << label << ": ok (FailOnBinaryArchiveMiss rejected empty archive)"
            << " error=" << ns_error_description(objc, err) << '\n';
        out << "NEGATIVE_CONTROL_NSError domain_hex="
            << hex_encode(fingerprint->domain)
            << " code=" << fingerprint->code << '\n';
    }
    return true;
}

}  // namespace

std::string build_binary_archive(
    std::string_view metallib_path,
    std::string_view archive_path,
    std::string_view entry_name,
    std::uint64_t max_total_threads_per_threadgroup,
    bool threadgroup_size_is_multiple_of_thread_execution_width) {
    std::ostringstream out;
    out << "pid=" << static_cast<long>(::getpid()) << '\n';

    ObjcRuntime objc;
    if (!objc.load(out)) {
        return out.str();
    }

    Obj device = objc.mtl_create_system_default_device();
    if (device == nullptr) {
        out << "MTLCreateSystemDefaultDevice: failed\n";
        return out.str();
    }
    out << "MTLCreateSystemDefaultDevice: ok\n";
    using RegistryIdFn = unsigned long long (*)(Obj, Sel);
    const std::uint64_t registry_id = static_cast<std::uint64_t>(
        objc.msg<RegistryIdFn>()(device, objc.sel("registryID")));
    if (registry_id == 0u) {
        out << "MTLDevice registryID: failed\n";
        return out.str();
    }
    out << "HELPER_QUALIFICATION_V2 protocol="
        << k_archive_helper_protocol_version
        << " build=" << k_archive_helper_build_identity
        << " registry_id=" << registry_id << '\n';

    Obj function = load_function_from_metallib(objc, device, metallib_path, entry_name, out);
    if (function == nullptr) {
        return out.str();
    }

    if (!archive_path.empty()) {
        ::unlink(std::string(archive_path).c_str());
        out << "archive pre-unlink: path=" << archive_path << '\n';
    }

    Obj archive = make_empty_archive(objc, device, out);
    if (archive == nullptr) {
        return out.str();
    }

    Obj desc = make_pipeline_descriptor(
        objc,
        function,
        archive,
        max_total_threads_per_threadgroup,
        threadgroup_size_is_multiple_of_thread_execution_width,
        out);
    if (desc == nullptr) {
        return out.str();
    }

    if (!archive_add_compute_pipeline(objc, archive, desc, out)) {
        return out.str();
    }
    if (!archive_serialize(objc, archive, archive_path, out)) {
        return out.str();
    }
    Obj reopened_archive = reopen_archive(objc, device, archive_path, out);
    if (reopened_archive == nullptr) {
        return out.str();
    }

    Obj qualified_desc = make_pipeline_descriptor(
        objc,
        function,
        reopened_archive,
        max_total_threads_per_threadgroup,
        threadgroup_size_is_multiple_of_thread_execution_width,
        out);
    if (qualified_desc == nullptr
        || !require_compute_pipeline_archive_hit(
            objc,
            device,
            qualified_desc,
            "qualified archive positive control",
            true,
            out)) {
        return out.str();
    }

    Obj empty_control_archive = make_empty_archive(objc, device, out);
    if (empty_control_archive == nullptr) {
        return out.str();
    }
    Obj empty_control_desc = make_pipeline_descriptor(
        objc,
        function,
        empty_control_archive,
        max_total_threads_per_threadgroup,
        threadgroup_size_is_multiple_of_thread_execution_width,
        out);
    if (empty_control_desc == nullptr
        || !require_compute_pipeline_archive_hit(
            objc,
            device,
            empty_control_desc,
            "same-descriptor empty-archive negative control",
            false,
            out)) {
        return out.str();
    }
    out << "RESULT: ok\n";
    return out.str();
}

}  // namespace bioseq::gpu::metal_archive
