module;

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.namespace_materialization:tests.contract;

import :model;
import :manifest;
import :renderer;
import :validator;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

export namespace bioseq::codegen::namespace_materialization {

namespace detail {

inline void require(const bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

template <typename p_callable>
inline void require_rejected(p_callable&& callable, std::string_view message) {
    bool rejected = false;
    try {
        std::invoke(std::forward<p_callable>(callable));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}

[[nodiscard]] inline std::string replace_once(
    std::string source,
    std::string_view before,
    std::string_view after) {
    const std::size_t position = source.find(before);
    require(position != std::string::npos, "contract mutation anchor is absent");
    require(
        source.find(before, position + before.size()) == std::string::npos,
        "contract mutation anchor is ambiguous");
    source.replace(position, before.size(), after);
    return source;
}

[[nodiscard]] inline std::size_t occurrences(
    std::string_view source,
    std::string_view needle) noexcept {
    std::size_t count = 0u;
    std::size_t position = 0u;
    while ((position = source.find(needle, position))
           != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

[[nodiscard]] inline std::string normalized_token_digest(
    std::string_view source) {
    const structural_analysis::translation_unit unit =
        structural_analysis::scan_token_slice(
            "template_flatten_head_digest.cppm", source);
    std::ostringstream canonical;
    for (const structural_analysis::lexical_token& token : unit.tokens) {
        canonical << token.spelling.size() << ':' << token.spelling << '\n';
    }
    require(!unit.tokens.empty(), "normalized token digest requires tokens");
    return bioseq::common::sha256_hex(canonical.str());
}

[[nodiscard]] inline std::string declaration_head_digest(
    std::string_view source,
    const structural_analysis::declaration_record& declaration) {
    const auto& range = declaration.declaration_head_range;
    require(
        range.end.byte_offset > range.begin.byte_offset
            && range.end.byte_offset <= source.size(),
        "declaration-head digest range is invalid");
    return normalized_token_digest(source.substr(
        range.begin.byte_offset,
        range.end.byte_offset - range.begin.byte_offset));
}

struct production_scheduler_fixture final {
    std::filesystem::path repository_root;
    std::filesystem::path source_path;
};

[[nodiscard]] inline production_scheduler_fixture
production_scheduler_source() {
#ifndef BIOSEQ_NAMESPACE_MATERIALIZATION_TEST_REPOSITORY_ROOT
    throw std::runtime_error(
        "namespace-materialization production contracts require the configured "
        "BIOSEQ_REPOSITORY_ROOT");
#else
    std::error_code error;
    const std::filesystem::path configured_root{
        BIOSEQ_NAMESPACE_MATERIALIZATION_TEST_REPOSITORY_ROOT};
    if (!configured_root.is_absolute()) {
        throw std::runtime_error(
            "configured namespace-materialization repository root is not absolute");
    }
    const std::filesystem::path repository_root =
        std::filesystem::weakly_canonical(configured_root, error);
    if (error || repository_root != configured_root
        || !std::filesystem::is_directory(repository_root, error) || error) {
        throw std::runtime_error(
            "configured namespace-materialization repository root is not its "
            "canonical directory identity");
    }
    const std::filesystem::path scheduler = repository_root
        / "bioseq/backend/pg/scheduler/dispatch_processor/typed_template.cppm";
    if (!std::filesystem::is_regular_file(scheduler, error) || error) {
        throw std::runtime_error(
            "production scheduler materialization source is unavailable: "
            + scheduler.string());
    }
    return {
        .repository_root = repository_root,
        .source_path = std::filesystem::weakly_canonical(scheduler),
    };
#endif
}

[[nodiscard]] inline std::vector<std::uint32_t> enabled_ordinals(
    const topology_plan& topology,
    std::string_view profile) {
    std::vector<std::uint32_t> result;
    for (const materialization_coordinate& coordinate : topology.coordinates) {
        if (coordinate_enabled_for_profile(coordinate, profile)) {
            result.push_back(coordinate.ordinal);
        }
    }
    return result;
}

[[nodiscard]] inline generation_plan production_scheduler_plan(
    std::string profile = "all_coordinates") {
    const production_scheduler_fixture fixture =
        production_scheduler_source();
    const std::filesystem::path& source_path = fixture.source_path;
    const std::string source = read_artifact_bytes(source_path);
    const structural_analysis::translation_unit sliced =
        structural_analysis::scan_token_slice(source_path, source);
    materialization_region region = extract_materialization_region(
        sliced, "bioseq.postgres.scheduler.dispatch_processor", source);
    topology_plan topology = build_topology_plan(std::move(region));
    std::vector<std::uint32_t> enabled = enabled_ordinals(topology, profile);
    validate_enabled_ordinals(topology, profile, enabled);
    validate_entry_ownership(topology);
    return generation_plan{
        .topology = std::move(topology),
        .repository_root = fixture.repository_root,
        .source_path = source_path,
        .output_root = std::filesystem::absolute(
            "generated/dispatch_processor_typed_contract").lexically_normal(),
        .source_text = source,
        .source_sha256 = bioseq::common::sha256_hex(source),
        .name = "dispatch_processor_typed",
        .profile = std::move(profile),
        .enabled_ordinals = std::move(enabled),
        .clang_release = "contract-test",
        .compilation_signature = "contract-test",
        .compiler_module_inventory_sha256 =
            "00000000000000000000000000000000"
            "00000000000000000000000000000000",
    };
}

class scoped_temp_directory final {
public:
    scoped_temp_directory() {
        const std::string seed = std::string{__FILE__} + ':'
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + ':' + std::to_string(
                reinterpret_cast<std::uintptr_t>(this));
        const std::filesystem::path base =
            std::filesystem::temp_directory_path();
        for (std::uint32_t attempt = 0u; attempt != 32u; ++attempt) {
            std::error_code error;
            const std::filesystem::path candidate = base
                / ("bioseq-namespace-materialization-contract-"
                   + bioseq::common::sha256_hex(seed + ':' + std::to_string(attempt)).substr(
                       0u, 20u));
            if (!std::filesystem::create_directory(candidate, error)) {
                if (!error) continue;
                throw std::runtime_error(
                    "cannot create publisher contract directory: "
                    + error.message());
            }
            path_ = std::filesystem::weakly_canonical(candidate, error);
            if (error) {
                throw std::runtime_error(
                    "cannot canonicalize publisher contract directory: "
                    + error.message());
            }
            return;
        }
        throw std::runtime_error(
            "cannot allocate a unique publisher contract directory");
    }

    scoped_temp_directory(const scoped_temp_directory&) = delete;
    scoped_temp_directory& operator=(const scoped_temp_directory&) = delete;

    ~scoped_temp_directory() {
        if (path_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

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
    const std::filesystem::path repository_root =
        std::filesystem::absolute(".").lexically_normal();
    const std::filesystem::path source_path =
        repository_root / "flatten_fixture.cppm";
    const structural_analysis::translation_unit sliced =
        structural_analysis::scan_token_slice(source_path, source);
    materialization_region region = extract_materialization_region(
        sliced, "fixture.output", source);
    topology_plan topology = build_topology_plan(std::move(region), 16u);
    validate_entry_ownership(topology);
    return generation_plan{
        .topology = std::move(topology),
        .repository_root = repository_root,
        .source_path = source_path,
        .output_root = repository_root / "generated/flatten_fixture",
        .source_text = source,
        .source_sha256 = bioseq::common::sha256_hex(source),
        .name = "flatten_fixture",
        .profile = "all_coordinates",
        .enabled_ordinals = {0u, 1u, 2u, 3u},
        .clang_release = "contract-test",
        .compilation_signature = "contract-test",
        .compiler_module_inventory_sha256 =
            "00000000000000000000000000000000"
            "00000000000000000000000000000000",
    };
}

inline void attach_fixture_frontend_evidence(generation_plan& plan) {
    const generation_result provisional = render_generation(plan, false);
    plan.declaration_probes.clear();
    for (const materialization_artifact& artifact : provisional.artifacts) {
        if (artifact.artifact_kind
            != materialization_artifact::kind::declaration_probe) {
            continue;
        }
        require(
            !artifact.logical_coordinate_ordinals.empty(),
            "fixture declaration probe must cover an identity coordinate");
        structural_analysis::translation_unit probe;
        probe.source_path = plan.output_root / artifact.relative_path;
        probe.source_sha256 = bioseq::common::sha256_hex(artifact.content);
        probe.clang_release = plan.clang_release;
        probe.mode = structural_analysis::collection_mode::declaration_probe;
        probe.source_module.provided_module = artifact.module_name;
        structural_analysis::declaration_record identity;
        identity.stable_usr = "c:@F@fixture_probe_"
            + std::to_string(artifact.logical_coordinate_ordinals.front());
        identity.qualified_name = "fixture::probe_identity";
        identity.source_identity = "fixture-provenance-"
            + std::to_string(artifact.logical_coordinate_ordinals.front());
        identity.source_coordinate =
            std::to_string(artifact.logical_coordinate_ordinals.front());
        identity.linkage = structural_analysis::entity_linkage::module;
        identity.attachment =
            structural_analysis::module_attachment::named_module;
        probe.declarations.push_back(std::move(identity));
        plan.declaration_probes.push_back(std::move(probe));
    }
    plan.frontend_actions = frontend_action_counts{
        .dependency_preparation_validations = 1u,
        .canonical_token_slice_scans = 2u,
        .signature_donor_token_slice_scans = 1u
            + static_cast<std::uint32_t>(plan.declaration_probes.size()),
        .body_free_declaration_probe_scans =
            static_cast<std::uint32_t>(plan.declaration_probes.size()),
        .artifact_render_passes = 2u,
        .body_bearing_compiler_invocations = 0u,
    };
    plan.compilation_obligations = cmake_compilation_obligations{
        .api_partition_units = 1u,
        .selector_partition_units = 1u,
        .body_bearing_implementation_units =
            static_cast<std::uint32_t>(plan.enabled_ordinals.size()),
    };
}

enum class topology_mutation : std::uint8_t {
    reordered,
    grown,
    shrunk,
};

[[nodiscard]] inline generation_plan fixture_plan(topology_mutation mutation) {
    std::string source{fixture_source()};
    if (mutation == topology_mutation::reordered) {
        source = replace_once(
            std::move(source),
            R"("i32", "int",
        "i64", "long")]]
    = int;)",
            R"("i64", "long",
        "i32", "int")]]
    = long;)");
        source = replace_once(
            std::move(source),
            R"("i32", "4",
        "i64", "8")]]
    = 4;)",
            R"("i64", "8",
        "i32", "4")]]
    = 8;)");
    } else if (mutation == topology_mutation::grown) {
        source = replace_once(
            std::move(source),
            R"("i64", "long")]])",
            R"("i64", "long",
        "i128", "long long")]])");
        source = replace_once(
            std::move(source),
            R"("i64", "8")]])",
            R"("i64", "8",
        "i128", "16")]])");
    } else {
        source = replace_once(
            std::move(source),
            R"("i32", "int",
        "i64", "long")]])",
            R"("i32", "int")]])");
        source = replace_once(
            std::move(source),
            R"("i32", "4",
        "i64", "8")]])",
            R"("i32", "4")]])");
    }
    const std::filesystem::path repository_root =
        std::filesystem::absolute(".").lexically_normal();
    const std::filesystem::path source_path =
        repository_root / "flatten_fixture_mutation.cppm";
    const structural_analysis::translation_unit sliced =
        structural_analysis::scan_token_slice(
            source_path, source);
    materialization_region region = extract_materialization_region(
        sliced, "fixture.output", source);
    topology_plan topology = build_topology_plan(std::move(region), 16u);
    validate_entry_ownership(topology);
    std::vector<std::uint32_t> enabled = enabled_ordinals(
        topology, "all_coordinates");
    return generation_plan{
        .topology = std::move(topology),
        .repository_root = repository_root,
        .source_path = source_path,
        .output_root = repository_root / "generated/flatten_fixture_mutation",
        .source_text = source,
        .source_sha256 = bioseq::common::sha256_hex(source),
        .name = "flatten_fixture_mutation",
        .profile = "all_coordinates",
        .enabled_ordinals = std::move(enabled),
        .clang_release = "contract-test",
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
            static_cast<void>(parse_closed_string_arguments(
                R"("identity", "abi", "i32", "int",)"));
        },
        "closed grammar rejects a trailing comma");
    require_rejected(
        [] {
            static_cast<void>(parse_closed_string_arguments(
                R"("identity", AXIS_CASES)"));
        },
        "closed grammar rejects macro-spelled arguments");
    require_rejected(
        [] {
            static_cast<void>(parse_closed_string_arguments(
                R"("identity" "abi", "i32", "int")"));
        },
        "closed grammar rejects concatenated literals");
    require_rejected(
        [] {
            structural_analysis::declaration_record declaration;
            structural_analysis::attribute_record attribute{
                .argument_text =
                    R"("identity", "Bad", "i32", "int")",
            };
            static_cast<void>(parse_binding_attribute(declaration, attribute));
        },
        "group labels are lowercase stable identifiers");
    require_rejected(
        [] {
            structural_analysis::declaration_record declaration;
            structural_analysis::attribute_record attribute{
                .argument_text =
                    R"("identity", "abi", "i32", "int;")",
            };
            static_cast<void>(parse_binding_attribute(declaration, attribute));
        },
        "replacement spellings reject declaration terminators");
    require_rejected(
        [] {
            structural_analysis::attribute_record attribute{
                .argument_text = R"("shape", "root", "root")",
            };
            static_cast<void>(parse_selection_attribute(attribute));
        },
        "selection markers reject duplicate case labels");
    require(
        cpp_token_sequence("long long") != cpp_token_sequence("longlong"),
        "first-case authentication preserves token boundaries rather than deleting whitespace");
}

