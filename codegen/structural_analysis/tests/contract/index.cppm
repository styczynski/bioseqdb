module;

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef BIOSEQ_PREPARE_TEST_CLANG_BINARY
#error "PREPARE contract tests require the configured Clang binary"
#endif
#ifndef BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS
#error "PREPARE contract tests require configured clang-scan-deps"
#endif
#ifndef BIOSEQ_PREPARE_TEST_LLVM_CONFIG
#error "PREPARE contract tests require configured llvm-config"
#endif
#ifndef BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY
#error "PREPARE contract tests require configured libc++ module metadata"
#endif
#ifndef BIOSEQ_PREPARE_TEST_CLANG_SHA256
#error "PREPARE contract tests require the configure-time Clang hash"
#endif
#ifndef BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS_SHA256
#error "PREPARE contract tests require the configure-time scanner hash"
#endif
#ifndef BIOSEQ_PREPARE_TEST_LLVM_CONFIG_SHA256
#error "PREPARE contract tests require the configure-time llvm-config hash"
#endif
#ifndef BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY_SHA256
#error "PREPARE contract tests require the configure-time inventory hash"
#endif
#ifndef BIOSEQ_PREPARE_TEST_REPOSITORY_ROOT
#error "PREPARE contract tests require the repository root"
#endif

export module bioseq.codegen.structural_analysis.tests.contract;

import bioseq.codegen.structural_analysis;

namespace bioseq::codegen::structural_analysis::contract_detail {

struct configured_toolchain {
    std::filesystem::path clang_binary{
        BIOSEQ_PREPARE_TEST_CLANG_BINARY};
    std::filesystem::path clang_scan_deps{
        BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS};
    std::filesystem::path llvm_config{
        BIOSEQ_PREPARE_TEST_LLVM_CONFIG};
    std::filesystem::path compiler_inventory{
        BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY};
    std::string clang_sha256{BIOSEQ_PREPARE_TEST_CLANG_SHA256};
    std::string clang_scan_deps_sha256{
        BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS_SHA256};
    std::string llvm_config_sha256{
        BIOSEQ_PREPARE_TEST_LLVM_CONFIG_SHA256};
    std::string compiler_inventory_sha256{
        BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY_SHA256};
};

class temporary_tree {
  public:
    temporary_tree() {
        const auto nonce =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        const std::filesystem::path parent =
            std::filesystem::temp_directory_path();
        for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
            root_ = parent
                / ("bioseq-prepare-contract-"
                   + std::to_string(nonce) + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(root_, error)) {
                return;
            }
            if (error
                && error
                    != std::make_error_code(
                        std::errc::file_exists)) {
                throw std::runtime_error(
                    "PREPARE contract test cannot create temporary "
                    "directory: " + error.message());
            }
        }
        throw std::runtime_error(
            "PREPARE contract test cannot allocate a unique temporary "
            "directory");
    }

    temporary_tree(const temporary_tree&) = delete;
    temporary_tree& operator=(const temporary_tree&) = delete;

    ~temporary_tree() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

  private:
    std::filesystem::path root_;
};

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            "PREPARE contract test failed: "
            + std::string{message});
    }
}

[[nodiscard]] inline std::string_view source_text(
    std::string_view source,
    source_range range) {
    if (range.end.byte_offset < range.begin.byte_offset
        || range.end.byte_offset > source.size()) {
        throw std::runtime_error(
            "PREPARE contract fixture received an invalid source range");
    }
    return source.substr(
        range.begin.byte_offset,
        range.end.byte_offset - range.begin.byte_offset);
}

[[nodiscard]] inline std::string_view callable_kind_name(
    callable_kind kind) noexcept {
    switch (kind) {
        case callable_kind::function: return "function";
        case callable_kind::member_function: return "member_function";
        case callable_kind::constructor: return "constructor";
        case callable_kind::destructor: return "destructor";
        case callable_kind::operator_function: return "operator_function";
        case callable_kind::conversion_function:
            return "conversion_function";
        case callable_kind::lambda: return "lambda";
    }
    return "unknown";
}

[[nodiscard]] inline std::string callable_inventory(
    const marked_namespace_record& name_space) {
    std::ostringstream result;
    result << "observed " << name_space.callables.size()
           << " callable(s):";
    for (const callable_insertion_record& callable :
         name_space.callables) {
        result << " [" << callable_kind_name(callable.kind)
               << ", name='" << callable.qualified_name
               << "', usr='" << callable.stable_usr << "']";
    }
    return result.str();
}

template <typename p_operation>
inline void require_failure(
    p_operation&& operation,
    std::string_view expected_diagnostic,
    std::string_view case_name = {}) {
    const std::string context = case_name.empty()
        ? std::string{}
        : " [" + std::string{case_name} + "]";
    try {
        std::invoke(std::forward<p_operation>(operation));
    } catch (const std::exception& error) {
        if (std::string_view{error.what()}.contains(
                expected_diagnostic)) {
            return;
        }
        throw std::runtime_error(
            "PREPARE contract test" + context
            + " expected diagnostic containing '"
            + std::string{expected_diagnostic}
            + "' but received: " + error.what());
    }
    throw std::runtime_error(
        "PREPARE contract test" + context
        + " expected failure containing '"
        + std::string{expected_diagnostic} + "'");
}

inline void write_file(
    const std::filesystem::path& path,
    std::string_view content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output{
        path,
        std::ios::binary | std::ios::trunc,
    };
    if (!output) {
        throw std::runtime_error(
            "PREPARE contract test cannot write "
            + path.string());
    }
    output.write(
        content.data(),
        static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error(
            "PREPARE contract test cannot finish writing "
            + path.string());
    }
}

[[nodiscard]] inline std::string read_file(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "PREPARE contract test cannot read "
            + path.string());
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw std::runtime_error(
            "PREPARE contract test cannot finish reading "
            + path.string());
    }
    return content.str();
}

[[nodiscard]] inline std::string json_escape(
    std::string_view value) {
    std::string result;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(hex[character >> 4U]);
                    result.push_back(hex[character & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    return result;
}

struct raw_command {
    std::filesystem::path directory;
    std::filesystem::path source;
    std::vector<std::string> arguments;
};

[[nodiscard]] inline std::string render_compile_database(
    const std::vector<raw_command>& commands) {
    std::ostringstream output;
    output << "[\n";
    for (std::size_t command_index = 0;
         command_index < commands.size();
         ++command_index) {
        const raw_command& command = commands[command_index];
        output
            << (command_index == 0 ? "" : ",\n")
            << "  {\n"
            << "    \"directory\": \""
            << json_escape(command.directory.string()) << "\",\n"
            << "    \"file\": \""
            << json_escape(command.source.string()) << "\",\n"
            << "    \"arguments\": [";
        for (std::size_t argument_index = 0;
             argument_index < command.arguments.size();
             ++argument_index) {
            output
                << (argument_index == 0 ? "" : ", ")
                << "\"" << json_escape(
                       command.arguments[argument_index])
                << "\"";
        }
        output << "]\n  }";
    }
    output << "\n]\n";
    return output.str();
}

[[nodiscard]] inline raw_command make_command(
    const configured_toolchain& toolchain,
    const std::filesystem::path& directory,
    const std::filesystem::path& source,
    std::vector<std::string> extra_arguments = {},
    std::vector<std::string> executable_prefix = {}) {
    if (executable_prefix.empty()) {
        executable_prefix.push_back(
            toolchain.clang_binary.string());
    }
    std::vector<std::string> arguments =
        std::move(executable_prefix);
    arguments.emplace_back("-std=c++26");
    arguments.emplace_back("-x");
    arguments.emplace_back("c++-module");
    arguments.insert(
        arguments.end(),
        std::make_move_iterator(extra_arguments.begin()),
        std::make_move_iterator(extra_arguments.end()));
    arguments.push_back(source.string());
    return {
        .directory = directory,
        .source = source,
        .arguments = std::move(arguments),
    };
}

[[nodiscard]] inline dependency_compilation_environment prepare(
    const configured_toolchain& toolchain,
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& raw_database,
    const std::filesystem::path& pcm_root,
    std::string_view profile = "production") {
    return prepare_compilation_environment(
        sources,
        raw_database,
        toolchain.clang_binary,
        toolchain.clang_scan_deps,
        toolchain.llvm_config,
        toolchain.compiler_inventory,
        toolchain.clang_sha256,
        toolchain.clang_scan_deps_sha256,
        toolchain.llvm_config_sha256,
        toolchain.compiler_inventory_sha256,
        pcm_root,
        profile,
        "support");
}

[[nodiscard]] inline dependency_compilation_validation validate(
    const configured_toolchain& toolchain,
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& owned_database,
    const std::filesystem::path& pcm_root,
    const std::filesystem::path& manifest,
    std::string_view profile = "production",
    std::optional<std::string_view> clang_sha256 = std::nullopt,
    std::optional<std::filesystem::path> clang_binary = std::nullopt) {
    return validate_dependency_compilation_environment(
        sources,
        owned_database,
        pcm_root,
        manifest,
        clang_binary.value_or(toolchain.clang_binary),
        toolchain.clang_scan_deps,
        toolchain.llvm_config,
        toolchain.compiler_inventory,
        clang_sha256.value_or(toolchain.clang_sha256),
        toolchain.clang_scan_deps_sha256,
        toolchain.llvm_config_sha256,
        toolchain.compiler_inventory_sha256,
        profile,
        "support");
}

inline void publish_prepared(
    const dependency_compilation_environment& prepared,
    const std::filesystem::path& owned_database,
    const std::filesystem::path& manifest) {
    write_file(owned_database, prepared.compile_database_json);
    write_file(manifest, prepared.pcm_manifest_json);
}

[[nodiscard]] inline std::string replace_json_string_field(
    std::string document,
    std::string_view field,
    std::string_view replacement) {
    const std::string marker =
        "\"" + std::string{field} + "\": \"";
    const std::size_t begin = document.find(marker);
    if (begin == std::string::npos) {
        throw std::runtime_error(
            "PREPARE contract fixture cannot find manifest field '"
            + std::string{field} + "'");
    }
    const std::size_t value_begin = begin + marker.size();
    std::size_t value_end = value_begin;
    bool escaped = false;
    for (; value_end < document.size(); ++value_end) {
        const char character = document[value_end];
        if (escaped) {
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            break;
        }
    }
    if (value_end == document.size()) {
        throw std::runtime_error(
            "PREPARE contract fixture found unterminated manifest "
            "field '" + std::string{field} + "'");
    }
    document.replace(
        value_begin,
        value_end - value_begin,
        json_escape(replacement));
    return document;
}

[[nodiscard]] inline std::filesystem::path first_pcm_dependency(
    const dependency_compilation_environment& prepared) {
    const auto pcm = std::ranges::find_if(
        prepared.dependencies,
        [](const std::filesystem::path& dependency) {
            return dependency.extension() == ".pcm";
        });
    if (pcm == prepared.dependencies.end()) {
        throw std::runtime_error(
            "PREPARE contract fixture did not publish a PCM dependency");
    }
    return *pcm;
}

[[nodiscard]] inline std::filesystem::path first_dependency_scan_artifact(
    const std::filesystem::path& pcm_root,
    std::string_view suffix) {
    const std::filesystem::path scan_root =
        pcm_root / ".module-dependency-scan";
    std::vector<std::filesystem::path> results;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{scan_root}) {
        if (entry.is_regular_file()
            && entry.path().filename().string().ends_with(suffix)) {
            results.push_back(entry.path());
        }
    }
    std::ranges::sort(results);
    if (results.empty()) {
        throw std::runtime_error(
            "PREPARE contract fixture did not publish the requested "
            "active-import scan artifact");
    }
    return results.front();
}

[[nodiscard]] inline std::filesystem::path first_dependency_scan_result(
    const std::filesystem::path& pcm_root) {
    return first_dependency_scan_artifact(
        pcm_root, ".p1689.json");
}

inline void assert_toolchain_exists(
    const configured_toolchain& toolchain) {
    require(
        std::filesystem::is_regular_file(
            toolchain.clang_binary),
        "configured Clang binary is absent");
    require(
        std::filesystem::is_regular_file(
            toolchain.clang_scan_deps),
        "configured clang-scan-deps is absent");
    require(
        std::filesystem::is_regular_file(
            toolchain.llvm_config),
        "configured llvm-config is absent");
    require(
        std::filesystem::is_regular_file(
            toolchain.compiler_inventory),
        "configured libc++ module inventory is absent");
    require(
        toolchain.clang_sha256.size() == 64
            && toolchain.clang_scan_deps_sha256.size() == 64
            && toolchain.llvm_config_sha256.size() == 64
            && toolchain.compiler_inventory_sha256.size() == 64,
        "configure-time tool hashes are not SHA-256 values");
}

}  // namespace bioseq::codegen::structural_analysis::contract_detail

