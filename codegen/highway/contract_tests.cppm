module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.highway:contract_tests;

import :manifest;
import :model;
import :renderer;
import :validator;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.namespace_materialization;
import bioseq.codegen.support;

namespace bioseq::codegen::highway {

namespace contract_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            "Highway generator contract failed: " + std::string{message});
    }
}

/// Require a negative generator-contract fixture to fail closed.
template <class function_type>
inline void require_rejected(
    function_type&& function,
    std::string_view message) {
    bool rejected = false;
    try {
        std::forward<function_type>(function)();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, message);
}

[[nodiscard]] inline std::size_t occurrences(
    std::string_view text,
    std::string_view needle) noexcept
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position))
           != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

[[nodiscard]] inline generation_plan two_target_plan() {
    const source_spec source{
        .key = "prefilter",
        .path = "fixture_highway_kernel.cppm",
        .module_name = "bioseq.highway.sources.fixture",
        .partition = "fixture",
        .family = "common",
    };

    return generation_plan{
        .target_inventory = target_manifest{
            .policy_digest =
                "e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855",
            .highway_version = "contract-test",
            .highway_digest =
                "00000000000000000000000000000000"
                "00000000000000000000000000000000",
            .static_target_token = "HWY_EMU128",
            .targets = {
                target_spec{
                    .platform = "x86",
                    .slug = "emu128",
                    .token = "HWY_EMU128",
                    .target_namespace = "N_EMU128",
                    .bit = 1u,
                },
                target_spec{
                    .platform = "x86",
                    .slug = "sse2",
                    .token = "HWY_SSE2",
                    .target_namespace = "N_SSE2",
                    .bit = 2u,
                },
            },
        },
        .source_inventory = source_manifest{
            .contract_module = "bioseq.highway.dispatch_contract",
            .dispatch_module = "bioseq.scheduler.dispatch_processor",
            .require_complete_table = true,
            .sources = {source},
            .slots = {
                slot_spec{
                    .owner_key = "prefilter",
                    .family_field = "common",
                    .slot_field = "sneaky_snake_simd_batch",
                    .expression =
                        "&::bioseq::fixture::{target_namespace}::kernel",
                },
            },
            .required_slots = {
                required_slot_spec{
                    .family_field = "common",
                    .slot_field = "sneaky_snake_simd_batch",
                },
            },
        },
        .extracted_sources = {
            extracted_source{
                .source = source,
                .enclosing_namespace = "bioseq::fixture",
                .body = "int kernel() { return 7; }\n",
                .body_first_line = 5u,
                .source_sha256 =
                    "11111111111111111111111111111111"
                    "11111111111111111111111111111111",
                .neutral_imports = {"bioseq.fixture.neutral"},
                .callables = {
                    callable_insertion{
                        .body_relative_offset = 0u,
                        .qualified_name = "bioseq::fixture::target::kernel",
                        .kind = "function",
                    },
                },
            },
        },
        .clang_release = "contract-test",
        .compilation_signature = "contract-test",
    };
}

inline void append_fixture_source(
    generation_plan& plan,
    const std::string_view key,
    const std::string_view family) {
    source_spec source{
        .key = std::string{key},
        .path = std::string{key} + "_fixture.cppm",
        .module_name = "bioseq.highway.sources." + std::string{key},
        .partition = std::string{key},
        .family = std::string{family},
    };
    plan.source_inventory.sources.push_back(source);
    plan.extracted_sources.push_back(extracted_source{
        .source = std::move(source),
        .enclosing_namespace = "bioseq::simd",
        .body = {},
        .body_first_line = 1u,
        .source_sha256 =
            "22222222222222222222222222222222"
            "22222222222222222222222222222222",
    });
}

inline void verify_exact_structural_marker() {
    constexpr std::string_view source = R"CPP(
export module fixture.highway_marker;
export namespace bioseq::fixture {
namespace [[ bioseq :: simd ]] target {
int kernel() { return 7; }
}
namespace [[bioseq::similar]] decoy {}
}
)CPP";

    const structural_analysis::translation_unit unit =
        structural_analysis::scan_translation_unit(
            "fixture_highway_marker.cppm", source);
    require(
        unit.marked_namespaces.size() == 1u,
        "only the exact bioseq::simd namespace marker is accepted");
    const structural_analysis::marked_namespace_record& marker =
        unit.marked_namespaces.front();
    require(
        marker.qualified_name == "bioseq::fixture::target"
            && marker.marker.qualified_name == "bioseq::simd"
            && marker.marker.vendor_namespace == "bioseq"
            && marker.marker.name == "simd"
            && source.substr(
                   marker.name_range.begin.byte_offset,
                   marker.name_range.end.byte_offset
                       - marker.name_range.begin.byte_offset)
                == "target",
        "the structural marker retains its exact identity and namespace");
    require(
        !marker.has_explicit_export
            && !marker.crosses_preprocessor_region,
        "the marked target body satisfies generator ownership constraints");
}

inline void verify_debug_assert_partition_preamble() {
    generation_plan release_plan = two_target_plan();
    append_fixture_source(release_plan, "unrelated", "common");
    const generation_result without_assert = render_generation(release_plan);
    for (const generated_artifact& artifact : without_assert.artifacts) {
        if (artifact.kind == artifact_kind::partition) {
            require(
                !artifact.content.contains("libassert/assert.hpp")
                    && !artifact.content.contains("#define DEBUG_ASSERT"),
                "a partition without DEBUG_ASSERT has no assertion preamble");
        }
    }

    release_plan.extracted_sources.front().body =
        "int kernel() { DEBUG_ASSERT(true, \"fixture\"); return 7; }\n";
    const generation_result with_assert = render_generation(release_plan);
    std::size_t asserted_partitions = 0u;
    for (const generated_artifact& artifact : with_assert.artifacts) {
        if (artifact.kind != artifact_kind::partition) continue;
        if (artifact.source_key != "prefilter") {
            require(
                !artifact.content.contains("libassert/assert.hpp")
                    && !artifact.content.contains("#define DEBUG_ASSERT"),
                "the assertion preamble remains scoped to its source owner");
            continue;
        }
        ++asserted_partitions;
        require(
            artifact.content.contains(
                "#ifdef NDEBUG\n"
                "#ifndef DEBUG_ASSERT\n"
                "#define DEBUG_ASSERT(...) (void)0\n"
                "#endif\n"
                "#else\n"
                "#include <libassert/assert.hpp>\n"
                "#endif\n")
                && artifact.content.contains(
                    "DEBUG_ASSERT(true, \"fixture\")"),
            "a DEBUG_ASSERT user preserves release no-evaluation semantics "
            "and imports the real debug implementation outside NDEBUG");
    }
    require(
        asserted_partitions == release_plan.target_inventory.targets.size(),
        "every fixed target receives the source-scoped assertion preamble");
}

