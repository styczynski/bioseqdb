module;

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.highway:manifest;

import :model;
import bioseq.codegen.support;

export namespace bioseq::codegen::highway {

namespace detail {

[[nodiscard]] inline std::vector<std::string> split_fields(
    std::string_view line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const std::size_t separator = line.find('\t', begin);
        fields.emplace_back(line.substr(
            begin,
            separator == std::string_view::npos
                ? line.size() - begin
                : separator - begin));
        if (separator == std::string_view::npos) break;
        begin = separator + 1;
    }
    return fields;
}

[[nodiscard]] inline std::vector<std::vector<std::string>> manifest_lines(
    const std::filesystem::path& path,
    std::string_view expected_header) {
    const std::string document = read_artifact_bytes(path);
    if (document.contains('\0')) {
        throw std::runtime_error(
            path.string() + ": manifest contains a NUL byte");
    }
    std::vector<std::vector<std::string>> records;
    std::size_t begin = 0;
    std::size_t line_number = 1;
    bool saw_header = false;
    while (begin <= document.size()) {
        const std::size_t newline = document.find('\n', begin);
        std::string_view line{document.data() + begin,
            (newline == std::string::npos ? document.size() : newline)
                - begin};
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) {
            if (!saw_header) {
                if (line != expected_header) {
                    throw std::runtime_error(
                        path.string() + ":1: expected manifest header '"
                        + std::string{expected_header} + "'");
                }
                saw_header = true;
            } else if (!line.starts_with('#')) {
                auto fields = split_fields(line);
                if (fields.empty() || fields.front().empty()) {
                    throw std::runtime_error(
                        path.string() + ":" + std::to_string(line_number)
                        + ": empty record kind");
                }
                records.push_back(std::move(fields));
            }
        }
        if (newline == std::string::npos) break;
        begin = newline + 1;
        ++line_number;
    }
    if (!saw_header) {
        throw std::runtime_error(path.string() + ": empty manifest");
    }
    return records;
}

[[nodiscard]] inline std::uint64_t parse_target_bit(
    std::string_view spelling,
    const std::filesystem::path& path) {
    int base = 10;
    if (spelling.starts_with("0x") || spelling.starts_with("0X")) {
        base = 16;
        spelling.remove_prefix(2);
    }
    if (spelling.empty()) {
        throw std::runtime_error(path.string() + ": empty target bit");
    }
    std::uint64_t value{};
    const auto parsed = std::from_chars(
        spelling.data(), spelling.data() + spelling.size(), value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != spelling.data() + spelling.size()
        || value == 0) {
        throw std::runtime_error(
            path.string() + ": invalid positive target bit '"
            + std::string{spelling} + "'");
    }
    return value;
}

[[nodiscard]] inline std::uint32_t parse_ordinal(
    const std::string_view spelling,
    const std::filesystem::path& path,
    const std::string_view record) {
    std::uint32_t value{};
    const auto parsed = std::from_chars(
        spelling.data(), spelling.data() + spelling.size(), value);
    if (parsed.ec != std::errc{}
        || parsed.ptr != spelling.data() + spelling.size()) {
        throw std::runtime_error(
            path.string() + ": invalid " + std::string{record}
            + " ordinal '" + std::string{spelling} + "'");
    }
    return value;
}

inline void require_field_count(
    const std::filesystem::path& path,
    std::string_view record,
    std::size_t actual,
    std::size_t expected);

[[nodiscard]] inline domain_topology_expectation
parse_domain_topology_record(
    const std::vector<std::string>& fields,
    const std::filesystem::path& path,
    const std::size_t expected_ordinal) {
    require_field_count(
        path, "domain-topology", fields.size(), 6u);
    const std::uint32_t ordinal = parse_ordinal(
        fields[1], path, "domain-topology");
    if (ordinal != expected_ordinal) {
        throw std::runtime_error(
            path.string()
            + ": domain-topology ordinals must be contiguous from zero");
    }
    return domain_topology_expectation{
        .name = fields[2],
        .path = std::filesystem::path{fields[3]},
        .topology_digest = fields[4],
        .content_sha256 = fields[5],
    };
}

