module;

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
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

[[nodiscard]] inline std::string normalized_token_digest(
    std::string_view source) {
    const clang_structural::translation_unit unit =
        clang_structural::scan_token_slice(
            "template_flatten_head_digest.cppm", source);
    std::ostringstream canonical;
    for (const clang_structural::lexical_token& token : unit.tokens) {
        canonical << token.spelling.size() << ':' << token.spelling << '\n';
    }
    require(!unit.tokens.empty(), "normalized token digest requires tokens");
    return sha256_hex(canonical.str());
}

[[nodiscard]] inline std::string declaration_head_digest(
    std::string_view source,
    const clang_structural::declaration_record& declaration) {
    const auto& range = declaration.declaration_head_range;
    require(
        range.end.byte_offset > range.begin.byte_offset
            && range.end.byte_offset <= source.size(),
        "declaration-head digest range is invalid");
    return normalized_token_digest(source.substr(
        range.begin.byte_offset,
        range.end.byte_offset - range.begin.byte_offset));
}

[[nodiscard]] inline std::filesystem::path production_scheduler_source() {
    std::error_code error;
    std::filesystem::path current = std::filesystem::path{__FILE__};
    if (!current.is_absolute()) {
        current = std::filesystem::absolute(current, error);
        if (error) {
            throw std::runtime_error(
                "cannot resolve namespace-materialization contract source path");
        }
    }
    current = std::filesystem::weakly_canonical(current, error);
    if (error) {
        throw std::runtime_error(
            "cannot canonicalize namespace-materialization contract source path");
    }
    const std::filesystem::path main_root =
        current.parent_path().parent_path().parent_path();
    const std::filesystem::path scheduler = main_root
        / "bioseq/backend/pg/scheduler/dispatch_processor_typed_template.cppm";
    if (!std::filesystem::is_regular_file(scheduler, error) || error) {
        throw std::runtime_error(
            "production scheduler materialization source is unavailable: "
            + scheduler.string());
    }
    return std::filesystem::weakly_canonical(scheduler);
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
    std::string profile = "2") {
    const std::filesystem::path source_path = production_scheduler_source();
    const std::string source = read_file(source_path);
    const clang_structural::translation_unit sliced =
        clang_structural::scan_token_slice(source_path, source);
    materialization_region region = extract_materialization_region(
        sliced, "bioseq.scheduler.dispatch_processor", source);
    topology_plan topology = build_topology_plan(std::move(region));
    std::vector<std::uint32_t> enabled = enabled_ordinals(topology, profile);
    validate_enabled_ordinals(topology, profile, enabled);
    validate_entry_ownership(topology);
    return generation_plan{
        .topology = std::move(topology),
        .repository_root = source_path.parent_path().parent_path().parent_path()
            .parent_path().parent_path().parent_path(),
        .source_path = source_path,
        .output_root = std::filesystem::absolute(
            "generated/dispatch_processor_typed_contract").lexically_normal(),
        .source_text = source,
        .source_sha256 = sha256_hex(source),
        .name = "dispatch_processor_typed",
        .profile = std::move(profile),
        .enabled_ordinals = std::move(enabled),
        .clang_version = "contract-test",
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
                   + sha256_hex(seed + ':' + std::to_string(attempt)).substr(
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
    const clang_structural::translation_unit sliced =
        clang_structural::scan_token_slice(
            "flatten_fixture_mutation.cppm", source);
    materialization_region region = extract_materialization_region(
        sliced, "fixture.output", source);
    topology_plan topology = build_topology_plan(std::move(region), 16u);
    validate_entry_ownership(topology);
    std::vector<std::uint32_t> enabled = enabled_ordinals(topology, "2");
    return generation_plan{
        .topology = std::move(topology),
        .repository_root = ".",
        .source_path = "flatten_fixture_mutation.cppm",
        .output_root = "generated/flatten_fixture_mutation",
        .source_text = source,
        .source_sha256 = sha256_hex(source),
        .name = "flatten_fixture_mutation",
        .profile = "2",
        .enabled_ordinals = std::move(enabled),
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
    require_rejected(
        [] {
            clang_structural::attribute_record attribute{
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
                const clang_structural::translation_unit sliced =
                    clang_structural::scan_token_slice(
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
            "516324cb45febb96fda36f7bf10c86e7ac1a8192a0f740e4c0919b0b20085933"},
        std::string_view{
            "bd679dd0fa31bc50d4098202b96d0fcdf8633a65c164b677c245872fa15a194c"},
        std::string_view{
            "18a1c85dfed86035f7866d60d676fe94583a2f4ef7e5bcbcb821c09315267f56"},
        std::string_view{
            "bf45c58a1ab151a5c3e13b9e6bfc74299e58ad5d39d34ce7cb7a723446a2a6db"},
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

inline void verify_production_scheduler_golden() {
    generation_plan plan = production_scheduler_plan();
    require(
        plan.topology.topology_digest
            == "8ee361e05a27e3755b11dd23a51272f8fb3250bbb4047184f13503a4342519d5",
        "production scheduler topology digest drifted from the migration golden");
    validate_configure_topology_digest(
        plan.topology,
        "8ee361e05a27e3755b11dd23a51272f8fb3250bbb4047184f13503a4342519d5");
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
                    }[shape],
            "production scheduler coordinate order drifted");
    }
    require(
        enabled_ordinals(plan.topology, "0")
                == std::vector<std::uint32_t>{
                    21u, 22u, 23u, 24u, 25u, 26u, 27u}
            && enabled_ordinals(plan.topology, "1")
                == std::vector<std::uint32_t>{
                    21u, 22u, 23u, 24u, 25u, 26u, 27u,
                    49u, 50u, 51u, 52u, 53u, 54u, 55u}
            && enabled_ordinals(plan.topology, "2").size() == 56u,
        "production scheduler profile projections must remain 7/14/56");

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
    std::map<std::uint32_t, const materialization_artifact*> implementations;
    std::size_t probe_count = 0u;
    std::set<std::uint32_t> probe_representatives;
    for (const materialization_artifact& artifact : generated.artifacts) {
        if (artifact.artifact_kind
            == materialization_artifact::kind::api_partition) {
            require(api == nullptr, "scheduler rendering repeated its API partition");
            api = &artifact;
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
        api != nullptr && implementations.size() == 56u && probe_count == 8u
            && probe_representatives
                == std::set<std::uint32_t>{
                    0u, 7u, 14u, 21u, 28u, 35u, 42u, 49u},
        "scheduler API/probe/implementation cardinalities drifted");

    const clang_structural::translation_unit api_unit =
        clang_structural::scan_token_slice(api->relative_path, api->content);
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

        std::size_t api_matches = 0u;
        for (const clang_structural::declaration_record& declaration :
             api_unit.declarations) {
            if (declaration.name != name) continue;
            ++api_matches;
            require(
                declaration_head_digest(api->content, declaration)
                    == expected_digest,
                "generated API declaration-head digest drifted");
        }
        require(
            api_matches == 8u,
            "generated API must repeat each exact entry head once per identity");

        const std::uint32_t coordinate =
            source_entry->clauses.front().accepted_case_labels.front() == "root"
            ? 0u
            : source_entry->clauses.front().accepted_case_labels.front()
                    == "scanner_pg"
                ? 1u
                : source_entry->clauses.front().accepted_case_labels.front()
                        == "inmemory"
                    ? 2u
                    : source_entry->clauses.front().accepted_case_labels.front()
                            == "inmemory_standard"
                        ? 3u
                        : source_entry->clauses.front().accepted_case_labels.front()
                                == "inmemory_segmented"
                            ? 4u
                            : source_entry->clauses.front().accepted_case_labels.front()
                                    == "inmemory_chunked"
                                ? 5u
                                : 6u;
        const materialization_artifact& implementation =
            *implementations.at(coordinate);
        const clang_structural::translation_unit implementation_unit =
            clang_structural::scan_token_slice(
                implementation.relative_path, implementation.content);
        std::size_t definition_matches = 0u;
        for (const clang_structural::declaration_record& declaration :
             implementation_unit.declarations) {
            if (declaration.name != name) continue;
            ++definition_matches;
            require(
                declaration_head_digest(implementation.content, declaration)
                    == expected_digest,
                "generated implementation declaration-head digest drifted");
        }
        require(
            definition_matches == 1u,
            "generated shard must own exactly one selected entry definition");
    }
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

inline void verify_record_probe_provenance_placement() {
    constexpr std::string_view source =
        "namespace probe { struct scalar_type; }";
    const clang_structural::translation_unit sliced =
        clang_structural::scan_token_slice(
            "record_probe_contract.cppm", source);
    require(
        sliced.declarations.size() == 1u
            && sliced.declarations.front().kind
                == clang_structural::declaration_kind::record,
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
        + entry.declaration.provenance_id
        + "\", \"7\")]] scalar_type";
    require(
        rendered == expected && !rendered.starts_with("[["),
        "record declaration probes place provenance after the record "
        "keyword rather than before the declaration");
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

inline void verify_authenticated_incremental_publication() {
    scoped_temp_directory temporary;
    const std::filesystem::path root = temporary.path();
    const std::filesystem::path manifest = root / "artifacts.json";
    const std::filesystem::path stamp = root / "publication.stamp";
    const std::string topology_digest = sha256_hex("publisher topology");
    const std::string keep_content =
        "module bioseq.codegen.publisher_contract.keep;\n";
    const std::string stale_content =
        "module bioseq.codegen.publisher_contract.stale;\n";
    const std::vector<publication_artifact> initial_artifacts{
        publication_artifact{
            .relative_path = "keep.cppm",
            .content = keep_content,
            .content_sha256 = sha256_hex(keep_content),
        },
        publication_artifact{
            .relative_path = "nested/stale.cppm",
            .content = stale_content,
            .content_sha256 = sha256_hex(stale_content),
        },
    };
    const auto render = [&](std::string prior,
                            std::vector<publication_artifact> artifacts) {
        return render_publication_manifest(publication_manifest_request{
            .output_root = root,
            .policy = "template_flatten",
            .name = "incremental_contract",
            .topology_digest = topology_digest,
            .prior_manifest_identity = std::move(prior),
            .policy_evidence_json =
                R"({"schema_version":1,"producer":"contract"})",
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

    const rendered_publication_manifest first = render({}, initial_artifacts);
    publish(first, initial_artifacts);
    const std::filesystem::path keep = root / "keep.cppm";
    const std::filesystem::path stale = root / "nested/stale.cppm";
    write_if_different(root / "unowned.cppm", "unowned\n");
    const auto sentinel = std::filesystem::file_time_type::clock::now()
        - std::chrono::hours{24};
    std::filesystem::last_write_time(keep, sentinel);
    std::filesystem::last_write_time(stale, sentinel);
    std::filesystem::last_write_time(manifest, sentinel);
    const auto keep_mtime = std::filesystem::last_write_time(keep);
    const auto stale_mtime = std::filesystem::last_write_time(stale);
    const auto manifest_mtime = std::filesystem::last_write_time(manifest);
    const std::string first_manifest_bytes = read_file(manifest);

    const previous_publication previous = read_previous_publication(
        manifest, root, "template_flatten", topology_digest);
    const rendered_publication_manifest repeat = render(
        previous.manifest_identity, initial_artifacts);
    require(
        repeat.json != first.json,
        "repeat manifest fixture must exercise predecessor/self identity churn");
    publish(repeat, initial_artifacts);
    require(
        read_file(keep) == keep_content && read_file(stale) == stale_content
            && read_file(manifest) == first_manifest_bytes
            && std::filesystem::last_write_time(keep) == keep_mtime
            && std::filesystem::last_write_time(stale) == stale_mtime
            && std::filesystem::last_write_time(manifest) == manifest_mtime,
        "semantic no-op publication must preserve artifact/manifest bytes and mtimes");
    const rendered_publication_manifest unauthorized_repeat = render(
        sha256_hex("unauthorized predecessor"), initial_artifacts);
    require_rejected(
        [&] { publish(unauthorized_repeat, initial_artifacts); },
        "semantic no-op publication still requires the authenticated predecessor");

    const std::vector<publication_artifact> shrunk_artifacts{
        initial_artifacts.front()};
    const rendered_publication_manifest shrunk = render(
        previous.manifest_identity, shrunk_artifacts);
    write_if_different(stale, "locally modified stale artifact\n");
    require_rejected(
        [&] { publish(shrunk, shrunk_artifacts); },
        "modified stale artifacts cannot be deleted by manifest ownership alone");
    require(
        std::filesystem::is_regular_file(stale),
        "rejected stale cleanup must retain the modified file");
    write_if_different(stale, stale_content);
    publish(shrunk, shrunk_artifacts);
    require(
        !std::filesystem::exists(stale)
            && std::filesystem::is_regular_file(keep)
            && std::filesystem::is_regular_file(root / "unowned.cppm"),
        "authenticated shrink removes only the formerly owned stale artifact");
    require_rejected(
        [&] {
            static_cast<void>(render_publication_manifest(
                publication_manifest_request{
                    .output_root = root,
                    .policy = "template_flatten",
                    .name = "incremental_contract",
                    .topology_digest = topology_digest,
                    .policy_evidence_json = R"({"schema_version":1})",
                    .artifacts = {
                        publication_artifact{
                            .relative_path = "../escape.cppm",
                            .content = "escape\n",
                            .content_sha256 = sha256_hex("escape\n"),
                        },
                    },
                }));
        },
        "publication ownership rejects traversal outside its exact root");
}

}  // namespace detail

inline void run_namespace_materialization_contract_tests() {
    detail::verify_closed_grammar();
    detail::verify_malformed_materialization_sources();
    detail::verify_zip_product_and_digest();
    detail::verify_configure_structural_digest_mutations();
    detail::verify_scheduler_profile_policy();
    detail::verify_production_scheduler_golden();
    detail::verify_production_declaration_heads_and_probes();
    detail::verify_exact_edits();
    detail::verify_record_probe_provenance_placement();
    detail::verify_generated_topology();
    detail::verify_policy_neutral_publication_envelope();
    detail::verify_authenticated_incremental_publication();
}

}  // namespace bioseq::codegen::namespace_materialization