inline void verify_template_flatten_separation() {
    const source_manifest inventory{
        .contract_module = "bioseq.highway.dispatch_contract",
        .dispatch_module = "bioseq.scheduler.dispatch_processor",
        .sources = {
            source_spec{
                .key = "fixture",
                .path = "fixture_highway_source.cppm",
                .module_name = "bioseq.highway.sources.fixture",
                .partition = "fixture",
                .family = "fixture",
            },
        },
    };
    const source_spec& source = inventory.sources.front();
    validate_highway_source_policy(
        structural_analysis::scan_token_slice(
            "fixture_highway_source.cppm",
            "export module bioseq.highway.sources.fixture;\n"
            "import bioseq.highway.dispatch_contract;\n"));
    validate_highway_direct_import(
        inventory, source, inventory.contract_module);

    constexpr std::array policy_attributes{
        std::string_view{"bioseq::template_flatten"},
        std::string_view{"bioseq::template_flatten_when"},
        std::string_view{"bioseq::template_flatten_entry"},
    };
    for (const std::string_view attribute : policy_attributes) {
        const std::string marked_source =
            "export module bioseq.highway.sources.fixture;\n"
              "namespace fixture {\n[["
            + std::string{attribute}
            + "(\"shape\", \"root\")]]\n"
              "int marked_value;\n}\n";
        const std::string message =
            "a registered Highway source rejects policy attribute "
            + std::string{attribute};
        require_rejected(
            [&] {
                validate_highway_source_policy(
                    structural_analysis::scan_token_slice(
                        "fixture_highway_source.cppm", marked_source));
            },
            message);
    }

    const std::array forbidden_providers{
        std::string{"bioseq.codegen.inputs.dispatch_processor_typed"},
        inventory.dispatch_module + ":typed_flat_api",
        inventory.dispatch_module + ":typed_flat_selector",
    };
    for (const std::string& provider : forbidden_providers) {
        require_rejected(
            [&] {
                validate_highway_direct_import(
                    inventory, source, provider);
            },
            "a direct typed scheduler provider import is rejected");

        structural_analysis::dependency_compilation_validation prepared{
            .modules = {
                structural_analysis::dependency_module_identity{
                    .module_name = inventory.contract_module,
                },
                structural_analysis::dependency_module_identity{
                    .module_name = provider,
                },
            },
        };
        require_rejected(
            [&] {
                validate_highway_prepared_closure(inventory, prepared);
            },
            "an exact forbidden provider is rejected from the transitive "
            "PREPARE closure");
    }

    validate_highway_prepared_closure(
        inventory,
        structural_analysis::dependency_compilation_validation{
            .modules = {
                structural_analysis::dependency_module_identity{
                    .module_name = inventory.contract_module,
                },
            },
        });
}

/// Cover owner-scoped slot lookup, overload/template names, and misspellings.
inline void verify_slot_callable_ownership() {
    generation_plan plan = two_target_plan();
    validate_slot_callable_ownership(
        plan.source_inventory, plan.extracted_sources);

    plan.source_inventory.slots.front().expression =
        "&::bioseq::fixture::{target_namespace}::kernel<std::int32_t>";
    plan.extracted_sources.front().callables.push_back(callable_insertion{
        .body_relative_offset = 4u,
        .qualified_name = "bioseq::fixture::target::kernel",
        .kind = "function",
    });
    validate_slot_callable_ownership(
        plan.source_inventory, plan.extracted_sources);

    generation_plan typo = two_target_plan();
    typo.source_inventory.slots.front().expression =
        "&::bioseq::fixture::{target_namespace}::kernle";
    require_rejected(
        [&] {
            validate_slot_callable_ownership(
                typo.source_inventory, typo.extracted_sources);
        },
        "a misspelled slot callable is rejected structurally");

    generation_plan wrong_owner = two_target_plan();
    source_spec unrelated = wrong_owner.source_inventory.sources.front();
    unrelated.key = "unrelated";
    unrelated.path = "fixture_unrelated_kernel.cppm";
    unrelated.module_name = "bioseq.highway.sources.unrelated";
    unrelated.partition = "unrelated";
    wrong_owner.source_inventory.sources.push_back(unrelated);
    extracted_source unrelated_extracted{
        .source = unrelated,
        .enclosing_namespace = "bioseq::fixture",
        .body = "int other_kernel() { return 9; }\n",
        .body_first_line = 5u,
        .source_sha256 =
            "22222222222222222222222222222222"
            "22222222222222222222222222222222",
        .callables = {
            callable_insertion{
                .body_relative_offset = 0u,
                .qualified_name =
                    "bioseq::fixture::target::other_kernel",
                .kind = "function",
            },
        },
    };
    wrong_owner.extracted_sources.push_back(std::move(unrelated_extracted));
    wrong_owner.source_inventory.slots.front().owner_key = "unrelated";
    require_rejected(
        [&] {
            validate_slot_callable_ownership(
                wrong_owner.source_inventory,
                wrong_owner.extracted_sources);
        },
        "a callable assigned to the wrong source owner is rejected");
}

inline void verify_complete_table_target_policy() {
    const generation_plan valid = two_target_plan();
    validate_complete_table_target_policy(valid.target_inventory);

    constexpr std::array<std::string_view, 6> excluded_targets{
        "HWY_SCALAR",
        "HWY_RVV",
        "HWY_SVE",
        "HWY_SVE2",
        "HWY_SVE_256",
        "HWY_SVE2_128",
    };
    for (std::size_t index = 0; index < excluded_targets.size(); ++index) {
        generation_plan excluded = valid;
        excluded.target_inventory.targets.push_back(target_spec{
            .platform = "fixture",
            .slug = "excluded_" + std::to_string(index),
            .token = std::string{excluded_targets[index]},
            .target_namespace = "N_EXCLUDED_" + std::to_string(index),
            .bit = 4u + index,
        });
        require_rejected(
            [&] {
                validate_complete_table_target_policy(
                    excluded.target_inventory);
            },
            "the complete-table target policy rejects scalar and sizeless targets");
    }

    generation_plan no_emu128 = valid;
    no_emu128.target_inventory.targets.erase(
        no_emu128.target_inventory.targets.begin());
    no_emu128.target_inventory.static_target_token = "HWY_SSE2";
    require_rejected(
        [&] {
            validate_complete_table_target_policy(
                no_emu128.target_inventory);
        },
        "the complete-table target policy retains HWY_EMU128");
}