[[nodiscard]] inline std::vector<std::string> split_dependencies(
    std::string_view value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t separator = value.find(',', begin);
        const std::string_view item = value.substr(
            begin,
            separator == std::string_view::npos
                ? value.size() - begin
                : separator - begin);
        if (item.empty()) {
            throw std::runtime_error(
                "source dependency list contains an empty key");
        }
        result.emplace_back(item);
        if (separator == std::string_view::npos) break;
        begin = separator + 1;
    }
    return result;
}

inline void require_field_count(
    const std::filesystem::path& path,
    std::string_view record,
    std::size_t actual,
    std::size_t expected) {
    if (actual != expected) {
        throw std::runtime_error(
            path.string() + ": '" + std::string{record} + "' requires "
            + std::to_string(expected - 1) + " field(s), found "
            + std::to_string(actual - 1));
    }
}

[[nodiscard]] inline std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    constexpr char digits[] = "0123456789abcdef";
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
                    result += "\\u00";
                    result.push_back(digits[(character >> 4U) & 0x0fU]);
                    result.push_back(digits[character & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    return result;
}

[[nodiscard]] inline std::string_view artifact_kind_name(
    artifact_kind kind) noexcept {
    switch (kind) {
        case artifact_kind::primary: return "primary";
        case artifact_kind::binder: return "binder";
        case artifact_kind::partition: return "partition";
        case artifact_kind::kernel_instantiation:
            return "kernel_instantiation";
        case artifact_kind::dispatch_registry_partition:
            return "dispatch_registry_partition";
        case artifact_kind::dispatch_inventory_implementation:
            return "dispatch_inventory_implementation";
        case artifact_kind::test_support: return "test_support";
        case artifact_kind::unit_probe_implementation:
            return "unit_probe_implementation";
        case artifact_kind::minimizer_representation_instantiation:
            return "minimizer_representation_instantiation";
    }
    return "unknown";
}

}  // namespace detail

[[nodiscard]] inline target_manifest parse_target_manifest(
    const std::filesystem::path& path) {
    target_manifest result;
    bool saw_policy = false;
    bool saw_highway = false;
    bool saw_static_target = false;
    for (const auto& fields : detail::manifest_lines(
             path, "bioseq-highway-targets-v1")) {
        if (fields.front() == "policy") {
            detail::require_field_count(path, fields.front(), fields.size(), 2);
            if (saw_policy) {
                throw std::runtime_error(path.string() + ": duplicate policy record");
            }
            result.policy_digest = std::string{fields[1]};
            saw_policy = true;
        } else if (fields.front() == "policy-definition") {
            detail::require_field_count(path, fields.front(), fields.size(), 3);
            result.policy_definitions.emplace_back(fields[1], fields[2]);
        } else if (fields.front() == "highway") {
            detail::require_field_count(path, fields.front(), fields.size(), 3);
            if (saw_highway) {
                throw std::runtime_error(path.string() + ": duplicate highway record");
            }
            result.highway_version = std::string{fields[1]};
            result.highway_digest = std::string{fields[2]};
            saw_highway = true;
        } else if (fields.front() == "static-target") {
            detail::require_field_count(path, fields.front(), fields.size(), 2);
            if (saw_static_target) {
                throw std::runtime_error(
                    path.string() + ": duplicate static-target record");
            }
            result.static_target_token = std::string{fields[1]};
            saw_static_target = true;
        } else if (fields.front() == "target") {
            detail::require_field_count(path, fields.front(), fields.size(), 6);
            target_spec target{
                .platform = std::string{fields[1]},
                .slug = std::string{fields[2]},
                .token = std::string{fields[3]},
                .target_namespace = std::string{fields[4]},
                .bit = detail::parse_target_bit(fields[5], path),
            };
            result.targets.push_back(std::move(target));
        } else if (fields.front() == "domain-topology") {
            result.domain_topologies.push_back(
                detail::parse_domain_topology_record(
                    fields, path, result.domain_topologies.size()));
        } else {
            throw std::runtime_error(
                path.string() + ": unknown target-manifest record '"
                + std::string{fields.front()} + "'");
        }
    }
    if (!saw_policy || !saw_highway || !saw_static_target
        || result.targets.empty()) {
        throw std::runtime_error(
            path.string()
            + ": target manifest requires policy, highway, static-target, "
              "and target records");
    }
    std::ranges::sort(
        result.policy_definitions,
        {},
        [](const auto& definition) { return definition.first; });
    return result;
}

