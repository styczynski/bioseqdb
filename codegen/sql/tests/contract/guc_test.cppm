module;

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql:tests.contract.guc;

import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.renderer;

export namespace bioseq::codegen::sql {

namespace guc_contract_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            "GUC generator contract: " + std::string{message});
    }
}

[[nodiscard]] inline std::filesystem::path repository_root() {
#ifndef BIOSEQ_PREPARE_TEST_REPOSITORY_ROOT
    throw std::runtime_error(
        "GUC generator contracts require the configured repository root");
#else
    const std::filesystem::path configured_root{
        BIOSEQ_PREPARE_TEST_REPOSITORY_ROOT};
    std::error_code error;
    const std::filesystem::path source =
        std::filesystem::weakly_canonical(configured_root, error);
    require(
        !error
            && std::filesystem::is_directory(source / "bioseq", error)
            && !error,
        "configured repository root does not own the bioseq source tree");
    return source;
#endif
}

[[nodiscard]] inline const rendered_guc_owner_module& owner(
    const rendered_guc_package& package,
    std::string_view owner_key) {
    const auto found = std::ranges::find(
        package.owners,
        owner_key,
        &rendered_guc_owner_module::owner_key);
    require(
        found != package.owners.end(),
        "rendered package lacks an expected owner");
    return *found;
}

[[nodiscard]] inline const collected_guc& guc(
    const guc_inventory& inventory,
    std::string_view name) {
    const auto found = std::ranges::find(
        inventory.gucs, name, &collected_guc::name);
    require(
        found != inventory.gucs.end(),
        "inventory lacks an expected natural GUC");
    return *found;
}

[[nodiscard]] inline collected_entity& annotated_guc(
    collected_program& program,
    std::string_view cpp_qualified_name) {
    const auto found = std::ranges::find_if(
        program.entities,
        [cpp_qualified_name](const collected_entity& entity) {
            return entity.kind == entity_kind::guc
                && entity.cpp_qualified_name == cpp_qualified_name;
        });
    require(
        found != program.entities.end(),
        "fixture lacks an expected annotated GUC");
    return *found;
}

template <typename p_operation>
inline void require_rejected(
    p_operation&& operation,
    std::initializer_list<std::string_view> fragments,
    std::string_view message) {
    bool matched = false;
    try {
        operation();
    } catch (const std::exception& error) {
        const std::string_view diagnostic{error.what()};
        matched = std::ranges::all_of(
            fragments,
            [diagnostic](std::string_view fragment) {
                return diagnostic.contains(fragment);
            });
    }
    require(matched, message);
}

inline void validate_package(
    const guc_inventory& inventory,
    const rendered_guc_package& package,
    std::size_t core_count) {
    require(
        package.owners.size() == 4,
        "profile must render exactly four registered owners");
    require(
        owner(package, "bioseq").guc_names.size() == core_count
            && owner(package, "scheduler").guc_names.size() == 3
            && owner(package, "storage").guc_names.size() == 1
            && owner(package, "aligner").guc_names.size() == 43,
        "registered owner cardinality changed");
    require(
        package.registry.guc_names.size() == inventory.gucs.size(),
        "registry count differs from annotation inventory");
    for (std::size_t index = 0; index < inventory.gucs.size(); ++index) {
        require(
            package.registry.guc_names[index]
                == inventory.gucs[index].name,
            "registry does not retain registered-source order");
    }
    for (const rendered_guc_owner_module& facet : package.owners) {
        require(
            !facet.content.contains("[&]")
                && !facet.content.contains("[=]"),
            "generated owner contains a capture-all lambda");
        require(
            !facet.content.contains(".value())"),
            "generated owner bypassed the typed PostgreSQL support API");
        require(
            !facet.content.contains("DefineCustomBoolVariable")
                || (facet.content.contains(
                        "static bool complete = false;")
                    && facet.content.contains(
                        "::bioseq::postgres::support::"
                        "invoke_pg_as_cpp(define_variable);")),
            "primitive registration lost one-shot boundary handling");
    }
}

}  // namespace guc_contract_detail

