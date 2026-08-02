module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

module bioseq.codegen.highway:contract_tests;

import :model;
import :renderer;
import :validator;
import bioseq.codegen.clang_structural;

namespace bioseq::codegen::highway {

namespace contract_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(
            "Highway generator contract failed: " + std::string{message});
    }
}

/// Require a negative generator-contract fixture to fail closed.
template <class Function>
inline void require_rejected(Function&& function, std::string_view message) {
    bool rejected = false;
    try {
        std::forward<Function>(function)();
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
                    .name_space = "N_EMU128",
                    .bit = 1u,
                },
                target_spec{
                    .platform = "x86",
                    .slug = "sse2",
                    .token = "HWY_SSE2",
                    .name_space = "N_SSE2",
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
        .clang_version = "contract-test",
        .compilation_signature = "contract-test",
    };
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

    const clang_structural::translation_unit unit =
        clang_structural::scan_translation_unit(
            "fixture_highway_marker.cppm", source);
    require(
        unit.marked_namespaces.size() == 1u,
        "only the exact bioseq::simd namespace marker is accepted");
    const clang_structural::marked_namespace_record& marker =
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

    generation_plan scalar = valid;
    scalar.target_inventory.targets.push_back(target_spec{
        .platform = "x86",
        .slug = "scalar",
        .token = "HWY_SCALAR",
        .name_space = "N_SCALAR",
        .bit = 4u,
    });
    require_rejected(
        [&] {
            validate_complete_table_target_policy(
                scalar.target_inventory);
        },
        "the complete-table target policy rejects HWY_SCALAR");

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

inline void verify_generated_closure() {
    const generation_result generated = render_generation(two_target_plan());
    require(
        generated.artifacts.size() == 9u,
        "two targets and one source produce two primaries, binders, "
        "partitions, one private registry, one cold inventory, and "
        "fixed-target test support");

    std::size_t primary_count = 0;
    std::size_t binder_count = 0;
    std::size_t partition_count = 0;
    std::size_t registry_count = 0;
    std::size_t inventory_count = 0;
    std::size_t test_support_count = 0;
    std::size_t export_count = 0;
    std::size_t dispatch_count = 0;
    std::set<std::pair<std::string, std::string>> target_source_closure;

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
                        && !artifact.content.contains("#include <hwy/")
                        && !artifact.content.contains("import :")
                        && !artifact.content.contains("HWY_TARGET"),
                    "each target primary is a minimal neutral BMI");
                break;
            case artifact_kind::binder:
                ++binder_count;
                require(
                    artifact.content.contains("MakeHighwayKernelTable")
                        && artifact.content.contains("kernels.complete()")
                        && artifact.content.contains(
                            "highway_kernel_set(HighwayKernelId::"
                            "prefilter_sneaky_snake_simd_batch)")
                        && artifact.content.contains("BindHighwayTarget"),
                    "each target binder owns a complete availability/table binding");
                break;
            case artifact_kind::partition:
                ++partition_count;
                target_source_closure.emplace(
                    artifact.target_token, artifact.source_key);
                require(
                    artifact.content.contains("#define HWY_TARGET ")
                        && artifact.content.contains("HWY_ATTR int kernel()")
                        && artifact.content.contains(
                            "import bioseq.fixture.neutral;"),
                    "each target/source partition fixes its target and rewrites the callable");
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
                            "binding->kernels->complete()")
                        && !artifact.content.contains(
                            "registered_highway_dispatch_info"),
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
                            "registered_highway_dispatch_info() noexcept")
                        && artifact.content.contains(
                            "static_cast<std::int64_t>(3ULL)")
                        && !artifact.content.contains("HWY_EXPORT(")
                        && !artifact.content.contains(
                            "HWY_DYNAMIC_DISPATCH(")
                        && !artifact.content.contains(
                            "bioseq.highway.generated."),
                    "the cold contract implementation owns only the exact "
                    "compiled mask");
                break;
            case artifact_kind::test_support:
                ++test_support_count;
                require(
                    artifact.module_name == "bioseq.highway.test_support"
                        && artifact.target_token == "HWY_EMU128"
                        && artifact.content.contains(
                            "N_EMU128::BindHighwayTarget()")
                        && artifact.content.contains(
                            "highway_test_kernel_table()")
                        && !artifact.content.contains("HWY_DYNAMIC_DISPATCH("),
                    "test support binds the configured fixed target without "
                    "introducing runtime dispatch");
                break;
        }
    }

    require(
        primary_count == 2u && binder_count == 2u
            && partition_count == 2u && registry_count == 1u
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
        generated.artifact_manifest_json.contains(
            "\"path\":\"highway_registry.cppm\"")
            && generated.artifact_manifest_json.contains(
                "\"path\":\"dispatch_inventory.cppm\"")
            && generated.artifact_manifest_json.contains(
                "\"path\":\"test_support.cppm\"")
            && generated.artifact_manifest_json.contains(
                "\"path\":\"x86/emu128/fixture.cppm\"")
            && generated.artifact_manifest_json.contains(
                "\"path\":\"x86/sse2/fixture.cppm\""),
        "the artifact manifest records the complete generated topology");
}