inline void verify_external_domain_topology_contract() {
    const std::filesystem::path manifest =
        detail::path_identity(
            std::filesystem::temp_directory_path()
            / "topology.configure.json");
    const domain_topology_manifest_input parsed =
        parse_domain_topology_manifest_argument(
            "typed_scheduler=" + manifest.generic_string());
    require(
        parsed.name == "typed_scheduler" && parsed.path == manifest,
        "named domain topology CLI grammar preserves name and absolute path");

    const domain_topology_expectation manifest_record =
        detail::parse_domain_topology_record(
            {"domain-topology",
             "0",
             "typed_scheduler",
             manifest.generic_string(),
             std::string(64u, 'a'),
             std::string(64u, 'b')},
            "targets.tsv",
            0u);
    require(
        manifest_record.name == "typed_scheduler"
            && manifest_record.path == manifest
            && manifest_record.topology_digest == std::string(64u, 'a')
            && manifest_record.content_sha256 == std::string(64u, 'b'),
        "target/source manifests preserve sealed ordered topology authority");
    require_rejected(
        [&] {
            (void)detail::parse_domain_topology_record(
                {"domain-topology",
                 "1",
                 "typed_scheduler",
                 manifest.generic_string(),
                 std::string(64u, 'a'),
                 std::string(64u, 'b')},
                "sources.tsv",
                0u);
        },
        "target/source manifests reject non-contiguous topology ordinals");

    require_rejected(
        [] {
            (void)parse_domain_topology_manifest_argument(
                "typed_scheduler=relative/topology.configure.json");
        },
        "domain topology CLI rejects relative paths");
    require_rejected(
        [] {
            (void)parse_domain_topology_manifest_argument(
                "invalid-name=/tmp/topology.configure.json");
        },
        "domain topology CLI rejects invalid names");

    std::vector<domain_topology_manifest_input> duplicate_names{
        {.name = "typed_scheduler", .path = manifest},
        {.name = "typed_scheduler",
         .path = manifest.parent_path() / "other.configure.json"},
    };
    require_rejected(
        [&] { (void)load_domain_topology_manifests(duplicate_names); },
        "domain topology aliases must have unique names before file access");

    generation_plan plan = two_target_plan();
    plan.domain_topologies = {
        domain_topology_provenance{
            .name = "typed_scheduler",
            .path = manifest,
            .topology_digest =
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            .content_sha256 =
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        },
        domain_topology_provenance{
            .name = "storage_layout",
            .path = manifest.parent_path() / "storage.configure.json",
            .topology_digest =
                "cccccccccccccccccccccccccccccccc"
                "cccccccccccccccccccccccccccccccc",
            .content_sha256 =
                "dddddddddddddddddddddddddddddddd"
                "dddddddddddddddddddddddddddddddd",
        },
    };
    std::vector<domain_topology_provenance> observed =
        plan.domain_topologies;
    validate_domain_topology_authority(
        plan.domain_topologies, observed, "contract fixture");
    std::ranges::reverse(observed);
    require_rejected(
        [&] {
            validate_domain_topology_authority(
                plan.domain_topologies, observed, "contract fixture");
        },
        "ordered topology authority rejects reordered CLI inputs");
    observed = plan.domain_topologies;
    observed.front().content_sha256.front() = 'e';
    require_rejected(
        [&] {
            validate_domain_topology_authority(
                plan.domain_topologies, observed, "contract fixture");
        },
        "topology content changes require reconfiguration");

    const generation_result without_domains =
        render_generation(two_target_plan());
    const generation_result with_domains = render_generation(plan);
    require(
        with_domains.artifacts.size() == without_domains.artifacts.size(),
        "external domain topology count never multiplies Highway artifacts");
    for (std::size_t index = 0;
         index < with_domains.artifacts.size();
         ++index) {
        require(
            with_domains.artifacts[index].relative_path
                    == without_domains.artifacts[index].relative_path
                && with_domains.artifacts[index].module_name
                    == without_domains.artifacts[index].module_name
                && with_domains.artifacts[index].target_token
                    == without_domains.artifacts[index].target_token
                && with_domains.artifacts[index].source_key
                    == without_domains.artifacts[index].source_key
                && with_domains.artifacts[index].kind
                    == without_domains.artifacts[index].kind
                && with_domains.artifacts[index].content
                    == without_domains.artifacts[index].content,
            "external domains are provenance only, not Highway coordinates");
    }
    const std::size_t typed_position =
        with_domains.policy_evidence_json.find(
            "\"name\":\"typed_scheduler\"");
    const std::size_t storage_position =
        with_domains.policy_evidence_json.find(
            "\"name\":\"storage_layout\"");
    require(
        typed_position != std::string::npos
            && storage_position != std::string::npos
            && typed_position < storage_position
            && with_domains.policy_evidence_json.contains(
                "\"topology_digest\":\"aaaaaaaa")
            && with_domains.policy_evidence_json.contains(
                "\"content_sha256\":\"bbbbbbbb"),
        "artifact provenance preserves ordered named topology digests");
}

inline void verify_shared_target_coordinates() {
    const generation_plan plan = two_target_plan();
    const auto coordinates = detail::build_target_coordinates(
        plan.target_inventory);
    require(
        coordinates.size() == 2u
            && coordinates[0].ordinal == 0u
            && coordinates[0].assignments.size() == 1u
            && coordinates[0].assignments[0].role
                == namespace_materialization::axis_role::external
            && coordinates[0].assignments[0].group == "highway_target"
            && coordinates[0].assignments[0].case_label == "HWY_EMU128"
            && coordinates[0].identity_namespace == "N_EMU128"
            && coordinates[0].module_name
                == "bioseq.highway.generated.x86.emu128"
            && coordinates[0].relative_path
                == std::filesystem::path{"x86/emu128/index.cppm"}
            && coordinates[1].ordinal == 1u
            && coordinates[1].assignments.size() == 1u
            && coordinates[1].assignments[0].case_label == "HWY_SSE2"
            && coordinates[1].identity_namespace == "N_SSE2"
            && coordinates[1].module_name
                == "bioseq.highway.generated.x86.sse2"
            && coordinates[1].relative_path
                == std::filesystem::path{"x86/sse2/index.cppm"},
        "Highway targets reuse the shared ordered external-coordinate IR");
}

