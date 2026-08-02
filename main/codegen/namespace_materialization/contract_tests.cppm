module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.namespace_materialization:contract_tests;

import :model;
import :manifest;
import :renderer;
import :validator;
import bioseq.codegen.clang_structural;
import bioseq.codegen.support;

export namespace bioseq::codegen::namespace_materialization {

namespace detail {

inline void require(const bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <typename Callable>
inline void require_rejected(Callable&& callable, std::string_view message) {
    bool rejected = false;
    try {
        std::invoke(std::forward<Callable>(callable));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}

[[nodiscard]] inline std::string_view fixture_source() {
    return R"CPP(export module bioseq.codegen.inputs.flatten_fixture;

import fixture.output;

namespace fixture {
namespace typed {

using Value
    [[bioseq::template_flatten(
        "identity", "abi",
        "i32", "int",
        "i64", "long")]]
    = int;

static constexpr int Bytes
    [[bioseq::template_flatten(
        "identity", "abi",
        "i32", "4",
        "i64", "8")]]
    = 4;

static constexpr int CompileShard
    [[bioseq::template_flatten(
        "shard", "shape",
        "root", "0",
        "leaf", "1")]]
    = 0;

namespace {
int common_support(int value) { return value + Bytes + CompileShard; }
}

[[bioseq::template_flatten_when("shape", "leaf")]]
[[bioseq::template_flatten_entry("leaf")]]
Value leaf(std::uint8_t abi_code, int value) {
    return static_cast<Value>(common_support(value));
}

[[bioseq::template_flatten_when("shape", "root")]]
[[bioseq::template_flatten_entry("root")]]
long root(std::uint8_t abi_code, int value) {
    return leaf(abi_code, value);
}

}  // namespace typed
}  // namespace fixture
)CPP";
}

[[nodiscard]] inline generation_plan fixture_plan() {
    const std::string source{fixture_source()};
    const clang_structural::translation_unit sliced =
        clang_structural::scan_token_slice("flatten_fixture.cppm", source);
    materialization_region region = extract_materialization_region(
        sliced, "fixture.output", source);
    topology_plan topology = build_topology_plan(std::move(region), 16u);
    validate_entry_ownership(topology);
    return generation_plan{
        .topology = std::move(topology),
        .repository_root = ".",
        .source_path = "flatten_fixture.cppm",
        .output_root = "generated/flatten_fixture",
        .source_text = source,
        .source_sha256 = sha256_hex(source),
        .name = "flatten_fixture",
        .profile = "2",
        .enabled_ordinals = {0u, 1u, 2u, 3u},
        .clang_version = "contract-test",
        .compilation_signature = "contract-test",
        .compiler_module_inventory_sha256 =
            "00000000000000000000000000000000"
            "00000000000000000000000000000000",
    };
}

inline void verify_closed_grammar() {
    const std::vector<std::string> parsed = parse_closed_string_arguments(
        R"("identity", "abi", "i32", "::std::int32_t")");
    require(
        parsed.size() == 4u && parsed[0] == "identity"
            && parsed[3] == "::std::int32_t",
        "closed grammar decodes ordinary narrow string literals");
    require_rejected(
        [] {
            static_cast<void>(parse_closed_string_arguments(
                R"("identity", abi, "i32", "int")"));
        },
        "closed grammar rejects an unquoted argument");
    require_rejected(
        [] {
            static_cast<void>(parse_closed_string_arguments(
                R"(("identity", "abi", "i32", "int"))"));
        },
        "closed grammar rejects an extra parenthesized argument layer");
    require_rejected(
        [] {
            clang_structural::declaration_record declaration;
            clang_structural::attribute_record attribute{
                .argument_text =
                    R"("identity", "Bad", "i32", "int")",
            };
            static_cast<void>(parse_binding_attribute(declaration, attribute));
        },
        "group labels are lowercase stable identifiers");
    require_rejected(
        [] {
            clang_structural::declaration_record declaration;
            clang_structural::attribute_record attribute{
                .argument_text =
                    R"("identity", "abi", "i32", "int;")",
            };
            static_cast<void>(parse_binding_attribute(declaration, attribute));
        },
        "replacement spellings reject declaration terminators");
    require(
        cpp_token_sequence("long long") != cpp_token_sequence("longlong"),
        "first-case authentication preserves token boundaries rather than deleting whitespace");
}

inline void verify_zip_product_and_digest() {
    generation_plan plan = fixture_plan();
    require(
        plan.topology.groups.size() == 2u
            && plan.topology.groups[0].name == "abi"
            && plan.topology.groups[0].binding_indices.size() == 2u
            && plan.topology.groups[1].name == "shape",
        "same-group bindings zip while first declaration orders groups");
    require(
        plan.topology.coordinates.size() == 4u
            && plan.topology.coordinates[0].assignments[0].case_label == "i32"
            && plan.topology.coordinates[0].assignments[1].case_label == "root"
            && plan.topology.coordinates[1].assignments[1].case_label == "leaf"
            && plan.topology.coordinates[2].assignments[0].case_label == "i64",
        "different groups form a rightmost-fast Cartesian product");
    require(
        plan.topology.identity_representative_ordinals
            == std::vector<std::uint32_t>{0u, 2u},
        "shard cases do not create additional stable API identities");
    const std::string digest = plan.topology.topology_digest;
    plan.topology.region.bindings[1].cases[1].spelling = "16";
    topology_plan changed = build_topology_plan(
        std::move(plan.topology.region), 16u);
    require(
        digest != changed.topology_digest,
        "topology digest authenticates every zipped binding replacement");
}

inline void verify_scheduler_profile_policy() {
    materialization_coordinate known{
        .assignments = {
            axis_case_assignment{
                .group = "alphabet",
                .role = axis_role::identity,
                .case_label = "dna5",
            },
            axis_case_assignment{
                .group = "width",
                .role = axis_role::identity,
                .case_label = "w8",
            },
        },
    };
    require(
        coordinate_enabled_for_profile(known, "1"),
        "profile 1 accepts the closed dna4/dna5 and w8 scheduler policy");
    known.assignments.front().case_label = "rna4";
    require(
        !coordinate_enabled_for_profile(known, "1"),
        "profile 1 rejects newly added alphabet labels until policy is updated");
}

inline void verify_exact_edits() {
    const std::string source = "0123456789";
    const std::string edited = apply_exact_edits(
        source,
        {
            source_edit{.begin_byte = 2u, .end_byte = 4u, .replacement = "A"},
            source_edit{.begin_byte = 7u, .end_byte = 9u, .replacement = "B"},
        });
    require(edited == "01A456B9", "exact edits apply in reverse byte order");
    require_rejected(
        [&] {
            static_cast<void>(apply_exact_edits(
                source,
                {
                    source_edit{.begin_byte = 2u, .end_byte = 6u},
                    source_edit{.begin_byte = 5u, .end_byte = 7u},
                }));
        },
        "overlapping exact edits fail closed");
    require_rejected(
        [&] {
            static_cast<void>(apply_exact_edits(
                source,
                {source_edit{.begin_byte = 9u, .end_byte = 11u}}));
        },
        "out-of-bounds exact edits fail closed");
}

inline void verify_generated_topology() {
    const generation_result generated = render_generation(fixture_plan(), false);
    require(
        generated.artifacts.size() == 8u,
        "two identities/two shards render API, selector, two probes, and four units");
    std::size_t probes = 0u;
    std::size_t instances = 0u;
    for (const materialization_artifact& artifact : generated.artifacts) {
        require(
            artifact.relative_path.extension() == ".cppm",
            "every generated C++ artifact is a .cppm");
        require(
            artifact.content_sha256 == sha256_hex(artifact.content),
            "artifact content digest covers final bytes");
        if (artifact.artifact_kind
            == materialization_artifact::kind::declaration_probe) {
            ++probes;
            require(
                artifact.content.contains("template_flatten_provenance")
                    && !artifact.content.contains("return leaf("),
                "declaration probes retain provenance but no function bodies");
        } else {
            require(
                !artifact.content.contains("[[bioseq::template_flatten"),
                "production artifacts contain no policy markers");
        }
        if (artifact.artifact_kind
            == materialization_artifact::kind::implementation_unit) {
            ++instances;
            require(
                artifact.content.contains("module fixture.output;")
                    && artifact.content.contains("import :typed_flat_api;")
                    && artifact.content.contains("bioseq_flattened"),
                "instances attach once and import the private API partition");
        }
    }
    require(probes == 2u && instances == 4u, "probe/heavy action counts are exact");
}

inline void verify_policy_neutral_publication_envelope() {
    const std::string content = "export module fixture.generated;\n";
    const std::filesystem::path output_root = std::filesystem::absolute(
        "generated/publication_contract").lexically_normal();
    const rendered_publication_manifest rendered =
        render_publication_manifest(publication_manifest_request{
            .output_root = output_root,
            .policy = "highway",
            .name = "fixture_highway",
            .topology_digest = sha256_hex("fixture topology"),
            .policy_evidence_json =
                R"({"schema_version":1,"producer":"highway"})",
            .artifacts = {
                publication_artifact{
                    .relative_path = "fixture.cppm",
                    .content = content,
                    .content_sha256 = sha256_hex(content),
                    .artifact_class =
                        publication_artifact_class::production_cppm,
                },
            },
        });
    require(
        rendered.json.contains("\"policy\": \"highway\"")
            && rendered.json.contains(
                "\"class\":\"production_cppm\"")
            && rendered.json.contains(
                "\"evidence\": {\"schema_version\":1"),
        "the shared publication envelope owns only common fields and opaque policy evidence");
    require(
        rendered.manifest_identity.size() == 64u
            && rendered.json.contains(rendered.manifest_identity),
        "the shared publication envelope is self-sealed");
    require_rejected(
        [&] {
            static_cast<void>(render_publication_manifest(
                publication_manifest_request{
                    .output_root = output_root,
                    .policy = "highway",
                    .name = "fixture_highway",
                    .topology_digest = sha256_hex("fixture topology"),
                    .policy_evidence_json = "[]",
                    .artifacts = {
                        publication_artifact{
                            .relative_path = "fixture.cppm",
                            .content = content,
                            .content_sha256 = sha256_hex(content),
                        },
                    },
                }));
        },
        "the shared publisher accepts policy evidence only as a JSON object");
}

}  // namespace detail

inline void run_namespace_materialization_contract_tests() {
    detail::verify_closed_grammar();
    detail::verify_zip_product_and_digest();
    detail::verify_scheduler_profile_policy();
    detail::verify_exact_edits();
    detail::verify_generated_topology();
    detail::verify_policy_neutral_publication_envelope();
}

}  // namespace bioseq::codegen::namespace_materialization
