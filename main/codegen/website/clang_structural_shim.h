#pragma once

// Plain (non-module) interface to the Clang 22 structural/export pass (§11.3).
//
// Clang/LLVM headers are not C++26-module-clean under GCC 16: a module partition
// whose global module fragment includes them produces a BMI that segfaults the
// GCC 16 module serializer when imported (the same incompatibility that forced
// Jinja2Cpp behind a shim). This header exposes only POD std types, so the
// Clang implementation lives in an ordinary TU (clang_structural_impl.cpp,
// scanning off) and clang_entity_index.cppm includes just this header — never a
// Clang header — keeping the module graph Clang-free.

#include <string>
#include <vector>

namespace bioseq::codegen::website::clang_shim {

struct parameter {
    std::string name;
    std::string type;
    std::string default_value;
    bool has_default{};
};

struct entity {
    std::string kind;         // namespace/struct/class/union/enum/enum_value/
                              // concept/type_alias/function/variable
    std::string qualified_name;
    std::string display_name;
    std::string normalized_signature;
    std::string return_type;
    bool has_return_type{};
    std::string module_name;
    bool has_module{};
    std::string member_kind;  // function member classification
    std::string path;
    unsigned long line{};
    unsigned long column{};
    bool has_location{};
    std::vector<parameter> parameters;
};

struct pg_entry {
    std::string symbol;
    std::string normalized_signature;
    std::string owning_module;
    std::string path;
    unsigned long line{};
};

struct index {
    std::vector<entity> entities;
    std::vector<pg_entry> pg_entrypoints;
    std::string clang_version;
};

// One module interface unit's provided module and its import edges (§11.3). An
// `export import` is a public edge (re-exported); a plain `import` is an internal
// dependency and never a public edge — the distinction the closure relies on.
struct module_edges {
    std::string source_path;
    std::string provided_module;
    bool has_provided{};
    std::vector<std::string> exported_imports;
    std::vector<std::string> plain_imports;
};

struct module_graph {
    std::vector<module_edges> units;
    std::string clang_version;
};

// Scans each reachable unit's provided module and export/plain import edges via a
// read-only AST pass over the compile database (§11.3).
[[nodiscard]] module_graph scan_module_edges_from_db(
    const std::vector<std::string>& reachable_units,
    const std::string& compile_database);

// Scans one in-memory unit's provided module and import edges (tests).
[[nodiscard]] module_graph scan_module_edges_from_code(
    const std::string& code, const std::vector<std::string>& args);

// Runs the structural pass on a self-contained in-memory module unit (tests).
[[nodiscard]] index run_structural_from_code(
    const std::string& code, const std::vector<std::string>& args);

// Runs the structural pass over reachable units using a compile database.
[[nodiscard]] index run_structural_from_db(
    const std::vector<std::string>& reachable_units,
    const std::string& compile_database);

// The exact Clang version the generator is linked against (§11.3 version pin).
[[nodiscard]] std::string clang_version();

}  // namespace bioseq::codegen::website::clang_shim