inline void verify_malformed_materialization_sources() {
    const auto reject_source = [](std::string source, std::string_view message) {
        require_rejected(
            [&source] {
                const structural_analysis::translation_unit sliced =
                    structural_analysis::scan_token_slice(
                        "malformed_template_flatten.cppm", source);
                materialization_region region = extract_materialization_region(
                    sliced, "fixture.output", source);
                static_cast<void>(build_topology_plan(std::move(region), 32u));
            },
            message);
    };

    reject_source(
        replace_once(
            std::string{fixture_source()},
            "static constexpr int Bytes",
            "constexpr int Bytes"),
        "binding variables require exact static constexpr ownership");
    reject_source(
        replace_once(
            std::string{fixture_source()},
            R"("identity", "abi",
        "i32", "int",
        "i64", "long")",
            "AXIS_CASES"),
        "macro-spelled binding arguments fail closed");
    std::string conditional = replace_once(
        std::string{fixture_source()},
        "namespace {\nint common_support",
        "#if 1\nnamespace {\nint common_support");
    conditional = replace_once(
        std::move(conditional),
        "}\n\n[[bioseq::template_flatten_when(\"shape\", \"leaf\")]]",
        "}\n#endif\n\n[[bioseq::template_flatten_when(\"shape\", \"leaf\")]]");
    reject_source(
        std::move(conditional),
        "preprocessor directives inside the materialization namespace fail closed");

    reject_source(
        R"CPP(export module malformed.prefix_binding;
namespace fixture::typed {
[[bioseq::template_flatten("identity", "abi", "i32", "int")]]
using Value = int;
static constexpr int CompileShard
    [[bioseq::template_flatten("shard", "shape", "root", "0")]] = 0;
[[bioseq::template_flatten_when("shape", "root")]]
[[bioseq::template_flatten_entry("root")]]
int root(int abi_code) { return abi_code; }
}
)CPP",
        "binding attributes in declaration-prefix position fail closed");
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

inline void verify_configure_structural_digest_mutations() {
    constexpr std::array expected_digests{
        std::string_view{
            "e97ead358799c54147182004259e7f5a8ab6992094cf31b4fa5ff816bb695561"},
        std::string_view{
            "3c7692b6d3be17a4719670c5b0e25fc8729af4e38f8dc9051a70cafe03fd9aaa"},
        std::string_view{
            "7c41e4433682c0f51bfbeea6e269873de8e96f322a5b1190f489af073bbabe6d"},
        std::string_view{
            "18180e4fa629891eb7fe8eb8a4f0a025020f4fe054f16a2d5081fc9e3a0b0632"},
    };
    std::array<generation_plan, 4u> plans{
        fixture_plan(),
        fixture_plan(topology_mutation::reordered),
        fixture_plan(topology_mutation::grown),
        fixture_plan(topology_mutation::shrunk),
    };
    for (std::size_t index = 0u; index < plans.size(); ++index) {
        require(
            plans[index].topology.topology_digest == expected_digests[index],
            "configure/structural topology digest golden drifted");
        validate_configure_topology_digest(
            plans[index].topology, expected_digests[index]);
    }
    require(
        plans[0].topology.coordinates.size() == 4u
            && plans[1].topology.coordinates.size() == 4u
            && plans[2].topology.coordinates.size() == 6u
            && plans[3].topology.coordinates.size() == 2u,
        "normal/reordered/grown/shrunk fixture cardinalities are exact");
    for (std::size_t index = 1u; index < plans.size(); ++index) {
        require_rejected(
            [&plans, index] {
                validate_configure_topology_digest(
                    plans[index].topology,
                    plans[0].topology.topology_digest);
            },
            "stale configure digest must reject reordered/grown/shrunk topology");
    }
}