inline void verify_generated_closure() {
    const generation_result generated = render_generation(two_target_plan());
    require(
        generated.artifacts.size() == 11u,
        "two targets and one source produce two primaries, binders, "
        "partitions, owner instantiations, one private registry, one cold "
        "inventory, and fixed-target test support");

    std::size_t primary_count = 0;
    std::size_t binder_count = 0;
    std::size_t partition_count = 0;
    std::size_t owner_unit_count = 0;
    std::size_t registry_count = 0;
    std::size_t inventory_count = 0;
    std::size_t test_support_count = 0;
    std::size_t export_count = 0;
    std::size_t dispatch_count = 0;
    std::set<std::pair<std::string, std::string>> target_source_closure;
    std::set<std::pair<std::string, std::string>> target_owner_unit_closure;

    for (const generated_artifact& artifact : generated.artifacts) {
        require(
            artifact.relative_path.extension() == ".cppm",
            "every generated production artifact is a .cppm");
        export_count += occurrences(artifact.content, "HWY_EXPORT(");
        dispatch_count += occurrences(
            artifact.content, "HWY_DYNAMIC_DISPATCH(");

        if (artifact.kind
            != artifact_kind::dispatch_registry_partition) {
            require(
                !artifact.content.contains("HWY_EXPORT(")
                    && !artifact.content.contains("HWY_DYNAMIC_DISPATCH("),
                "dispatch macros remain confined to the registry");
        }
        require(
            !artifact.content.contains("[[bioseq::simd]]")
                && !artifact.content.contains("hwy/foreach_target.h")
                && !artifact.content.contains("HWY_STATIC_DISPATCH"),
            "authored marker and forbidden dispatch scaffolding do not "
            "escape into generated output");

        switch (artifact.kind) {
            case artifact_kind::primary:
                ++primary_count;
                require(
                    artifact.content.contains(
                        "import bioseq.highway.dispatch_contract;")
                        && occurrences(
                               artifact.content,
                               "void populate_prefilter("
                               "::bioseq::simd_dispatch::highway_kernel_table& "
                               "table) noexcept;") == 1u
                        && !artifact.content.contains("#include <hwy/")
                        && !artifact.content.contains("import :")
                        && !artifact.content.contains("HWY_TARGET"),
                    "each target primary is a minimal neutral BMI");
                break;
            case artifact_kind::binder:
                ++binder_count;
                require(
                    artifact.content.contains("make_highway_kernel_table")
                        && artifact.content.contains("kernels.is_complete()")
                        && artifact.content.contains(
                            "highway_kernel_mask_for(highway_kernel::"
                            "prefilter_sneaky_snake_simd_batch)")
                        && artifact.content.contains(
                            "binding_detail::populate_prefilter(table)")
                        && !artifact.content.contains(
                            "&::bioseq::fixture::N_")
                        && !artifact.content.contains("import :")
                        && artifact.content.contains("bind_highway_target"),
                    "each target binder owns a complete table without "
                    "instantiating source callables");
                break;
            case artifact_kind::partition:
                ++partition_count;
                target_source_closure.emplace(
                    artifact.target_token, artifact.source_key);
                require(
                    artifact.content.contains("#define HWY_TARGET ")
                        && artifact.content.contains(
                            "#include <hwy/aligned_allocator.h>")
                        && artifact.content.contains("HWY_ATTR int kernel()")
                        && artifact.content.contains(
                            "import bioseq.fixture.neutral;"),
                    "each target/source partition fixes its target, provides "
                    "the Highway allocation surface, and rewrites the callable");
                break;
            case artifact_kind::kernel_instantiation:
                ++owner_unit_count;
                target_owner_unit_closure.emplace(
                    artifact.target_token, artifact.source_key);
                require(
                    artifact.source_key == "prefilter"
                        && artifact.relative_path.generic_string().contains(
                            "/instances/prefilter.cppm")
                        && artifact.content.contains(
                            "module " + artifact.module_name + ";\n\n")
                        && !artifact.content.contains(
                            "export module " + artifact.module_name)
                        && !artifact.content.contains(
                            "module " + artifact.module_name + ":")
                        && artifact.content.contains("import :fixture;")
                        && artifact.content.contains(
                            "HWY_ATTR void populate_prefilter")
                        && artifact.content.contains(
                            "&::bioseq::fixture::N_")
                        && !artifact.content.contains("bind_highway_target")
                        && ((artifact.target_token == "HWY_EMU128"
                                && artifact.content.ends_with(
                                    "}  // namespace "
                                    "bioseq::simd_dispatch::N_EMU128::"
                                    "binding_detail\n"))
                            || (artifact.target_token == "HWY_SSE2"
                                && artifact.content.ends_with(
                                    "}  // namespace "
                                    "bioseq::simd_dispatch::N_SSE2::"
                                    "binding_detail\n"))),
                    "each target compiles concrete slot ownership in an "
                    "ordinary owner unit with an exact namespace closure");
                break;
            case artifact_kind::dispatch_registry_partition:
                ++registry_count;
                require(
                    occurrences(artifact.content, "HWY_EXPORT(") == 1u
                        && occurrences(
                            artifact.content, "HWY_DYNAMIC_DISPATCH(") == 1u
                        && artifact.module_name
                            == "bioseq.scheduler.dispatch_processor:highway_registry"
                        && artifact.content.contains(
                            "module bioseq.scheduler.dispatch_processor:highway_registry;")
                        && artifact.content.contains(
                            "binding->kernels->is_complete()")
                        && !artifact.content.contains(
                            "registered_highway_dispatch_plan"),
                    "the private registry partition owns one dispatch and no "
                    "cold inventory");
                break;
            case artifact_kind::dispatch_inventory_implementation:
                ++inventory_count;
                require(
                    artifact.module_name
                            == "bioseq.highway.dispatch_contract"
                        && artifact.content.contains(
                            "module bioseq.highway.dispatch_contract;")
                        && artifact.content.contains(
                            "registered_highway_dispatch_plan() noexcept")
                        && artifact.content.contains(
                            "registered_highway_target_binding(")
                        && artifact.content.contains(
                            "static_cast<std::int64_t>(3ULL)")
                        && artifact.content.contains(
                            "N_EMU128::bind_highway_target()")
                        && artifact.content.contains(
                            "N_SSE2::bind_highway_target()")
                        && artifact.content.contains(
                            "plan.runnable_targets & exact_target")
                        && artifact.content.contains(
                            "binding->target != exact_target")
                        && artifact.content.contains(
                            "binding->kernels->is_complete()")
                        && !artifact.content.contains("hwy::")
                        && !artifact.content.contains("HWY_EXPORT(")
                        && !artifact.content.contains(
                            "HWY_DYNAMIC_DISPATCH("),
                    "the cold contract implementation owns the exact mask and "
                    "direct exact-target rebinder without redispatch");
                break;
            case artifact_kind::test_support:
                ++test_support_count;
                require(
                    artifact.module_name == "bioseq.highway.test_support"
                        && artifact.target_token == "HWY_EMU128"
                        && artifact.content.contains(
                            "N_EMU128::bind_highway_target()")
                        && artifact.content.contains(
                            "highway_test_kernel_table()")
                        && !artifact.content.contains("HWY_DYNAMIC_DISPATCH("),
                    "test support binds the configured fixed target without "
                    "introducing runtime dispatch");
                break;
            case artifact_kind::unit_probe_implementation:
            case artifact_kind::minimizer_representation_instantiation:
                require(
                    false,
                    "the minimal generated-closure fixture has no optional "
                    "probe or representation owners");
                break;
        }
    }

    require(
        primary_count == 2u && binder_count == 2u
            && partition_count == 2u && owner_unit_count == 2u
            && registry_count == 1u
            && inventory_count == 1u && test_support_count == 1u,
        "the generated artifact-kind closure is exact");
    require(
        export_count == 1u && dispatch_count == 1u,
        "the complete generated graph contains exactly one export and dynamic dispatch");
    require(
        target_source_closure
            == std::set<std::pair<std::string, std::string>>{
                {"HWY_EMU128", "prefilter"},
                {"HWY_SSE2", "prefilter"},
            },
        "every target has exactly one generated partition for every registered source");
    require(
        target_owner_unit_closure
            == std::set<std::pair<std::string, std::string>>{
                {"HWY_EMU128", "prefilter"},
                {"HWY_SSE2", "prefilter"},
            },
        "every target has exactly one ordinary owner unit for every slot owner");
    require(
        generated.policy_evidence_json.contains(
            "\"path\":\"highway_registry.cppm\"")
            && generated.policy_evidence_json.contains(
                "\"path\":\"dispatch_inventory.cppm\"")
            && generated.policy_evidence_json.contains(
                "\"path\":\"test_support.cppm\"")
            && generated.policy_evidence_json.contains(
                "\"path\":\"x86/emu128/fixture.cppm\"")
            && generated.policy_evidence_json.contains(
                "\"path\":\"x86/sse2/fixture.cppm\"")
            && generated.policy_evidence_json.contains(
                "\"path\":\"x86/emu128/instances/prefilter.cppm\"")
            && generated.policy_evidence_json.contains(
                "\"path\":\"x86/sse2/instances/prefilter.cppm\""),
        "the artifact manifest records the complete generated topology");
}

inline void verify_lambda_attribute_order() {
    extracted_source source{
        .source = source_spec{
            .key = "lambda_fixture",
            .path = "lambda_fixture.cppm",
        },
        .enclosing_namespace = "bioseq::fixture",
        .body =
            "auto value = [](int x) constexpr [[gnu::always_inline]] "
            "-> int { return x; };\n",
    };
    source.callables.push_back(callable_insertion{
        .body_relative_offset = source.body.find(')') + 1u,
        .qualified_name = "bioseq::fixture::target::<lambda>",
        .kind = "lambda",
        .lambda_preceding_token = ")",
        .lambda_following_token = "constexpr",
        .lambda_following_relative_offset = source.body.find("constexpr"),
    });
    const std::string rendered = detail::body_with_highway_attributes(source);
    require(
        rendered.contains(
            "[](int x) HWY_ATTR constexpr [[gnu::always_inline]] -> int")
            && !rendered.contains("]] HWY_ATTR")
            && !rendered.contains("-> int HWY_ATTR"),
        "lambda HWY_ATTR precedes specifiers, standard attributes, and "
        "trailing return types");
}

inline void verify_type_only_target_source() {
    generation_plan plan = two_target_plan();
    const source_spec foundation{
        .key = "foundation",
        .path = "fixture_highway_foundation.cppm",
        .module_name = "bioseq.highway.sources.foundation",
        .partition = "foundation",
        .family = "common",
    };
    plan.source_inventory.sources.push_back(foundation);
    plan.extracted_sources.push_back(extracted_source{
        .source = foundation,
        .enclosing_namespace = "bioseq::fixture",
        .body =
            "template<class Value> struct foundation { using type = Value; };\n",
        .body_first_line = 5u,
        .source_sha256 =
            "33333333333333333333333333333333"
            "33333333333333333333333333333333",
    });

    const generation_result generated = render_generation(plan);
    std::size_t type_only_partitions = 0u;
    for (const generated_artifact& artifact : generated.artifacts) {
        if (artifact.kind != artifact_kind::partition
            || artifact.source_key != foundation.key) {
            continue;
        }
        ++type_only_partitions;
        require(
            artifact.content.contains("struct foundation")
                && !artifact.content.contains("HWY_ATTR"),
            "a type-only target source is emitted without invented callables");
    }
    require(
        type_only_partitions == plan.target_inventory.targets.size(),
        "each fixed target receives the type-only dependency partition");
}