export namespace bioseq::codegen::structural_analysis {

inline void run_preparation_contract_self_tests() {
    using namespace contract_detail;

    constexpr std::string_view token_slice_source = R"CPP(module;
export module fixture.token_slice;
import fixture.dependency;
#define BIOSEQ_TOKEN_BRANCH 1
namespace envelope {
inline namespace [[bioseq::template_flatten("Alphabet")]] selected {
[[bioseq::template_flatten_when("small"), maybe_unused]]
static constexpr unsigned table = 7U;
template <class p_value>
[[bioseq::template_flatten_entry("map")]]
inline p_value map_value(p_value value = p_value{}) { return value; }
using scalar_type = unsigned long;
#if BIOSEQ_TOKEN_BRANCH
constexpr int branch_true = 1;
#else
constexpr int branch_false = 2;
#endif
namespace nested {
constexpr int indirect = 9;
}
}
}
)CPP";
    const translation_unit token_slice = scan_token_slice(
        "token_slice_contract.cppm", token_slice_source);
    require(
        token_slice.mode == collection_mode::token_slice
            && token_slice.schema_revision == k_model_schema_revision
            && token_slice.source_path
                == std::filesystem::path{"token_slice_contract.cppm"}
            && !token_slice.source_sha256.empty(),
        "token-slice collection does not identify its source and schema");
    require(
        token_slice.module_name == "fixture.token_slice"
            && token_slice.source_module.provided_module
                == "fixture.token_slice"
            && token_slice.source_module.is_interface
            && !token_slice.source_module.is_partition
            && token_slice.source_module.imported_modules
                == std::vector<std::string>{"fixture.dependency"}
            && source_text(
                   token_slice_source,
                   token_slice.source_module.declaration_range)
                == "export module fixture.token_slice;",
        "token-slice collection lost exact module/import geometry");

    const auto selected_namespace = std::ranges::find(
        token_slice.namespaces,
        "envelope::selected",
        &namespace_record::qualified_name);
    require(
        selected_namespace != token_slice.namespaces.end(),
        "token-slice collection omitted the selected namespace");
    require(
        selected_namespace->is_inline
            && !selected_namespace->is_anonymous
            && !selected_namespace->is_reopened
            && !selected_namespace->source_identity.empty()
            && selected_namespace->stable_usr.empty(),
        "token-slice namespace mixed lexical and semantic identity facts");
    require(
        source_text(
            token_slice_source,
            selected_namespace->keyword_range) == "namespace"
            && source_text(
                   token_slice_source,
                   selected_namespace->name_range) == "selected"
            && source_text(
                   token_slice_source,
                   selected_namespace->open_brace_range) == "{"
            && source_text(
                   token_slice_source,
                   selected_namespace->close_brace_range) == "}"
            && selected_namespace->body_range.begin.byte_offset
                == selected_namespace->open_brace_range.end.byte_offset
            && selected_namespace->body_range.end.byte_offset
                == selected_namespace->close_brace_range.begin.byte_offset,
        "token-slice namespace keyword/name/brace/body geometry is not exact");
    require(
        selected_namespace->attributes.size() == 1,
        "token-slice namespace marker count is not exact");
    const attribute_record& namespace_marker =
        selected_namespace->attributes.front();
    require(
        namespace_marker.qualified_name == "bioseq::template_flatten"
            && namespace_marker.vendor_namespace == "bioseq"
            && namespace_marker.name == "template_flatten"
            && namespace_marker.argument_text == "\"Alphabet\""
            && namespace_marker.placement
                == attribute_placement::namespace_head
            && source_text(
                   token_slice_source,
                   namespace_marker.spelling_range)
                == "bioseq::template_flatten(\"Alphabet\")"
            && source_text(
                   token_slice_source,
                   namespace_marker.argument_range) == "\"Alphabet\""
            && source_text(
                   token_slice_source,
                   namespace_marker.removal_range)
                == "[[bioseq::template_flatten(\"Alphabet\")]]",
        "token-slice namespace marker identity or source geometry changed");

    std::vector<const declaration_record*> selected_children;
    for (const declaration_record& declaration :
         token_slice.declarations) {
        if (declaration.enclosing_qualified_name
            == "envelope::selected") {
            selected_children.push_back(&declaration);
        }
    }
    std::vector<std::string> selected_child_names;
    std::ranges::transform(
        selected_children,
        std::back_inserter(selected_child_names),
        [](const declaration_record* declaration) {
            return declaration->name;
        });
    std::ranges::sort(selected_child_names);
    std::string selected_child_inventory{"["};
    for (std::size_t index = 0;
         index < selected_child_names.size();
         ++index) {
        if (index != 0) selected_child_inventory += ", ";
        selected_child_inventory += "'" + selected_child_names[index] + "'";
    }
    selected_child_inventory += "]";
    require(
        selected_child_names == std::vector<std::string>{
            "branch_false",
            "branch_true",
            "map_value",
            "nested",
            "scalar_type",
            "table",
        },
        std::string{
            "token-slice collection did not return exactly the direct namespace "
            "children from both raw conditional branches; observed sorted "
            "inventory ("}
            + std::to_string(selected_child_names.size()) + "): "
            + selected_child_inventory);
    require(
        std::ranges::all_of(
            selected_children,
            [](const declaration_record* declaration) {
                return declaration->specifiers.direct_namespace_member
                    && !declaration->source_identity.empty()
                    && !declaration
                            ->immediate_namespace_source_identity.empty()
                    && declaration
                            ->immediate_namespace_stable_usr.empty()
                    && declaration->stable_usr.empty();
            }),
        "token-slice direct children mixed semantic identity into raw facts");
    const auto selected_child = [&](std::string_view name)
        -> const declaration_record& {
        const auto found = std::ranges::find(
            selected_children,
            name,
            [](const declaration_record* declaration) {
                return std::string_view{declaration->name};
            });
        if (found == selected_children.end()) {
            throw std::runtime_error(
                "PREPARE contract fixture cannot find direct child '"
                + std::string{name} + "'");
        }
        return **found;
    };
    const declaration_record& table = selected_child("table");
    require(
        table.kind == declaration_kind::variable
            && table.payload_kind
                == declaration_payload_kind::variable_initializer
            && table.specifiers.is_variable
            && table.specifiers.is_definition
            && table.specifiers.is_static
            && table.specifiers.is_constexpr
            && table.specifiers.has_explicit_type
            && table.explicit_declared_type == "unsigned"
            && source_text(token_slice_source, table.declaration_range)
                == "[[bioseq::template_flatten_when(\"small\"), "
                   "maybe_unused]]\n"
                   "static constexpr unsigned table = 7U;"
            && source_text(
                   token_slice_source,
                   table.declaration_head_range)
                == "[[bioseq::template_flatten_when(\"small\"), "
                   "maybe_unused]]\n"
                   "static constexpr unsigned table "
            && source_text(
                   token_slice_source,
                   table.body_or_initializer_range) == "7U",
        "token-slice variable declaration/head/initializer facts changed");
    require(
        table.attributes.size() == 2,
        "token-slice shared attribute block did not retain both segments");
    const auto table_marker = std::ranges::find(
        table.attributes,
        "bioseq::template_flatten_when",
        &attribute_record::qualified_name);
    require(
        table_marker != table.attributes.end()
            && table_marker->argument_text == "\"small\""
            && source_text(
                   token_slice_source,
                   table_marker->spelling_range)
                == "bioseq::template_flatten_when(\"small\")"
            && source_text(
                   token_slice_source,
                   table_marker->argument_range) == "\"small\""
            && source_text(
                   token_slice_source,
                   table_marker->removal_range)
                == "bioseq::template_flatten_when(\"small\"),",
        "token-slice shared attribute segment geometry is not removable");

    const declaration_record& map_value = selected_child("map_value");
    require(
        map_value.name == "map_value"
            && source_text(token_slice_source, map_value.name_range)
                == "map_value",
        "token-slice function template head leaked its template parameter "
        "into declaration identity");
    require(
        map_value.kind == declaration_kind::function
            && map_value.payload_kind
                == declaration_payload_kind::function_body
            && map_value.specifiers.is_function
            && map_value.specifiers.is_definition
            && map_value.specifiers.is_template
            && map_value.specifiers.is_inline
            && map_value.specifiers.has_default_arguments
            && map_value.parameters.size() == 1
            && map_value.parameters.front().name == "value"
            && map_value.parameters.front().has_default
            && source_text(
                   token_slice_source,
                   map_value.declaration_head_range)
                == "template <class p_value>\n"
                   "[[bioseq::template_flatten_entry(\"map\")]]\n"
                   "inline p_value map_value(p_value value = p_value{}) "
            && source_text(
                   token_slice_source,
                   map_value.body_or_initializer_range)
                == "{ return value; }",
        "token-slice function head/body/template/default-argument facts "
        "changed");
    const auto map_marker = std::ranges::find(
        map_value.attributes,
        "bioseq::template_flatten_entry",
        &attribute_record::qualified_name);
    require(
        map_marker != map_value.attributes.end()
            && source_text(
                   token_slice_source,
                   map_marker->removal_range)
                == "[[bioseq::template_flatten_entry(\"map\")]]",
        "token-slice function marker geometry changed");

    const declaration_record& scalar_type =
        selected_child("scalar_type");
    require(
        scalar_type.kind == declaration_kind::alias
            && scalar_type.payload_kind
                == declaration_payload_kind::alias_rhs
            && scalar_type.specifiers.is_using_alias
            && scalar_type.specifiers.is_definition
            && scalar_type.explicit_declared_type == "unsigned long"
            && source_text(
                   token_slice_source,
                   scalar_type.body_or_initializer_range)
                == "unsigned long",
        "token-slice alias RHS geometry or classification changed");
    const auto indirect = std::ranges::find(
        token_slice.declarations,
        "indirect",
        &declaration_record::name);
    require(
        indirect != token_slice.declarations.end()
            && indirect->enclosing_qualified_name
                == "envelope::selected::nested"
            && std::ranges::none_of(
                selected_children,
                [](const declaration_record* declaration) {
                    return declaration->name == "indirect";
                }),
        "token-slice direct-child classification leaked a nested member");

    require(
        token_slice.preprocessor_directives.size() == 4,
        "token-slice preprocessor geometry omitted a raw directive");
    const auto branch_if = std::ranges::find(
        token_slice.preprocessor_directives,
        preprocessor_directive_kind::if_directive,
        &preprocessor_directive_record::kind);
    const auto branch_else = std::ranges::find(
        token_slice.preprocessor_directives,
        preprocessor_directive_kind::else_directive,
        &preprocessor_directive_record::kind);
    const auto branch_endif = std::ranges::find(
        token_slice.preprocessor_directives,
        preprocessor_directive_kind::endif,
        &preprocessor_directive_record::kind);
    require(
        branch_if != token_slice.preprocessor_directives.end()
            && branch_else
                != token_slice.preprocessor_directives.end()
            && branch_endif
                != token_slice.preprocessor_directives.end()
            && source_text(
                   token_slice_source,
                   branch_if->complete_range)
                == "#if BIOSEQ_TOKEN_BRANCH\n"
            && source_text(
                   token_slice_source,
                   branch_if->argument_range)
                == "BIOSEQ_TOKEN_BRANCH"
            && branch_if->nesting_depth == 0
            && branch_else->nesting_depth == 0
            && branch_endif->nesting_depth == 0
            && !branch_if->active
            && !branch_else->active
            && !branch_endif->active,
        "token-slice preprocessor range/depth/activity facts changed");

    constexpr std::string_view final_token_source =
        "namespace final_token { constexpr int retained = 1; }";
    const translation_unit final_token_slice = scan_token_slice(
        "final_token_contract.cppm", final_token_source);
    const auto retained = std::ranges::find(
        final_token_slice.declarations,
        "retained",
        &declaration_record::name);
    require(
        !final_token_slice.tokens.empty()
            && final_token_slice.tokens.back().spelling == "}"
            && retained != final_token_slice.declarations.end()
            && retained->enclosing_qualified_name == "final_token",
        "token-slice raw lexer discarded a valid final token at the "
        "buffer end");

    constexpr std::string_view probe_source = R"CPP(
export module fixture.declaration_probe;
export namespace bioseq::fixture {
struct [[bioseq::template_flatten_provenance(
    "canonical.cppm#declaration:101", "0")]] scalar_type;
[[bioseq::template_flatten_provenance("canonical.cppm#declaration:202", "1")]]
scalar_type map_value(scalar_type value);
}
)CPP";
    const translation_unit declaration_probe =
        scan_declaration_probe_from_code(
            "declaration_probe_contract.cppm",
            probe_source,
            {"-std=c++20", "-xc++"},
            "declaration_probe_contract");
    require(
        declaration_probe.mode == collection_mode::declaration_probe
            && declaration_probe.source_module.provided_module
                == "fixture.declaration_probe"
            && declaration_probe.source_module.is_interface,
        "declaration probe lost its collection mode or module identity");
    const auto probed_scalar = std::ranges::find(
        declaration_probe.declarations,
        "canonical.cppm#declaration:101",
        &declaration_record::source_identity);
    const auto probed_map = std::ranges::find(
        declaration_probe.declarations,
        "canonical.cppm#declaration:202",
        &declaration_record::source_identity);
    require(
        probed_scalar != declaration_probe.declarations.end()
            && probed_map != declaration_probe.declarations.end(),
        "declaration probe did not restore exact canonical provenance IDs");
    require(
        !probed_scalar->stable_usr.empty()
            && !probed_map->stable_usr.empty()
            && probed_scalar->stable_usr != probed_map->stable_usr
            && !probed_scalar
                    ->immediate_namespace_source_identity.empty()
            && !probed_map
                    ->immediate_namespace_source_identity.empty()
            && !probed_scalar->immediate_namespace_stable_usr.empty()
            && probed_scalar->immediate_namespace_stable_usr
                == probed_map->immediate_namespace_stable_usr,
        "declaration probe did not bind stable declaration/namespace "
        "semantic identities");
    require(
        probed_scalar->attachment == module_attachment::named_module
            && probed_map->attachment
                == module_attachment::named_module
            && probed_scalar->linkage == entity_linkage::external
            && probed_map->linkage == entity_linkage::external
            && probed_scalar->payload_kind
                == declaration_payload_kind::none
            && probed_map->payload_kind
                == declaration_payload_kind::none,
        "declaration probe linkage/module attachment/body-free facts "
        "changed");
    require(
        std::ranges::contains(
            probed_map->referenced_declaration_source_identities,
            probed_scalar->source_identity),
        "declaration probe did not link a referenced semantic declaration "
        "back to canonical provenance");

    constexpr std::string_view global_probe_source = R"CPP(