inline void verify_scheduler_highway_separation() {
    constexpr std::string_view output_module =
        "bioseq.postgres.scheduler.dispatch_processor";
    const auto scan = [](std::string source) {
        return structural_analysis::scan_token_slice(
            "scheduler_highway_separation.cppm", source);
    };
    const auto source_with = [](std::string_view addition) {
        return std::string{
                   "export module fixture.scheduler_highway_separation;\n"
                   "import bioseq.postgres.scheduler.dispatch_processor;\n"}
            + std::string{addition}
            + "\nnamespace fixture::scheduler_highway_separation {\n"
              "inline constexpr int HWY_TARGETED = 1;\n"
              "inline constexpr int HWY_EXPORTABLE = 2;\n"
              "inline constexpr int HWY_DYNAMIC_DISPATCHED = 3;\n"
              "namespace [[bioseq::similar]] decoy {}\n"
              "}\n";
    };

    const structural_analysis::translation_unit neutral = scan(source_with(
        "import bioseq.highway.dispatch_contract;\n"
        "import bioseq.highway.dispatch_contract:pipeline;"));
    validate_scheduler_highway_source_separation(neutral, output_module);

    for (const std::string_view imported : {
             std::string_view{"bioseq.highway.generated.linux.avx2"},
             std::string_view{
                 "bioseq.postgres.scheduler.dispatch_processor:highway_registry"},
         }) {
        const structural_analysis::translation_unit forbidden_import = scan(
            source_with("import " + std::string{imported} + ";"));
        require_rejected(
            [&forbidden_import] {
                validate_scheduler_highway_source_separation(
                    forbidden_import,
                    "bioseq.postgres.scheduler.dispatch_processor");
            },
            "scheduler source accepted a direct Highway generated/registry import");
    }

    for (const std::string_view token : {
             std::string_view{"HWY_TARGET"},
             std::string_view{"HWY_EXPORT"},
             std::string_view{"HWY_DYNAMIC_DISPATCH"},
         }) {
        const structural_analysis::translation_unit forbidden_token = scan(
            source_with(
                "namespace fixture_highway_token { inline constexpr int "
                + std::string{token} + " = 1; }"));
        require_rejected(
            [&forbidden_token] {
                validate_scheduler_highway_source_separation(
                    forbidden_token,
                    "bioseq.postgres.scheduler.dispatch_processor");
            },
            "scheduler source accepted a forbidden Highway lexical token");
    }

    const structural_analysis::translation_unit simd_marked = scan(source_with(
        "namespace [[bioseq::simd]] target { inline int value() { return 1; } }"));
    require_rejected(
        [&simd_marked] {
            validate_scheduler_highway_source_separation(
                simd_marked,
                "bioseq.postgres.scheduler.dispatch_processor");
        },
        "scheduler source accepted a bioseq::simd structural marker");

    structural_analysis::dependency_compilation_validation prepared;
    prepared.modules = {
        structural_analysis::dependency_module_identity{
            .module_name = "bioseq.postgres.scheduler.dispatch_processor",
            .source_path = "dispatch_processor.cppm",
        },
        structural_analysis::dependency_module_identity{
            .module_name = "bioseq.highway.dispatch_contract",
            .source_path = "highway_dispatch_contract.cppm",
        },
        structural_analysis::dependency_module_identity{
            .module_name = "bioseq.highway.dispatch_contract:pipeline",
            .source_path = "highway_dispatch_pipeline_contract.cppm",
        },
    };
    validate_scheduler_highway_prepared_closure(output_module, prepared);

    for (const std::string_view provider : {
             std::string_view{"bioseq.highway.generated.linux.avx2"},
             std::string_view{
                 "bioseq.postgres.scheduler.dispatch_processor:highway_registry"},
         }) {
        structural_analysis::dependency_compilation_validation forbidden = prepared;
        forbidden.modules.push_back(
            structural_analysis::dependency_module_identity{
                .module_name = std::string{provider},
                .source_path = "forbidden_highway_provider.cppm",
            });
        require_rejected(
            [&forbidden] {
                validate_scheduler_highway_prepared_closure(
                    "bioseq.postgres.scheduler.dispatch_processor", forbidden);
            },
            "authenticated scheduler closure accepted a forbidden Highway provider");
    }

    validate_scheduler_highway_source_separation(
        simd_marked, "fixture.unrelated_output");
    structural_analysis::dependency_compilation_validation unrelated = prepared;
    unrelated.modules.push_back(
        structural_analysis::dependency_module_identity{
            .module_name = "bioseq.highway.generated.linux.avx2",
            .source_path = "unrelated_policy_provider.cppm",
        });
    validate_scheduler_highway_prepared_closure(
        "fixture.unrelated_output", unrelated);
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
        coordinate_enabled_for_profile(known, "all_alphabets_uint64"),
        "all_alphabets_uint64 accepts the closed dna4/dna5 w8 policy");
    known.assignments.front().case_label = "rna4";
    require(
        !coordinate_enabled_for_profile(known, "all_alphabets_uint64"),
        "all_alphabets_uint64 rejects labels outside its closed policy");
}

inline constexpr std::array<std::string_view, 8u>
    scheduler_identity_namespaces{
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna4::width_w1",
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna4::width_w2",
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna4::width_w4",
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna4::width_w8",
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna5::width_w1",
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna5::width_w2",
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna5::width_w4",
        "bioseq::simd::typed_dispatch::bioseq_flattened::alphabet_dna5::width_w8",
    };

inline void verify_production_scheduler_golden() {
    generation_plan plan = production_scheduler_plan();
    require(
        plan.topology.topology_digest
            == "4d20a72fb4fa5f4137ccbd56bc2e396fbf387126baaf4bdaea08d45a773d405c",
        "production scheduler topology digest drifted from the reviewed golden");
    validate_configure_topology_digest(
        plan.topology,
        "4d20a72fb4fa5f4137ccbd56bc2e396fbf387126baaf4bdaea08d45a773d405c");
    require(
        plan.topology.groups.size() == 3u
            && plan.topology.groups[0].name == "alphabet"
            && plan.topology.groups[0].role == axis_role::identity
            && plan.topology.groups[1].name == "width"
            && plan.topology.groups[1].role == axis_role::identity
            && plan.topology.groups[2].name == "shape"
            && plan.topology.groups[2].role == axis_role::shard,
        "production scheduler group order/roles drifted");
    const auto case_projection = [](const axis_group& group) {
        std::vector<std::string> result;
        for (const replacement_case& replacement : group.cases) {
            result.push_back(
                replacement.label + '=' + replacement.spelling + '@'
                + std::to_string(replacement.ordinal));
        }
        return result;
    };
    require(
        case_projection(plan.topology.groups[0])
                == std::vector<std::string>{
                    "dna4=::seq::dna4@0", "dna5=::seq::dna5@1"}
            && case_projection(plan.topology.groups[1])
                == std::vector<std::string>{
                    "w1=1@0", "w2=2@1", "w4=4@2", "w8=8@3"}
            && case_projection(plan.topology.groups[2])
                == std::vector<std::string>{
                    "root=0@0",
                    "scanner_pg=1@1",
                    "inmemory=2@2",
                    "inmemory_standard=3@3",
                    "inmemory_segmented=4@4",
                    "inmemory_chunked=5@5",
                    "inmemory_chunked_segmented=6@6",
                },
        "production scheduler cases/replacements/ordinals drifted");
    require(
        plan.topology.coordinates.size() == 56u
            && plan.topology.identity_representative_ordinals
                == std::vector<std::uint32_t>{
                    0u, 7u, 14u, 21u, 28u, 35u, 42u, 49u},
        "production scheduler must retain 56 rows and eight identities");
    for (std::size_t ordinal = 0u;
         ordinal < plan.topology.coordinates.size();
         ++ordinal) {
        const materialization_coordinate& coordinate =
            plan.topology.coordinates[ordinal];
        const std::size_t alphabet = ordinal / 28u;
        const std::size_t width = (ordinal / 7u) % 4u;
        const std::size_t shape = ordinal % 7u;
        require(
            coordinate.ordinal == ordinal
                && coordinate.assignments[0].case_label
                    == (alphabet == 0u ? "dna4" : "dna5")
                && coordinate.assignments[1].case_label
                    == std::array{
                        std::string_view{"w1"},
                        std::string_view{"w2"},
                        std::string_view{"w4"},
                        std::string_view{"w8"},
                    }[width]
                && coordinate.assignments[2].case_label
                    == std::array{
                        std::string_view{"root"},
                        std::string_view{"scanner_pg"},
                        std::string_view{"inmemory"},
                        std::string_view{"inmemory_standard"},
                        std::string_view{"inmemory_segmented"},
                        std::string_view{"inmemory_chunked"},
                        std::string_view{"inmemory_chunked_segmented"},
                    }[shape]
                && coordinate.identity_namespace
                    == scheduler_identity_namespaces[alphabet * 4u + width],
            "production scheduler coordinate order drifted");
    }
    require(
        enabled_ordinals(plan.topology, "dna4_uint64")
                == std::vector<std::uint32_t>{
                    21u, 22u, 23u, 24u, 25u, 26u, 27u}
            && enabled_ordinals(plan.topology, "all_alphabets_uint64")
                == std::vector<std::uint32_t>{
                    21u, 22u, 23u, 24u, 25u, 26u, 27u,
                    49u, 50u, 51u, 52u, 53u, 54u, 55u}
            && enabled_ordinals(plan.topology, "all_coordinates").size() == 56u,
        "production scheduler semantic profile projections must remain 7/14/56");

    struct role_golden {
        std::string_view name;
        std::string_view accepted_shapes;
        bool api_entry;
        bool support_member;
    };
    constexpr std::array role_map{
        role_golden{"same_dispatch_target_locator", "root", false, true},
        role_golden{
            "format6_parallel_reader_identity_valid", "root", false, true},
        role_golden{
            "shape_v5_parallel_reader_identity_valid", "root", false, true},
        role_golden{
            "attach_retained_format6_query_proof", "root", false, true},
        role_golden{
            "with_source_provider_pg_locator", "root,scanner_pg", false, true},
        role_golden{
            "with_pg_sequence_recipe", "root,scanner_pg", false, true},
        role_golden{
            "with_source_provider_pg_target", "root,scanner_pg", false, true},
        role_golden{"process_scanner_pg_row_typed", "scanner_pg", false, true},
        role_golden{
            "process_inmemory_standard_row_typed",
            "inmemory_standard",
            false,
            true},
        role_golden{
            "process_inmemory_segmented_row_typed",
            "inmemory_segmented",
            false,
            true},
        role_golden{
            "process_inmemory_chunked_row_typed",
            "inmemory_chunked",
            false,
            true},
        role_golden{
            "process_inmemory_chunked_segmented_row_typed",
            "inmemory_chunked_segmented",
            false,
            true},
        role_golden{"process_inmemory_row_typed", "inmemory", false, true},
        role_golden{"process_sequences_typed", "root", false, true},
        role_golden{"process_scanner_pg_row", "scanner_pg", true, false},
        role_golden{"process_inmemory_row", "inmemory", true, false},
        role_golden{
            "process_inmemory_standard_row", "inmemory_standard", true, false},
        role_golden{
            "process_inmemory_segmented_row", "inmemory_segmented", true, false},
        role_golden{
            "process_inmemory_chunked_row", "inmemory_chunked", true, false},
        role_golden{
            "process_inmemory_chunked_segmented_row",
            "inmemory_chunked_segmented",
            true,
            false},
        role_golden{"process_sequences", "root", true, false},
    };
    require(
        plan.topology.region.support_island.has_value()
            && plan.topology.region.controlled_declarations.size()
                == role_map.size(),
        "production scheduler support island/controlled role cardinality drifted");
    std::map<std::string, const owned_declaration*, std::less<>> declarations;
    for (const owned_declaration& declaration :
         plan.topology.region.controlled_declarations) {
        require(
            declarations.emplace(declaration.declaration.name, &declaration).second,
            "production scheduler controlled declaration name is duplicated");
    }
    const auto& support = *plan.topology.region.support_island;
    for (const role_golden& expected : role_map) {
        const auto found = declarations.find(expected.name);
        require(found != declarations.end(), "production scheduler role is missing");
        const owned_declaration& declaration = *found->second;
        std::string selected;
        for (const selection_clause& clause : declaration.clauses) {
            require(
                clause.group == "shape" && !clause.accepted_case_labels.empty(),
                "production scheduler role must select only the shape group");
            for (const std::string& label : clause.accepted_case_labels) {
                if (!selected.empty()) selected.push_back(',');
                selected += label;
            }
        }
        const bool inside_support =
            support.body_range.begin.byte_offset
                <= declaration.declaration.declaration_range.begin.byte_offset
            && declaration.declaration.declaration_range.end.byte_offset
                <= support.body_range.end.byte_offset;
        require(
            selected == expected.accepted_shapes
                && declaration.generated_api_entry == expected.api_entry
                && inside_support == expected.support_member,
            "production scheduler support/role map drifted");
    }
}