inline void verify_shared_publication_envelope() {
    const generation_plan plan = two_target_plan();
    const generation_result generated = render_generation(plan);
    const std::vector<namespace_materialization::publication_artifact>
        publication = make_publication_artifacts(generated);
    const std::filesystem::path output_root =
        std::filesystem::absolute(
            std::filesystem::temp_directory_path()
            / "bioseq-highway-publication-contract")
            .lexically_normal();
    const namespace_materialization::rendered_publication_manifest manifest =
        render_highway_publication_manifest(
            output_root, {}, plan, generated, publication);

    require(
        publication.size() == generated.artifacts.size()
            && std::ranges::all_of(
                publication,
                [](const namespace_materialization::publication_artifact&
                       artifact) {
                    return artifact.artifact_class
                            == namespace_materialization::
                                publication_artifact_class::production_cppm
                        && artifact.relative_path.extension() == ".cppm"
                        && artifact.content_sha256
                            == sha256_hex(artifact.content);
                }),
        "Highway maps its complete production closure into the shared publisher");
    require(
        generated.publication_topology_digest.size() == 64u
            && manifest.manifest_identity.size() == 64u
            && manifest.json.contains(
                "\"publication_schema_version\": 1")
            && manifest.json.contains("\"policy\": \"highway\"")
            && manifest.json.contains("\"name\": \"highway\"")
            && manifest.json.contains(
                "\"prior_manifest_identity\": \"\"")
            && manifest.json.contains("\"evidence\": {")
            && manifest.json.contains("\"schema_version\": 1")
            && manifest.json.contains("\"targets\": [")
            && manifest.json.contains("\"sources\": [")
            && manifest.json.contains("\"slots\": [")
            && manifest.json.contains("\"manifest_identity\": \"")
            && manifest.json.contains(manifest.manifest_identity),
        "Highway evidence is embedded in the policy-neutral self-sealed envelope");

    const std::string prior_identity(64u, 'a');
    const namespace_materialization::rendered_publication_manifest chained =
        render_highway_publication_manifest(
            output_root, prior_identity, plan, generated, publication);
    require(
        chained.manifest_identity != manifest.manifest_identity
            && chained.json.contains(
                "\"prior_manifest_identity\": \"" + prior_identity + "\""),
        "Highway transports the exact authenticated prior identity into the seal");

    generation_result stale = generated;
    stale.policy_evidence_json += " ";
    require_rejected(
        [&] {
            (void)render_highway_publication_manifest(
                output_root, {}, plan, stale, publication);
        },
        "the Highway adapter rejects evidence that diverges from its generation identity");

    std::vector<namespace_materialization::publication_artifact> tampered =
        publication;
    tampered.front().content += "// not generated\n";
    tampered.front().content_sha256 = sha256_hex(tampered.front().content);
    require_rejected(
        [&] {
            (void)render_highway_publication_manifest(
                output_root, {}, plan, generated, tampered);
        },
        "the Highway adapter binds shared ownership to its exact evidence closure");

    generation_result jointly_tampered = generated;
    jointly_tampered.artifacts.front().content += "// not evidenced\n";
    const std::vector<namespace_materialization::publication_artifact>
        jointly_tampered_publication =
            make_publication_artifacts(jointly_tampered);
    require_rejected(
        [&] {
            (void)render_highway_publication_manifest(
                output_root,
                {},
                plan,
                jointly_tampered,
                jointly_tampered_publication);
        },
        "the Highway adapter revalidates evidence after artifact mutation");
}

