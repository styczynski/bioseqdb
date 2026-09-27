/**
 * @file
 * @brief Exercises build-configuration generator contracts.
 */
module;

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.build_config:tests.contract;

import :model;
import :renderer;
import :validator;

namespace bioseq::codegen::build_config {

namespace contract_detail {

inline void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error("build-config generator contract failed: " + std::string{message});
    }
}

template <class p_function>
inline void require_rejected(p_function&& function, const std::string_view message) {
    bool rejected = false;
    try {
        std::forward<p_function>(function)();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, message);
}

[[nodiscard]] inline std::string fixture(const std::string_view declaration) {
    return std::string{"/** @file */\n"
                       "export module bioseq.build_config;\n"
                       "export namespace bioseq::build_config {\n"} +
           std::string{declaration} + "\n}\n";
}

[[nodiscard]] inline std::string documented_variable(const std::string_view type,
                                                     const std::string_view name,
                                                     const std::string_view macro,
                                                     const std::string_view unit,
                                                     const std::string_view domain,
                                                     const std::string_view initializer) {
    return "/**\n"
           " * @brief Fixture value.\n"
           " * @remark Unit: " +
           std::string{unit} + ". Domain: " + std::string{domain} +
           ". Default: " + std::string{initializer} + ".\n */\ninline constexpr " +
           std::string{type} + " " + std::string{name} + " [[bioseq::build_config(\"" +
           std::string{macro} + "\", \"" + std::string{unit} + "\", \"" + std::string{domain} +
           "\")]] = " + std::string{initializer} + ";";
}

inline void verify_default_and_override_rendering() {
    const std::string source =
        fixture(documented_variable("bool",
                                    "bioseq_cache_testing_enabled",
                                    "BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED",
                                    "boolean",
                                    "false|true",
                                    "false") +
                "\n" +
                documented_variable("unsigned int",
                                    "bioseq_fixture_count",
                                    "BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT",
                                    "count",
                                    "1..9",
                                    "3U"));
    const config_model model =
        collect_config_model("codegen/build_config/tests/fixture.cppm", source);
    require(model.entries.size() == 2U, "both annotated declarations are collected");

    const generation_result defaults = generate(model, {});
    const generation_result repeated = generate(model, {});
    require(defaults.module_source == repeated.module_source &&
                defaults.compile_contract_source == repeated.compile_contract_source &&
                defaults.manifest_json == repeated.manifest_json,
            "identical inputs produce byte-identical outputs");
    require(
        defaults.module_source.starts_with("module;\n\n#define ") &&
            defaults.module_source.contains(
                "#define BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED 0") &&
            defaults.module_source.contains("#define BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT 3") &&
            defaults.module_source.contains("bioseq_cache_testing_enabled") &&
            defaults.module_source.contains("(BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED != 0)") &&
            defaults.module_source.contains(
                "static_cast<unsigned int>(BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT)") &&
            !defaults.module_source.contains("[[bioseq::build_config") &&
            defaults.module_source.contains("#undef BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED") &&
            defaults.compile_contract_source.contains("import bioseq.build_config;") &&
            defaults.compile_contract_source.contains(
                "static_assert(bioseq::build_config::bioseq_cache_testing_enabled == false);") &&
            defaults.compile_contract_source.contains(
                "static_assert(bioseq::build_config::bioseq_fixture_count == 3U);"),
        "the generated boundary owns macros and exports constexpr values");

    const generation_result configured =
        generate(model,
                 {
                     {"BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED", "1"},
                     {"BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT", "9"},
                 });
    const generation_result configured_reordered =
        generate(model,
                 {
                     {"BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT", "9"},
                     {"BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED", "1"},
                 });
    require(configured.entries[0].value == 1 && configured.entries[1].value == 9 &&
                configured.module_source == configured_reordered.module_source &&
                configured.compile_contract_source ==
                    configured_reordered.compile_contract_source &&
                configured.manifest_json == configured_reordered.manifest_json &&
                configured.module_source.contains(
                    "#define BIOSEQ_BUILD_CONFIG_CACHE_TESTING_ENABLED 1") &&
                configured.module_source.contains("#define BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT 9") &&
                configured.compile_contract_source.contains(
                    "static_assert(bioseq::build_config::bioseq_cache_testing_enabled == true);") &&
                configured.compile_contract_source.contains(
                    "static_assert(bioseq::build_config::bioseq_fixture_count == 9U);") &&
                configured.manifest_json.contains("\"value\":9"),
            "canonical overrides map exactly to generated values");
}