inline void verify_production_declaration_heads_and_probes() {
    generation_plan plan = production_scheduler_plan();
    const generation_result generated = render_generation(plan, false);
    constexpr std::array expected_heads{
        std::pair{
            std::string_view{"process_scanner_pg_row"},
            std::string_view{
                "68454dfb1fe062a0118f5e7c8971b0b55353f90fd1b0a0ce27f478a6ff17ffa1"}},
        std::pair{
            std::string_view{"process_inmemory_row"},
            std::string_view{
                "43b53b927c18e6babcfdf1eab0410e8ad05c68abfa74c0c059460218a2bd9171"}},
        std::pair{
            std::string_view{"process_inmemory_standard_row"},
            std::string_view{
                "8eb5d9bc26210ecf988673aff5497212be4bcf023a36e5517207e8ae3a92d3a2"}},
        std::pair{
            std::string_view{"process_inmemory_segmented_row"},
            std::string_view{
                "38061a0c1af4606936b57db30d7dc27745bd41b293052396e2a97db13dbd10fb"}},
        std::pair{
            std::string_view{"process_inmemory_chunked_row"},
            std::string_view{
                "b81b935da9833ff2a6c01051c12fd37930e0fba9afcb4cc06fcb19ee109aea90"}},
        std::pair{
            std::string_view{"process_inmemory_chunked_segmented_row"},
            std::string_view{
                "76247923be9a2555d59d5ae448cdf6c62221c704a3e088baafe90659ebd60a08"}},
        std::pair{
            std::string_view{"process_sequences"},
            std::string_view{
                "2b552fda94dd9d45e5ea5892fed96972aec6b5976e5e2512c57e4eaed163de7d"}},
    };
    require(
        generated.artifacts.size() == 66u,
        "full scheduler rendering must own API, selector, eight probes, and 56 units");

    const materialization_artifact* api = nullptr;
    const materialization_artifact* selector = nullptr;
    std::map<std::uint32_t, const materialization_artifact*> implementations;
    std::size_t probe_count = 0u;
    std::set<std::uint32_t> probe_representatives;
    for (const materialization_artifact& artifact : generated.artifacts) {
        if (artifact.artifact_kind
            == materialization_artifact::kind::api_partition) {
            require(api == nullptr, "scheduler rendering repeated its API partition");
            api = &artifact;
        } else if (artifact.artifact_kind
                   == materialization_artifact::kind::selector_partition) {
            require(
                selector == nullptr,
                "scheduler rendering repeated its selector partition");
            selector = &artifact;
        } else if (artifact.artifact_kind
                   == materialization_artifact::kind::implementation_unit) {
            require(
                artifact.logical_coordinate_ordinals.size() == 1u
                    && implementations.emplace(
                           artifact.logical_coordinate_ordinals.front(),
                           &artifact).second,
                "scheduler implementation ownership is not one coordinate per unit");
        } else if (artifact.artifact_kind
                   == materialization_artifact::kind::declaration_probe) {
            ++probe_count;
            require(
                artifact.logical_coordinate_ordinals.size() == 7u
                    && probe_representatives.insert(
                           artifact.logical_coordinate_ordinals.front()).second
                    && !artifact.content.contains(
                        "same_dispatch_target_locator"),
                "identity probe must cover seven shards without the support island");
        }
    }
    require(
        api != nullptr && selector != nullptr
            && implementations.size() == 56u && probe_count == 8u
            && probe_representatives
                == std::set<std::uint32_t>{
                    0u, 7u, 14u, 21u, 28u, 35u, 42u, 49u},
        "scheduler API/probe/implementation cardinalities drifted");
    require(
        !api->content.contains(":cpu_preparation_support")
            && !selector->content.contains(":cpu_preparation_support"),
        "scheduler preparation support must remain private to body-bearing units");

    const structural_analysis::translation_unit api_unit =
        structural_analysis::scan_token_slice(api->relative_path, api->content);
    const materialization_coordinate& representative =
        plan.topology.coordinates.front();
    for (const auto& [name, expected_digest] : expected_heads) {
        const auto source_entry = std::ranges::find(
            plan.topology.region.controlled_declarations,
            name,
            [](const owned_declaration& declaration) {
                return declaration.declaration.name;
            });
        require(
            source_entry
                != plan.topology.region.controlled_declarations.end(),
            "scheduler declaration-head golden entry is missing");
        const std::string source_head = render_entry_head(
            plan, *source_entry, representative, false);
        require(
            normalized_token_digest(source_head) == expected_digest,
            "authored scheduler declaration-head digest drifted");

        std::set<std::string, std::less<>> api_qualified_names;
        for (const structural_analysis::declaration_record& declaration :
             api_unit.declarations) {
            if (declaration.name != name) continue;
            require(
                api_qualified_names.insert(declaration.qualified_name).second,
                "generated API repeated a qualified entry declaration");
            require(
                declaration_head_digest(api->content, declaration)
                    == expected_digest,
                "generated API declaration-head digest drifted");
        }
        std::set<std::string, std::less<>> expected_qualified_names;
        for (const std::string_view identity : scheduler_identity_namespaces) {
            expected_qualified_names.emplace(
                std::string{identity} + "::" + std::string{name});
        }
        require(
            api_qualified_names == expected_qualified_names,
            "generated API must own each exact entry head in all eight identities");
    }

    constexpr std::array<std::size_t, 7u> head_index_by_shape{
        6u, 0u, 1u, 2u, 3u, 4u, 5u};
    for (const materialization_coordinate& coordinate :
         plan.topology.coordinates) {
        const std::size_t identity_index = coordinate.ordinal / 7u;
        const std::size_t shape_index = coordinate.ordinal % 7u;
        const std::string_view identity =
            scheduler_identity_namespaces.at(identity_index);
        const auto& [name, expected_digest] =
            expected_heads.at(head_index_by_shape.at(shape_index));
        const materialization_artifact& implementation =
            *implementations.at(coordinate.ordinal);
        require(
            occurrences(
                implementation.content,
                "import :cpu_preparation_support;") == 1u
                && occurrences(
                    implementation.content,
                    "namespace " + std::string{identity} + " {")
                    == 1u,
            "each scheduler implementation must import preparation support and own its exact identity namespace");
        const structural_analysis::translation_unit implementation_unit =
            structural_analysis::scan_token_slice(
                implementation.relative_path, implementation.content);
        std::size_t definition_matches = 0u;
        for (const structural_analysis::declaration_record& declaration :
             implementation_unit.declarations) {
            if (declaration.name != name) continue;
            ++definition_matches;
            require(
                declaration_head_digest(implementation.content, declaration)
                        == expected_digest
                    && declaration.qualified_name
                        == std::string{identity} + "::" + std::string{name},
                "generated implementation declaration-head digest drifted");
        }
        require(
            definition_matches == 1u,
            "generated shard must own exactly one selected entry definition");
    }

    require(
        occurrences(selector->content, "\n    switch (alphabet_code)") == 1u
            && occurrences(
                   selector->content,
                   "\n            switch (storage_unit_byte_width)")
                == 2u
            && occurrences(selector->content, "\n        case 0: {") == 1u
            && occurrences(selector->content, "\n        case 1: {") == 1u,
        "scheduler selector must route the exact two-by-four identity switch");
    constexpr std::array<std::string_view, 4u> width_case_spellings{
        "1", "2", "4", "8"};
    std::array<std::size_t, scheduler_identity_namespaces.size()>
        route_positions{};
    for (std::size_t identity_index = 0u;
         identity_index < scheduler_identity_namespaces.size();
         ++identity_index) {
        const std::string route =
            "\n                case "
            + std::string{width_case_spellings[identity_index % 4u]}
            + ": {\n                    return ::"
            + std::string{scheduler_identity_namespaces[identity_index]}
            + "::process_sequences(";
        require(
            occurrences(selector->content, route) == 1u,
            "scheduler selector must bind every width case to its exact identity");
        route_positions[identity_index] = selector->content.find(route);
    }
    const std::size_t alphabet_dna4 =
        selector->content.find("\n        case 0: {");
    const std::size_t alphabet_dna5 =
        selector->content.find("\n        case 1: {");
    require(
        alphabet_dna4 < route_positions[0]
            && route_positions[0] < route_positions[1]
            && route_positions[1] < route_positions[2]
            && route_positions[2] < route_positions[3]
            && route_positions[3] < alphabet_dna5
            && alphabet_dna5 < route_positions[4]
            && route_positions[4] < route_positions[5]
            && route_positions[5] < route_positions[6]
            && route_positions[6] < route_positions[7],
        "scheduler selector must preserve the exact alphabet/width route order");
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

inline void verify_coordinate_axis_domains() {
    const materialization_coordinate explicit_coordinate{
        .assignments = {
            axis_case_assignment{
                .group = "alphabet",
                .role = axis_role::identity,
                .case_label = "dna4",
            },
            axis_case_assignment{
                .group = "shape",
                .role = axis_role::shard,
                .case_label = "root",
            },
        },
    };
    validate_coordinate_axis_domain(explicit_coordinate);

    const materialization_coordinate external_coordinate{
        .ordinal = 1u,
        .assignments = {
            axis_case_assignment{
                .group = "highway_target",
                .role = axis_role::external,
                .case_label = "HWY_SSE2",
                .case_ordinal = 1u,
            },
        },
    };
    validate_coordinate_axis_domain(external_coordinate);

    materialization_coordinate mixed = explicit_coordinate;
    mixed.assignments.push_back(external_coordinate.assignments.front());
    require_rejected(
        [&] { validate_coordinate_axis_domain(mixed); },
        "a coordinate cannot mix explicit axes with an external provider");
    require_rejected(
        [] { validate_coordinate_axis_domain(materialization_coordinate{}); },
        "a coordinate must name one axis domain");
}

inline void verify_record_probe_provenance_placement() {
    constexpr std::string_view source =
        "namespace probe { struct scalar_type; }";
    const structural_analysis::translation_unit sliced =
        structural_analysis::scan_token_slice(
            "record_probe_contract.cppm", source);
    require(
        sliced.declarations.size() == 1u
            && sliced.declarations.front().kind
                == structural_analysis::declaration_kind::record,
        "record probe fixture has one structural record declaration");
    generation_plan plan{.source_text = std::string{source}};
    owned_declaration entry{
        .declaration = sliced.declarations.front(),
    };
    const materialization_coordinate coordinate{.ordinal = 7u};
    const std::string rendered = render_entry_head(
        plan, entry, coordinate, true);
    const std::string expected =
        "struct [[bioseq::template_flatten_provenance(\""
        + entry.declaration.source_identity
        + "\", \"7\")]] scalar_type";
    require(
        rendered == expected && !rendered.starts_with("[["),
        "record declaration probes place provenance after the record "
        "keyword rather than before the declaration");
}

inline void verify_alias_probe_provenance_placement() {
    constexpr std::string_view source =
        "namespace probe { using scalar_type = int; }";
    const structural_analysis::translation_unit sliced =
        structural_analysis::scan_token_slice(
            "alias_probe_contract.cppm", source);
    require(
        sliced.declarations.size() == 1u
            && sliced.declarations.front().kind
                == structural_analysis::declaration_kind::alias,
        "alias probe fixture has one structural alias declaration");
    generation_plan plan{.source_text = std::string{source}};
    binding_record binding{
        .declaration = sliced.declarations.front(),
        .role = axis_role::identity,
        .group = "abi",
        .cases = {replacement_case{
            .label = "selected",
            .spelling = "long",
            .ordinal = 0u,
        }},
    };
    const materialization_coordinate coordinate{
        .ordinal = 7u,
        .assignments = {axis_case_assignment{
            .group = "abi",
            .role = axis_role::identity,
            .case_label = "selected",
            .case_ordinal = 0u,
        }},
    };
    const std::string rendered = render_binding_declaration(
        plan, binding, coordinate, true);
    const std::string expected =
        "using scalar_type [[bioseq::template_flatten_provenance(\""
        + binding.declaration.source_identity
        + "\", \"7\")]] = long;\n";
    require(
        rendered == expected && !rendered.starts_with("[["),
        "alias declaration probes place provenance after the alias name");
}

inline void verify_generated_topology() {
    const generation_result generated = render_generation(fixture_plan(), false);
    require(
        generated.artifacts.size() == 8u,
        "two identities/two shards render API, selector, two probes, and four units");
    require(
        generated.topology_manifest_json.contains(
            "\"schema_revision\": 1")
            && generated.topology_manifest_json.contains(
                "\"generator_identity\": "
                "\"bioseq-namespace-materialization\""),
        "topology manifest uses the current schema and generator identity");
    std::size_t probes = 0u;
    std::size_t instances = 0u;
    for (const materialization_artifact& artifact : generated.artifacts) {
        require(
            artifact.relative_path.extension() == ".cppm",
            "every generated C++ artifact is a .cppm");
        require(
            artifact.content_sha256 == bioseq::common::sha256_hex(artifact.content),
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

inline void verify_template_flatten_manifest_evidence() {
    scoped_temp_directory temporary;
    generation_plan plan = fixture_plan();
    plan.repository_root = temporary.path();
    plan.source_path = temporary.path() / "flatten_fixture.cppm";
    plan.topology.region.source_module.source_path = plan.source_path;
    plan.output_root = temporary.path() / "generated";
    attach_fixture_frontend_evidence(plan);

    const generation_result generated = render_generation(plan);
    require(
        generated.manifest_identity.size() == 64u
            && generated.artifact_manifest_json.contains(
                "\"schema_revision\": 3")
            && generated.artifact_manifest_json.contains(
                "\"generator_identity\": "
                "\"bioseq-namespace-materialization\"")
            && generated.artifact_manifest_json.contains(
                "\"canonical_token_slice_scans\":2")
            && generated.artifact_manifest_json.contains(
                "\"signature_donor_token_slice_scans\":3")
            && generated.artifact_manifest_json.contains(
                "\"body_free_declaration_probe_scans\":2")
            && generated.artifact_manifest_json.contains(
                "\"body_bearing_compiler_invocations\":0")
            && generated.artifact_manifest_json.contains(
                "\"body_bearing_implementation_units\":4"),
        "template-flatten manifest must round-trip the observed action ledger "
        "separately from CMake compilation obligations");

    generation_plan false_action = plan;
    false_action.frontend_actions.canonical_token_slice_scans = 1u;
    require_rejected(
        [&] { static_cast<void>(render_generation(false_action)); },
        "template-flatten manifest rejects a fabricated frontend action count");
    false_action = plan;
    false_action.frontend_actions.body_free_declaration_probe_scans = 1u;
    require_rejected(
        [&] { static_cast<void>(render_generation(false_action)); },
        "template-flatten manifest binds probe scans to probe evidence");
    generation_plan duplicate_probe = plan;
    duplicate_probe.declaration_probes.back() =
        duplicate_probe.declaration_probes.front();
    require_rejected(
        [&] { static_cast<void>(render_generation(duplicate_probe)); },
        "template-flatten manifest binds each probe to one exact artifact");
    generation_plan false_obligation = plan;
    false_obligation.compilation_obligations
        .body_bearing_implementation_units = 3u;
    require_rejected(
        [&] { static_cast<void>(render_generation(false_obligation)); },
        "template-flatten manifest rejects a truncated CMake compilation obligation");

    generation_result wrong_validation = render_generation(plan, false);
    const auto probe = std::ranges::find_if(
        wrong_validation.artifacts,
        [](const materialization_artifact& artifact) {
            return artifact.artifact_kind
                == materialization_artifact::kind::declaration_probe;
        });
    require(
        probe != wrong_validation.artifacts.end(),
        "manifest validation fixture requires a declaration probe artifact");
    probe->validation = validation_class::body_bearing;
    wrong_validation.artifacts.push_back(materialization_artifact{
        .artifact_kind = materialization_artifact::kind::manifest,
        .relative_path = "topology.json",
        .validation = validation_class::body_free,
        .content = render_topology_manifest(plan),
    });
    wrong_validation.artifacts.back().content_sha256 =
        bioseq::common::sha256_hex(wrong_validation.artifacts.back().content);
    require_rejected(
        [&] {
            static_cast<void>(render_artifact_manifest(
                plan, wrong_validation.artifacts, std::string_view{}));
        },
        "template-flatten manifest binds every artifact kind to its body class");
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
            .topology_digest = bioseq::common::sha256_hex("fixture topology"),
            .policy_evidence_json =
                R"({"schema_revision":1,"producer":"highway"})",
            .artifacts = {
                publication_artifact{
                    .relative_path = "fixture.cppm",
                    .content = content,
                    .content_sha256 = bioseq::common::sha256_hex(content),
                    .artifact_class =
                        publication_artifact_class::production_cppm,
                },
            },
        });
    require(
        rendered.json.contains(
            "\"publication_schema_revision\": 1")
            && rendered.json.contains("\"policy\": \"highway\"")
            && rendered.json.contains(
                "\"class\":\"production_cppm\"")
            && rendered.json.contains(
                "\"evidence\": {\"schema_revision\":1"),
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
                    .topology_digest = bioseq::common::sha256_hex("fixture topology"),
                    .policy_evidence_json = "[]",
                    .artifacts = {
                        publication_artifact{
                            .relative_path = "fixture.cppm",
                            .content = content,
                            .content_sha256 = bioseq::common::sha256_hex(content),
                        },
                    },
                }));
        },
        "the shared publisher accepts policy evidence only as a JSON object");
}

