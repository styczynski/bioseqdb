module;

#include <span>
#include <string_view>
#include <print>

export module bioseq.codegen:cli;

export import :components;
import bioseq.codegen.support;

export namespace bioseq::codegen {

/// @brief Finds a registered component by name.
/// @param name Component token from the command line.
/// @return The matching entry, or nullptr when it was not built in.
[[nodiscard]] inline const component_entry* find_component(std::string_view name) {
    for (const component_entry& entry : registered_components()) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

/// @brief Prints the components compiled into this binary to stderr.
inline void print_enabled_components() {
    std::println(stderr, "enabled components:");
    for (const component_entry& entry : registered_components()) {
        std::println(stderr, "  {}", entry.name);
    }
}

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
        if (const component_entry* component = find_component(args[2])) {
            return component->self_test();
        }
        std::println(stderr, "component '{}' not built", args[2]);
        print_enabled_components();
        return 2;
    }
    if (args.size() >= 2) {
        if (const component_entry* component = find_component(args[1])) {
            return component->run(args.subspan(2));
        }
    }
    std::println(stderr, "usage: {} <command> [args...]", args[0]);
    print_enabled_components();
    return 2;
}

}  // namespace bioseq::codegen
