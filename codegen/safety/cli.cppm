module;

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

export module bioseq.codegen.safety:cli;

import :analyzer;
import :fact_producer;
import :json;
import :manifest;
import :runtime_inventory;
import :serialization;
import :validation;
import bioseq.codegen.support;

export namespace bioseq::codegen::safety {

namespace cli_detail {

struct emit_facts_options {
    std::uint32_t schema_version{};
    std::string profile;
    std::filesystem::path source_list;
    std::filesystem::path compile_database;
    std::filesystem::path pcm_root;
    std::filesystem::path pcm_manifest;
    std::filesystem::path sql_manifest;
    std::filesystem::path provenance;
    std::filesystem::path provenance_candidates_output;
    std::filesystem::path producer_executable;
    std::filesystem::path compile_graph_output;
    std::filesystem::path output;
    std::filesystem::path stamp;
};

struct analyze_facts_options {
    std::filesystem::path sql_manifest;
    std::filesystem::path compiler_facts;
    std::filesystem::path compile_graph;
    std::filesystem::path provenance;
    std::filesystem::path producer_executable;
    std::filesystem::path output;
    std::filesystem::path stamp;
    std::string profile;
    std::string generator_revision;
};

struct validate_pair_options {
    std::filesystem::path production_ledger;
    std::filesystem::path test_indexes_ledger;
    std::filesystem::path provenance;
    std::filesystem::path producer_executable;
    std::filesystem::path output;
    std::filesystem::path stamp;
};

struct emit_inventory_options {
    std::string profile;
    std::filesystem::path ledger;
    std::filesystem::path compiler_facts;
    std::filesystem::path compile_graph;
    std::filesystem::path sql_manifest;
    std::filesystem::path producer_executable;
    std::filesystem::path output;
    std::filesystem::path stamp;
};

struct parsed_option {
    std::string_view name;
    std::string_view value;
};

class option_reader {
  public:
    option_reader(
        std::span<std::string_view const> arguments,
        std::string_view command) noexcept
        : arguments_{arguments}, command_{command} {}

    [[nodiscard]] bool empty() const noexcept {
        return offset_ == arguments_.size();
    }

    [[nodiscard]] parsed_option take() {
        const std::string_view name = arguments_[offset_++];
        if (offset_ == arguments_.size()) {
            throw std::runtime_error(
                std::string{command_} + " option has no value: "
                + std::string{name});
        }
        return {
            .name = name,
            .value = arguments_[offset_++],
        };
    }