inline void verify_authenticated_incremental_publication() {
    scoped_temp_directory temporary;
    const std::filesystem::path root = temporary.path();
    const std::filesystem::path manifest = root / "artifacts.json";
    const std::filesystem::path stamp = root / "publication.stamp";
    const std::string topology_digest = bioseq::common::sha256_hex("publisher topology A");
    const std::string replacement_topology_digest =
        bioseq::common::sha256_hex("publisher topology B");
    const std::string keep_content =
        "module bioseq.codegen.publisher_contract.keep;\n";
    const std::string stale_content =
        "module bioseq.codegen.publisher_contract.stale;\n";
    const std::vector<publication_artifact> initial_artifacts{
        publication_artifact{
            .relative_path = "keep.cppm",
            .content = keep_content,
            .content_sha256 = bioseq::common::sha256_hex(keep_content),
        },
        publication_artifact{
            .relative_path = "nested/stale.cppm",
            .content = stale_content,
            .content_sha256 = bioseq::common::sha256_hex(stale_content),
        },
    };
    const auto render_for_topology = [&root](
                                         std::string prior,
                                         std::vector<publication_artifact> artifacts,
                                         std::string topology) {
        return render_publication_manifest(publication_manifest_request{
            .output_root = root,
            .policy = "template_flatten",
            .name = "incremental_contract",
            .topology_digest = std::move(topology),
            .prior_manifest_identity = std::move(prior),
            .policy_evidence_json =
                R"({"schema_revision":1,"producer":"contract"})",
            .artifacts = std::move(artifacts),
        });
    };
    const auto render = [&](std::string prior,
                            std::vector<publication_artifact> artifacts) {
        return render_for_topology(
            std::move(prior), std::move(artifacts), topology_digest);
    };
    const auto publish_at = [&](const rendered_publication_manifest& rendered,
                                std::vector<publication_artifact> artifacts,
                                const std::filesystem::path& publication_stamp) {
        publish_artifacts(publication_request{
            .output_root = root,
            .artifact_manifest = manifest,
            .stamp = publication_stamp,
            .artifact_manifest_json = rendered.json,
            .artifacts = std::move(artifacts),
        });
    };
    const auto publish = [&](const rendered_publication_manifest& rendered,
                             std::vector<publication_artifact> artifacts) {
        publish_at(rendered, std::move(artifacts), stamp);
    };

    const rendered_publication_manifest first = render({}, initial_artifacts);
    publish(first, initial_artifacts);
    const std::filesystem::path keep = root / "keep.cppm";
    const std::filesystem::path stale = root / "nested/stale.cppm";
    publish_artifact_if_changed(root / "unowned.cppm", "unowned\n");
    const auto sentinel = std::filesystem::file_time_type::clock::now()
        - std::chrono::hours{24};
    std::filesystem::last_write_time(keep, sentinel);
    std::filesystem::last_write_time(stale, sentinel);
    std::filesystem::last_write_time(manifest, sentinel);
    const auto keep_mtime = std::filesystem::last_write_time(keep);
    const auto stale_mtime = std::filesystem::last_write_time(stale);
    const auto manifest_mtime = std::filesystem::last_write_time(manifest);
    const std::string first_manifest_bytes = read_artifact_bytes(manifest);

    const previous_publication previous = read_previous_publication(
        manifest, root, "template_flatten", topology_digest);
    const rendered_publication_manifest repeat = render(
        previous.manifest_identity, initial_artifacts);
    require(
        repeat.json != first.json,
        "repeat manifest fixture must exercise predecessor/self identity churn");
    publish(repeat, initial_artifacts);
    require(
        read_artifact_bytes(keep) == keep_content && read_artifact_bytes(stale) == stale_content
            && read_artifact_bytes(manifest) == first_manifest_bytes
            && std::filesystem::last_write_time(keep) == keep_mtime
            && std::filesystem::last_write_time(stale) == stale_mtime
            && std::filesystem::last_write_time(manifest) == manifest_mtime,
        "semantic no-op publication must preserve artifact/manifest bytes and mtimes");
    const rendered_publication_manifest unauthorized_repeat = render(
        bioseq::common::sha256_hex("unauthorized predecessor"), initial_artifacts);
    require_rejected(
        [&] { publish(unauthorized_repeat, initial_artifacts); },
        "semantic no-op publication still requires the authenticated predecessor");
    rendered_publication_manifest tampered_repeat = repeat;
    tampered_repeat.json = replace_once(
        std::move(tampered_repeat.json),
        "\"name\": \"incremental_contract\"",
        "\"name\": \"tampered_contract\"");
    require_rejected(
        [&] { publish(tampered_repeat, initial_artifacts); },
        "publication rejects a byte mutation not covered by its stale self-seal");
    const detail::previous_publication_snapshot authenticated_snapshot =
        detail::read_previous_publication_snapshot(
            manifest, root, "template_flatten", topology_digest);
    publish_artifact_if_changed(manifest, repeat.json);
    require_rejected(
        [&] {
            detail::require_unchanged_previous_publication_snapshot(
                manifest, authenticated_snapshot);
        },
        "publication rejects a newer manifest swapped after authentication");
    publish_artifact_if_changed(manifest, first_manifest_bytes);
    require_rejected(
        [&] { publish_at(repeat, initial_artifacts, manifest); },
        "publication stamp cannot alias the artifacts manifest");
    require_rejected(
        [&] {
            publish_at(
                repeat,
                initial_artifacts,
                std::filesystem::path{manifest.string() + ".tmp"});
        },
        "publication stamp cannot alias the artifacts manifest temporary path");
    require_rejected(
        [&] {
            publish_at(
                repeat,
                initial_artifacts,
                std::filesystem::path{manifest.string()
                                      + ".transaction/publication.stamp"});
        },
        "publication stamp cannot nest beneath a protocol-owned file path");
    require_rejected(
        [&] { publish_at(repeat, initial_artifacts, keep); },
        "publication stamp cannot alias a desired artifact");
    require_rejected(
        [&] {
            publish_at(
                repeat,
                initial_artifacts,
                std::filesystem::path{keep.string() + ".tmp"});
        },
        "publication stamp cannot alias a desired artifact temporary path");

    const std::vector<publication_artifact> shrunk_artifacts{
        initial_artifacts.front()};
    const rendered_publication_manifest shrunk = render_for_topology(
        previous.manifest_identity,
        shrunk_artifacts,
        replacement_topology_digest);
    const rendered_publication_manifest empty_cross_topology =
        render_for_topology(
            {}, shrunk_artifacts, replacement_topology_digest);
    const rendered_publication_manifest unauthorized_cross_topology =
        render_for_topology(
            bioseq::common::sha256_hex("unauthorized cross-topology predecessor"),
            shrunk_artifacts,
            replacement_topology_digest);
    require_rejected(
        [&] { publish(empty_cross_topology, shrunk_artifacts); },
        "cross-topology cleanup requires a non-empty predecessor identity");
    require_rejected(
        [&] { publish(unauthorized_cross_topology, shrunk_artifacts); },
        "cross-topology cleanup requires the exact predecessor identity");
    require_rejected(
        [&] { publish_at(shrunk, shrunk_artifacts, stale); },
        "publication stamp cannot alias an authenticated stale artifact");
    require_rejected(
        [&] {
            publish_at(
                shrunk,
                shrunk_artifacts,
                std::filesystem::path{stale.string() + ".tmp"});
        },
        "publication stamp cannot alias an authenticated stale artifact "
        "temporary path");
    require(
        read_artifact_bytes(manifest) == first_manifest_bytes
            && read_artifact_bytes(stale) == stale_content,
        "rejected cross-topology and stamp aliases preserve prior ownership");
    publish_artifact_if_changed(stale, "locally modified stale artifact\n");
    require_rejected(
        [&] { publish(shrunk, shrunk_artifacts); },
        "modified stale artifacts cannot be deleted by manifest ownership alone");
    require(
        std::filesystem::is_regular_file(stale),
        "rejected stale cleanup must retain the modified file");
    publish_artifact_if_changed(stale, stale_content);
    publish(shrunk, shrunk_artifacts);
    const previous_publication transitioned = read_previous_publication(
        manifest,
        root,
        "template_flatten",
        replacement_topology_digest);
    require(
        !std::filesystem::exists(stale)
            && std::filesystem::is_regular_file(keep)
            && std::filesystem::is_regular_file(root / "unowned.cppm")
            && transitioned.present
            && transitioned.manifest_identity == shrunk.manifest_identity
            && read_artifact_bytes(manifest) == shrunk.json,
        "authenticated cross-topology shrink removes only the prior stale "
        "artifact and publishes the new self-sealed ownership");
    require_rejected(
        [&] {
            static_cast<void>(render_publication_manifest(
                publication_manifest_request{
                    .output_root = root,
                    .policy = "template_flatten",
                    .name = "incremental_contract",
                    .topology_digest = topology_digest,
                    .policy_evidence_json = R"({"schema_revision":1})",
                    .artifacts = {
                        publication_artifact{
                            .relative_path = "../escape.cppm",
                            .content = "escape\n",
                            .content_sha256 = bioseq::common::sha256_hex("escape\n"),
                        },
                    },
                }));
        },
        "publication ownership rejects traversal outside its exact root");
}