[[nodiscard]] inline std::string profile_declarations() {
    return documented_variable("unsigned int",
                               "bioseq_template_flatten_profile",
                               "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_PROFILE",
                               "profile_ordinal",
                               "0..5",
                               "0U") +
           "\n" +
           documented_variable("bool",
                               "bioseq_template_flatten_has_all_alphabets_uint64",
                               "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64",
                               "boolean",
                               "false|true",
                               "false") +
           "\n" +
           documented_variable("bool",
                               "bioseq_template_flatten_has_all_coordinates",
                               "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES",
                               "boolean",
                               "false|true",
                               "false") +
           "\n" +
           documented_variable("bool",
                               "bioseq_template_flatten_has_adaptive_lengths",
                               "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS",
                               "boolean",
                               "false|true",
                               "false") +
           "\n" +
           documented_variable("bool",
                               "bioseq_template_flatten_has_extended_kmer_coordinates",
                               "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES",
                               "boolean",
                               "false|true",
                               "false") +
           "\n" +
           documented_variable("bool",
                               "bioseq_template_flatten_has_full_specialization",
                               "BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION",
                               "boolean",
                               "false|true",
                               "false") +
           "\n" +
           documented_variable("unsigned int",
                               "bioseq_scheduler_typed_heavy_unit_count",
                               "BIOSEQ_BUILD_CONFIG_SCHEDULER_TYPED_HEAVY_UNIT_COUNT",
                               "count",
                               "7|14|56",
                               "7U");
}

[[nodiscard]] inline std::vector<config_override> profile_overrides(const int ordinal) {
    const auto enabled = [ordinal](const int threshold) {
        return ordinal >= threshold ? "1" : "0";
    };
    return {
        {"BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_PROFILE", std::to_string(ordinal)},
        {"BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64", enabled(1)},
        {"BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES", enabled(2)},
        {"BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS", enabled(3)},
        {"BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES", enabled(4)},
        {"BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION", enabled(5)},
        {"BIOSEQ_BUILD_CONFIG_SCHEDULER_TYPED_HEAVY_UNIT_COUNT",
         ordinal == 0   ? "7"
         : ordinal == 1 ? "14"
                        : "56"},
    };
}

inline void verify_profile_contracts() {
    const config_model model = collect_config_model(
        "codegen/build_config/tests/profile_fixture.cppm", fixture(profile_declarations()));
    for (int ordinal = 0; ordinal <= 5; ++ordinal) {
        const generation_result generated = generate(model, profile_overrides(ordinal));
        require(generated.entries.front().value == ordinal,
                "each semantic profile maps to its exact ordinal");
        require(generated.entries.back().value == (ordinal == 0   ? 7
                                                   : ordinal == 1 ? 14
                                                                  : 56),
                "each semantic profile maps to its exact scheduler width");
    }
    require_rejected(
        [&model] {
            static_cast<void>(
                generate(model, {{"BIOSEQ_BUILD_CONFIG_TEMPLATE_FLATTEN_PROFILE", "1"}}));
        },
        "inconsistent cumulative profile overrides are rejected");
}