inline void verify_typed_alignment_binder() {
    generation_plan plan = two_target_plan();
    plan.source_inventory.require_complete_table = false;
    plan.source_inventory.required_slots.clear();
    plan.source_inventory.slots.clear();
    append_fixture_source(plan, "ksw2", "alignment");
    append_fixture_source(plan, "ksw2_batched", "alignment");
    append_fixture_source(plan, "block_aligner", "alignment");

    const auto append_alphabet = [&plan](
        const std::uint8_t code,
        const std::string_view alphabet) {
        const std::string family =
            "alignment_" + std::to_string(code);
        const auto append = [&](
            const std::string_view owner,
            const std::string_view member) {
            plan.source_inventory.slots.push_back(slot_spec{
                .owner_key = std::string{owner},
                .family_field = family,
                .slot_field = std::string{member},
                .expression =
                    "&::bioseq::simd::{target_namespace}::"
                    + std::string{member} + "<" + std::string{alphabet} + ">",
            });
        };
        append("ksw2", "align_ksw2_single_affine");
        append("ksw2", "align_ksw2_dual_affine");
        append("ksw2_batched", "align_ksw2_batched");
        append("block_aligner", "prepare_block_global_gap_certificate");
        append("block_aligner", "try_block_certified_gap");
        append("block_aligner", "align_block");
    };
    append_alphabet(0u, "::seq::dna4");
    append_alphabet(1u, "::seq::dna5");

    const generation_result generated = render_generation(plan);
    std::size_t checked_primaries = 0u;
    std::size_t checked_binders = 0u;
    std::size_t checked_alignment_owner_units = 0u;
    for (const generated_artifact& artifact : generated.artifacts) {
        if (artifact.kind == artifact_kind::primary) {
            ++checked_primaries;
            const auto has_declaration = [&artifact](
                const std::string_view owner,
                const std::uint8_t alphabet_code,
                const std::string_view alphabet) {
                const std::string expected =
                    "void populate_" + std::string{owner}
                    + "_alignment_" + std::to_string(alphabet_code)
                    + "(::bioseq::simd::alignment_kernels<"
                    + std::string{alphabet} + ">& table) noexcept;";
                return occurrences(artifact.content, expected) == 1u;
            };
            require(
                occurrences(artifact.content, "void populate_") == 6u
                    && has_declaration("ksw2", 0u, "::seq::dna4")
                    && has_declaration("ksw2", 1u, "::seq::dna5")
                    && has_declaration(
                        "ksw2_batched", 0u, "::seq::dna4")
                    && has_declaration(
                        "ksw2_batched", 1u, "::seq::dna5")
                    && has_declaration(
                        "block_aligner", 0u, "::seq::dna4")
                    && has_declaration(
                        "block_aligner", 1u, "::seq::dna5"),
                "each primary declares the exact typed alignment populators");
            continue;
        }
        if (artifact.kind == artifact_kind::kernel_instantiation
            && (artifact.source_key == "ksw2"
                || artifact.source_key == "ksw2_batched"
                || artifact.source_key == "block_aligner")) {
            ++checked_alignment_owner_units;
            require(
                artifact.target_token == "HWY_EMU128"
                    || artifact.target_token == "HWY_SSE2",
                "each alignment owner unit belongs to a fixture target");
            const std::string target_namespace =
                artifact.target_token == "HWY_EMU128"
                ? "N_EMU128"
                : "N_SSE2";
            const auto has_assignment = [&](
                const std::string_view member,
                const std::string_view alphabet) {
                const std::string expected =
                    "    table." + std::string{member}
                    + " = &::bioseq::simd::" + target_namespace + "::"
                    + std::string{member} + "<" + std::string{alphabet}
                    + ">;";
                return occurrences(artifact.content, expected) == 1u;
            };
            bool has_exact_owner_assignments = false;
            if (artifact.source_key == "ksw2") {
                has_exact_owner_assignments =
                    occurrences(artifact.content, "    table.") == 4u
                    && has_assignment(
                        "align_ksw2_dual_affine", "::seq::dna4")
                    && has_assignment(
                        "align_ksw2_single_affine", "::seq::dna4")
                    && has_assignment(
                        "align_ksw2_dual_affine", "::seq::dna5")
                    && has_assignment(
                        "align_ksw2_single_affine", "::seq::dna5");
            } else if (artifact.source_key == "ksw2_batched") {
                has_exact_owner_assignments =
                    occurrences(artifact.content, "    table.") == 2u
                    && has_assignment("align_ksw2_batched", "::seq::dna4")
                    && has_assignment("align_ksw2_batched", "::seq::dna5");
            } else {
                has_exact_owner_assignments =
                    occurrences(artifact.content, "    table.") == 6u
                    && has_assignment("align_block", "::seq::dna4")
                    && has_assignment(
                        "prepare_block_global_gap_certificate",
                        "::seq::dna4")
                    && has_assignment(
                        "try_block_certified_gap", "::seq::dna4")
                    && has_assignment("align_block", "::seq::dna5")
                    && has_assignment(
                        "prepare_block_global_gap_certificate",
                        "::seq::dna5")
                    && has_assignment(
                        "try_block_certified_gap", "::seq::dna5");
            }
            require(
                occurrences(
                    artifact.content,
                    "HWY_ATTR void populate_" + artifact.source_key
                        + "_alignment_") == 2u
                    && has_exact_owner_assignments,
                "each alignment owner unit assigns exactly its typed slots");
            continue;
        }
        if (artifact.kind != artifact_kind::binder) continue;
        ++checked_binders;
        require(
            artifact.content.contains("import bioseq.sequences;")
                && artifact.content.contains(
                    "alignment_kernels<::seq::dna4> alignment_0")
                && artifact.content.contains(
                    "alignment_kernels<::seq::dna5> alignment_1")
                && artifact.content.contains(
                    ".alphabet_code = 0u, .typed_table_complete = "
                    "::bioseq::simd::is_alignment_kernel_table_complete("
                    "alignment_0), .typed_table = &alignment_0")
                && artifact.content.contains(
                    ".alphabet_code = 1u, .typed_table_complete = "
                    "::bioseq::simd::is_alignment_kernel_table_complete("
                    "alignment_1), .typed_table = &alignment_1")
                && occurrences(
                       artifact.content,
                       "::bioseq::simd::is_alignment_kernel_table_complete(")
                    == 2u
                && artifact.content.contains(
                    "highway_alphabet_identity_entry alphabet_identities[]")
                && occurrences(
                       artifact.content,
                       "highway_alphabet_identity_for<::seq::dna4>()") == 1u
                && occurrences(
                       artifact.content,
                       "highway_alphabet_identity_for<::seq::dna5>()") == 1u
                && artifact.content.contains(
                    "table.alphabet_identities = alphabet_identities")
                && artifact.content.contains(
                    "highway_kernel_mask_for(highway_kernel::alignment_ksw2)")
                && artifact.content.contains(
                    "highway_kernel_mask_for(highway_kernel::alignment_ksw2_batched)")
                && artifact.content.contains(
                    "highway_kernel_mask_for(highway_kernel::alignment_block_aligner)")
                && artifact.content.contains(
                    "binding_detail::populate_ksw2_alignment_0(table)")
                && artifact.content.contains(
                    "binding_detail::populate_block_aligner_alignment_1(table)")
                && !artifact.content.contains("align_ksw2_single_affine<")
                && !artifact.content.contains("align_block<")
                && !artifact.content.contains("import :")
                && !artifact.content.contains("table.alignment_0"),
            "each binder synthesizes exact typed alignment tables without "
            "instantiating their templates or inventing root-table fields");
    }
    require(
        checked_primaries == 2u,
        "both fixed targets declare typed alignment populators");
    require(checked_binders == 2u, "both fixed targets receive typed alignment tables");
    require(
        checked_alignment_owner_units == 6u,
        "each fixed target compiles three alignment owner units");

    generation_plan incomplete = plan;
    incomplete.source_inventory.slots.pop_back();
    require_rejected(
        [&] { (void)render_generation(incomplete); },
        "a typed alignment family missing any function slot is rejected");

    generation_plan mixed = plan;
    mixed.source_inventory.slots.front().expression =
        "&::bioseq::simd::{target_namespace}::align_ksw2_single_affine<::seq::dna5>";
    require_rejected(
        [&] { (void)render_generation(mixed); },
        "a typed alignment family mixing alphabet arguments is rejected");

    generation_plan duplicate_type = plan;
    for (slot_spec& slot : duplicate_type.source_inventory.slots) {
        if (slot.family_field == "alignment_1") {
            const std::size_t dna5 = slot.expression.find("::seq::dna5");
            slot.expression.replace(dna5, 11u, "::seq::dna4");
        }
    }
    require_rejected(
        [&] { (void)render_generation(duplicate_type); },
        "two alignment codes may not erase the same concrete alphabet type");
}