namespace bioseq::fixture {
[[bioseq::template_flatten_provenance("global.cpp#declaration:11", "0")]]
extern int global_value;
}
)CPP";
    const translation_unit global_probe = scan_declaration_probe_from_code(
        "global_declaration_probe.cpp",
        global_probe_source,
        {"-std=c++20", "-xc++"},
        "global_declaration_probe_contract");
    const auto global_value = std::ranges::find(
        global_probe.declarations,
        "global.cpp#declaration:11",
        &declaration_record::source_identity);
    require(
        global_value != global_probe.declarations.end()
            && global_value->attachment
                == module_attachment::global_module
            && global_value->linkage == entity_linkage::external,
        "declaration probe did not distinguish global-module attachment");

    require_failure(
        [&] {
            constexpr std::string_view source = R"CPP(
export module fixture.invalid_body_probe;
export namespace bioseq::fixture {
[[bioseq::template_flatten_provenance("canonical.cppm#declaration:303", "0")]]
int invalid_body() { return 3; }
}
)CPP";
            static_cast<void>(scan_declaration_probe_from_code(
                "invalid_body_probe.cppm",
                source,
                {"-std=c++20", "-xc++"},
                "invalid_body_probe_contract"));
        },
        "declaration probe contains a function body",
        "body-free function-declaration probe");
    require_failure(
        [&] {
            constexpr std::string_view source = R"CPP(
export module fixture.invalid_initializer_probe;
export namespace bioseq::fixture {
struct [[bioseq::template_flatten_provenance(
    "canonical.cppm#declaration:404", "0")]] invalid_record {
    int initialized_member = 4;
};
}
)CPP";
            static_cast<void>(scan_declaration_probe_from_code(
                "invalid_initializer_probe.cppm",
                source,
                {"-std=c++20", "-xc++"},
                "invalid_initializer_probe_contract"));
        },
        "declaration probe contains a default member initializer",
        "body-free default-member-initializer probe");

    constexpr std::string_view simd_marker_source = R"CPP(
export module fixture.simd_marker;
export namespace bioseq::fixture {
namespace [[ bioseq :: simd ]] target {
template<class p_value>
p_value map_value(p_value value) { return value; }

#define FIXTURE_INLINE inline
template<class p_value>
FIXTURE_INLINE p_value macro_map_value(p_value value) { return value; }
#undef FIXTURE_INLINE

struct worker {
    worker() {}
    worker(const worker&) = default;
    worker(worker&&) = delete;
    worker& operator=(const worker&) = default;
    ~worker() = default;
    int operator()() const { return 1; }
    int member() { return 2; }
};

inline int invoke() {
    auto local = [] [[nodiscard]] () constexpr noexcept(true)
        [[gnu::always_inline]]
        -> int { return 3; };
    auto implicit = [] { return 4; };
    auto generic_implicit = []<class p_value> { return sizeof(p_value); };
    return local() + implicit()
        + static_cast<int>(generic_implicit.operator()<int>());
}
}
namespace [[bioseq::similar]] decoy {}
}
)CPP";
    const translation_unit lexical_simd = scan_translation_unit(
        "simd_marker_lexical.cppm", simd_marker_source);
    require(
        lexical_simd.marked_namespaces.size() == 1,
        "lexical structural scan accepts only exact bioseq::simd markers");
    const marked_namespace_record& lexical_namespace =
        lexical_simd.marked_namespaces.front();
    const auto source_slice = [&](source_range range) {
        return simd_marker_source.substr(
            range.begin.byte_offset,
            range.end.byte_offset - range.begin.byte_offset);
    };
    require(
        lexical_namespace.qualified_name
                == "bioseq::fixture::target"
            && lexical_namespace.marker.qualified_name
                == "bioseq::simd"
            && lexical_namespace.marker.vendor_namespace == "bioseq"
            && lexical_namespace.marker.name == "simd"
            && lexical_namespace.marker.placement
                == attribute_placement::namespace_head
            && source_slice(lexical_namespace.marker_block_range)
                == "[[ bioseq :: simd ]]",
        "lexical structural scan retains the exact SIMD marker identity");
    require(
        source_slice(lexical_namespace.keyword_range) == "namespace"
            && source_slice(lexical_namespace.name_range) == "target"
            && source_slice(lexical_namespace.open_brace_range) == "{"
            && source_slice(lexical_namespace.close_brace_range) == "}"
            && lexical_namespace.body_range.begin.byte_offset
                == lexical_namespace.open_brace_range.end.byte_offset
            && lexical_namespace.body_range.end.byte_offset
                == lexical_namespace.close_brace_range.begin.byte_offset,
        "lexical structural scan proves exact namespace token geometry");
    require(
        lexical_namespace.callables.empty()
            && !lexical_namespace.has_explicit_export
            && !lexical_namespace.crosses_preprocessor_region,
        "lexical scan leaves callable validation to Clang and reports policy facts");

    const translation_unit clang_simd = scan_translation_unit_from_code(
        "simd_marker_ast.cppm",
        simd_marker_source,
        {"-std=c++26", "-xc++"},
        "simd_marker_contract");
    require(
        clang_simd.marked_namespaces.size() == 1,
        "Clang structural scan binds one exact SIMD namespace");
    const marked_namespace_record& clang_namespace =
        clang_simd.marked_namespaces.front();
    const std::string clang_callable_inventory =
        callable_inventory(clang_namespace);
    require(
        clang_namespace.qualified_name
            == "bioseq::fixture::target",
        "Clang structural scan bound the SIMD marker to namespace '"
            + clang_namespace.qualified_name
            + "' instead of 'bioseq::fixture::target'");
    require(
        !clang_namespace.target_usr.empty(),
        "Clang structural scan returned an empty namespace USR; "
            + clang_callable_inventory);
    require(
        clang_namespace.callables.size() == 9,
        "Clang structural scan expected exactly nine authored compound-body "
        "callables and must omit defaulted/deleted declarations; "
            + clang_callable_inventory);
    require(
        std::ranges::all_of(
            clang_namespace.callables,
            [](const callable_insertion_record& callable) {
                return !callable.qualified_name.empty()
                    && !callable.stable_usr.empty();
            }),
        "Clang structural scan returned a callable without a qualified name "
        "or stable USR; " + clang_callable_inventory);
    require(
        std::ranges::all_of(
            clang_namespace.callables,
            [&](const callable_insertion_record& callable) {
                const bool lambda =
                    callable.kind == callable_kind::lambda;
                return (lambda
                            ? callable.highway_attribute_insertion.byte_offset
                                    >= callable.declaration_range.begin.byte_offset
                                && callable.highway_attribute_insertion.byte_offset
                                    <= callable.body_range.begin.byte_offset
                            : callable.highway_attribute_insertion.byte_offset
                                    >= callable.declaration_range.begin
                                           .byte_offset
                                && callable.highway_attribute_insertion.byte_offset
                                    < callable.body_range.begin.byte_offset)
                    && callable.body_range.begin.byte_offset
                        < callable.body_range.end.byte_offset
                    && simd_marker_source[
                           callable.body_range.begin.byte_offset]
                        == '{';
            }),
        "Clang structural scan proves named-callable prefix and lambda-front "
        "target-attribute insertion points");
    const auto lambda = std::ranges::find(
        clang_namespace.callables,
        callable_kind::lambda,
        &callable_insertion_record::kind);
    constexpr std::string_view lambda_attribute_boundary =
        "] [[nodiscard]]";
    constexpr std::string_view lambda_introducer_suffix = "]";
    require(
        lambda != clang_namespace.callables.end()
            && lambda->highway_attribute_insertion.byte_offset
                >= lambda_introducer_suffix.size()
            && simd_marker_source.substr(
                lambda->highway_attribute_insertion.byte_offset
                    - lambda_introducer_suffix.size(),
                lambda_attribute_boundary.size())
                == lambda_attribute_boundary,
        "Clang structural scan did not place the lambda target attribute in "
        "the front slot after its introducer and before authored attributes; "
            + clang_callable_inventory);
    require(
        std::ranges::any_of(
            clang_namespace.callables,
            [&](const callable_insertion_record& callable) {
                return callable.kind == callable_kind::lambda
                    && simd_marker_source.substr(
                        callable.highway_attribute_insertion.byte_offset,
                        std::string_view{" {"}.size()) == " {";
            }),
        "Clang structural scan did not place the parenthesis-free lambda "
        "target-attribute insertion immediately after its introducer; "
            + clang_callable_inventory);
    require(
        std::ranges::any_of(
            clang_namespace.callables,
            [&](const callable_insertion_record& callable) {
                const std::size_t insertion =
                    callable.highway_attribute_insertion.byte_offset;
                return callable.kind == callable_kind::lambda
                    && insertion > 0u
                    && simd_marker_source[insertion - 1u] == '>'
                    && simd_marker_source.substr(
                        insertion,
                        std::string_view{" {"}.size()) == " {";
            }),
        "Clang structural scan did not place the parenthesis-free generic "
        "lambda target-attribute insertion after its complete template head; "
            + clang_callable_inventory);

    constexpr std::string_view lambda_geometry_source = R"CPP(
export module fixture.lambda_geometry;
export namespace bioseq::fixture {
namespace [[bioseq::simd]] target {
inline int exercise_lambda_geometry() {
    int state = 0;
    auto explicit_generic = []<class T> [[nodiscard]] (T value) constexpr
        noexcept(noexcept(T{value})) [[gnu::always_inline]] -> T
        requires (sizeof(T) > 0) { return value; };
    auto template_requires = []<class T> requires (sizeof(T) > 0)
        [[nodiscard]] (T value) static noexcept { return value; };
    auto mutable_case = [state] [[nodiscard]] (int value) mutable noexcept {
        return state += value;
    };
    auto consteval_case = []() consteval { return 3; };
    auto static_case = [] static noexcept { return 4; };
    auto implicit_case = [] [[nodiscard]] constexpr noexcept -> int {
        return 5;
    };
    auto invented_generic = [] [[nodiscard]] (auto value)
        noexcept(noexcept(value + 1)) { return value; };
    return explicit_generic(1) + template_requires(1) + mutable_case(1)
        + consteval_case() + static_case() + implicit_case()
        + invented_generic(1);
}
}
}
)CPP";
    const translation_unit lambda_geometry = scan_translation_unit_from_code(
        "lambda_geometry.cppm",
        lambda_geometry_source,
        {"-std=c++26", "-xc++"},
        "lambda_geometry_contract");
    require(
        lambda_geometry.marked_namespaces.size() == 1,
        "lambda geometry contract expected one marked namespace");
    const auto& geometry_callables =
        lambda_geometry.marked_namespaces.front().callables;
    const auto require_lambda_anchor =
        [&](std::string_view declaration_marker,
            std::string_view anchor,
            std::string_view following) {
            const std::size_t declaration =
                lambda_geometry_source.find(declaration_marker);
            const std::size_t anchor_begin =
                lambda_geometry_source.find(anchor, declaration);
            const std::size_t expected = anchor_begin + anchor.size();
            const auto found = std::ranges::find_if(
                geometry_callables,
                [&](const callable_insertion_record& callable) {
                    return callable.kind == callable_kind::lambda
                        && callable.declaration_range.begin.byte_offset
                            <= expected
                        && expected <= callable.body_range.begin.byte_offset;
                });
            require(
                declaration != std::string_view::npos
                    && anchor_begin != std::string_view::npos
                    && found != geometry_callables.end()
                    && found->highway_attribute_insertion.byte_offset
                        == expected
                    && lambda_geometry_source.substr(
                           expected, following.size()) == following,
                "lambda front-attribute anchor mismatch for "
                    + std::string{declaration_marker});
        };
    require_lambda_anchor(
        "explicit_generic", "[]<class T>", " [[nodiscard]]");
    require_lambda_anchor(
        "template_requires",
        "requires (sizeof(T) > 0)",
        "\n        [[nodiscard]]");
    require_lambda_anchor(
        "mutable_case", "[state]", " [[nodiscard]]");
    require_lambda_anchor("consteval_case", "[]", "()");
    require_lambda_anchor("static_case", "[]", " static");
    require_lambda_anchor(
        "implicit_case", "[]", " [[nodiscard]]");
    require_lambda_anchor(
        "invented_generic", "[]", " [[nodiscard]]");
    for (const auto [kind, description] : {
             std::pair{callable_kind::function, "free function"},
             std::pair{callable_kind::constructor, "constructor"},
             std::pair{
                 callable_kind::operator_function,
                 "operator function"},
             std::pair{
                 callable_kind::member_function,
                 "member function"},
             std::pair{callable_kind::lambda, "lambda"},
         }) {
        require(
            std::ranges::contains(
                clang_namespace.callables,
                kind,
                &callable_insertion_record::kind),
            "Clang structural scan omitted the expected "
                + std::string{description} + "; "
                + clang_callable_inventory);
    }
    const auto constructor = std::ranges::find(
        clang_namespace.callables,
        callable_kind::constructor,
        &callable_insertion_record::kind);
    require(
        constructor != clang_namespace.callables.end()
            && constructor->qualified_name
                == "bioseq::fixture::target::worker::worker",
        "Clang structural scan returned the wrong constructor identity; "
            + clang_callable_inventory);
    const auto call_operator = std::ranges::find(
        clang_namespace.callables,
        callable_kind::operator_function,
        &callable_insertion_record::kind);
    require(
        call_operator != clang_namespace.callables.end()
            && call_operator->qualified_name
                == "bioseq::fixture::target::worker::operator()",
        "Clang structural scan returned the wrong overloaded-operator "
        "identity; " + clang_callable_inventory);
    require(
        std::ranges::any_of(
            clang_namespace.callables,
            [](const callable_insertion_record& callable) {
                return callable.kind == callable_kind::function
                    && callable.templated
                    && callable.qualified_name.contains("map_value");
            }),
        "Clang structural scan omitted the templated map_value definition; "
            + clang_callable_inventory);
    const auto macro_map_value = std::ranges::find_if(
        clang_namespace.callables,
        [](const callable_insertion_record& callable) {
            return callable.kind == callable_kind::function
                && callable.templated
                && callable.qualified_name.contains("macro_map_value");
        });
    require(
        macro_map_value != clang_namespace.callables.end()
            && simd_marker_source.substr(
                macro_map_value->highway_attribute_insertion.byte_offset,
                std::string_view{"FIXTURE_INLINE"}.size())
                == "FIXTURE_INLINE",
        "Clang structural scan did not map a macro-prefixed callable "
        "insertion to the writable expansion site; "
            + clang_callable_inventory);

    constexpr std::string_view macro_lambda_source = R"CPP(