inline void verify_interrupted_publication_recovery() {
    struct fault_case {
        publication_fault_point point{};
        std::size_t ordinal{};
    };
    constexpr std::array faults{
        fault_case{publication_fault_point::preparing_intent_durable, 0u},
        fault_case{publication_fault_point::desired_artifact_staged, 0u},
        fault_case{publication_fault_point::desired_artifact_staged, 1u},
        fault_case{publication_fault_point::ready_intent_durable, 0u},
        fault_case{publication_fault_point::desired_artifact_promoted, 0u},
        fault_case{publication_fault_point::desired_artifact_promoted, 1u},
        fault_case{publication_fault_point::stale_artifact_removed, 0u},
        fault_case{publication_fault_point::manifest_committed, 0u},
    };
    const auto make_artifact = [](std::filesystem::path path,
                                  std::string content) {
        return publication_artifact{
            .relative_path = std::move(path),
            .content = content,
            .content_sha256 = bioseq::common::sha256_hex(content),
        };
    };

    for (const bool changed_retry : {false, true}) {
        for (const fault_case fault : faults) {
            scoped_temp_directory temporary;
            const std::filesystem::path root = temporary.path();
            const std::filesystem::path manifest = root / "artifacts.json";
            const std::filesystem::path stamp = root / "publication.stamp";
            const auto render = [&root](
                                    std::string prior,
                                    std::vector<publication_artifact> artifacts,
                                    std::string_view topology) {
                return render_publication_manifest(publication_manifest_request{
                    .output_root = root,
                    .policy = "template_flatten",
                    .name = "interruption_contract",
                    .topology_digest = bioseq::common::sha256_hex(topology),
                    .prior_manifest_identity = std::move(prior),
                    .policy_evidence_json =
                        R"({"schema_revision":1,"producer":"interruption-contract"})",
                    .artifacts = std::move(artifacts),
                });
            };
            const auto publish = [&](const rendered_publication_manifest& rendered,
                                     std::vector<publication_artifact> artifacts) {
                publish_artifacts(publication_request{
                    .output_root = root,
                    .artifact_manifest = manifest,
                    .stamp = stamp,
                    .artifact_manifest_json = rendered.json,
                    .artifacts = std::move(artifacts),
                });
            };

            const std::vector<publication_artifact> initial{
                make_artifact("keep.cppm", "module interruption.keep;\n"),
                make_artifact("replace.cppm", "module interruption.old;\n"),
                make_artifact(
                    "nested/stale.cppm", "module interruption.stale;\n"),
            };
            const rendered_publication_manifest initial_manifest =
                render({}, initial, "interruption topology 0");
            publish(initial_manifest, initial);
            const auto sentinel = std::filesystem::file_time_type::clock::now()
                - std::chrono::hours{24};
            std::filesystem::last_write_time(root / "keep.cppm", sentinel);

            const previous_publication previous = read_previous_publication(
                manifest, root, "template_flatten", {});
            const std::vector<publication_artifact> transition{
                make_artifact("keep.cppm", "module interruption.keep;\n"),
                make_artifact("replace.cppm", "module interruption.new;\n"),
                make_artifact("nested/added.cppm", "module interruption.added;\n"),
            };
            const rendered_publication_manifest transition_manifest = render(
                previous.manifest_identity,
                transition,
                "interruption topology 1");
            publication_fault_for_testing = publication_fault_injection{
                .point = fault.point,
                .ordinal = fault.ordinal,
            };
            require_rejected(
                [&] { publish(transition_manifest, transition); },
                "every durable publication boundary must be interruptible");
            publication_fault_for_testing.reset();

            recover_interrupted_publication(
                root, manifest, stamp, "template_flatten");
            const previous_publication recovered = read_previous_publication(
                manifest, root, "template_flatten", {});
            const std::vector<publication_artifact> final_artifacts =
                changed_retry
                ? std::vector<publication_artifact>{
                      make_artifact(
                          "keep.cppm", "module interruption.keep;\n"),
                      make_artifact(
                          "replace.cppm", "module interruption.newer;\n"),
                      make_artifact(
                          "final.cppm", "module interruption.final;\n"),
                  }
                : transition;
            const rendered_publication_manifest final_manifest = render(
                recovered.manifest_identity,
                final_artifacts,
                changed_retry ? "interruption topology 2"
                              : "interruption topology 1");
            publish(final_manifest, final_artifacts);

            require(
                read_artifact_bytes(root / "keep.cppm")
                        == "module interruption.keep;\n"
                    && std::filesystem::last_write_time(root / "keep.cppm")
                        == sentinel
                    && read_artifact_bytes(root / "replace.cppm")
                        == (changed_retry ? "module interruption.newer;\n"
                                          : "module interruption.new;\n")
                    && std::filesystem::is_regular_file(
                        root / (changed_retry ? "final.cppm"
                                              : "nested/added.cppm"))
                    && !std::filesystem::exists(root / "nested/stale.cppm")
                    && (!changed_retry
                        || !std::filesystem::exists(root / "nested/added.cppm"))
                    && std::filesystem::is_regular_file(stamp),
                "same-input and changed-input retries converge without rewriting unchanged artifacts");
            require(
                read_artifact_bytes(manifest)
                    == (changed_retry ? final_manifest.json
                                      : transition_manifest.json),
                "interruption recovery preserves one exact manifest identity chain");
            for (const std::filesystem::path& path : std::array{
                     std::filesystem::path{manifest.string() + ".transaction"},
                     std::filesystem::path{manifest.string() + ".transaction.tmp"},
                     std::filesystem::path{manifest.string() + ".ready"},
                     std::filesystem::path{(root / "replace.cppm").string()
                                           + ".tmp"},
                     std::filesystem::path{(root / "nested/added.cppm").string()
                                           + ".tmp"},
                     std::filesystem::path{(root / "final.cppm").string()
                                           + ".tmp"},
                 }) {
                require(
                    !std::filesystem::exists(path),
                    "successful retry removes every protocol path");
            }
        }
    }

    scoped_temp_directory temporary;
    const std::filesystem::path root = temporary.path();
    const std::filesystem::path manifest = root / "artifacts.json";
    const std::filesystem::path stamp = root / "publication.stamp";
    const std::vector<publication_artifact> initial{
        make_artifact("replace.cppm", "module interruption.old;\n"),
    };
    const auto render = [&root](
                            std::string prior,
                            std::vector<publication_artifact> artifacts) {
        return render_publication_manifest(publication_manifest_request{
            .output_root = root,
            .policy = "template_flatten",
            .name = "interruption_adversarial_contract",
            .topology_digest = bioseq::common::sha256_hex("interruption adversarial topology"),
            .prior_manifest_identity = std::move(prior),
            .policy_evidence_json = R"({"schema_revision":1})",
            .artifacts = std::move(artifacts),
        });
    };
    const auto publish = [&](const rendered_publication_manifest& rendered,
                             std::vector<publication_artifact> artifacts) {
        publish_artifacts(publication_request{
            .output_root = root,
            .artifact_manifest = manifest,
            .stamp = stamp,
            .artifact_manifest_json = rendered.json,
            .artifacts = std::move(artifacts),
        });
    };
    const rendered_publication_manifest first = render({}, initial);
    publish(first, initial);
    const previous_publication previous = read_previous_publication(
        manifest, root, "template_flatten", {});
    const std::vector<publication_artifact> replacement{
        make_artifact("replace.cppm", "module interruption.new;\n"),
    };
    const rendered_publication_manifest second = render(
        previous.manifest_identity, replacement);
    publication_fault_for_testing = publication_fault_injection{
        .point = publication_fault_point::ready_intent_durable,
    };
    require_rejected(
        [&] { publish(second, replacement); },
        "adversarial fixture must stop with an authenticated READY intent");
    publication_fault_for_testing.reset();
    publish_artifact_if_changed(
        std::filesystem::path{(root / "replace.cppm").string() + ".tmp"},
        "tampered staged bytes\n");
    require_rejected(
        [&] {
            recover_interrupted_publication(
                root, manifest, stamp, "template_flatten");
        },
        "READY recovery rejects staged bytes outside the authenticated digest");
    require(
        read_artifact_bytes(manifest) == first.json
            && read_artifact_bytes(root / "replace.cppm")
                == "module interruption.old;\n",
        "rejected READY recovery preserves the authenticated live publication");
}

