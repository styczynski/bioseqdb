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

export module bioseq.codegen.clang_structural.contract_tests;

import bioseq.codegen.clang_structural;

namespace bioseq::codegen::clang_structural::contract_detail {

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

template <typename Operation>
inline void require_failure(
    Operation&& operation,
    std::string_view expected_diagnostic,
    std::string_view case_name = {}) {
    const std::string context = case_name.empty()
        ? std::string{}
        : " [" + std::string{case_name} + "]";
    try {
        std::invoke(std::forward<Operation>(operation));
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

[[nodiscard]] inline prepared_compilation_environment prepare(
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

[[nodiscard]] inline prepared_compilation_validation validate(
    const configured_toolchain& toolchain,
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& owned_database,
    const std::filesystem::path& pcm_root,
    const std::filesystem::path& manifest,
    std::string_view profile = "production",
    std::optional<std::string_view> clang_sha256 = std::nullopt,
    std::optional<std::filesystem::path> clang_binary = std::nullopt) {
    return validate_prepared_compilation_environment(
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
    const prepared_compilation_environment& prepared,
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
    const prepared_compilation_environment& prepared) {
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

[[nodiscard]] inline std::filesystem::path first_dependency_scan_result(
    const std::filesystem::path& pcm_root) {
    const std::filesystem::path scan_root =
        pcm_root / ".module-dependency-scan";
    std::vector<std::filesystem::path> results;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{scan_root}) {
        if (entry.is_regular_file()
            && entry.path().filename().string().ends_with(
                ".p1689.json")) {
            results.push_back(entry.path());
        }
    }
    std::ranges::sort(results);
    if (results.empty()) {
        throw std::runtime_error(
            "PREPARE contract fixture did not publish an active-import "
            "scan result");
    }
    return results.front();
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

}  // namespace bioseq::codegen::clang_structural::contract_detail

export namespace bioseq::codegen::clang_structural {

inline void run_preparation_contract_self_tests() {
    using namespace contract_detail;

    const configured_toolchain toolchain;
    assert_toolchain_exists(toolchain);
    temporary_tree temporary;
    const std::filesystem::path root = temporary.root();

    const std::filesystem::path source_root = root / "sources";
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
    const prepared_compilation_environment first =
        prepare(
            toolchain,
            {conditional_root},
            raw_database,
            production_pcm_root);
    const prepared_compilation_environment second =
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
    const prepared_compilation_environment permuted =
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
    const prepared_compilation_environment shuffled =
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
        [&](const std::filesystem::path& lane,
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
    const prepared_compilation_environment lane_a_prepared =
        prepare(
            toolchain,
            {conditional_root},
            lane_a_database,
            lane_a_pcm);
    const prepared_compilation_environment lane_b_prepared =
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
    const prepared_compilation_environment lane_c_prepared =
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
    const prepared_compilation_validation lane_a_validation =
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
    const prepared_compilation_environment semantic_mutation =
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
    const prepared_compilation_environment alias_prepared =
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
    const prepared_compilation_environment physical_prepared =
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

    const prepared_compilation_environment test_prepared =
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
    const prepared_compilation_environment partition_prepared =
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
    const prepared_compilation_validation partition_validation =
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
    const prepared_compilation_validation valid =
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
    const prepared_compilation_validation restored_scan_validation =
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
            && resource_dir_graph.units.front().provided_module
                == "fixture.conditional_root",
        "owned compile database cannot resolve selected-Clang builtin "
        "headers in the LibTooling module-graph pass");
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
    const prepared_compilation_environment symlink_prepared =
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
    const prepared_compilation_environment
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
            + "\" -DFROM_QUOTED_RESPONSE=1\n");
    const std::filesystem::path quoted_database =
        invalid_database(
            "quoted-response",
            {make_command(
                toolchain,
                response_root,
                response_source,
                {"@" + quoted_response.string()})});
    const prepared_compilation_environment quoted_prepared =
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
    const prepared_compilation_environment quoted_control =
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
            + "\" -DFROM_QUOTED_RESPONSE=2\n");
    const prepared_compilation_environment
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
            + "\" -DFROM_QUOTED_RESPONSE=1\n");
    write_file(
        response_header,
        "#define BIOSEQ_PREPARE_RESPONSE_FIXTURE 2\n");
    const prepared_compilation_environment
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
    const prepared_compilation_environment nested_prepared =
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
        / "main/cmake/gcc_hwy_header_unit_launcher.py";
    require(
        std::filesystem::is_regular_file(launcher),
        "exact GCC Highway launcher fixture is absent");
    const prepared_compilation_environment launcher_prepared =
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

}  // namespace bioseq::codegen::clang_structural