inline void verify_typed_minimizer_binder() {
    generation_plan plan = two_target_plan();
    plan.source_inventory.require_complete_table = false;
    plan.source_inventory.required_slots.clear();
    plan.source_inventory.slots.clear();
    append_fixture_source(plan, "minimizer_minimap2_reference", "minimizers");
    append_fixture_source(plan, "minimizer_two_stack_nthash", "minimizers");
    append_fixture_source(plan, "minimizer_bounded_nthash", "minimizers");
    append_fixture_source(plan, "ksw2", "alignment");
    append_fixture_source(plan, "ksw2_batched", "alignment");
    append_fixture_source(plan, "block_aligner", "alignment");

    const auto append_case = [&plan](
        const std::uint8_t alphabet_code,
        const std::string_view alphabet,
        const std::uint8_t storage_unit_bytes,
        const std::string_view storage_unit) {
        const std::string family =
            "minimizer_case_" + std::to_string(alphabet_code) + "_"
            + std::to_string(storage_unit_bytes);
        const auto append = [&](
            const std::string_view owner,
            const std::string_view member) {
            plan.source_inventory.slots.push_back(slot_spec{
                .owner_key = std::string{owner},
                .family_field = family,
                .slot_field = std::string{member},
                .expression =
                    "&::bioseq::simd::{target_namespace}::"
                    + std::string{member} + "_case<"
                    + std::string{alphabet} + ","
                    + std::string{storage_unit} + ">",
            });
        };
        append("minimizer_minimap2_reference", "minimap2_reference");
        append("minimizer_two_stack_nthash", "two_stack_nthash");
        append("minimizer_bounded_nthash", "bounded_nthash");
    };

    // Deliberately reverse both axes: renderer output must still be ordered by
    // source-owned alphabet code and storage byte width.
    for (const auto& [bytes, storage] : std::array{
             std::pair<std::uint8_t, std::string_view>{
                 8u, "::std::uint64_t"},
             std::pair<std::uint8_t, std::string_view>{
                 4u, "::std::uint32_t"},
             std::pair<std::uint8_t, std::string_view>{
                 2u, "::std::uint16_t"},
             std::pair<std::uint8_t, std::string_view>{
                 1u, "::std::uint8_t"},
         }) {
        append_case(1u, "::seq::dna5", bytes, storage);
    }
    for (const auto& [bytes, storage] : std::array{
             std::pair<std::uint8_t, std::string_view>{
                 8u, "::std::uint64_t"},
             std::pair<std::uint8_t, std::string_view>{
                 4u, "::std::uint32_t"},
             std::pair<std::uint8_t, std::string_view>{
                 2u, "::std::uint16_t"},
             std::pair<std::uint8_t, std::string_view>{
                 1u, "::std::uint8_t"},
         }) {
        append_case(0u, "::seq::dna4", bytes, storage);
    }

    const generation_result generated = render_generation(plan);
    std::size_t checked_binders = 0u;
    std::size_t checked_minimizer_owner_units = 0u;
    for (const generated_artifact& artifact : generated.artifacts) {
        if (artifact.kind == artifact_kind::kernel_instantiation
            && artifact.source_key.starts_with("minimizer_")) {
            ++checked_minimizer_owner_units;
            require(
                occurrences(
                    artifact.content,
                    "switch (invocation.sequence.alphabet_code)") == 1u
                    && occurrences(
                           artifact.content,
                           "switch (invocation.sequence.storage_unit_bytes)")
                        == 2u
                    && occurrences(
                           artifact.content,
                           "minimizer_kernel_status::invalid_configuration")
                        == 1u
                    && occurrences(
                           artifact.content, "    table.minimizers.") == 1u,
                "each minimizer owner unit contains one typed wrapper and "
                "one wrapper-to-slot assignment");

            if (artifact.source_key == "minimizer_minimap2_reference") {
                require(
                    artifact.content.contains(
                        "minimizer_dispatch_minimap2_reference")
                        && artifact.content.contains(
                            "table.minimizers.minimap2_reference = "
                            "&minimizer_dispatch_minimap2_reference;")
                        && occurrences(
                               artifact.content,
                               "minimap2_reference_case<") == 8u,
                    "the minimap2-reference owner instantiates its complete case matrix");
                const std::size_t dna4_u8 = artifact.content.find(
                    "minimap2_reference_case<::seq::dna4,::std::uint8_t>");
                const std::size_t dna4_u16 = artifact.content.find(
                    "minimap2_reference_case<::seq::dna4,::std::uint16_t>");
                const std::size_t dna5_u8 = artifact.content.find(
                    "minimap2_reference_case<::seq::dna5,::std::uint8_t>");
                require(
                    dna4_u8 != std::string::npos
                        && dna4_u16 != std::string::npos
                        && dna5_u8 != std::string::npos
                        && dna4_u8 < dna4_u16 && dna4_u16 < dna5_u8,
                    "typed minimizer wrapper cases are deterministically ordered");
            } else if (artifact.source_key == "minimizer_two_stack_nthash") {
                require(
                    artifact.content.contains(
                        "minimizer_dispatch_two_stack_nthash")
                        && artifact.content.contains(
                            "table.minimizers.two_stack_nthash = "
                            "&minimizer_dispatch_two_stack_nthash;")
                        && occurrences(
                               artifact.content,
                               "two_stack_nthash_case<") == 8u,
                    "the two-stack owner instantiates its complete case matrix");
            } else if (artifact.source_key == "minimizer_bounded_nthash") {
                require(
                    artifact.content.contains(
                        "minimizer_dispatch_bounded_nthash")
                        && artifact.content.contains(
                            "table.minimizers.bounded_nthash = "
                            "&minimizer_dispatch_bounded_nthash;")
                        && occurrences(
                               artifact.content,
                               "bounded_nthash_case<") == 8u,
                    "the bounded-ntHash owner instantiates its complete case matrix");
            }
            continue;
        }
        if (artifact.kind != artifact_kind::binder) continue;
        ++checked_binders;
        require(
            artifact.content.contains("import bioseq.sequences;")
                && artifact.content.contains(
                    "binding_detail::populate_minimizer_minimap2_reference(table)")
                && artifact.content.contains(
                    "binding_detail::populate_minimizer_two_stack_nthash(table)")
                && artifact.content.contains(
                    "binding_detail::populate_minimizer_bounded_nthash(table)")
                && !artifact.content.contains("minimizer_dispatch_")
                && !artifact.content.contains("table.minimizers.")
                && artifact.content.contains(
                    "highway_kernel_mask_for(highway_kernel::"
                    "minimizer_minimap2_reference)")
                && artifact.content.contains(
                    "highway_kernel_mask_for(highway_kernel::"
                    "minimizer_two_stack_nthash)")
                && artifact.content.contains(
                    "highway_kernel_mask_for(highway_kernel::"
                    "minimizer_bounded_nthash)")
                && occurrences(
                       artifact.content,
                       "switch (invocation.sequence.alphabet_code)") == 0u
                && occurrences(
                       artifact.content,
                       "switch (invocation.sequence.storage_unit_bytes)")
                    == 0u
                && occurrences(
                       artifact.content,
                       "minimap2_reference_case<") == 0u
                && occurrences(
                       artifact.content,
                       "two_stack_nthash_case<") == 0u
                && occurrences(
                       artifact.content,
                       "bounded_nthash_case<") == 0u
                && occurrences(
                       artifact.content,
                       "minimizer_kernel_status::invalid_configuration") == 0u
                && artifact.content.contains(
                    "highway_alphabet_identity_entry alphabet_identities[]")
                && occurrences(
                       artifact.content,
                       "highway_alphabet_identity_for<::seq::dna4>()") == 1u
                && occurrences(
                       artifact.content,
                       "highway_alphabet_identity_for<::seq::dna5>()") == 1u
                && artifact.content.contains(
                    "table.alphabet_identities = alphabet_identities")
                && !artifact.content.contains("import :")
                && !artifact.content.contains("table.minimizer_case_"),
            "each binder assembles neutral minimizer wrapper pointers without "
            "instantiating the typed case matrix");
    }
    require(
        checked_binders == 2u,
        "both fixed targets receive typed minimizer wrappers");
    require(
        checked_minimizer_owner_units == 6u,
        "each fixed target compiles three minimizer owner units");

    generation_plan incomplete = plan;
    incomplete.source_inventory.slots.erase(
        incomplete.source_inventory.slots.begin(),
        incomplete.source_inventory.slots.begin() + 3);
    require_rejected(
        [&] { (void)render_generation(incomplete); },
        "a typed minimizer registry missing one Cartesian case is rejected");

    generation_plan missing_member = plan;
    missing_member.source_inventory.slots.pop_back();
    require_rejected(
        [&] { (void)render_generation(missing_member); },
        "a typed minimizer family missing an algorithm member is rejected");

    generation_plan duplicate_member = plan;
    duplicate_member.source_inventory.slots.push_back(
        duplicate_member.source_inventory.slots.front());
    require_rejected(
        [&] { (void)render_generation(duplicate_member); },
        "a duplicate typed minimizer algorithm member is rejected");

    generation_plan second_registry = plan;
    second_registry.source_inventory.slots.push_back(slot_spec{
        .owner_key = "minimizer_minimap2_reference",
        .family_field = "minimizers",
        .slot_field = "minimap2_reference",
        .expression =
            "&::bioseq::simd::{target_namespace}::minimap2_reference",
    });
    require_rejected(
        [&] { (void)render_generation(second_registry); },
        "typed minimizer cases reject a second hand-owned root-slot registry");

    generation_plan mixed = plan;
    mixed.source_inventory.slots.front().expression =
        "&::bioseq::simd::{target_namespace}::minimap2_reference_case<"
        "::seq::dna4,::std::uint64_t>";
    require_rejected(
        [&] { (void)render_generation(mixed); },
        "a typed minimizer family mixing template arguments is rejected");

    generation_plan mismatched_width = plan;
    for (slot_spec& slot : mismatched_width.source_inventory.slots) {
        if (slot.family_field == "minimizer_case_1_8") {
            slot.family_field = "minimizer_case_1_3";
        }
    }
    require_rejected(
        [&] { (void)render_generation(mismatched_width); },
        "a typed minimizer case with an invalid topology byte width is "
        "rejected");

    generation_plan wrong_leaf = plan;
    wrong_leaf.source_inventory.slots.front().expression =
        "&::bioseq::simd::{target_namespace}::bounded_nthash_case<"
        "::seq::dna5,::std::uint64_t>";
    require_rejected(
        [&] { (void)render_generation(wrong_leaf); },
        "a typed minimizer member naming the wrong case leaf is rejected");

    const auto append_alignment = [](
        generation_plan& target,
        const std::uint8_t code,
        const std::string_view alphabet) {
        const std::string family = "alignment_" + std::to_string(code);
        const auto append = [&target, &family, alphabet](
            const std::string_view owner,
            const std::string_view member) {
            target.source_inventory.slots.push_back(slot_spec{
                .owner_key = std::string{owner},
                .family_field = family,
                .slot_field = std::string{member},
                .expression =
                    "&::bioseq::simd::{target_namespace}::"
                    + std::string{member} + "<" + std::string{alphabet} + ">",
            });
        };
        append("ksw2", "align_ksw2_single_affine");
        append("ksw2", "align_ksw2_dual_affine");
        append("ksw2_batched", "align_ksw2_batched");
        append("block_aligner", "prepare_block_global_gap_certificate");
        append("block_aligner", "try_block_certified_gap");
        append("block_aligner", "align_block");
    };

    std::map<std::string, std::vector<const slot_spec*>, std::less<>>
        topology_families;
    for (const slot_spec& slot : plan.source_inventory.slots) {
        topology_families[slot.family_field].push_back(&slot);
    }
    const auto topology_identities = detail::collect_alphabet_identities(
        {}, detail::collect_minimizer_cases(topology_families));
    require(
        topology_identities.size() > 1u,
        "combined-consumer fixture exposes multiple topology alphabets");

    generation_plan combined = plan;
    for (const detail::alphabet_identity_binding& identity
         : topology_identities) {
        append_alignment(
            combined, identity.alphabet_code, identity.alphabet_type);
    }
    const generation_result combined_generated = render_generation(combined);
    std::size_t combined_binders = 0u;
    for (const generated_artifact& artifact : combined_generated.artifacts) {
        if (artifact.kind != artifact_kind::binder) continue;
        ++combined_binders;
        require(
            occurrences(
                artifact.content,
                "highway_alphabet_identity_for<::seq::dna4>()") == 1u
                && occurrences(
                       artifact.content,
                       "highway_alphabet_identity_for<::seq::dna5>()") == 1u,
            "alignment and minimizer families share one alphabet identity "
            "axis in each binder");
        for (const detail::alphabet_identity_binding& identity
             : topology_identities) {
            require(
                artifact.content.contains(
                    "highway_alphabet_identity_for<"
                    + identity.alphabet_type
                    + ">(), .alphabet_code = "
                    + std::to_string(identity.alphabet_code) + "u"),
                "the reconciled identity axis preserves each generated "
                "topology code/type pair");
        }
    }
    require(
        combined_binders == checked_binders,
        "each fixed target receives the reconciled identity axis");

    generation_plan mismatched_consumers = plan;
    for (std::size_t index = 0u; index < topology_identities.size(); ++index) {
        const detail::alphabet_identity_binding& code_owner =
            topology_identities[index];
        const detail::alphabet_identity_binding& wrong_type =
            topology_identities[(index + 1u) % topology_identities.size()];
        append_alignment(
            mismatched_consumers,
            code_owner.alphabet_code,
            wrong_type.alphabet_type);
    }
    require_rejected(
        [&] { (void)render_generation(mismatched_consumers); },
        "alignment and minimizer consumers must agree on topology code/type "
        "identity");
}