inline void verify_typed_alignment_binder() {
    generation_plan plan = two_target_plan();
    plan.source_inventory.require_complete_table = false;
    plan.source_inventory.required_slots.clear();
    plan.source_inventory.slots.clear();

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
        append("ksw2", "ksw2_z2");
        append("ksw2", "ksw2_d2");
        append("ksw2_batched", "ksw2_batched");
        append("block_aligner", "block_prepare");
        append("block_aligner", "block_try_certified_gap");
        append("block_aligner", "block_align");
    };
    append_alphabet(0u, "::seq::dna4");
    append_alphabet(1u, "::seq::dna5");

    const generation_result generated = render_generation(plan);
    std::size_t checked_binders = 0u;
    for (const generated_artifact& artifact : generated.artifacts) {
        if (artifact.kind != artifact_kind::binder) continue;
        ++checked_binders;
        require(
            artifact.content.contains("import bioseq.sequences;")
                && artifact.content.contains(
                    "AlignmentKernels<::seq::dna4> alignment_0")
                && artifact.content.contains(
                    "AlignmentKernels<::seq::dna5> alignment_1")
                && artifact.content.contains(
                    ".alphabet_code = 0u, .typed_table = &alignment_0")
                && artifact.content.contains(
                    ".alphabet_code = 1u, .typed_table = &alignment_1")
                && artifact.content.contains(
                    "highway_kernel_set(HighwayKernelId::alignment_ksw2)")
                && artifact.content.contains(
                    "highway_kernel_set(HighwayKernelId::alignment_ksw2_batched)")
                && artifact.content.contains(
                    "highway_kernel_set(HighwayKernelId::alignment_block_aligner)")
                && !artifact.content.contains("table.alignment_0"),
            "each binder synthesizes exact typed alignment tables without "
            "inventing root-table fields");
    }
    require(checked_binders == 2u, "both fixed targets receive typed alignment tables");

    generation_plan incomplete = plan;
    incomplete.source_inventory.slots.pop_back();
    require_rejected(
        [&] { (void)render_generation(incomplete); },
        "a typed alignment family missing any function slot is rejected");

    generation_plan mixed = plan;
    mixed.source_inventory.slots.front().expression =
        "&::bioseq::simd::{target_namespace}::ksw2_z2<::seq::dna5>";
    require_rejected(
        [&] { (void)render_generation(mixed); },
        "a typed alignment family mixing alphabet arguments is rejected");
}

}  // namespace contract_detail

void run_highway_contract_self_tests() {
    contract_detail::verify_exact_structural_marker();
    contract_detail::verify_slot_callable_ownership();
    contract_detail::verify_complete_table_target_policy();
    contract_detail::verify_generated_closure();
    contract_detail::verify_typed_alignment_binder();
}

}  // namespace bioseq::codegen::highway
