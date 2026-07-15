module;

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <print>

export module bioseq.codegen:cli;

#ifdef BIOSEQ_CODEGEN_ENABLE_EXTERNAL
import bioseq.codegen.external;
#endif
import bioseq.codegen.support;

export namespace bioseq::codegen {

/// @brief Dispatches the codegen command line to an enabled component.
/// @param args Whole argument vector, where args[0] is the program name.
/// @return Zero on success; two for an unknown command or a component that is
///         not built into this binary; the component's own status otherwise.
inline int run_cli(std::span<std::string_view const> args) {
    if (args.empty()) {
        std::println(stderr, "bioseq_codegen: missing program name");
        return 2;
    }
    if (args.size() >= 2 && args[1] == "self_test") {
        if (args.size() < 3) {
            std::println(stderr, "usage: {} self_test <component>", args[0]);
            return 2;
        }
        if (args[2] == "external") {
#ifdef BIOSEQ_CODEGEN_ENABLE_EXTERNAL
            return bioseq::codegen::external::self_test();
#else
            std::println(stderr, "component 'external' not built");
            return 2;
#endif
        }
        if (args[2] == "website") {
            std::println(stderr, "component 'website' not built");
            return 2;
        }
        std::println(stderr, "unknown component: {}", args[2]);
        return 2;
    }
    if (args.size() >= 2 && args[1] == "external") {
        if (args.size() < 3 || args[2] != "generate") {
            std::println(stderr, "usage: {} external generate [options]", args[0]);
            return 2;
        }
#ifdef BIOSEQ_CODEGEN_ENABLE_EXTERNAL
        std::vector<std::string> argv_storage;
        argv_storage.reserve(args.size());
        argv_storage.emplace_back(args[0]);
        for (std::size_t index = 3; index < args.size(); ++index) {
            argv_storage.emplace_back(args[index]);
        }
        std::vector<char*> argv_pointers;
        argv_pointers.reserve(argv_storage.size());
        for (auto& argument : argv_storage) {
            argv_pointers.push_back(argument.data());
        }
        return bioseq::codegen::external::generate(
            static_cast<int>(argv_pointers.size()), argv_pointers.data());
#else
        std::println(stderr, "component 'external' not built");
        return 2;
#endif
    }
    std::println(stderr, "usage: {} <command> [args...]", args[0]);
    std::println(stderr, "enabled components:");
#ifdef BIOSEQ_CODEGEN_ENABLE_EXTERNAL
    std::println(stderr, "  external");
#endif
#ifdef BIOSEQ_CODEGEN_ENABLE_WEBSITE
    std::println(stderr, "  website");
#endif
    return 2;
}

}