export module fixture.macro_lambda;
export namespace bioseq::fixture {
namespace [[bioseq::simd]] target {
#define FIXTURE_PARAMS(...) (__VA_ARGS__)
inline int invoke() {
    auto local = [] FIXTURE_PARAMS(int value) { return value; };
    return local(3);
}
#undef FIXTURE_PARAMS
}
}
)CPP";
    const translation_unit macro_lambda = scan_translation_unit_from_code(
        "macro_lambda.cppm",
        macro_lambda_source,
        {"-std=c++26", "-xc++"},
        "macro_lambda_contract");
    const std::size_t macro_lambda_declaration =
        macro_lambda_source.find("auto local");
    const std::size_t macro_lambda_introducer_end =
        macro_lambda_source.find(']', macro_lambda_declaration) + 1u;
    require(
        macro_lambda_declaration != std::string_view::npos
            && macro_lambda_introducer_end != 0u
            && macro_lambda.marked_namespaces.size() == 1
            && std::ranges::any_of(
                macro_lambda.marked_namespaces.front().callables,
                [&](const callable_insertion_record& callable) {
                    return callable.kind == callable_kind::lambda
                        && callable.declaration_range.begin.byte_offset
                            <= macro_lambda_introducer_end
                        && macro_lambda_introducer_end
                            <= callable.body_range.begin.byte_offset
                        && callable.highway_attribute_insertion.byte_offset
                            == macro_lambda_introducer_end;
                }),
        "macroized lambda parameters must not invalidate the direct writable "
        "front-attribute slot");

    const configured_toolchain toolchain;
    assert_toolchain_exists(toolchain);
    temporary_tree temporary;
    const std::filesystem::path root = temporary.root();

    const std::filesystem::path identical_dependency_source =
        root / "dependency-only-identical.cppm";
    const auto require_dependency_only_failure =
        [&](const auto& canonical_sources,
            std::string_view expected_diagnostic,
            std::string_view case_name) {
            require_failure(
                [&] {
                    static_cast<void>(
                        prepare_dependency_compilation_environment(
                            canonical_sources,
                            identical_dependency_source,
                            root / "dependency-only-unused-commands.json",
                            toolchain.clang_binary,
                            toolchain.clang_scan_deps,
                            toolchain.llvm_config,
                            toolchain.compiler_inventory,
                            toolchain.clang_sha256,
                            toolchain.clang_scan_deps_sha256,
                            toolchain.llvm_config_sha256,
                            toolchain.compiler_inventory_sha256,
                            root / "dependency-only-unused-pcm",
                            "production",
                            "support"));
                },
                expected_diagnostic,
                case_name);
        };
    require_dependency_only_failure(
        identical_dependency_source,
        "canonical source and signature donor must be distinct",
        "scalar dependency-only source classification");
    require_dependency_only_failure(
        std::vector<std::filesystem::path>{},
        "dependency-only preparation requires at least one canonical source",
        "vector dependency-only empty classification");
    require_dependency_only_failure(
        std::vector<std::filesystem::path>{
            root / "canonical-a.cppm",
            identical_dependency_source,
        },
        "canonical source and signature donor must be distinct",
        "vector dependency-only donor classification");

    // This canonical module is syntactically valid but cannot pass Sema. A
    // successful dependency-only cold/warm PREPARE therefore proves that the
    // canonical body never entered a compiler frontend. The authenticated
    // action trace then makes that evidence independent of fixture filenames.
    std::error_code guard_root_error;
    const std::filesystem::path canonical_temporary_root =
        std::filesystem::weakly_canonical(root, guard_root_error);
    require(
        !guard_root_error
            && std::filesystem::is_directory(
                canonical_temporary_root),
        "dependency-only positive guard cannot canonicalize its existing "
        "temporary root");
    const std::filesystem::path guard_root =
        canonical_temporary_root / "dependency-only-positive";
    const std::filesystem::path guard_canonical =
        guard_root / "canonical.cppm";
    const std::filesystem::path guard_donor =
        guard_root / "donor.cppm";
    const std::filesystem::path guard_provider =
        guard_root / "provider.cppm";
    const std::filesystem::path guard_header =
        guard_root / "dependency_guard_header.hpp";
    const std::filesystem::path guard_header_alias =
        guard_root / "dependency_guard_header_alias.hpp";
    const std::filesystem::path guard_distinct_header =
        guard_root / "dependency_guard_distinct_header.hpp";
    const std::filesystem::path guard_distinct_alias =
        guard_root / "dependency_guard_distinct_alias.hpp";
    const std::filesystem::path guard_raw_database =
        guard_root / "compile_commands.json";
    const std::filesystem::path guard_pcm_root =
        guard_root / "pcm";
    const std::filesystem::path guard_owned_database =
        guard_root / "owned_compile_commands.json";
    const std::filesystem::path guard_pcm_manifest =
        guard_root / "pcm_manifest.json";
    const std::filesystem::path guard_action_trace =
        guard_root / "prepare_actions.json";
    write_file(
        guard_canonical,
        "export module fixture.dependency_guard.canonical;\n"
        "import fixture.dependency_guard.provider;\n"
        "namespace fixture::dependency_guard {\n"
        "[[maybe_unused]] inline int canonical_body_must_not_reach_sema() {\n"
        "  static_assert(false, \"canonical body reached Sema\");\n"
        "  return 0;\n"
        "}\n"
        "}\n");
    write_file(
        guard_donor,
        "export module fixture.dependency_guard.donor;\n"
        "export int dependency_guard_donor();\n");
    write_file(
        guard_header,
        "#pragma once\n"
        "inline constexpr int dependency_guard_header_value = 7;\n");
    write_file(
        guard_provider,
        "module;\n"
        "#include \"dependency_guard_header.hpp\"\n"
        "export module fixture.dependency_guard.provider;\n"
        "export int dependency_guard_provider();\n");
    require(
        std::filesystem::weakly_canonical(
            guard_root, guard_root_error) == guard_root
            && !guard_root_error,
        "dependency-only positive guard paths do not retain one canonical "
        "identity after publication");
    write_file(
        guard_raw_database,
        render_compile_database({
            make_command(toolchain, guard_root, guard_canonical),
            make_command(toolchain, guard_root, guard_donor),
            make_command(toolchain, guard_root, guard_provider),
        }));
    const auto prepare_guard = [&] {
        return prepare_dependency_compilation_environment(
            guard_canonical,
            guard_donor,
            guard_raw_database,
            toolchain.clang_binary,
            toolchain.clang_scan_deps,
            toolchain.llvm_config,
            toolchain.compiler_inventory,
            toolchain.clang_sha256,
            toolchain.clang_scan_deps_sha256,
            toolchain.llvm_config_sha256,
            toolchain.compiler_inventory_sha256,
            guard_pcm_root,
            "production",
            "support");
    };
    const dependency_compilation_authority guard_authority =
        derive_dependency_compilation_authority(
            {guard_canonical},
            guard_donor,
            guard_raw_database,
            "production");
    require(
        guard_authority.root_source_paths.size() == 2u
            && guard_authority.canonical_inputs.size() == 1u
            && guard_authority.signature_donor.module_name
                == "fixture.dependency_guard.donor",
        "dependency-only positive guard derived the wrong canonical, "
        "donor, or provider authority");
    const std::string guard_manifest_authority =
        dependency_compilation_manifest_authority(
            "support", guard_authority);
    const auto validate_guard_environment =
        [&](const dependency_compilation_environment& prepared) {
            publish_prepared(
                prepared,
                guard_owned_database,
                guard_pcm_manifest);
            validate_dependency_compilation_roots(
                guard_authority, guard_owned_database);
            return validate_dependency_compilation_environment(
                guard_authority.root_source_paths,
                guard_owned_database,
                guard_pcm_root,
                guard_pcm_manifest,
                toolchain.clang_binary,
                toolchain.clang_scan_deps,
                toolchain.llvm_config,
                toolchain.compiler_inventory,
                toolchain.clang_sha256,
                toolchain.clang_scan_deps_sha256,
                toolchain.llvm_config_sha256,
                toolchain.compiler_inventory_sha256,
                "production",
                guard_manifest_authority);
        };
    const auto count_actions =
        [](const dependency_compilation_environment& prepared,
           const frontend_action_kind kind,
           const frontend_action_outcome outcome) {
            return std::ranges::count_if(
                prepared.observed_actions,
                [&](const frontend_action_record& action) {
                    return action.kind == kind
                        && action.outcome == outcome;
                });
        };

    const dependency_compilation_environment cold_guard =
        prepare_guard();
    require(
        cold_guard.observed_actions.size() == 7u
            && count_actions(
                   cold_guard,
                   frontend_action_kind::canonical_token_slice,
                   frontend_action_outcome::executed) == 1u
            && count_actions(
                   cold_guard,
                   frontend_action_kind::p1689_dependency_discovery,
                   frontend_action_outcome::executed) == 2u
            && count_actions(
                   cold_guard,
                   frontend_action_kind::make_dependency_scan,
                   frontend_action_outcome::executed) == 2u
            && count_actions(
                   cold_guard,
                   frontend_action_kind::pcm_compilation,
                   frontend_action_outcome::executed) == 2u,
        "cold dependency-only PREPARE did not observe the exact token, "
        "P1689, make-dependency, and PCM action set");
    const dependency_compilation_validation cold_guard_validation =
        validate_guard_environment(cold_guard);
    const std::string cold_guard_trace =
        render_frontend_action_trace(cold_guard);
    require(
        cold_guard_trace.starts_with(
            "{\n  \"schema\": "
            "\"bioseq_clang_preparation_action_trace\",\n"
            "  \"schema_revision\": 1,\n"),
        "dependency-only action trace omitted its semantic schema or "
        "numeric revision");
    write_file(guard_action_trace, cold_guard_trace);
    const frontend_trace_validation cold_trace_validation =
        validate_frontend_action_trace(
            guard_action_trace,
            guard_authority,
            cold_guard_validation);
    require(
        cold_trace_validation.counts.canonical_token_slice_actions
                == 1u
            && cold_trace_validation.counts
                    .canonical_semantic_frontend_actions
                == 0u,
        "cold dependency-only action trace did not prove one canonical "
        "token scan and zero canonical semantic actions");

    const auto rewrite_guard_depfile_identity =
        [&](const std::filesystem::path& from,
            const std::filesystem::path& to) {
            std::size_t rewritten = 0u;
            const std::filesystem::path dependency_root =
                guard_pcm_root / ".dependencies";
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator{dependency_root}) {
                if (!entry.is_regular_file()
                    || entry.path().extension() != ".d") {
                    continue;
                }
                std::string document = read_file(entry.path());
                const std::string source = from.string();
                const std::string replacement = to.string();
                std::size_t position = 0u;
                bool changed = false;
                while ((position = document.find(source, position))
                       != std::string::npos) {
                    document.replace(
                        position, source.size(), replacement);
                    position += replacement.size();
                    changed = true;
                }
                if (changed) {
                    write_file(entry.path(), document);
                    ++rewritten;
                }
            }
            return rewritten;
        };
    std::error_code guard_alias_error;
    std::filesystem::create_symlink(
        guard_header, guard_header_alias, guard_alias_error);
    require(
        !guard_alias_error
            && rewrite_guard_depfile_identity(
                   guard_header, guard_header_alias) == 1u,
        "dependency-only guard could not rewrite one cached depfile "
        "through a same-file symlink alias");

    write_file(
        guard_action_trace,
        replace_json_string_field(
            cold_guard_trace,
            "trace_identity",
            std::string(64u, '0')));
    require_failure(
        [&] {
            static_cast<void>(validate_frontend_action_trace(
                guard_action_trace,
                guard_authority,
                cold_guard_validation));
        },
        "self-seal is invalid",
        "dependency-only action trace byte tamper");

    dependency_compilation_environment semantic_tamper = cold_guard;
    semantic_tamper.observed_actions.front().input_role =
        compilation_input_role::signature_donor;
    write_file(
        guard_action_trace,
        render_frontend_action_trace(semantic_tamper));
    require_failure(
        [&] {
            static_cast<void>(validate_frontend_action_trace(
                guard_action_trace,
                guard_authority,
                cold_guard_validation));
        },
        "canonical token-slice action has invalid",
        "dependency-only re-sealed semantic trace tamper");

    dependency_compilation_environment closure_tamper = cold_guard;
    closure_tamper.compile_database_json.push_back(' ');
    write_file(
        guard_action_trace,
        render_frontend_action_trace(closure_tamper));
    require_failure(
        [&] {
            static_cast<void>(validate_frontend_action_trace(
                guard_action_trace,
                guard_authority,
                cold_guard_validation));
        },
        "metadata differs from the validated",
        "dependency-only re-sealed closure trace tamper");

    const dependency_compilation_environment warm_guard =
        prepare_guard();
    require(
        warm_guard.observed_actions.size() == 7u
            && count_actions(
                   warm_guard,
                   frontend_action_kind::p1689_dependency_discovery,
                   frontend_action_outcome::executed) == 2u
            && count_actions(
                   warm_guard,
                   frontend_action_kind::make_dependency_scan,
                   frontend_action_outcome::validated_cache_hit)
                == 2u
            && count_actions(
                   warm_guard,
                   frontend_action_kind::pcm_compilation,
                   frontend_action_outcome::validated_cache_hit)
                == 2u,
        "warm dependency-only PREPARE did not authenticate exact scanner "
        "and PCM cache-hit actions");
    const dependency_compilation_validation warm_guard_validation =
        validate_guard_environment(warm_guard);
    write_file(
        guard_action_trace,
        render_frontend_action_trace(warm_guard));
    const frontend_trace_validation warm_trace_validation =
        validate_frontend_action_trace(
            guard_action_trace,
            guard_authority,
            warm_guard_validation);
    require(
        warm_trace_validation.counts.canonical_token_slice_actions
                == 1u
            && warm_trace_validation.counts
                    .canonical_semantic_frontend_actions
                == 0u,
        "warm dependency-only action trace did not preserve the "
        "canonical Sema exclusion evidence");

    const std::filesystem::path interrupted_pcm =
        first_pcm_dependency(warm_guard);
    const std::filesystem::path interrupted_provenance{
        interrupted_pcm.string() + ".provenance"};
    require(
        std::filesystem::remove(interrupted_provenance),
        "dependency-only guard could not remove one PCM commit marker");
    const std::filesystem::path interrupted_pcm_depfile{
        interrupted_pcm.string() + ".d"};
    write_file(
        interrupted_pcm_depfile,
        "this is an intentionally malformed stale compiler depfile\n");
    const dependency_compilation_environment recovered_guard =
        prepare_guard();
    require(
        count_actions(
            recovered_guard,
            frontend_action_kind::pcm_compilation,
            frontend_action_outcome::executed) == 1u
            && count_actions(
                   recovered_guard,
                   frontend_action_kind::pcm_compilation,
                   frontend_action_outcome::validated_cache_hit)
                == 1u,
        "an interrupted PCM tuple was not rebuilt while the committed "
        "sibling remained a validated hit");
    require(
        !std::filesystem::exists(interrupted_pcm_depfile),
        "recovered PCM publication retained its stale transient depfile");
    const dependency_compilation_environment recovered_warm_guard =
        prepare_guard();
    require(
        count_actions(
            recovered_warm_guard,
            frontend_action_kind::pcm_compilation,
            frontend_action_outcome::validated_cache_hit) == 2u,
        "a recovered PCM tuple did not become a warm validated hit");

    write_file(
        guard_distinct_header,
        "#pragma once\n"
        "inline constexpr int dependency_guard_header_value = 9;\n");
    guard_alias_error.clear();
    std::filesystem::create_symlink(
        guard_distinct_header,
        guard_distinct_alias,
        guard_alias_error);
    require(
        !guard_alias_error
            && rewrite_guard_depfile_identity(
                   guard_header_alias, guard_distinct_alias) == 1u,
        "dependency-only guard could not install a distinct-file depfile "
        "alias");
    require_failure(
        [&] { static_cast<void>(prepare_guard()); },
        "dependency scan and PCM compilation disagree",
        "distinct compiler depfile identity");
    require_failure(
        [&] { static_cast<void>(prepare_guard()); },
        "dependency scan and PCM compilation disagree",
        "distinct compiler depfile identity immediate retry");

    // Mirror the current repository-root layout so absolute fixture sources
    // retain the same canonical identity contract as production sources.
    const std::filesystem::path source_root = root / "codegen";
    const std::filesystem::path conditional_root =
        source_root / "conditional_root.cppm";
    const std::filesystem::path production_only =
        source_root / "production_only.cppm";
    const std::filesystem::path test_only =
        source_root / "test_only.cppm";
    const std::filesystem::path production_profile =
        root / "generated/sql/production/profile.cppm";
    const std::filesystem::path test_profile =
        root / "generated/sql/test_indexes/profile.cppm";
    write_file(
        conditional_root,
        "module;\n"
        "#include <stdarg.h>\n"
        "export module fixture.conditional_root;\n"
        "#if 0\n"
        "import fixture.inactive_missing;\n"
        "#elif defined(BIOSEQ_TEST_INDEXES)\n"
        "import fixture.test_only;\n"
        "#else\n"
        "import fixture.production_only;\n"
        "#endif\n"
        "import fixture.profile;\n"
        "export int conditional_root_value();\n");
    write_file(
        production_only,
        "export module fixture.production_only;\n"
        "export int production_only_value();\n");
    write_file(
        test_only,
        "export module fixture.test_only;\n"
        "export int test_only_value();\n");
    write_file(
        production_profile,
        "export module fixture.profile;\n"
        "export constexpr int selected_profile = 1;\n");
    write_file(
        test_profile,
        "export module fixture.profile;\n"
        "export constexpr int selected_profile = 2;\n");

    std::vector<raw_command> dual_profile_commands{
        make_command(
            toolchain,
            root,
            conditional_root,
            {
                "-resource-dir",
                (root / "untrusted-resource-dir").string(),
                "-save-temps",
                "--save-temps",
                "-save-temps=obj",
                "--save-temps=obj",
                "-DKEEP_SAVE_TEMPS=1",
                "-g",
                "-Xclang",
                "-fno-builtin-headers-in-system-modules",
                "-fbuiltin-headers-in-system-modules",
            }),
        make_command(
            toolchain, root, production_only),
        make_command(
            toolchain, root, test_only),
        make_command(
            toolchain, root, production_profile),
        make_command(
            toolchain, root, test_profile),
    };
    const std::vector<raw_command>
        canonical_dual_profile_commands =
            dual_profile_commands;
    const std::filesystem::path raw_database =
        root / "dual-profile-compile-commands.json";
    write_file(
        raw_database,
        render_compile_database(dual_profile_commands));
    const std::filesystem::path production_pcm_root =
        root / "pcm/production";
    const dependency_compilation_environment first =
        prepare(
            toolchain,
            {conditional_root},
            raw_database,
            production_pcm_root);
    const dependency_compilation_environment second =
        prepare(
            toolchain,
            {conditional_root},
            raw_database,
            production_pcm_root);
    require(
        first.compile_database_json
            == second.compile_database_json,
        "repeat PREPARE changed the owned compile database");
    require(
        first.pcm_manifest_json == second.pcm_manifest_json,
        "repeat PREPARE changed the PCM manifest");
    require(
        first.pcm_manifest_json.starts_with(
            "{\n  \"schema\": \"bioseq_clang_pcm_manifest\",\n"
            "  \"schema_revision\": 1,\n"),
        "PCM manifest omitted its semantic schema or numeric revision");
    require(
        first.compilation_signature
            == second.compilation_signature,
        "repeat PREPARE changed the compilation signature");
    require(
        first.relocatable_compilation_signature
            == second.relocatable_compilation_signature,
        "repeat PREPARE changed the relocatable compilation signature");
    require(
        first.compile_database_json.contains(
            "\"-resource-dir\"")
            && !first.compile_database_json.contains(
                (root / "untrusted-resource-dir").string()),
        "owned compile database did not replace an untrusted raw "
        "resource directory with the selected Clang resource directory");
    require(
        !first.compile_database_json.contains("save-temps")
            && first.compile_database_json.contains(
                "\"-DKEEP_SAVE_TEMPS=1\"")
            && first.compile_database_json.contains("\"-g\""),
        "owned compile database retained a non-semantic compiler temporary "
        "emission flag or removed an independent debug flag");
    const auto count_occurrences = [](
        std::string_view document,
        std::string_view token) {
        std::size_t count = 0;
        for (std::size_t position = 0;
             (position = document.find(token, position))
                 != std::string_view::npos;
             position += token.size()) {
            ++count;
        }
        return count;
    };
    const auto has_canonical_builtin_header_modes =
        [&count_occurrences](
            std::string_view document,
            std::string_view argument_vector_key) {
        static constexpr std::string_view canonical_mode =
            "\"-Xclang\", \"-fbuiltin-headers-in-system-modules\"";
        static constexpr std::string_view enabled_mode =
            "\"-fbuiltin-headers-in-system-modules\"";
        static constexpr std::string_view disabled_mode =
            "\"-fno-builtin-headers-in-system-modules\"";
        const std::size_t vector_count =
            count_occurrences(document, argument_vector_key);
        const std::size_t canonical_count =
            count_occurrences(document, canonical_mode);
        return vector_count != 0
            && canonical_count == vector_count
            && count_occurrences(document, enabled_mode)
                == canonical_count
            && count_occurrences(document, disabled_mode) == 0;
    };
    require(
        has_canonical_builtin_header_modes(
            first.compile_database_json,
            "\"arguments\": [")
            && has_canonical_builtin_header_modes(
                first.pcm_manifest_json,
                "\"base_arguments\": ["),
        "owned compile database or PCM manifest did not canonicalize the "
        "linked-Clang builtin-header system-module mode exactly once per "
        "argument vector");
    require(
        first.pcm_manifest_json.contains(
            production_profile.string())
            && !first.pcm_manifest_json.contains(
                test_profile.string())
            && first.pcm_manifest_json.contains(
                production_only.string())
            && !first.pcm_manifest_json.contains(
                test_only.string())
            && !first.pcm_manifest_json.contains(
                "fixture.inactive_missing"),
        "production P1689 closure selected an inactive or opposite-profile "
        "provider");

    std::ranges::reverse(dual_profile_commands);
    const std::filesystem::path permuted_database =
        root / "dual-profile-permuted-compile-commands.json";
    write_file(
        permuted_database,
        render_compile_database(dual_profile_commands));
    const dependency_compilation_environment permuted =
        prepare(
            toolchain,
            {conditional_root},
            permuted_database,
            production_pcm_root);
    require(
        first.compile_database_json
                == permuted.compile_database_json
            && first.pcm_manifest_json
                == permuted.pcm_manifest_json
            && first.compilation_signature
                == permuted.compilation_signature
            && first.relocatable_compilation_signature
                == permuted.relocatable_compilation_signature,
        "raw compile-database ordering changed PREPARE bytes");

    std::vector<raw_command> shuffled_commands =
        canonical_dual_profile_commands;
    std::rotate(
        shuffled_commands.begin(),
        shuffled_commands.begin() + 2,
        shuffled_commands.end());
    std::ranges::swap(
        shuffled_commands[1],
        shuffled_commands[3]);
    const std::filesystem::path shuffled_database =
        root / "dual-profile-shuffled-compile-commands.json";
    write_file(
        shuffled_database,
        render_compile_database(shuffled_commands));
    const dependency_compilation_environment shuffled =
        prepare(
            toolchain,
            {conditional_root},
            shuffled_database,
            production_pcm_root);
    require(
        first.compile_database_json
                == shuffled.compile_database_json
            && first.pcm_manifest_json
                == shuffled.pcm_manifest_json
            && first.compilation_signature
                == shuffled.compilation_signature
            && first.relocatable_compilation_signature
                == shuffled.relocatable_compilation_signature,
        "shuffled raw compile-database ordering changed PREPARE bytes");

    const std::filesystem::path lane_a =
        root / "build/lane-a";
    const std::filesystem::path lane_b =
        root / "build/lane-b";
    const std::filesystem::path lane_c =
        root / "build/lane-c";
    const auto lane_commands =
        [&toolchain,
         &conditional_root,
         &production_only,
         &test_only,
         &production_profile,
         &test_profile](const std::filesystem::path& lane,
            const std::filesystem::path& relative_include) {
            const std::filesystem::path package_include =
                lane / relative_include;
            std::filesystem::create_directories(package_include);
            const std::vector<std::string> lane_arguments{
                "-isystem",
                package_include.string(),
            };
            return std::vector<raw_command>{
                make_command(
                    toolchain,
                    lane,
                    conditional_root,
                    lane_arguments),
                make_command(
                    toolchain,
                    lane,
                    production_only,
                    lane_arguments),
                make_command(
                    toolchain,
                    lane,
                    test_only,
                    lane_arguments),
                make_command(
                    toolchain,
                    lane,
                    production_profile,
                    lane_arguments),
                make_command(
                    toolchain,
                    lane,
                    test_profile,
                    lane_arguments),
            };
        };
    const std::filesystem::path lane_a_database =
        lane_a / "compile_commands.json";
    const std::filesystem::path lane_b_database =
        lane_b / "compile_commands.json";
    write_file(
        lane_a_database,
        render_compile_database(lane_commands(
            lane_a, "vcpkg_installed/include")));
    write_file(
        lane_b_database,
        render_compile_database(lane_commands(
            lane_b, "vcpkg_installed/include")));
    const std::filesystem::path lane_a_pcm =
        lane_a / "pcm/production";
    const std::filesystem::path lane_b_pcm =
        lane_b / "pcm/production";
    const dependency_compilation_environment lane_a_prepared =
        prepare(
            toolchain,
            {conditional_root},
            lane_a_database,
            lane_a_pcm);
    const dependency_compilation_environment lane_b_prepared =
        prepare(
            toolchain,
            {conditional_root},
            lane_b_database,
            lane_b_pcm);
    const auto module_string_field =
        [](const std::string& manifest,
           std::string_view module,
           std::string_view field) {
            const std::string module_marker =
                "\"name\": \"" + std::string{module} + "\"";
            const std::size_t module_offset =
                manifest.find(module_marker);
            if (module_offset == std::string::npos) {
                throw std::runtime_error(
                    "PREPARE contract fixture cannot find module "
                    + std::string{module});
            }
            const std::string field_marker =
                "\"" + std::string{field} + "\": \"";
            const std::size_t field_offset =
                manifest.find(field_marker, module_offset);
            if (field_offset == std::string::npos) {
                throw std::runtime_error(
                    "PREPARE contract fixture cannot find field "
                    + std::string{field} + " for module "
                    + std::string{module});
            }
            const std::size_t value_begin =
                field_offset + field_marker.size();
            const std::size_t value_end =
                manifest.find('"', value_begin);
            if (value_end == std::string::npos) {
                throw std::runtime_error(
                    "PREPARE contract fixture found an unterminated "
                    "module field");
            }
            return manifest.substr(
                value_begin, value_end - value_begin);
        };
    for (const std::string_view field : {
             "relocatable_arguments_sha256",
             "relocatable_command_input_sha256",
             "relocatable_active_import_scan_database_sha256",
             "relocatable_active_import_scan_result_sha256",
             "relocatable_active_import_scan_diagnostics_sha256",
             "relocatable_depfile_sha256",
             "relocatable_dependency_content_sha256",
             "relocatable_content_key",
         }) {
        require(
            module_string_field(
                lane_a_prepared.pcm_manifest_json,
                "fixture.production_only",
                field)
                == module_string_field(
                    lane_b_prepared.pcm_manifest_json,
                    "fixture.production_only",
                    field),
            "lane-local build roots changed fixture.production_only "
                + std::string{field});
    }
    require(
        lane_a_prepared.compilation_signature
            != lane_b_prepared.compilation_signature,
        "lane-local build roots did not change physical PREPARE "
        "provenance");
    const auto project_lane =
        [](std::string document,
           const std::filesystem::path& lane) {
            constexpr std::string_view logical{
                "$BIOSEQ_BUILD_ROOT"};
            for (const std::filesystem::path& spelling : {
                     std::filesystem::weakly_canonical(lane),
                     lane.lexically_normal(),
                 }) {
                const std::string physical =
                    spelling.generic_string();
                std::size_t offset = 0;
                while ((offset = document.find(
                            physical, offset))
                       != std::string::npos) {
                    document.replace(
                        offset, physical.size(), logical);
                    offset += logical.size();
                }
            }
            return document;
        };
    const std::string lane_a_projected_database =
        project_lane(
            lane_a_prepared.compile_database_json, lane_a);
    const std::string lane_b_projected_database =
        project_lane(
            lane_b_prepared.compile_database_json, lane_b);
    if (lane_a_projected_database
        != lane_b_projected_database) {
        const auto [lane_a_mismatch, lane_b_mismatch] =
            std::ranges::mismatch(
                lane_a_projected_database,
                lane_b_projected_database);
        const std::size_t mismatch_offset =
            static_cast<std::size_t>(
                lane_a_mismatch
                - lane_a_projected_database.begin());
        const std::size_t excerpt_begin =
            mismatch_offset > 40 ? mismatch_offset - 40 : 0;
        require(
            false,
            "lane-local owned compile databases differ beyond their "
            "exact build roots at offset "
                + std::to_string(mismatch_offset)
                + ": lane-a='"
                + lane_a_projected_database.substr(
                    excerpt_begin, 120)
                + "' lane-b='"
                + lane_b_projected_database.substr(
                    excerpt_begin, 120)
                + "'");
    }
    require(
        lane_a_prepared.relocatable_compile_database_sha256
            == lane_b_prepared.relocatable_compile_database_sha256,
        "lane-local build roots changed the canonical owned compile "
        "database");
    require(
        lane_a_prepared.relocatable_compilation_signature
            == lane_b_prepared.relocatable_compilation_signature,
        "lane-local build roots changed the canonical PREPARE "
        "signature");
    const std::filesystem::path lane_c_database =
        lane_c / "compile_commands.json";
    write_file(
        lane_c_database,
        render_compile_database(lane_commands(
            lane_c, "other_include")));
    const dependency_compilation_environment lane_c_prepared =
        prepare(
            toolchain,
            {conditional_root},
            lane_c_database,
            lane_c / "pcm/production");
    require(
        lane_c_prepared.relocatable_compilation_signature
            != lane_a_prepared.relocatable_compilation_signature,
        "build-root normalization dropped a semantic lane-relative "
        "argument path");
    const std::filesystem::path lane_a_owned_database =
        lane_a / "owned_compile_commands.json";
    const std::filesystem::path lane_a_manifest =
        lane_a / "pcm-manifest.json";
    publish_prepared(
        lane_a_prepared,
        lane_a_owned_database,
        lane_a_manifest);
    const dependency_compilation_validation lane_a_validation =
        validate(
            toolchain,
            {conditional_root},
            lane_a_owned_database,
            lane_a_pcm,
            lane_a_manifest);
    require(
        lane_a_validation.relocatable_compilation_signature
            == lane_a_prepared.relocatable_compilation_signature,
        "validation did not preserve canonical build-lane provenance");

    const std::string original_production_only =
        read_file(production_only);
    write_file(
        production_only,
        "export module fixture.production_only;\n"
        "export int semantically_changed_production_value();\n");
    const dependency_compilation_environment semantic_mutation =
        prepare(
            toolchain,
            {conditional_root},
            raw_database,
            root / "pcm/semantic-mutation");
    require(
        semantic_mutation.relocatable_compilation_signature
            != first.relocatable_compilation_signature,
        "semantic source mutation did not change the relocatable "
        "PREPARE signature");
    write_file(production_only, original_production_only);

    const std::filesystem::path physical_clang =
        std::filesystem::weakly_canonical(
            toolchain.clang_binary);
    const std::filesystem::path clang_alias =
        root / "selected-clang-alias/clang++";
    std::filesystem::create_directories(
        clang_alias.parent_path());
    std::filesystem::create_symlink(
        physical_clang, clang_alias);
    configured_toolchain alias_toolchain = toolchain;
    alias_toolchain.clang_binary = clang_alias;
    configured_toolchain physical_toolchain = toolchain;
    physical_toolchain.clang_binary = physical_clang;
    const std::filesystem::path compiler_identity_pcm_root =
        root / "pcm/compiler-identity";
    const dependency_compilation_environment alias_prepared =
        prepare(
            alias_toolchain,
            {conditional_root},
            raw_database,
            compiler_identity_pcm_root);
    std::error_code compiler_identity_cleanup_error;
    std::filesystem::remove_all(
        compiler_identity_pcm_root,
        compiler_identity_cleanup_error);
    require(
        !compiler_identity_cleanup_error,
        "cannot reset the compiler-identity PCM fixture");
    const dependency_compilation_environment physical_prepared =
        prepare(
            physical_toolchain,
            {conditional_root},
            raw_database,
            compiler_identity_pcm_root);
    require(
        alias_prepared.relocatable_compilation_signature
                == physical_prepared
                       .relocatable_compilation_signature
            && alias_prepared
                   .relocatable_compile_database_sha256
                == physical_prepared
                       .relocatable_compile_database_sha256
            && alias_prepared
                   .relocatable_pcm_manifest_sha256
                == physical_prepared
                       .relocatable_pcm_manifest_sha256,
        "symlink and physical spellings of the same authenticated Clang "
        "changed relocatable PREPARE provenance");

    const std::filesystem::path different_clang =
        root / "different-clang/clang++";
    const std::filesystem::path different_clang_sentinel =
        root / "different-clang-was-executed";
    write_file(
        different_clang,
        "#!/bin/sh\n"
        ": > \"" + different_clang_sentinel.string() + "\"\n"
        "exit 0\n");
    std::filesystem::permissions(
        different_clang,
        std::filesystem::perms::owner_exec
            | std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    configured_toolchain different_toolchain = toolchain;
    different_toolchain.clang_binary = different_clang;
    require_failure(
        [&] {
            static_cast<void>(prepare(
                different_toolchain,
                {conditional_root},
                raw_database,
                root / "pcm/different-compiler"));
        },
        "configure-time SHA-256",
        "different compiler bytes");
    require(
        !std::filesystem::exists(
            different_clang_sentinel),
        "different compiler bytes were executed before authentication");

    const dependency_compilation_environment test_prepared =
        prepare(
            toolchain,
            {conditional_root},
            raw_database,
            root / "pcm/test_indexes",
            "test_indexes");
    require(
        test_prepared.pcm_manifest_json.contains(
            test_profile.string())
            && !test_prepared.pcm_manifest_json.contains(
                production_profile.string())
            && test_prepared.pcm_manifest_json.contains(
                test_only.string())
            && !test_prepared.pcm_manifest_json.contains(
                production_only.string()),
        "test-index P1689 closure selected the production branch");

    const std::filesystem::path partition_source =
        source_root / "partitioned_entities.cppm";
    const std::filesystem::path partitioned_primary =
        source_root / "partitioned.cppm";
    const std::filesystem::path partition_consumer =
        source_root / "partition_consumer.cppm";
    write_file(
        partition_source,
        "export module fixture.partitioned:entities;\n"
        "export inline constexpr int partition_value = 42;\n");
    write_file(
        partitioned_primary,
        "export module fixture.partitioned;\n"
        "export import :entities;\n");
    write_file(
        partition_consumer,
        "export module fixture.partition_consumer;\n"
        "import fixture.partitioned;\n"
        "static_assert(partition_value == 42);\n");
    const std::filesystem::path partition_database =
        root / "partition-compile-commands.json";
    write_file(
        partition_database,
        render_compile_database(
            {
                make_command(
                    toolchain, root, partition_source),
                make_command(
                    toolchain, root, partitioned_primary),
                make_command(
                    toolchain, root, partition_consumer),
            }));
    const std::filesystem::path partition_pcm_root =
        root / "pcm/partition";
    const dependency_compilation_environment partition_prepared =
        prepare(
            toolchain,
            {partition_consumer},
            partition_database,
            partition_pcm_root);
    require(
        partition_prepared.compile_database_json.contains(
            "-fmodule-file=fixture.partitioned:entities=")
            && partition_prepared.compile_database_json.contains(
                "-fmodule-file=fixture.partitioned="),
        "consumer command omitted a transitive partition PCM mapping");
    require(
        partition_prepared.pcm_manifest_json.contains(
            "\"direct_imports\": [\"fixture.partitioned\"]")
            && partition_prepared.pcm_manifest_json.contains(
                "\"direct_imports\": "
                "[\"fixture.partitioned:entities\"]"),
        "partition fixture flattened the manifest's direct import graph");
    const std::filesystem::path partition_owned_database =
        root / "partition-owned-compile-commands.json";
    const std::filesystem::path partition_manifest =
        root / "partition-pcm-manifest.json";
    publish_prepared(
        partition_prepared,
        partition_owned_database,
        partition_manifest);
    const dependency_compilation_validation partition_validation =
        validate(
            toolchain,
            {partition_consumer},
            partition_owned_database,
            partition_pcm_root,
            partition_manifest);
    require(
        partition_validation.compilation_signature
            == partition_prepared.compilation_signature,
        "transitive partition mapping failed PREPARE validation");
    require(
        partition_validation.relocatable_compilation_signature
            == partition_prepared.relocatable_compilation_signature,
        "transitive partition mapping changed the relocatable PREPARE "
        "signature");

    const std::filesystem::path owned_database =
        root / "owned-compile-commands.json";
    const std::filesystem::path manifest =
        root / "pcm-manifest.json";
    publish_prepared(first, owned_database, manifest);
    const dependency_compilation_validation valid =
        validate(
            toolchain,
            {conditional_root},
            owned_database,
            production_pcm_root,
            manifest);
    require(
        valid.compilation_signature
            == first.compilation_signature,
        "valid PREPARE manifest reconstructed a different signature");
    require(
        valid.relocatable_compilation_signature
            == first.relocatable_compilation_signature,
        "valid PREPARE manifest reconstructed a different relocatable "
        "signature");

    const std::filesystem::path dependency_scan_result =
        first_dependency_scan_result(production_pcm_root);
    const std::string original_dependency_scan_result =
        read_file(dependency_scan_result);
    write_file(
        dependency_scan_result,
        original_dependency_scan_result + "\n");
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "active-import scan artifact hash is stale");
    write_file(
        dependency_scan_result,
        original_dependency_scan_result);
    const dependency_compilation_validation restored_scan_validation =
        validate(
            toolchain,
            {conditional_root},
            owned_database,
            production_pcm_root,
            manifest);
    require(
        restored_scan_validation.compilation_signature
                == first.compilation_signature
            && restored_scan_validation.relocatable_compilation_signature
                == first.relocatable_compilation_signature,
        "restored active-import scan artifact failed PREPARE validation");

    const module_graph resource_dir_graph =
        scan_module_graph_from_database(
            {conditional_root},
            owned_database,
            "production",
            production_pcm_root);
    require(
        resource_dir_graph.units.size() == 1
            && resource_dir_graph.units.front().source_identity
                == "codegen/conditional_root.cppm"
            && resource_dir_graph.units.front().provided_module
                == "fixture.conditional_root",
        "owned compile database cannot retain current-root source identity "
        "and resolve selected-Clang builtin headers in the LibTooling "
        "module-graph pass");
    const std::filesystem::path pcm =
        first_pcm_dependency(first);
    require(
        std::ranges::find(first.dependencies, pcm)
            != first.dependencies.end()
            && std::ranges::find(
                   first.dependencies,
                   std::filesystem::path{
                       pcm.string() + ".provenance"})
                != first.dependencies.end(),
        "PREPARE depfile closure omits PCM or provenance");

    const std::string original_pcm = read_file(pcm);
    write_file(pcm, original_pcm + "corrupt-nonzero-pcm");
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "PCM is missing, foreign, or corrupt");
    write_file(pcm, original_pcm);

    const std::filesystem::path provenance{
        pcm.string() + ".provenance"};
    const std::string original_provenance =
        read_file(provenance);
    write_file(
        provenance,
        original_provenance + "corrupt-provenance");
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "PCM provenance is corrupt or stale");
    write_file(provenance, original_provenance);

    const std::string original_source =
        read_file(conditional_root);
    write_file(
        conditional_root,
        original_source + "\n// stale source\n");
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "source hash is stale");
    write_file(conditional_root, original_source);

    const std::string original_owned_database =
        read_file(owned_database);
    write_file(
        owned_database,
        original_owned_database + "\n");
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "owned compile database differs");
    write_file(owned_database, original_owned_database);

    const std::string original_manifest = read_file(manifest);
    std::vector<std::filesystem::path> prepared_depfiles;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{
             production_pcm_root / ".dependencies"}) {
        if (entry.is_regular_file()
            && entry.path().extension() == ".d") {
            prepared_depfiles.push_back(entry.path());
        }
    }
    std::ranges::sort(prepared_depfiles);
    require(
        !prepared_depfiles.empty(),
        "PREPARE contract fixture did not publish a make depfile");
    const std::filesystem::path alternate_depfile =
        production_pcm_root / "alternate-manifest-depfile.d";
    write_file(
        alternate_depfile, read_file(prepared_depfiles.front()));
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "depfile",
            alternate_depfile.string()));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "exact content-derived path",
        "alternate in-root depfile spelling");
    const std::filesystem::path alternate_pcm =
        production_pcm_root / "alternate-manifest-pcm.pcm";
    write_file(alternate_pcm, read_file(pcm));
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "pcm",
            alternate_pcm.string()));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "exact content-derived path",
        "alternate in-root PCM spelling");
    const std::filesystem::path alternate_provenance =
        production_pcm_root / "alternate-manifest.provenance";
    write_file(alternate_provenance, read_file(provenance));
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "pcm_provenance",
            alternate_provenance.string()));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "exact content-derived path",
        "alternate in-root PCM provenance spelling");
    write_file(manifest, original_manifest);
    std::string unknown_field_manifest = original_manifest;
    const std::size_t root_end =
        unknown_field_manifest.rfind("\n}\n");
    require(
        root_end != std::string::npos,
        "PREPARE contract fixture cannot locate manifest root end");
    unknown_field_manifest.insert(
        root_end,
        ",\n  \"unknown_contract_field\": \"rejected\"");
    write_file(manifest, unknown_field_manifest);
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "unknown or duplicate field");
    write_file(manifest, original_manifest);
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "clang_binary_sha256",
            std::string(64, '0')));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "Clang binary hash is stale");
    write_file(manifest, original_manifest);
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "clang_resource_dir",
            (root / "untrusted-resource-dir").string()));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "toolchain build/resource identity is stale");
    write_file(manifest, original_manifest);
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "relocatable_compilation_signature",
            std::string(64, '0')));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "relocatable compilation signature is stale");
    write_file(manifest, original_manifest);
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest,
                "production",
                std::string(64, '0')));
        },
        "configure-time SHA-256");

    const std::filesystem::path fake_clang =
        root / "fake-clang";
    const std::filesystem::path fake_sentinel =
        root / "fake-clang-was-executed";
    write_file(
        fake_clang,
        "#!/bin/sh\n"
        ": > \"" + fake_sentinel.string() + "\"\n"
        "exit 0\n");
    std::filesystem::permissions(
        fake_clang,
        std::filesystem::perms::owner_exec
            | std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "clang_binary",
            fake_clang.string()));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest,
                "production",
                toolchain.clang_sha256,
                fake_clang));
        },
        "configure-time SHA-256");
    require(
        !std::filesystem::exists(fake_sentinel),
        "manifest-controlled fake Clang was executed");
    write_file(manifest, original_manifest);

    const std::filesystem::path outside_root =
        root / "outside/escaped.pcm";
    write_file(outside_root, original_pcm);
    const std::filesystem::path pcm_escape =
        production_pcm_root / "escaped.pcm";
    std::error_code symlink_error;
    std::filesystem::create_symlink(
        outside_root, pcm_escape, symlink_error);
    if (symlink_error) {
        throw std::runtime_error(
            "PREPARE contract test cannot create PCM escape symlink: "
            + symlink_error.message());
    }
    write_file(
        manifest,
        replace_json_string_field(
            original_manifest,
            "pcm",
            pcm_escape.string()));
    require_failure(
        [&] {
            static_cast<void>(validate(
                toolchain,
                {conditional_root},
                owned_database,
                production_pcm_root,
                manifest));
        },
        "foreign");
    write_file(manifest, original_manifest);

    const std::filesystem::path symlink_case =
        root / "symlink-root";
    const std::filesystem::path real_pcm_root =
        symlink_case / "real-pcm";
    const std::filesystem::path linked_pcm_root =
        symlink_case / "linked-pcm";
    std::filesystem::create_directories(real_pcm_root);
    symlink_error.clear();
    std::filesystem::create_directory_symlink(
        real_pcm_root, linked_pcm_root, symlink_error);
    if (symlink_error) {
        throw std::runtime_error(
            "PREPARE contract test cannot create positive PCM-root "
            "symlink: " + symlink_error.message());
    }
    const dependency_compilation_environment symlink_prepared =
        prepare(
            toolchain,
            {conditional_root},
            raw_database,
            linked_pcm_root);
    const std::filesystem::path symlink_owned =
        symlink_case / "owned.json";
    const std::filesystem::path symlink_manifest =
        symlink_case / "manifest.json";
    publish_prepared(
        symlink_prepared,
        symlink_owned,
        symlink_manifest);
    static_cast<void>(validate(
        toolchain,
        {conditional_root},
        symlink_owned,
        linked_pcm_root,
        symlink_manifest));

    const std::filesystem::path outside_sentinel_directory =
        root / "prepare-owned-sentinel";
    const std::filesystem::path outside_sentinel =
        outside_sentinel_directory / "sentinel.txt";
    constexpr std::string_view sentinel_bytes{
        "outside PREPARE sentinel must remain unchanged\n"};
    write_file(outside_sentinel, sentinel_bytes);
    const auto require_owned_directory_symlink_rejection =
        [&](std::string_view component,
            std::string_view case_name) {
            const std::filesystem::path case_root =
                root / ("linked-owned-directory-"
                        + std::string{case_name});
            std::filesystem::create_directories(case_root);
            if (component == ".dependencies") {
                std::filesystem::create_directory(
                    case_root / ".module-dependency-scan");
            }
            std::error_code link_error;
            std::filesystem::create_directory_symlink(
                outside_sentinel_directory,
                case_root / component,
                link_error);
            require(
                !link_error,
                "PREPARE contract fixture cannot create nested owned "
                "directory symlink");
            require_failure(
                [&] {
                    static_cast<void>(prepare(
                        toolchain,
                        {conditional_root},
                        raw_database,
                        case_root));
                },
                "owned directory is a symlink or non-directory",
                case_name);
            require(
                read_file(outside_sentinel) == sentinel_bytes,
                "nested owned-directory symlink mutated the outside "
                "sentinel");
        };
    require_owned_directory_symlink_rejection(
        ".module-dependency-scan", "P1689 directory confinement");
    require_owned_directory_symlink_rejection(
        ".dependencies", "Make dependency directory confinement");

    const std::filesystem::path leaf_pcm_root =
        root / "linked-owned-leaf-pcm";
    const dependency_compilation_environment leaf_prepared =
        prepare(
            toolchain,
            {conditional_root},
            raw_database,
            leaf_pcm_root);
    const auto reject_leaf_symlink =
        [&](const std::filesystem::path& leaf,
            std::string_view expected_diagnostic,
            std::string_view case_name) {
            const std::string original =
                std::filesystem::is_regular_file(leaf)
                ? read_file(leaf)
                : std::string{};
            std::error_code leaf_error;
            std::filesystem::remove(leaf, leaf_error);
            leaf_error.clear();
            std::filesystem::create_symlink(
                outside_sentinel, leaf, leaf_error);
            require(
                !leaf_error,
                "PREPARE contract fixture cannot create owned leaf "
                "symlink");
            require_failure(
                [&] {
                    static_cast<void>(prepare(
                        toolchain,
                        {conditional_root},
                        raw_database,
                        leaf_pcm_root));
                },
                expected_diagnostic,
                case_name);
            require(
                read_file(outside_sentinel) == sentinel_bytes,
                "owned leaf symlink mutated the outside sentinel");
            leaf_error.clear();
            std::filesystem::remove(leaf, leaf_error);
            require(
                !leaf_error,
                "PREPARE contract fixture cannot remove owned leaf "
                "symlink");
            if (!original.empty()) {
                write_file(leaf, original);
            }
        };
    reject_leaf_symlink(
        first_dependency_scan_result(leaf_pcm_root),
        "P1689 scan result is a symlink or non-regular file",
        "P1689 result leaf confinement");
    reject_leaf_symlink(
        first_dependency_scan_artifact(
            leaf_pcm_root, ".diagnostics.txt"),
        "P1689 diagnostics is a symlink or non-regular file",
        "P1689 diagnostics leaf confinement");
    const std::filesystem::path linked_leaf_pcm =
        first_pcm_dependency(leaf_prepared);
    reject_leaf_symlink(
        linked_leaf_pcm,
        "PCM output is a symlink or non-regular file",
        "PCM leaf confinement");
    reject_leaf_symlink(
        std::filesystem::path{linked_leaf_pcm.string() + ".o"},
        "PCM object output is a symlink or non-regular file",
        "PCM object leaf confinement");
    reject_leaf_symlink(
        std::filesystem::path{linked_leaf_pcm.string() + ".d"},
        "PCM dependency output is a symlink or non-regular file",
        "PCM depfile leaf confinement");

    const std::filesystem::path invalid_root =
        root / "invalid";
    const auto invalid_database =
        [&](std::string_view name,
            const std::vector<raw_command>& commands) {
            const std::filesystem::path path =
                invalid_root / (std::string{name} + ".json");
            write_file(path, render_compile_database(commands));
            return path;
        };

    const std::filesystem::path missing_source =
        invalid_root / "missing.cppm";
    const std::filesystem::path empty_database =
        invalid_database("empty", {});
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {missing_source},
                empty_database,
                invalid_root / "pcm-missing-source"));
        },
        "source does not exist");

    const std::filesystem::path no_command =
        invalid_root / "no_command.cppm";
    const std::filesystem::path other =
        invalid_root / "other.cppm";
    write_file(
        no_command,
        "export module fixture.no_command;\n");
    write_file(
        other,
        "export module fixture.other;\n");
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {no_command},
                invalid_database(
                    "missing-command",
                    {make_command(
                        toolchain, invalid_root, other)}),
                invalid_root / "pcm-missing-command"));
        },
        "no command for registered source");

    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {no_command, no_command},
                invalid_database(
                    "duplicate-root",
                    {make_command(
                        toolchain,
                        invalid_root,
                        no_command)}),
                invalid_root / "pcm-duplicate-root"));
        },
        "duplicate");

    const std::filesystem::path duplicate_a =
        invalid_root / "duplicate_a.cppm";
    const std::filesystem::path duplicate_b =
        invalid_root / "duplicate_b.cppm";
    write_file(
        duplicate_a,
        "export module fixture.duplicate;\n");
    write_file(
        duplicate_b,
        "export module fixture.duplicate;\n");
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {duplicate_a},
                invalid_database(
                    "duplicate-provider",
                    {
                        make_command(
                            toolchain,
                            invalid_root,
                            duplicate_a),
                        make_command(
                            toolchain,
                            invalid_root,
                            duplicate_b),
                    }),
                invalid_root / "pcm-duplicate-provider"));
        },
        "duplicate Clang module provider");

    const std::filesystem::path missing_provider =
        invalid_root / "missing_provider.cppm";
    write_file(
        missing_provider,
        "export module fixture.missing_provider_root;\n"
        "import fixture.provider_that_does_not_exist;\n");
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {missing_provider},
                invalid_database(
                    "missing-provider",
                    {make_command(
                        toolchain,
                        invalid_root,
                        missing_provider)}),
                invalid_root / "pcm-missing-provider"));
        },
        "has no source provider",
        "missing imported provider");

    const std::filesystem::path cycle_a =
        invalid_root / "cycle_a.cppm";
    const std::filesystem::path cycle_b =
        invalid_root / "cycle_b.cppm";
    write_file(
        cycle_a,
        "export module fixture.cycle_a;\n"
        "import fixture.cycle_b;\n");
    write_file(
        cycle_b,
        "export module fixture.cycle_b;\n"
        "import fixture.cycle_a;\n");
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {cycle_a},
                invalid_database(
                    "cycle",
                    {
                        make_command(
                            toolchain,
                            invalid_root,
                            cycle_a),
                        make_command(
                            toolchain,
                            invalid_root,
                            cycle_b),
                    }),
                invalid_root / "pcm-cycle"));
        },
        "module dependency cycle");

    const std::filesystem::path unknown_std =
        invalid_root / "unknown_std.cppm";
    write_file(
        unknown_std,
        "export module fixture.unknown_std;\n"
        "import std.unknown;\n");
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {unknown_std},
                invalid_database(
                    "unknown-std",
                    {make_command(
                        toolchain,
                        invalid_root,
                        unknown_std)}),
                invalid_root / "pcm-unknown-std"));
        },
        "unsupported compiler-owned module import");

    const std::filesystem::path opposite_root =
        invalid_root / "opposite_profile_root.cppm";
    const std::filesystem::path opposite_test_profile =
        invalid_root
        / "generated/sql/test_indexes/opposite_profile.cppm";
    write_file(
        opposite_root,
        "export module fixture.opposite_profile_root;\n"
        "import fixture.opposite_profile;\n");
    write_file(
        opposite_test_profile,
        "export module fixture.opposite_profile;\n");
    const std::vector<raw_command> opposite_only_commands{
        make_command(
            toolchain, invalid_root, opposite_root),
        make_command(
            toolchain,
            invalid_root,
            opposite_test_profile),
    };
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {opposite_root},
                invalid_database(
                    "opposite-only-profile",
                    opposite_only_commands),
                invalid_root / "pcm-opposite-only"));
        },
        "has no source provider",
        "opposite-only generated profile");

    const std::filesystem::path ambiguous =
        invalid_root / "ambiguous.cppm";
    write_file(
        ambiguous,
        "export module fixture.ambiguous;\n");
    std::vector<raw_command> ambiguous_commands{
        make_command(
            toolchain,
            invalid_root,
            ambiguous,
            {"-DFIXTURE_COMMAND=1"}),
        make_command(
            toolchain,
            invalid_root,
            ambiguous,
            {"-DFIXTURE_COMMAND=2"}),
    };
    const std::filesystem::path unrelated_root =
        invalid_root / "unrelated_ambiguous_root.cppm";
    write_file(
        unrelated_root,
        "export module fixture.unrelated_ambiguous_root;\n");
    std::vector<raw_command> unrelated_ambiguous_commands =
        ambiguous_commands;
    unrelated_ambiguous_commands.push_back(
        make_command(
            toolchain,
            invalid_root,
            unrelated_root));
    const dependency_compilation_environment
        unrelated_ambiguous_prepared = prepare(
            toolchain,
            {unrelated_root},
            invalid_database(
                "unrelated-ambiguous",
                unrelated_ambiguous_commands),
            invalid_root / "pcm-unrelated-ambiguous");
    require(
        !unrelated_ambiguous_prepared.compile_database_json.contains(
            ambiguous.string()),
        "unrelated ambiguous command entered the prepared closure");

    for (unsigned int order = 0; order < 2; ++order) {
        require_failure(
            [&] {
                static_cast<void>(prepare(
                    toolchain,
                    {ambiguous},
                    invalid_database(
                        "ambiguous-" + std::to_string(order),
                        ambiguous_commands),
                    invalid_root
                        / ("pcm-ambiguous-"
                           + std::to_string(order))));
            },
            "equal-preference commands");
        std::ranges::reverse(ambiguous_commands);
    }

    const std::filesystem::path response_root =
        root / "response";
    const std::filesystem::path response_source =
        response_root / "response.cppm";
    const std::filesystem::path include_with_spaces =
        response_root / "include with spaces";
    std::filesystem::create_directories(include_with_spaces);
    const std::filesystem::path response_header =
        include_with_spaces / "fixture.hpp";
    write_file(
        response_header,
        "#define BIOSEQ_PREPARE_RESPONSE_FIXTURE 1\n");
    write_file(
        response_source,
        "module;\n"
        "#include <fixture.hpp>\n"
        "export module fixture.response;\n");
    const std::filesystem::path quoted_response =
        response_root / "quoted.rsp";
    write_file(
        quoted_response,
        "\"-I" + include_with_spaces.string()
            + "\" -DFROM_QUOTED_RESPONSE=1 --save-temps=obj\n");
    const std::filesystem::path quoted_database =
        invalid_database(
            "quoted-response",
            {make_command(
                toolchain,
                response_root,
                response_source,
                {"@" + quoted_response.string()})});
    const dependency_compilation_environment quoted_prepared =
        prepare(
            toolchain,
            {response_source},
            quoted_database,
            response_root / "pcm-quoted");
    require(
        std::ranges::find(
            quoted_prepared.dependencies,
            std::filesystem::weakly_canonical(
                quoted_response))
            != quoted_prepared.dependencies.end(),
        "quoted response file is absent from PREPARE dependencies");
    require(
        !quoted_prepared.compile_database_json.contains("save-temps")
            && quoted_prepared.compile_database_json.contains(
                "\"-DFROM_QUOTED_RESPONSE=1\""),
        "PREPARE did not expand and sanitize the response-file compiler "
        "temporary emission flag");
    const dependency_compilation_environment quoted_control =
        prepare(
            toolchain,
            {response_source},
            quoted_database,
            response_root / "pcm-quoted");
    require(
        quoted_control.relocatable_compilation_signature
                == quoted_prepared.relocatable_compilation_signature
            && quoted_control.relocatable_compile_database_sha256
                == quoted_prepared.relocatable_compile_database_sha256
            && quoted_control.relocatable_pcm_manifest_sha256
                == quoted_prepared.relocatable_pcm_manifest_sha256,
        "an unchanged PREPARE closure changed during an immediate "
        "control repeat");
    write_file(
        quoted_response,
        "\"-I" + include_with_spaces.string()
            + "\" -DFROM_QUOTED_RESPONSE=2 --save-temps=obj\n");
    const dependency_compilation_environment
        quoted_response_mutation =
            prepare(
                toolchain,
                {response_source},
                quoted_database,
                response_root / "pcm-quoted");
    require(
        quoted_response_mutation.relocatable_compilation_signature
            != quoted_prepared.relocatable_compilation_signature,
        "response-file content drift did not change the relocatable "
        "PREPARE signature");
    write_file(
        quoted_response,
        "\"-I" + include_with_spaces.string()
            + "\" -DFROM_QUOTED_RESPONSE=1 --save-temps=obj\n");
    write_file(
        response_header,
        "#define BIOSEQ_PREPARE_RESPONSE_FIXTURE 2\n");
    const dependency_compilation_environment
        quoted_header_mutation =
            prepare(
                toolchain,
                {response_source},
                quoted_database,
                response_root / "pcm-quoted");
    require(
        quoted_header_mutation.relocatable_compilation_signature
            != quoted_prepared.relocatable_compilation_signature,
        "header dependency drift did not change the relocatable "
        "PREPARE signature");
    write_file(
        response_header,
        "#define BIOSEQ_PREPARE_RESPONSE_FIXTURE 1\n");

    const std::filesystem::path inner_response =
        response_root / "inner.rsp";
    const std::filesystem::path outer_response =
        response_root / "outer.rsp";
    write_file(
        inner_response,
        "\"-I" + include_with_spaces.string()
            + "\" -DFROM_NESTED_RESPONSE=1\n");
    write_file(
        outer_response,
        "@" + inner_response.string() + "\n");
    const dependency_compilation_environment nested_prepared =
        prepare(
            toolchain,
            {response_source},
            invalid_database(
                "nested-response",
                {make_command(
                    toolchain,
                    response_root,
                    response_source,
                    {"@" + outer_response.string()})}),
            response_root / "pcm-nested");
    require(
        std::ranges::find(
            nested_prepared.dependencies,
            std::filesystem::weakly_canonical(
                inner_response))
                != nested_prepared.dependencies.end()
            && std::ranges::find(
                   nested_prepared.dependencies,
                   std::filesystem::weakly_canonical(
                       outer_response))
                != nested_prepared.dependencies.end(),
        "nested response closure is incomplete");

    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {response_source},
                invalid_database(
                    "missing-response",
                    {make_command(
                        toolchain,
                        response_root,
                        response_source,
                        {"@" + (
                            response_root / "absent.rsp").string()})}),
                response_root / "pcm-missing-response"));
        },
        "response file does not exist");

    const std::filesystem::path cycle_response_a =
        response_root / "cycle-a.rsp";
    const std::filesystem::path cycle_response_b =
        response_root / "cycle-b.rsp";
    write_file(
        cycle_response_a,
        "@" + cycle_response_b.string() + "\n");
    write_file(
        cycle_response_b,
        "@" + cycle_response_a.string() + "\n");
    require_failure(
        [&] {
            static_cast<void>(prepare(
                toolchain,
                {response_source},
                invalid_database(
                    "cyclic-response",
                    {make_command(
                        toolchain,
                        response_root,
                        response_source,
                        {"@" + cycle_response_a.string()})}),
                response_root / "pcm-cyclic-response"));
        },
        "response-file cycle");

    const std::filesystem::path launcher =
        std::filesystem::path{
            BIOSEQ_PREPARE_TEST_REPOSITORY_ROOT}
        / "cmake/gcc_hwy_header_unit_launcher.py";
    require(
        std::filesystem::is_regular_file(launcher),
        "exact GCC Highway launcher fixture is absent");
    const dependency_compilation_environment launcher_prepared =
        prepare(
            toolchain,
            {response_source},
            invalid_database(
                "gcc-launcher",
                {make_command(
                    toolchain,
                    response_root,
                    response_source,
                    {
                        "-I" + include_with_spaces.string(),
                    },
                    {
                        launcher.string(),
                        "/opt/gcc-16.1.0/bin/g++-16",
                    })}),
            response_root / "pcm-launcher");
    require(
        !launcher_prepared.compile_database_json.contains(
            "gcc_hwy_header_unit_launcher.py")
            && !launcher_prepared.compile_database_json.contains(
                "/g++-16"),
        "owned Clang command retained the GCC launcher or compiler");
}

}  // namespace bioseq::codegen::structural_analysis