[[nodiscard]] inline source_manifest parse_source_manifest(
    const std::filesystem::path& path) {
    source_manifest result;
    bool saw_contract = false;
    bool saw_dispatch = false;
    bool saw_completeness = false;
    for (const auto& fields : detail::manifest_lines(
             path, "bioseq-highway-sources-v1")) {
        if (fields.front() == "contract") {
            detail::require_field_count(path, fields.front(), fields.size(), 2);
            if (saw_contract) {
                throw std::runtime_error(path.string() + ": duplicate contract record");
            }
            result.contract_module = std::string{fields[1]};
            saw_contract = true;
        } else if (fields.front() == "dispatch") {
            detail::require_field_count(path, fields.front(), fields.size(), 2);
            if (saw_dispatch) {
                throw std::runtime_error(path.string() + ": duplicate dispatch record");
            }
            result.dispatch_module = std::string{fields[1]};
            saw_dispatch = true;
        } else if (fields.front() == "require-complete-table") {
            detail::require_field_count(path, fields.front(), fields.size(), 2);
            if (saw_completeness || (fields[1] != "0" && fields[1] != "1")) {
                throw std::runtime_error(
                    path.string()
                    + ": require-complete-table must occur once with 0 or 1");
            }
            result.require_complete_table = fields[1] == "1";
            saw_completeness = true;
        } else if (fields.front() == "source") {
            detail::require_field_count(path, fields.front(), fields.size(), 7);
            result.sources.push_back(source_spec{
                .key = std::string{fields[1]},
                .path = std::filesystem::path{fields[2]},
                .module_name = std::string{fields[3]},
                .partition = std::string{fields[4]},
                .family = std::string{fields[5]},
                .dependencies = detail::split_dependencies(fields[6]),
            });
        } else if (fields.front() == "slot") {
            detail::require_field_count(path, fields.front(), fields.size(), 5);
            result.slots.push_back(slot_spec{
                .owner_key = std::string{fields[1]},
                .family_field = std::string{fields[2]},
                .slot_field = std::string{fields[3]},
                .expression = std::string{fields[4]},
            });
        } else if (fields.front() == "required-slot") {
            detail::require_field_count(path, fields.front(), fields.size(), 3);
            result.required_slots.push_back(required_slot_spec{
                .family_field = std::string{fields[1]},
                .slot_field = std::string{fields[2]},
            });
        } else if (fields.front() == "unit-probe") {
            detail::require_field_count(path, fields.front(), fields.size(), 3);
            result.unit_probes.push_back(probe_spec{
                .key = std::string{fields[1]},
                .path = std::filesystem::path{fields[2]},
            });
        } else if (fields.front() == "minimizer-repr-instance") {
            detail::require_field_count(path, fields.front(), fields.size(), 6);
            result.minimizer_representation_instances.push_back(
                minimizer_representation_instance_spec{
                    .owner_key = std::string{fields[1]},
                    .algorithm = std::string{fields[2]},
                    .source_key = std::string{fields[3]},
                    .representation_type = std::string{fields[4]},
                    .storage_types = detail::split_dependencies(fields[5]),
                });
        } else if (fields.front() == "domain-topology") {
            result.domain_topologies.push_back(
                detail::parse_domain_topology_record(
                    fields, path, result.domain_topologies.size()));
        } else {
            throw std::runtime_error(
                path.string() + ": unknown source-manifest record '"
                + std::string{fields.front()} + "'");
        }
    }
    if (!saw_contract || !saw_dispatch || result.sources.empty()) {
        throw std::runtime_error(
            path.string()
            + ": source manifest requires contract, dispatch, and source records");
    }
    std::ranges::sort(
        result.sources,
        {},
        [](const source_spec& source) { return source.key; });
    for (source_spec& source : result.sources) {
        std::ranges::sort(source.dependencies);
    }
    std::ranges::sort(
        result.slots,
        {},
        [](const slot_spec& slot) {
            return slot.family_field + "." + slot.slot_field + ":" + slot.owner_key;
        });
    std::ranges::sort(
        result.required_slots,
        {},
        [](const required_slot_spec& slot) {
            return slot.family_field + "." + slot.slot_field;
        });
    const auto sort_probes = [](std::vector<probe_spec>& probes) {
        std::ranges::sort(
            probes, {}, [](const probe_spec& probe) { return probe.key; });
    };
    sort_probes(result.unit_probes);
    std::ranges::sort(
        result.minimizer_representation_instances,
        {},
        [](const minimizer_representation_instance_spec& instance) {
            return instance.owner_key;
        });
    return result;
}