  private:
    std::span<std::string_view const> arguments_;
    std::string_view command_;
    std::size_t offset_{};
};

template <typename p_destination_type>
inline void assign_once(
    p_destination_type& destination,
    std::string_view value,
    std::string_view option) {
    if (!destination.empty()) {
        throw std::runtime_error(
            "duplicate safety option "
            + std::string{option});
    }
    destination = p_destination_type{value};
}

[[nodiscard]] inline analyze_facts_options parse_analyze_facts(
    std::span<std::string_view const> args) {
    analyze_facts_options result;
    option_reader reader{args, "safety analyzer"};
    while (!reader.empty()) {
        const parsed_option parsed = reader.take();
        const std::string_view option = parsed.name;
        const std::string_view value = parsed.value;
        if (option == "--sql-manifest") {
            assign_once(result.sql_manifest, value, option);
        } else if (option == "--compiler-facts") {
            assign_once(result.compiler_facts, value, option);
        } else if (option == "--compile-graph") {
            assign_once(result.compile_graph, value, option);
        } else if (option == "--provenance") {
            assign_once(result.provenance, value, option);
        } else if (option == "--producer-executable") {
            assign_once(
                result.producer_executable, value, option);
        } else if (option == "--output") {
            assign_once(result.output, value, option);
        } else if (option == "--stamp") {
            assign_once(result.stamp, value, option);
        } else if (option == "--profile") {
            assign_once(result.profile, value, option);
        } else if (option == "--generator-revision") {
            assign_once(result.generator_revision, value, option);
        } else {
            throw std::runtime_error(
                "unknown safety analyzer option "
                + std::string{option});
        }
    }
    if (result.sql_manifest.empty()
        || result.compiler_facts.empty()
        || result.compile_graph.empty()
        || result.provenance.empty()
        || result.producer_executable.empty()
        || result.output.empty()
        || result.profile.empty()
        || result.generator_revision.empty()) {
        throw std::runtime_error(
            "safety analyze-facts requires --sql-manifest, "
            "--compiler-facts, --compile-graph, --provenance, "
            "--output, --profile, --producer-executable, and "
            "--generator-revision");
    }
    if (result.profile != "production"
        && result.profile != "test_indexes") {
        throw std::runtime_error(
            "--profile must be production or test_indexes");
    }
    return result;
}

[[nodiscard]] inline emit_facts_options parse_emit_facts(
    std::span<std::string_view const> args) {
    emit_facts_options result;
    std::string schema_version;
    option_reader reader{args, "safety fact producer"};
    while (!reader.empty()) {
        const parsed_option parsed = reader.take();
        const std::string_view option = parsed.name;
        const std::string_view value = parsed.value;
        if (option == "--schema-version") {
            assign_once(schema_version, value, option);
        } else if (option == "--profile") {
            assign_once(result.profile, value, option);
        } else if (option == "--source-list") {
            assign_once(result.source_list, value, option);
        } else if (option == "--compile-database") {
            assign_once(result.compile_database, value, option);
        } else if (option == "--pcm-root") {
            assign_once(result.pcm_root, value, option);
        } else if (option == "--pcm-manifest") {
            assign_once(result.pcm_manifest, value, option);
        } else if (option == "--sql-manifest") {
            assign_once(result.sql_manifest, value, option);
        } else if (option == "--provenance") {
            assign_once(result.provenance, value, option);
        } else if (
            option == "--provenance-candidates-output") {
            assign_once(
                result.provenance_candidates_output,
                value,
                option);
        } else if (option == "--producer-executable") {
            assign_once(
                result.producer_executable, value, option);
        } else if (option == "--compile-graph-output") {
            assign_once(
                result.compile_graph_output, value, option);
        } else if (option == "--output") {
            assign_once(result.output, value, option);
        } else if (option == "--stamp") {
            assign_once(result.stamp, value, option);
        } else {
            throw std::runtime_error(
                "unknown safety fact-producer option "
                + std::string{option});
        }
    }
    if (schema_version != "2") {
        throw std::runtime_error(
            "safety emit-facts requires --schema-version 2");
    }
    result.schema_version = 2;
    if (result.profile.empty()
        || result.source_list.empty()
        || result.compile_database.empty()
        || result.pcm_root.empty()
        || result.pcm_manifest.empty()
        || result.sql_manifest.empty()
        || result.provenance.empty()
        || result.provenance_candidates_output.empty()
        || result.producer_executable.empty()
        || result.compile_graph_output.empty()
        || result.output.empty()
        || result.stamp.empty()) {
        throw std::runtime_error(
            "safety emit-facts requires --schema-version, "
            "--profile, --source-list, --compile-database, "
            "--pcm-root, --pcm-manifest, --sql-manifest, "
            "--provenance, --provenance-candidates-output, "
            "--producer-executable, "
            "--compile-graph-output, --output, and --stamp");
    }
    if (result.profile != "production"
        && result.profile != "test_indexes") {
        throw std::runtime_error(
            "--profile must be production or test_indexes");
    }
    if (std::filesystem::absolute(result.output)
            .lexically_normal()
        == std::filesystem::absolute(
               result.compile_graph_output)
               .lexically_normal()) {
        throw std::runtime_error(
            "compiler facts and compile graph must be distinct "
            "outputs");
    }
    return result;
}

[[nodiscard]] inline emit_inventory_options parse_emit_inventory(
    std::span<std::string_view const> args) {
    emit_inventory_options result;
    option_reader reader{args, "safety inventory"};
    while (!reader.empty()) {
        const parsed_option parsed = reader.take();
        const std::string_view option = parsed.name;
        const std::string_view value = parsed.value;
        if (option == "--profile") {
            assign_once(result.profile, value, option);
        } else if (option == "--ledger") {
            assign_once(result.ledger, value, option);
        } else if (option == "--compiler-facts") {
            assign_once(
                result.compiler_facts, value, option);
        } else if (option == "--compile-graph") {
            assign_once(result.compile_graph, value, option);
        } else if (option == "--sql-manifest") {
            assign_once(result.sql_manifest, value, option);
        } else if (option == "--producer-executable") {
            assign_once(
                result.producer_executable, value, option);
        } else if (option == "--output") {
            assign_once(result.output, value, option);
        } else if (option == "--stamp") {
            assign_once(result.stamp, value, option);
        } else {
            throw std::runtime_error(
                "unknown safety inventory option "
                + std::string{option});
        }
    }
    if (result.profile.empty()
        || result.ledger.empty()
        || result.compiler_facts.empty()
        || result.compile_graph.empty()
        || result.sql_manifest.empty()
        || result.producer_executable.empty()
        || result.output.empty()
        || result.stamp.empty()) {
        throw std::runtime_error(
            "safety emit-inventory requires --profile, --ledger, "
            "--compiler-facts, --compile-graph, --sql-manifest, "
            "--producer-executable, --output, and --stamp");
    }
    if (result.profile != "production"
        && result.profile != "test_indexes") {
        throw std::runtime_error(
            "--profile must be production or test_indexes");
    }
    return result;
}

[[nodiscard]] inline validate_pair_options parse_validate_pair(
    std::span<std::string_view const> args) {
    validate_pair_options result;
    option_reader reader{args, "safety pair"};
    while (!reader.empty()) {
        const parsed_option parsed = reader.take();
        const std::string_view option = parsed.name;
        const std::string_view value = parsed.value;
        if (option == "--production-ledger") {
            assign_once(result.production_ledger, value, option);
        } else if (option == "--test-indexes-ledger") {
            assign_once(
                result.test_indexes_ledger, value, option);
        } else if (option == "--provenance") {
            assign_once(result.provenance, value, option);
        } else if (option == "--producer-executable") {
            assign_once(
                result.producer_executable, value, option);
        } else if (option == "--output") {
            assign_once(result.output, value, option);
        } else if (option == "--stamp") {
            assign_once(result.stamp, value, option);
        } else {
            throw std::runtime_error(
                "unknown safety pair option "
                + std::string{option});
        }
    }
    if (result.production_ledger.empty()
        || result.test_indexes_ledger.empty()
        || result.provenance.empty()
        || result.producer_executable.empty()
        || result.output.empty()
        || result.stamp.empty()) {
        throw std::runtime_error(
            "safety validate-pair requires --production-ledger, "
            "--test-indexes-ledger, --provenance, "
            "--producer-executable, --output, and --stamp");
    }
    return result;
}

struct profile_pair_parent {
    std::string owner_usr;
    std::string qualified_declaration;
    std::string generated_wrapper;
    std::string public_symbol;