inline void verify_bounded_minimizer_representation_owners() {
    generation_plan plan = two_target_plan();
    append_fixture_source(plan, "minimizer_minimap2_reference", "minimizers");
    append_fixture_source(plan, "minimizer_bounded_nthash", "minimizers");
    plan.source_inventory.minimizer_representation_instances = {
        {"minimap2_reference_l15_k15", "minimap2_reference", "minimizer_minimap2_reference",
         "::bioseq::minimizers::minimizer_representation<15u,15u>", {"u32", "u64"}},
        {"minimap2_reference_l28_k15", "minimap2_reference", "minimizer_minimap2_reference",
         "::bioseq::minimizers::minimizer_representation<28u,15u>", {"u32", "u64"}},
        {"minimap2_reference_l33_k15", "minimap2_reference", "minimizer_minimap2_reference",
         "::bioseq::minimizers::minimizer_representation<33u,15u>", {"u32", "u64"}},
        {"minimap2_reference_l63_k15", "minimap2_reference", "minimizer_minimap2_reference",
         "::bioseq::minimizers::minimizer_representation<63u,15u>", {"u32", "u64"}},
        {"bounded_nthash_l15_k15", "bounded_nthash", "minimizer_bounded_nthash",
         "::bioseq::minimizers::minimizer_representation<15u,15u>", {"u32", "u64"}},
        {"bounded_nthash_l28_k15", "bounded_nthash", "minimizer_bounded_nthash",
         "::bioseq::minimizers::minimizer_representation<28u,15u>", {"u32", "u64"}},
        {"bounded_nthash_l63_k15", "bounded_nthash", "minimizer_bounded_nthash",
         "::bioseq::minimizers::minimizer_representation<63u,15u>", {"u32", "u64"}},
    };

    const generation_result generated = render_generation(plan);
    std::size_t owner_units = 0u;
    std::size_t minimap2_reference_wrappers = 0u;
    std::size_t bounded_nthash_wrappers = 0u;
    for (const generated_artifact& artifact : generated.artifacts) {
        if (artifact.kind != artifact_kind::minimizer_representation_instantiation) {
            continue;
        }
        ++owner_units;
        minimap2_reference_wrappers += occurrences(
            artifact.content, "minimap2_reference_repr_");
        bounded_nthash_wrappers += occurrences(
            artifact.content, "bounded_nthash_repr_");
        require(
            artifact.content.contains("module bioseq.highway.generated.")
                && artifact.content.contains("import std;")
                && artifact.content.contains(
                    "import bioseq.sequences.kmers.highway_minimizer_contract;")
                && occurrences(artifact.content, "HWY_ATTR bool") == 2u
                && !artifact.content.contains("std::uint16_t")
                && !artifact.content.contains("std::array"),
            "each representation owner is an ordinary two-width typed seam, "
            "not widened neutral storage");
    }
    require(
        owner_units == 14u
            && minimap2_reference_wrappers == 16u
            && bounded_nthash_wrappers == 12u,
        "the two-target low profile renders exactly 7 owners / 14 typed "
        "instantiations per target");
}

}  // namespace contract_detail

void run_highway_contract_self_tests() {
    contract_detail::verify_exact_structural_marker();
    contract_detail::verify_debug_assert_partition_preamble();
    contract_detail::verify_template_flatten_separation();
    contract_detail::verify_slot_callable_ownership();
    contract_detail::verify_complete_table_target_policy();
    contract_detail::verify_external_domain_topology_contract();
    contract_detail::verify_shared_target_coordinates();
    contract_detail::verify_generated_closure();
    contract_detail::verify_lambda_attribute_order();
    contract_detail::verify_type_only_target_source();
    contract_detail::verify_shared_publication_envelope();
    contract_detail::verify_typed_alignment_binder();
    contract_detail::verify_typed_minimizer_binder();
    contract_detail::verify_bounded_minimizer_representation_owners();
}

}  // namespace bioseq::codegen::highway