inline void verify_fail_closed_declarations() {
    require_rejected(
        [] {
            static_cast<void>(collect_config_model(
                "missing_bioseq_prefix.cppm",
                fixture(documented_variable("bool",
                                            "fixture_enabled",
                                            "BIOSEQ_BUILD_CONFIG_FIXTURE_ENABLED",
                                            "boolean",
                                            "false|true",
                                            "false"))));
        },
        "public constants without the bioseq_ prefix are rejected");
    require_rejected(
        [] {
            static_cast<void>(
                collect_config_model("wrong_macro.cppm",
                                     fixture(documented_variable("bool",
                                                                 "bioseq_fixture_enabled",
                                                                 "BIOSEQ_BUILD_CONFIG_WRONG",
                                                                 "boolean",
                                                                 "false|true",
                                                                 "false"))));
        },
        "macro spelling mismatches are rejected");
    require_rejected(
        [] {
            static_cast<void>(collect_config_model(
                "unsupported_type.cppm",
                fixture(documented_variable("long",
                                            "bioseq_fixture_count",
                                            "BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT",
                                            "count",
                                            "1..9",
                                            "3"))));
        },
        "unsupported public types are rejected");
    require_rejected(
        [] {
            static_cast<void>(collect_config_model(
                "unsupported_expression.cppm",
                fixture(documented_variable("int",
                                            "bioseq_fixture_count",
                                            "BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT",
                                            "count",
                                            "1..9",
                                            "1 + 2"))));
        },
        "nonliteral authored defaults are rejected");
    require_rejected(
        [] {
            static_cast<void>(collect_config_model(
                "authored_ifdef.cppm",
                "#define BIOSEQ_FORBIDDEN 1\n" +
                    fixture(documented_variable("bool",
                                                "bioseq_fixture_enabled",
                                                "BIOSEQ_BUILD_CONFIG_FIXTURE_ENABLED",
                                                "boolean",
                                                "false|true",
                                                "false"))));
        },
        "authored preprocessor policy is rejected");
    require_rejected(
        [] {
            std::string declaration = documented_variable("int",
                                                          "bioseq_fixture_count",
                                                          "BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT",
                                                          "count",
                                                          "1..9",
                                                          "3");
            declaration.replace(declaration.find("Default: 3."),
                                std::string_view{"Default: 3."}.size(),
                                "Default: 4.");
            static_cast<void>(
                collect_config_model("documentation_mismatch.cppm", fixture(declaration)));
        },
        "documentation that disagrees with the declaration is rejected");
    require_rejected(
        [] {
            const std::string declaration =
                documented_variable("bool",
                                    "bioseq_fixture_enabled",
                                    "BIOSEQ_BUILD_CONFIG_FIXTURE_ENABLED",
                                    "boolean",
                                    "false|true",
                                    "false");
            static_cast<void>(collect_config_model("duplicate_name.cppm",
                                                   fixture(declaration + "\n" + declaration)));
        },
        "duplicate public names and macros are rejected");
}

inline void verify_fail_closed_overrides() {
    const config_model model =
        collect_config_model("override.cppm",
                             fixture(documented_variable("int",
                                                         "bioseq_fixture_count",
                                                         "BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT",
                                                         "count",
                                                         "1..9",
                                                         "3")));
    require_rejected(
        [&model] {
            static_cast<void>(generate(model, {{"BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT", "10"}}));
        },
        "out-of-domain overrides are rejected");
    require_rejected(
        [&model] { static_cast<void>(generate(model, {{"BIOSEQ_BUILD_CONFIG_UNKNOWN", "1"}})); },
        "unknown overrides are rejected");
    require_rejected(
        [&model] {
            static_cast<void>(generate(model,
                                       {
                                           {"BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT", "2"},
                                           {"BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT", "3"},
                                       }));
        },
        "duplicate overrides are rejected");

    const config_model unsigned_model =
        collect_config_model("unsigned_override.cppm",
                             fixture(documented_variable("unsigned int",
                                                         "bioseq_fixture_count",
                                                         "BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT",
                                                         "count",
                                                         "1..9",
                                                         "3U")));
    require_rejected(
        [&unsigned_model] {
            static_cast<void>(
                generate(unsigned_model, {{"BIOSEQ_BUILD_CONFIG_FIXTURE_COUNT", "3U"}}));
        },
        "unsigned override suffixes are rejected");

    const config_model vacuum_model = collect_config_model(
        "vacuum.cppm",
        fixture(documented_variable(
                    "int",
                    "bioseq_vacuum_truncate_lock_wait_interval_milliseconds",
                    "BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS",
                    "milliseconds",
                    "1..2147483647",
                    "50") +
                "\n" +
                documented_variable("int",
                                    "bioseq_vacuum_truncate_lock_timeout_milliseconds",
                                    "BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS",
                                    "milliseconds",
                                    "1..2147483647",
                                    "4500")));
    require_rejected(
        [&vacuum_model] {
            static_cast<void>(generate(
                vacuum_model,
                {{"BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL_MILLISECONDS", "5000"},
                 {"BIOSEQ_BUILD_CONFIG_VACUUM_TRUNCATE_LOCK_TIMEOUT_MILLISECONDS", "4500"}}));
        },
        "vacuum retry intervals that exceed the timeout are rejected");
}

} // namespace contract_detail

void run_build_config_contract_tests() {
    contract_detail::verify_default_and_override_rendering();
    contract_detail::verify_profile_contracts();
    contract_detail::verify_fail_closed_declarations();
    contract_detail::verify_fail_closed_overrides();
}

} // namespace bioseq::codegen::build_config