    bool operator==(const profile_pair_parent&) const = default;
};

struct profile_pair_ledger {
    std::string profile;
    std::string postgresql_revision;
    std::string provenance_sha256;
    std::string producer_sha256;
    std::string clang_version;
    std::string analyzer_version;
    std::string generator_revision;
    std::string symbol_groups;
    std::map<std::string, profile_pair_parent, std::less<>> parents;
};

[[nodiscard]] inline profile_pair_ledger parse_profile_pair_ledger(
    std::string_view text,
    std::string_view context) {
    const json_value document = parse_json(text);
    const json_value::object& root =
        json_object(document, context);
    if (serialization_detail::size_value(
            json_field(root, "schema_revision", context),
            std::string{context} + ".schema_revision")
            != 2
        || serialization_detail::size_value(
               json_field(root, "postgresql_major", context),
               std::string{context} + ".postgresql_major")
            != 17) {
        throw std::runtime_error(
            std::string{context}
            + " does not satisfy the current PostgreSQL-17 "
              "safety-ledger contract");
    }
    profile_pair_ledger result{
        .profile = serialization_detail::string_field(
            root, "profile", context),
        .postgresql_revision =
            serialization_detail::string_field(
                root, "postgresql_revision", context),
        .provenance_sha256 =
            serialization_detail::string_field(
                root, "provenance_bundle_sha256", context),
        .producer_sha256 =
            serialization_detail::string_field(
                root, "producer_executable_sha256", context),
        .clang_version =
            serialization_detail::string_field(
                root, "clang_version", context),
        .analyzer_version =
            serialization_detail::string_field(
                root, "analyzer_version", context),
        .generator_revision =
            serialization_detail::string_field(
                root, "generator_revision", context),
        .symbol_groups = render_json(
            json_field(root, "symbol_groups", context)),
    };
    const json_value::array& parents = json_array(
        json_field(root, "parents", context),
        std::string{context} + ".parents");
    for (std::size_t index = 0; index < parents.size(); ++index) {
        const std::string parent_context =
            std::string{context} + ".parents["
            + std::to_string(index) + "]";
        const json_value::object& parent =
            json_object(parents[index], parent_context);
        const std::string identity =
            serialization_detail::string_field(
                parent, "sql_identity", parent_context);
        const std::string parent_profile =
            serialization_detail::string_field(
                parent, "profile", parent_context);
        if (parent_profile != result.profile
            || !result.parents.emplace(
                    identity,
                    profile_pair_parent{
                        .owner_usr =
                            serialization_detail::string_field(
                                parent,
                                "owner_usr",
                                parent_context),
                        .qualified_declaration =
                            serialization_detail::string_field(
                                parent,
                                "qualified_declaration",
                                parent_context),
                        .generated_wrapper =
                            serialization_detail::string_field(
                                parent,
                                "generated_wrapper",
                                parent_context),
                        .public_symbol =
                            serialization_detail::string_field(
                                parent,
                                "public_symbol",
                                parent_context),
                    })
                    .second) {
            throw std::runtime_error(
                parent_context
                + " has a duplicate identity or wrong profile");
        }
    }
    if (result.parents.empty()) {
        throw std::runtime_error(
            std::string{context} + " has no parent closure");
    }
    return result;
}

inline int run_emit_facts(
    std::span<std::string_view const> args) {
    const emit_facts_options options = parse_emit_facts(args);
    const compilation_profile profile =
        serialization_detail::parse_profile(options.profile);
    compiler_fact_product product =
        produce_compiler_analysis_facts({
            .schema_version = options.schema_version,
            .profile = profile,
            .source_list = options.source_list,
            .compile_database = options.compile_database,
            .pcm_root = options.pcm_root,
            .pcm_manifest = options.pcm_manifest,
            .sql_manifest = options.sql_manifest,
            .provenance = options.provenance,
            .provenance_candidates_output =
                options.provenance_candidates_output,
            .producer_executable =
                options.producer_executable,
        });
    bioseq::codegen::publish_artifact_if_changed(
        options.compile_graph_output,
        product.compile_graph_json);
    const std::string persisted_graph =
        bioseq::codegen::read_artifact_bytes(
            options.compile_graph_output);
    if (persisted_graph != product.compile_graph_json
        || product.facts.compile_graph_sha256
            != bioseq::common::sha256_hex(persisted_graph)) {
        throw std::runtime_error(
            "persisted canonical compile graph differs from the "
            "analyzed graph");
    }
    if (product.facts.source_list_sha256
            != bioseq::common::sha256_hex(
                bioseq::codegen::read_artifact_bytes(
                    options.source_list))
        || product.facts.compile_database_sha256
            != bioseq::common::sha256_hex(
                bioseq::codegen::read_artifact_bytes(
                    options.compile_database))
        || product.facts.pcm_manifest_sha256
            != bioseq::common::sha256_hex(
                bioseq::codegen::read_artifact_bytes(
                    options.pcm_manifest))
        || product.facts.sql_manifest_sha256
            != bioseq::common::sha256_hex(
                bioseq::codegen::read_artifact_bytes(
                    options.sql_manifest))
        || product.facts.producer_executable_sha256
            != bioseq::common::sha256_hex(
                bioseq::codegen::read_artifact_bytes(
                    options.producer_executable))) {
        throw std::runtime_error(
            "a compiler-fact input changed during analysis");
    }
    const std::string rendered =
        render_compiler_analysis_facts_json(product.facts);
    const compiler_analysis_facts round_trip =
        parse_compiler_analysis_facts_json(rendered);
    if (render_compiler_analysis_facts_json(round_trip)
        != rendered) {
        throw std::runtime_error(
            "current compiler-facts contract is not canonical");
    }
    bioseq::codegen::publish_artifact_if_changed(
        options.output, rendered);
    if (bioseq::codegen::read_artifact_bytes(options.output)
        != rendered) {
        throw std::runtime_error(
            "persisted compiler facts differ from emitted bytes");
    }
    bioseq::codegen::mark_artifact_complete(options.stamp);
    return 0;
}

inline int run_analyze_facts(
    std::span<std::string_view const> args) {
    const analyze_facts_options options =
        parse_analyze_facts(args);
    const std::string manifest_text =
        bioseq::codegen::read_artifact_bytes(options.sql_manifest);
    const std::string facts_text =
        bioseq::codegen::read_artifact_bytes(options.compiler_facts);
    const std::string compile_graph_text =
        bioseq::codegen::read_artifact_bytes(options.compile_graph);
    const std::string provenance_text =
        bioseq::codegen::read_artifact_bytes(options.provenance);
    const safety_manifest manifest =
        parse_safety_manifest(manifest_text);
    const compiler_analysis_facts facts =
        parse_compiler_analysis_facts_json(facts_text);
    if (profile_name(facts.profile) != options.profile
        || facts.compiler_profile != options.profile) {
        throw std::runtime_error(
            "compiler facts profile differs from --profile");
    }
    if (facts.sql_manifest_sha256
        != bioseq::common::sha256_hex(manifest_text)) {
        throw std::runtime_error(
            "compiler facts were produced for a different SQL "
            "manifest");
    }
    if (facts.compile_graph_sha256
        != bioseq::common::sha256_hex(
            compile_graph_text)) {
        throw std::runtime_error(
            "compiler facts were produced for a different canonical "
            "compile graph");
    }
    if (facts.producer_executable_sha256
        != bioseq::common::sha256_hex(
            bioseq::codegen::read_artifact_bytes(
                options.producer_executable))) {
        throw std::runtime_error(
            "compiler facts were emitted by a different codegen "
            "executable");
    }
    const provenance_bundle provenance =
        parse_provenance_bundle_json(provenance_text);
    validate_provenance_bundle(provenance);
    const safety_ledger ledger =
        analyze_postgres_error_safety(
            manifest,
            facts,
            provenance,
            bioseq::common::sha256_hex(provenance_text),
            bioseq::common::sha256_hex(facts_text),
            options.generator_revision);
    bioseq::codegen::publish_artifact_if_changed(
        options.output, render_safety_ledger_json(ledger));
    if (!options.stamp.empty()) {
        bioseq::codegen::mark_artifact_complete(options.stamp);
    }
    return 0;
}

inline int run_emit_inventory(
    std::span<std::string_view const> args) {
    const emit_inventory_options options =
        parse_emit_inventory(args);
    const std::string ledger_text =
        bioseq::codegen::read_artifact_bytes(options.ledger);
    const std::string facts_text =
        bioseq::codegen::read_artifact_bytes(options.compiler_facts);
    const std::string compile_graph_text =
        bioseq::codegen::read_artifact_bytes(options.compile_graph);
    const std::string sql_manifest_text =
        bioseq::codegen::read_artifact_bytes(options.sql_manifest);
    const std::string producer_bytes =
        bioseq::codegen::read_artifact_bytes(
            options.producer_executable);
    const safety_ledger ledger =
        parse_safety_ledger_json(ledger_text);
    const compiler_analysis_facts facts =
        parse_compiler_analysis_facts_json(facts_text);
    if (profile_name(ledger.profile) != options.profile
        || profile_name(facts.profile) != options.profile
        || facts.compiler_profile != options.profile) {
        throw std::runtime_error(
            "runtime inventory inputs differ from --profile");
    }
    const runtime_probe_inventory inventory =
        build_runtime_probe_inventory(
            ledger,
            facts,
            ledger_text,
            facts_text,
            compile_graph_text,
            sql_manifest_text,
            producer_bytes);
    const std::string rendered =
        render_runtime_probe_inventory_json(inventory);
    const runtime_probe_inventory round_trip =
        parse_runtime_probe_inventory_json(rendered);
    validate_runtime_probe_inventory(round_trip);
    if (render_runtime_probe_inventory_json(round_trip)
        != rendered) {
        throw std::runtime_error(
            "schema revision 1 runtime inventory is not canonical");
    }
    bioseq::codegen::publish_artifact_if_changed(
        options.output, rendered);
    if (bioseq::codegen::read_artifact_bytes(options.output)
        != rendered) {
        throw std::runtime_error(
            "persisted runtime inventory differs from emitted "
            "bytes");
    }
    bioseq::codegen::mark_artifact_complete(options.stamp);
    return 0;
}

inline int run_validate_pair(
    std::span<std::string_view const> args) {
    const validate_pair_options options =
        parse_validate_pair(args);
    const std::string production_text =
        bioseq::codegen::read_artifact_bytes(
            options.production_ledger);
    const std::string test_indexes_text =
        bioseq::codegen::read_artifact_bytes(
            options.test_indexes_ledger);
    const std::string provenance_text =
        bioseq::codegen::read_artifact_bytes(options.provenance);
    const std::string producer_bytes =
        bioseq::codegen::read_artifact_bytes(
            options.producer_executable);
    const provenance_bundle provenance =
        parse_provenance_bundle_json(provenance_text);
    validate_provenance_bundle(provenance);
    const std::string provenance_sha256 =
        bioseq::common::sha256_hex(provenance_text);
    const safety_ledger production_ledger =
        parse_safety_ledger_json(production_text);
    const safety_ledger test_indexes_ledger =
        parse_safety_ledger_json(test_indexes_text);
    validate_profile_pair(
        production_ledger,
        test_indexes_ledger,
        provenance,
        provenance_sha256);
    const profile_pair_ledger production =
        parse_profile_pair_ledger(
            production_text, "production safety ledger");
    const profile_pair_ledger test_indexes =
        parse_profile_pair_ledger(
            test_indexes_text, "test-indexes safety ledger");
    const std::string producer_sha256 =
        bioseq::common::sha256_hex(producer_bytes);
    if (production.profile != "production"
        || test_indexes.profile != "test_indexes"
        || production.postgresql_revision
            != provenance.postgresql_revision
        || test_indexes.postgresql_revision
            != provenance.postgresql_revision
        || production.provenance_sha256 != provenance_sha256
        || test_indexes.provenance_sha256 != provenance_sha256
        || production.producer_sha256 != producer_sha256
        || test_indexes.producer_sha256 != producer_sha256
        || production.clang_version
            != test_indexes.clang_version
        || production.analyzer_version
            != test_indexes.analyzer_version
        || production.generator_revision
            != test_indexes.generator_revision
        || production.parents != test_indexes.parents
        || production.symbol_groups
            != test_indexes.symbol_groups) {
        throw std::runtime_error(
            "production/test-indexes safety ledgers do not form "
            "one exact tool/provenance/SQL ownership pair");
    }
    const std::string report = render_json(json_value::object{
        {"analyzer_version", production.analyzer_version},
        {"clang_version", production.clang_version},
        {"generator_revision", production.generator_revision},
        {"parent_count", json_value{production.parents.size()}},
        {"postgresql_revision",
         production.postgresql_revision},
        {"producer_executable_sha256", producer_sha256},
        {"production_ledger_sha256",
         bioseq::common::sha256_hex(production_text)},
        {"provenance_bundle_sha256", provenance_sha256},
        {"schema", "bioseq_safety_profile_pair"},
        {"schema_revision", json_value{std::size_t{1}}},
        {"test_indexes_ledger_sha256",
         bioseq::common::sha256_hex(test_indexes_text)},
    });
    bioseq::codegen::publish_artifact_if_changed(
        options.output, report);
    if (bioseq::codegen::read_artifact_bytes(options.output)
        != report) {
        throw std::runtime_error(
            "persisted safety profile-pair report differs from "
            "validated bytes");
    }
    bioseq::codegen::mark_artifact_complete(options.stamp);
    return 0;
}

}  // namespace cli_detail

/// Runs the PostgreSQL safety evidence component.
///
/// `emit-facts` performs the authoritative Clang AST/CFG/lifetime pass and
/// `analyze-facts` consumes only those sealed facts. Neither command falls
/// back to scanning source text.
[[nodiscard]] inline int run_cli(
    std::span<std::string_view const> args) {
    try {
        if (args.empty()) {
            throw std::runtime_error(
                "usage: safety "
                "<emit-facts|analyze-facts|emit-inventory|"
                "validate-pair> "
                "[options]");
        }
        if (args.front() == "emit-facts") {
            return cli_detail::run_emit_facts(args.subspan(1));
        }
        if (args.front() == "analyze-facts") {
            return cli_detail::run_analyze_facts(args.subspan(1));
        }
        if (args.front() == "emit-inventory") {
            return cli_detail::run_emit_inventory(
                args.subspan(1));
        }
        if (args.front() == "validate-pair") {
            return cli_detail::run_validate_pair(args.subspan(1));
        }
        throw std::runtime_error(
            "unknown safety command '" + std::string{args.front()} + "'");
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_codegen safety: {}", error.what());
        return 1;
    }
}

}  // namespace bioseq::codegen::safety