inline void run_guc_generator_contract_self_tests() {
    using guc_contract_detail::require;
    const std::filesystem::path root =
        guc_contract_detail::repository_root();
    const std::vector<std::filesystem::path> owners = {
        root / "bioseq/backend/pg/configuration/aligner.cppm",
        root / "bioseq/backend/pg/configuration/scheduler.cppm",
        root / "bioseq/backend/pg/configuration/core.cppm",
        root / "bioseq/backend/pg/configuration/storage.cppm",
    };
    const collected_program program = collect_fixture_sources(owners);
    const guc_inventory production =
        collect_guc_inventory(program, guc_profile::production);
    const guc_inventory test_indexes =
        collect_guc_inventory(program, guc_profile::test_indexes);
    require(
        production.gucs.size() == 73
            && test_indexes.gucs.size() == 76,
        "annotation-derived profile cardinality changed");

    const collected_guc& compression = guc_contract_detail::guc(
        production, "bioseq.storage.compression.algorithm");
    require(
        compression.cpp_qualified_name
                == "bioseq::settings::storage::compression::algorithm"
            && compression.source_module
                == "bioseq.postgres.configuration.core"
            && compression.storage_kind == "int32_enum"
            && compression.enum_cpp_name
                == "bioseq::compression_algorithm"
            && compression.default_expression
                == "bioseq::k_default_compression_algorithm"
            && compression.enum_default_name == "pglz"
            && compression.generated_live_authority.boot_value
                == "pglz"
            && compression.generated_live_authority
                    .visible_enum_values
                == std::vector<std::string>{"none", "pglz"},
        "qualified constexpr enum default did not resolve from annotations");

    const collected_guc& logging = guc_contract_detail::guc(
        production, "bioseq.diagnostics.logging.enabled");
    require(
        logging.owner_key == "bioseq"
            && logging.member == "diagnostics_logging_enabled"
            && logging.context == "superuser_set"
            && logging.generated_live_authority.boot_value == "off"
            && logging.source_path.generic_string().contains(
                "/backend/pg/configuration/core.cppm"),
        "natural GUC name or registered-source provenance changed");

    const collected_guc& minimizer = guc_contract_detail::guc(
        production, "bioseq.aligner.minimizer.algorithm");
    require(
        minimizer.owner_key == "aligner"
            && minimizer.member == "minimizer_algorithm"
            && minimizer.enum_cpp_name
                == "bioseq::aligner::minimizer_algorithm"
            && minimizer.enum_default_name == "scalar",
        "nested aligner enum registration changed");

    static constexpr std::array<std::string_view, 3> test_only_gucs{
        "bioseq.testing.chunked_scanner.enabled",
        "bioseq.testing.in_memory_scanner.enabled",
        "bioseq.testing.persisted_index.failure_mode",
    };
    require(
        std::ranges::all_of(
            test_only_gucs,
            [&](std::string_view name) {
                return std::ranges::none_of(
                           production.gucs,
                           [name](const collected_guc& value) {
                               return value.name == name;
                           })
                    && guc_contract_detail::guc(test_indexes, name)
                           .feature_predicate
                           .contains("BIOSEQ_TEST_INDEXES");
            }),
        "current-profile annotation selection changed");

    {
        collected_program duplicate = program;
        collected_entity extra = guc_contract_detail::annotated_guc(
            duplicate,
            "bioseq::settings::diagnostics::logging::enabled");
        duplicate.entities.push_back(std::move(extra));
        guc_contract_detail::require_rejected(
            [&duplicate] {
                static_cast<void>(collect_guc_inventory(
                    duplicate, guc_profile::production));
            },
            {"duplicate annotated GUC",
             "bioseq.diagnostics.logging.enabled"},
            "duplicate natural GUC name was accepted");
    }
    {
        collected_program wrong_module = program;
        guc_contract_detail::annotated_guc(
            wrong_module,
            "bioseq::settings::diagnostics::logging::enabled")
                .source_module =
            "bioseq.postgres.configuration.mutated";
        guc_contract_detail::require_rejected(
            [&wrong_module] {
                static_cast<void>(collect_guc_inventory(
                    wrong_module, guc_profile::production));
            },
            {"registered-source provenance"},
            "registered source-module drift was accepted");
    }
    {
        collected_program wrong_path = program;
        guc_contract_detail::annotated_guc(
            wrong_path,
            "bioseq::settings::diagnostics::logging::enabled")
                .source_path =
            root / "bioseq/backend/pg/configuration/missing.cppm";
        guc_contract_detail::require_rejected(
            [&wrong_path] {
                static_cast<void>(collect_guc_inventory(
                    wrong_path, guc_profile::production));
            },
            {"registered SQL source"},
            "unregistered source path was accepted");
    }
    {
        collected_program wrong_profile = program;
        wrong_profile.compiler_profile = "test_indexes";
        guc_contract_detail::require_rejected(
            [&wrong_profile] {
                static_cast<void>(collect_guc_inventory(
                    wrong_profile, guc_profile::production));
            },
            {"disagrees with registered compiler profile"},
            "compiler/profile identity drift was accepted");
    }

    const rendered_guc_package production_package =
        render_guc_package(production);
    const rendered_guc_package test_package =
        render_guc_package(test_indexes);
    guc_contract_detail::validate_package(
        production, production_package, 26);
    guc_contract_detail::validate_package(
        test_indexes, test_package, 29);

    const std::string& core =
        guc_contract_detail::owner(
            test_package, "bioseq").content;
    require(
        core.contains(
            "::bioseq::postgres::support::"
            "postgres_enum_registration<enum_type>")
            && core.contains(
                "::bioseq::postgres::support::"
                "postgres_enum_registration_access::initialize(")
            && core.contains(
                ".boot_value = "
                "::bioseq::k_default_compression_algorithm")
            && core.contains(
                "static_assert(static_cast<int>("
                "::bioseq::planner_log_level::debug5) == DEBUG5"),
        "enum renderer did not use the current PostgreSQL support contract");

    const std::string& aligner =
        guc_contract_detail::owner(
            production_package, "aligner").content;
    require(
        aligner.contains(".get_value()) != new_value;")
            && !aligner.contains(".value())"),
        "enum plan-cache hook does not read typed storage via get_value");

    collected_guc string_probe{
        .name = "bioseq.string_probe",
        .owner_key = "bioseq",
        .member = "string_probe",
        .kind = guc_kind::string,
        .cpp_qualified_name = "bioseq::settings::string_probe",
        .default_expression = "nullptr",
        .context = "user_set",
        .short_description =
            "Exercises string boot-value rendering.",
    };
    std::string rendered =
        render_primitive_guc_registration_contract_probe(string_probe);
    require(
        rendered.contains(
            "&::bioseq::settings::string_probe, nullptr, PGC_USERSET"),
        "null string boot value depends on its live cell");
    string_probe.reset_plan_cache = true;
    string_probe.authored_assign_hook =
        "bioseq::settings::assign_string_probe";
    string_probe.hooks[1] =
        "assign_string_probe_and_reset_plan_cache";
    rendered =
        render_primitive_guc_registration_contract_probe(string_probe);
    const std::size_t authored = rendered.find(
        "::bioseq::settings::assign_string_probe(new_value, extra);");
    const std::size_t reset =
        rendered.find("if (changed) ResetPlanCache();");
    require(
        rendered.contains("std::strcmp(")
            && authored != std::string::npos
            && reset != std::string::npos
            && authored < reset,
        "plan-cache wrapper does not compare, chain, then reset");

    const guc_profile_delta_validation validation =
        validate_guc_profile_delta(production, test_indexes);
    require(
        validation.added_in_test_indexes
            == std::vector<std::string>{
                "bioseq.testing.chunked_scanner.enabled",
                "bioseq.testing.in_memory_scanner.enabled",
                "bioseq.testing.persisted_index.failure_mode"}
            && validation.removed_from_test_indexes.empty()
            && validation.changed_in_test_indexes.empty(),
        "annotation-derived profile delta changed");

    const guc_profile_projection production_projection =
        make_guc_profile_projection(
            production, production_package);
    const guc_profile_projection test_projection =
        make_guc_profile_projection(
            test_indexes, test_package);
    require(
        production_projection.registrations.size()
                == production.gucs.size()
            && test_projection.registrations.size()
                == test_indexes.gucs.size()
            && production_projection.owner_content_sha256.size() == 4
            && test_projection.owner_content_sha256.size() == 4,
        "profile projections lost current annotation provenance");
    const guc_profile_delta_evidence evidence =
        make_guc_profile_delta_evidence(
            production_projection, test_projection);
    require(
        evidence.validation.added_in_test_indexes
                == validation.added_in_test_indexes
            && evidence.owner_content_sha256.size() == 4,
        "profile validation record differs from current inventories");

    const std::string manifest = merge_guc_manifest(
        "{\n  \"schema_revision\": 1\n}\n",
        production,
        production_package,
        &production,
        &production_package,
        &test_indexes,
        &test_package);
    require(
        manifest.contains("\"registration_count\": 73")
            && manifest.contains(
                "\"name\": "
                "\"bioseq.storage.compression.algorithm\"")
            && manifest.contains(
                "\"cpp_storage\": "
                "\"bioseq::settings::storage::compression::algorithm\"")
            && manifest.contains(
                "\"source_module\": "
                "\"bioseq.postgres.configuration.core\"")
            && manifest.contains(
                "\"source_path\": "
                "\"bioseq/backend/pg/configuration/core.cppm\"")
            && manifest.contains(
                "\"enum_default\": \"pglz\"")
            && manifest.contains(
                "\"added_in_test_indexes\": "
                "[\"bioseq.testing.chunked_scanner.enabled\", "
                "\"bioseq.testing.in_memory_scanner.enabled\", "
                "\"bioseq.testing.persisted_index.failure_mode\"]"),
        "manifest lost current annotation/source provenance");
}

}  // namespace bioseq::codegen::sql