/// Renders the deterministic Highway-specific evidence object embedded in
/// the shared publication envelope. Ownership and stale cleanup deliberately
/// remain outside this policy schema.
[[nodiscard]] inline std::string render_policy_evidence(
    const generation_plan& plan,
    const std::vector<generated_artifact>& artifacts) {
    std::set<std::string_view> slot_owners;
    for (const slot_spec& slot : plan.source_inventory.slots) {
        slot_owners.insert(slot.owner_key);
    }
    const std::size_t expected_artifact_count =
        plan.target_inventory.targets.size()
            * (2u + plan.extracted_sources.size() + slot_owners.size()
               + plan.source_inventory.minimizer_representation_instances
                     .size())
        + 3u + plan.source_inventory.unit_probes.size();
    if (plan.target_inventory.targets.empty()
        || artifacts.size() != expected_artifact_count
        || plan.clang_release.empty()
        || plan.compilation_signature.empty()) {
        throw std::runtime_error(
            "Highway policy evidence does not describe the complete "
            "target/source artifact closure");
    }
    std::vector<const generated_artifact*> ordered;
    ordered.reserve(artifacts.size());
    for (const generated_artifact& artifact : artifacts) {
        ordered.push_back(&artifact);
    }
    std::ranges::sort(
        ordered,
        {},
        [](const generated_artifact* artifact) {
            return artifact->relative_path.generic_string();
        });
    std::string previous_path;
    for (const generated_artifact* artifact : ordered) {
        const std::filesystem::path relative = validate_owned_artifact_path(
            artifact->relative_path,
            artifact_owner_view{"Highway policy-evidence artifact"});
        const std::string path = relative.generic_string();
        if ((!previous_path.empty() && previous_path >= path)
            || relative.extension() != ".cppm"
            || artifact->module_name.empty()
            || artifact->content.empty()) {
            throw std::runtime_error(
                "Highway policy evidence contains an invalid or duplicate "
                "generated artifact: " + path);
        }
        previous_path = path;
    }

    std::ostringstream output;
    output << "{\n"
           << "  \"schema_version\": " << manifest_schema_version << ",\n"
           << "  \"policy_digest\": \""
           << detail::json_escape(plan.target_inventory.policy_digest) << "\",\n"
           << "  \"highway_version\": \""
           << detail::json_escape(plan.target_inventory.highway_version) << "\",\n"
           << "  \"highway_digest\": \""
           << detail::json_escape(plan.target_inventory.highway_digest) << "\",\n"
           << "  \"static_target\": \""
           << detail::json_escape(
                  plan.target_inventory.static_target_token) << "\",\n"
           << "  \"clang_version\": \""
           << detail::json_escape(plan.clang_release) << "\",\n"
           << "  \"compilation_signature\": \""
           << detail::json_escape(plan.compilation_signature) << "\",\n"
           << "  \"domain_topologies\": [\n";
    for (std::size_t index = 0;
         index < plan.domain_topologies.size();
         ++index) {
        const domain_topology_provenance& topology =
            plan.domain_topologies[index];
        output << "    {\"name\":\"" << detail::json_escape(topology.name)
               << "\",\"path\":\""
               << detail::json_escape(topology.path.generic_string())
               << "\",\"topology_digest\":\""
               << detail::json_escape(topology.topology_digest)
               << "\",\"content_sha256\":\""
               << detail::json_escape(topology.content_sha256) << "\"}"
               << (index + 1 == plan.domain_topologies.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  ],\n  \"targets\": [\n";
    for (std::size_t index = 0;
         index < plan.target_inventory.targets.size();
         ++index) {
        const target_spec& target = plan.target_inventory.targets[index];
        output << "    {\"platform\":\"" << detail::json_escape(target.platform)
               << "\",\"slug\":\"" << detail::json_escape(target.slug)
               << "\",\"token\":\"" << detail::json_escape(target.token)
               << "\",\"namespace\":\""
               << detail::json_escape(target.target_namespace)
               << "\",\"bit\":\"" << target.bit << "\"}"
               << (index + 1 == plan.target_inventory.targets.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"sources\": [\n";
    for (std::size_t index = 0; index < plan.extracted_sources.size(); ++index) {
        const extracted_source& source = plan.extracted_sources[index];
        output << "    {\"key\":\"" << detail::json_escape(source.source.key)
               << "\",\"module\":\"" << detail::json_escape(source.source.module_name)
               << "\",\"partition\":\"" << detail::json_escape(source.source.partition)
               << "\",\"family\":\"" << detail::json_escape(source.source.family)
               << "\",\"sha256\":\"" << detail::json_escape(source.source_sha256)
               << "\",\"callables\":" << source.callables.size()
               << ",\"dependencies\":[";
        for (std::size_t dependency = 0;
             dependency < source.source.dependencies.size();
             ++dependency) {
            if (dependency != 0) output << ',';
            output << '"'
                   << detail::json_escape(source.source.dependencies[dependency])
                   << '"';
        }
        output << "]}"
               << (index + 1 == plan.extracted_sources.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"slots\": [\n";
    for (std::size_t index = 0;
         index < plan.source_inventory.slots.size();
         ++index) {
        const slot_spec& slot = plan.source_inventory.slots[index];
        output << "    {\"owner\":\"" << detail::json_escape(slot.owner_key)
               << "\",\"family\":\"" << detail::json_escape(slot.family_field)
               << "\",\"slot\":\"" << detail::json_escape(slot.slot_field)
               << "\",\"expression\":\"" << detail::json_escape(slot.expression)
               << "\"}"
               << (index + 1 == plan.source_inventory.slots.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  ],\n  \"artifacts\": [\n";
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const generated_artifact& artifact = *ordered[index];
        output << "    {\"path\":\""
               << detail::json_escape(artifact.relative_path.generic_string())
               << "\",\"kind\":\"" << detail::artifact_kind_name(artifact.kind)
               << "\",\"module\":\"" << detail::json_escape(artifact.module_name)
               << "\",\"target\":\"" << detail::json_escape(artifact.target_token)
               << "\",\"source\":\"" << detail::json_escape(artifact.source_key)
               << "\",\"sha256\":\"" << bioseq::common::sha256_hex(artifact.content) << "\"}"
               << (index + 1 == ordered.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

}  // namespace bioseq::codegen::highway