inline void verify_preparation_action_trace_stamp_binding() {
    const std::string action_trace{
        "{\"schema\":\"fixture.prepare_actions\","
        "\"schema_revision\":1}\n"};
    const std::string stamp =
        "bioseq_template_flatten_prepare_stamp\n"
        "revision=1\n"
        "profile=production\n"
        "prepare_action_trace_sha256="
        + bioseq::common::sha256_hex(action_trace) + "\n";
    validate_frontend_action_trace_stamp_binding(
        stamp, action_trace);
    require_rejected(
        [&] {
            validate_frontend_action_trace_stamp_binding(
                stamp, action_trace + " ");
        },
        "PREPARE stamp accepts independently substituted action-trace bytes");
    require_rejected(
        [&] {
            validate_frontend_action_trace_stamp_binding(
                replace_once(
                    stamp,
                    bioseq::common::sha256_hex(action_trace),
                    std::string(64u, '0')),
                action_trace);
        },
        "PREPARE stamp accepts a stale action-trace digest");
    require_rejected(
        [&] {
            validate_frontend_action_trace_stamp_binding(
                stamp + "prepare_action_trace_sha256="
                    + bioseq::common::sha256_hex(action_trace) + "\n",
                action_trace);
        },
        "PREPARE stamp accepts duplicate action-trace bindings");
}

#if defined(BIOSEQ_PREPARE_TEST_CLANG_BINARY) \
    && defined(BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS) \
    && defined(BIOSEQ_PREPARE_TEST_LLVM_CONFIG) \
    && defined(BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY) \
    && defined(BIOSEQ_PREPARE_TEST_CLANG_SHA256) \
    && defined(BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS_SHA256) \
    && defined(BIOSEQ_PREPARE_TEST_LLVM_CONFIG_SHA256) \
    && defined(BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY_SHA256)

struct prepare_transaction_fixture {
    scoped_temp_directory temporary;
    std::filesystem::path root{temporary.path()};
    std::filesystem::path canonical{root / "canonical.cppm"};
    std::filesystem::path donor{root / "donor.cppm"};
    std::filesystem::path provider{root / "provider.cppm"};
    std::filesystem::path header{root / "provider.hpp"};
    std::filesystem::path response{root / "provider.rsp"};
    std::filesystem::path raw_database{
        root / "compile_commands.json"};
    std::filesystem::path physical_pcm_root{
        root / "pcm-authority"};
    std::filesystem::path alternate_pcm_root{
        root / "pcm-retarget"};
    std::filesystem::path configured_pcm_root{root / "pcm"};
    prepare_command_line command;

    prepare_transaction_fixture() {
        publish_artifact_if_changed(
            canonical,
            "export module fixture.prepare_transaction.canonical;\n"
            "import fixture.prepare_transaction.provider;\n"
            "namespace fixture::prepare_transaction {\n"
            "inline int canonical_body_must_not_reach_sema() {\n"
            "  static_assert(false, \"canonical body reached Sema\");\n"
            "  return 0;\n"
            "}\n"
            "}\n");
        publish_artifact_if_changed(
            donor,
            "export module fixture.prepare_transaction.donor;\n"
            "export int prepare_transaction_donor();\n");
        publish_artifact_if_changed(
            header,
            "#pragma once\n"
            "#ifndef PREPARE_TRANSACTION_RESPONSE_VALUE\n"
            "#error provider response file was not expanded\n"
            "#endif\n"
            "inline constexpr int prepare_transaction_header_value =\n"
            "    PREPARE_TRANSACTION_RESPONSE_VALUE;\n");
        publish_artifact_if_changed(
            response,
            "-DPREPARE_TRANSACTION_RESPONSE_VALUE=17\n");
        publish_artifact_if_changed(
            provider,
            "module;\n"
            "#include \"provider.hpp\"\n"
            "export module fixture.prepare_transaction.provider;\n"
            "export inline int prepare_transaction_provider() {\n"
            "  return prepare_transaction_header_value;\n"
            "}\n");
        std::filesystem::create_directory(physical_pcm_root);
        std::filesystem::create_directory(alternate_pcm_root);
        std::filesystem::create_directory_symlink(
            physical_pcm_root, configured_pcm_root);
        const std::filesystem::path clang{
            BIOSEQ_PREPARE_TEST_CLANG_BINARY};
        const auto arguments = [&](
                                   const std::filesystem::path& source,
                                   bool use_response = false) {
            std::vector<std::string> result{
                clang.string(),
                "-std=c++26",
                "-x",
                "c++-module",
            };
            if (use_response) {
                result.push_back("@" + response.string());
            }
            result.push_back(source.string());
            return result;
        };
        const auto render_command = [&](
                                        std::ostringstream& output,
                                        const std::filesystem::path& source,
                                        bool use_response,
                                        bool first) {
            output << (first ? "" : ",\n")
                   << "  {\n    \"directory\": \""
                   << json_escape(root.string())
                   << "\",\n    \"file\": \""
                   << json_escape(source.string())
                   << "\",\n    \"arguments\": [";
            const std::vector<std::string> command_arguments =
                arguments(source, use_response);
            for (std::size_t index = 0;
                 index < command_arguments.size();
                 ++index) {
                output << (index == 0 ? "" : ", ")
                       << "\"" << json_escape(command_arguments[index])
                       << "\"";
            }
            output << "]\n  }";
        };
        std::ostringstream database;
        database << "[\n";
        render_command(database, canonical, false, true);
        render_command(database, donor, false, false);
        render_command(database, provider, true, false);
        database << "\n]\n";
        publish_artifact_if_changed(raw_database, database.str());
        const std::filesystem::path metadata =
            configured_pcm_root / "metadata";
        command = prepare_command_line{
            .source = canonical,
            .signature_source = donor,
            .raw_compile_database = raw_database,
            .clang_binary = clang,
            .clang_scan_deps =
                BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS,
            .llvm_config = BIOSEQ_PREPARE_TEST_LLVM_CONFIG,
            .compiler_module_inventory =
                BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY,
            .clang_sha256 = BIOSEQ_PREPARE_TEST_CLANG_SHA256,
            .clang_scan_deps_sha256 =
                BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS_SHA256,
            .llvm_config_sha256 =
                BIOSEQ_PREPARE_TEST_LLVM_CONFIG_SHA256,
            .compiler_module_inventory_sha256 =
                BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY_SHA256,
            .pcm_root = configured_pcm_root,
            .compile_database_output =
                metadata / "prepare_compile_commands.json",
            .pcm_manifest = metadata / "pcm_manifest.json",
            .action_trace = metadata / "prepare_actions.json",
            .depfile = metadata / "prepare.d",
            .stamp = metadata / "prepare.stamp",
        };
    }

    [[nodiscard]] std::filesystem::path canonical_stamp() const {
        return physical_pcm_root / "metadata/prepare.stamp";
    }
};

struct preserved_prepare_file {
    std::filesystem::path path;
    std::string content;
    std::filesystem::file_time_type mtime;

    explicit preserved_prepare_file(std::filesystem::path file)
        : path(std::move(file)),
          content(read_artifact_bytes(path)),
          mtime(std::filesystem::last_write_time(path)) {}

    void mutate(std::string_view suffix) const {
        publish_artifact_if_changed(path, content + std::string{suffix});
        std::filesystem::last_write_time(path, mtime);
    }

    void restore() const {
        publish_artifact_if_changed(path, content);
        std::filesystem::last_write_time(path, mtime);
    }
};

class scoped_prepare_checkpoint final {
  public:
    explicit scoped_prepare_checkpoint(
        prepare_publication_checkpoint_callback callback) {
        set_prepare_publication_test_checkpoint(
            std::move(callback));
    }

    scoped_prepare_checkpoint(const scoped_prepare_checkpoint&) = delete;
    scoped_prepare_checkpoint& operator=(
        const scoped_prepare_checkpoint&) = delete;

    ~scoped_prepare_checkpoint() {
        clear_prepare_publication_test_checkpoint();
    }
};

inline void verify_prepare_authority_revalidation_transaction() {
    prepare_transaction_fixture fixture;
    const auto require_failed_without_stamp =
        [&](std::string_view contract) {
            require(
                !std::filesystem::exists(fixture.canonical_stamp()),
                std::string{contract}
                    + " left a PREPARE completion stamp after failure");
        };
    const auto require_clean_retry = [&](std::string_view contract) {
        require(
            execute_prepare_contract(fixture.command) == 0
                && std::filesystem::is_regular_file(
                    fixture.canonical_stamp()),
            std::string{contract}
                + " did not converge on a clean stamped retry");
    };
    const auto verify_file_mutation = [&]<typename p_mutator>(
                                          const std::filesystem::path& path,
                                          prepare_publication_checkpoint point,
                                          std::string_view contract,
                                          p_mutator&& mutator) {
        preserved_prepare_file preserved{path};
        bool fired = false;
        std::string rejection_diagnostic;
        {
            scoped_prepare_checkpoint checkpoint{
                [&](const prepare_publication_checkpoint actual) {
                    if (actual != point || std::exchange(fired, true)) {
                        return;
                    }
                    std::invoke(mutator, preserved);
                }};
            try {
                static_cast<void>(
                    execute_prepare_contract(fixture.command));
            } catch (const std::runtime_error& error) {
                rejection_diagnostic = error.what();
            }
            require(
                !rejection_diagnostic.empty(),
                std::string{contract} + " mutation was not rejected");
        }
        require(
            fired,
            std::string{contract} + " checkpoint did not fire; earlier "
                "PREPARE failure: " + rejection_diagnostic);
        require_failed_without_stamp(contract);
        preserved.restore();
        require_clean_retry(contract);
    };

    verify_file_mutation(
        fixture.canonical,
        prepare_publication_checkpoint::after_environment_built,
        "canonical-source authority mutation",
        [](const preserved_prepare_file& file) {
            file.mutate("// canonical authority mutation\n");
        });
    verify_file_mutation(
        fixture.raw_database,
        prepare_publication_checkpoint::after_environment_built,
        "raw compile-database authority mutation",
        [](const preserved_prepare_file& file) {
            file.mutate(" \n");
        });
    verify_file_mutation(
        fixture.provider,
        prepare_publication_checkpoint::after_db_manifest_publish,
        "provider mutation after DB/manifest publication",
        [](const preserved_prepare_file& file) {
            file.mutate("// provider authority mutation\n");
        });
    verify_file_mutation(
        fixture.response,
        prepare_publication_checkpoint::before_completion_stamp,
        "response-file mutation before completion",
        [](const preserved_prepare_file& file) {
            file.mutate("-DPREPARE_TRANSACTION_UNUSED=1\n");
        });
    verify_file_mutation(
        fixture.header,
        prepare_publication_checkpoint::before_completion_stamp,
        "header mutation before completion",
        [](const preserved_prepare_file& file) {
            file.mutate("// header authority mutation\n");
        });

    bool throw_fired = false;
    {
        scoped_prepare_checkpoint checkpoint{
            [&](const prepare_publication_checkpoint point) {
                if (point
                        == prepare_publication_checkpoint::
                            after_db_manifest_publish
                    && !std::exchange(throw_fired, true)) {
                    throw std::runtime_error(
                        "injected failure after DB/manifest publication");
                }
            }};
        require_rejected(
            [&] {
                static_cast<void>(
                    execute_prepare_contract(fixture.command));
            },
            "injected post-publication failure");
    }
    require(throw_fired, "post-publication failure checkpoint did not fire");
    require_failed_without_stamp("post-publication failure");
    require_clean_retry("post-publication failure");

    bool retarget_fired = false;
    {
        scoped_prepare_checkpoint checkpoint{
            [&](const prepare_publication_checkpoint point) {
                if (point
                        != prepare_publication_checkpoint::
                            after_environment_built
                    || std::exchange(retarget_fired, true)) {
                    return;
                }
                require(
                    std::filesystem::remove(
                        fixture.configured_pcm_root),
                    "root-retarget contract could not remove the entry link");
                std::filesystem::create_directory_symlink(
                    fixture.alternate_pcm_root,
                    fixture.configured_pcm_root);
            }};
        require_rejected(
            [&] {
                static_cast<void>(
                    execute_prepare_contract(fixture.command));
            },
            "PCM-root symlink retarget");
    }
    require(retarget_fired, "PCM-root retarget checkpoint did not fire");
    require_failed_without_stamp("PCM-root symlink retarget");
    require(
        std::filesystem::remove(fixture.configured_pcm_root),
        "root-retarget contract could not remove the alternate link");
    std::filesystem::create_directory_symlink(
        fixture.physical_pcm_root,
        fixture.configured_pcm_root);
    require_clean_retry("PCM-root symlink retarget");
}

#endif

}  // namespace detail

inline void run_namespace_materialization_contract_tests() {
    detail::verify_preparation_action_trace_stamp_binding();
    detail::verify_closed_grammar();
    detail::verify_malformed_materialization_sources();
    detail::verify_zip_product_and_digest();
    detail::verify_configure_structural_digest_mutations();
    detail::verify_scheduler_highway_separation();
    detail::verify_scheduler_profile_policy();
    detail::verify_production_scheduler_golden();
    detail::verify_production_declaration_heads_and_probes();
    detail::verify_exact_edits();
    detail::verify_coordinate_axis_domains();
    detail::verify_record_probe_provenance_placement();
    detail::verify_alias_probe_provenance_placement();
    detail::verify_generated_topology();
    detail::verify_template_flatten_manifest_evidence();
    detail::verify_policy_neutral_publication_envelope();
    detail::verify_authenticated_incremental_publication();
    detail::verify_interrupted_publication_recovery();
#if defined(BIOSEQ_PREPARE_TEST_CLANG_BINARY) \
    && defined(BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS) \
    && defined(BIOSEQ_PREPARE_TEST_LLVM_CONFIG) \
    && defined(BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY) \
    && defined(BIOSEQ_PREPARE_TEST_CLANG_SHA256) \
    && defined(BIOSEQ_PREPARE_TEST_CLANG_SCAN_DEPS_SHA256) \
    && defined(BIOSEQ_PREPARE_TEST_LLVM_CONFIG_SHA256) \
    && defined(BIOSEQ_PREPARE_TEST_COMPILER_INVENTORY_SHA256)
    detail::verify_prepare_authority_revalidation_transaction();
#endif
}

}  // namespace bioseq::codegen::namespace_materialization
