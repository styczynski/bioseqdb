module;

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.guc_contract_tests;

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
    std::filesystem::path source{__FILE__};
    if (!source.is_absolute()) {
        source = std::filesystem::absolute(source);
    }
    for (int parent = 0; parent < 4; ++parent) {
        source = source.parent_path();
    }
    require(
        std::filesystem::exists(source / "main" / "bioseq"),
        "cannot resolve repository root from the contract module");
    return source;
}

[[nodiscard]] inline const rendered_guc_owner_module& owner(
    const rendered_guc_package& package,
    std::string_view owner_key) {
    const rendered_guc_owner_module* found = nullptr;
    for (const rendered_guc_owner_module& candidate :
         package.owners) {
        if (candidate.owner_key == owner_key) {
            found = &candidate;
            break;
        }
    }
    require(
        found != nullptr,
        "rendered package lacks an expected owner");
    return *found;
}

template <typename Operation>
inline void require_rejected(
    Operation&& operation,
    std::initializer_list<std::string_view> diagnostic_fragments,
    std::string_view message) {
    bool rejected_with_context = false;
    try {
        operation();
    } catch (const std::exception& error) {
        const std::string_view diagnostic{error.what()};
        rejected_with_context = std::ranges::all_of(
            diagnostic_fragments,
            [diagnostic](std::string_view fragment) {
                return diagnostic.contains(fragment);
            });
    }
    require(rejected_with_context, message);
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
        "mutation fixture lacks an expected annotated GUC");
    return *found;
}

[[nodiscard]] inline std::string read_text(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(
        input.is_open(),
        "cannot read the GUC mutation ledger fixture");
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

[[nodiscard]] inline std::string replace_once(
    std::string content,
    std::string_view before,
    std::string_view after) {
    const std::size_t position = content.find(before);
    require(
        position != std::string::npos,
        "GUC mutation source text is absent");
    require(
        content.find(before, position + before.size())
            == std::string::npos,
        "GUC mutation source text is ambiguous");
    content.replace(position, before.size(), after);
    return content;
}

[[nodiscard]] inline std::string replace_in_ledger_row(
    std::string content,
    std::string_view identity,
    std::string_view before,
    std::string_view after) {
    const std::string identity_token =
        "\"" + std::string{identity} + "\"";
    const std::size_t identity_position =
        content.find(identity_token);
    require(
        identity_position != std::string::npos,
        "GUC mutation ledger row is absent");
    const std::size_t row_begin =
        content.rfind('\n', identity_position);
    const std::size_t row_end =
        content.find('\n', identity_position);
    const std::size_t field_position =
        content.find(before, identity_position);
    require(
        field_position != std::string::npos
            && (row_end == std::string::npos
                || field_position < row_end)
            && (row_begin == std::string::npos
                || field_position > row_begin),
        "GUC mutation field is absent from the selected ledger row");
    const std::size_t repeated =
        content.find(before, field_position + before.size());
    require(
        repeated == std::string::npos
            || (row_end != std::string::npos && repeated >= row_end),
        "GUC mutation field is ambiguous within the ledger row");
    content.replace(field_position, before.size(), after);
    return content;
}

class temporary_ledger final {
public:
    explicit temporary_ledger(std::string_view content) {
        const std::string nonce = std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            std::error_code error;
            const std::filesystem::path candidate =
                std::filesystem::temp_directory_path()
                / ("bioseq-guc-contract-" + nonce + "-"
                   + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error)) {
                root_ = candidate;
                break;
            }
        }
        require(
            !root_.empty(),
            "cannot create a temporary GUC mutation directory");
        path_ = root_ / "ledger.json";
        std::ofstream output{path_, std::ios::binary};
        require(
            output.is_open(),
            "cannot create a temporary GUC mutation ledger");
        output.write(
            content.data(),
            static_cast<std::streamsize>(content.size()));
        output.close();
        require(
            output.good(),
            "cannot write a temporary GUC mutation ledger");
    }

    temporary_ledger(const temporary_ledger&) = delete;
    temporary_ledger& operator=(const temporary_ledger&) = delete;

    ~temporary_ledger() noexcept {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

inline void validate_package(
    const guc_inventory& inventory,
    const rendered_guc_package& package,
    std::size_t root_count) {
    require(
        package.owners.size() == 4,
        "profile must render exactly four owner facets");
    require(
        owner(package, "bioseq").guc_names.size() == root_count,
        "root owner registration count changed");
    require(
        owner(package, "scheduler").guc_names.size() == 3,
        "scheduler owner registration count changed");
    require(
        owner(package, "storage").guc_names.size() == 1,
        "storage owner registration count changed");
    require(
        owner(package, "aligner").guc_names.size() == 43,
        "aligner owner registration count changed");
    require(
        package.registry.guc_names.size() == inventory.gucs.size(),
        "registry count differs from inventory count");
    for (std::size_t index = 0; index < inventory.gucs.size(); ++index) {
        require(
            package.registry.guc_names[index] == inventory.gucs[index].name,
            "registry does not retain parity-ledger order");
    }
    for (const rendered_guc_owner_module& facet : package.owners) {
        require(
            !facet.content.contains("[&]")
                && !facet.content.contains("[=]"),
            "generated owner facet contains a capture-all lambda");
        require(
            facet.content.contains("export namespace ")
                && !facet.content.contains("\nexport inline "),
            "generated owner facet nests an export declaration");
        require(
            facet.content.contains(
                "invoke_pg_as_cpp(define_variable)")
                || !facet.content.contains(
                    "DefineCustomBoolVariable"),
            "primitive PostgreSQL registration bypasses the error bridge");
        require(
            !facet.content.contains(".value(),\n")
                && !facet.content.contains(
                    ", &::bioseq::settings::logging, "
                    "::bioseq::settings::logging"),
            "generated boot values depend on live backing cells");
        require(
            !facet.content.contains("DefineCustomBoolVariable")
                || facet.content.contains(
                    "static bool complete = false;"),
            "primitive registration lacks one-shot completion state");
    }
    require(
        !package.registry.content.contains("[&]")
            && !package.registry.content.contains("[=]"),
        "generated registry contains a capture-all lambda");
}

}  // namespace guc_contract_detail

inline void run_guc_generator_contract_self_tests() {
    using guc_contract_detail::require;
    const std::filesystem::path root =
        guc_contract_detail::repository_root();
    const std::vector<std::filesystem::path> owners = {
        root / "main/bioseq/backend/config/sql_guc_settings.cppm",
        root
            / "main/bioseq/backend/config/"
              "scheduler_sql_guc_settings.cppm",
        root
            / "main/bioseq/backend/config/"
              "storage_sql_guc_settings.cppm",
        root
            / "main/bioseq/backend/config/"
              "aligner_sql_guc_settings.cppm",
    };
    const std::filesystem::path ledger =
        root
        / "main/bioseq/testdata/doc0388_guc_parity_ledger.json";
    const collected_program program = collect_fixture_sources(owners);

    const guc_inventory production = collect_guc_inventory(
        program, ledger, guc_profile::production);
    const guc_inventory test_indexes = collect_guc_inventory(
        program, ledger, guc_profile::test_indexes);
    require(
        production.gucs.size() == 76,
        "production profile registration count changed");
    require(
        test_indexes.gucs.size() == 77,
        "test-index profile registration count changed");
    const auto fixture_guc =
        [](const guc_inventory& inventory,
           std::string_view name) -> const collected_guc& {
            const auto found = std::ranges::find(
                inventory.gucs, name, &collected_guc::name);
            guc_contract_detail::require(
                found != inventory.gucs.end(),
                "source-only fixture lacks an expected GUC");
            return *found;
        };
    const collected_guc& fixture_compression =
        fixture_guc(production, "bioseq.compression");
    require(
        fixture_compression
                .generated_live_authority.visible_enum_values
                == std::vector<std::string>{"none", "pglz"}
            && fixture_compression
                    .generated_live_authority.boot_value
                == "pglz",
        "source-only fixture did not retain unconditional enum values");
    const collected_guc& fixture_stripe =
        fixture_guc(production, "bioseq.stripe_row_limit");
    require(
        fixture_stripe.generated_live_authority.boot_value == "150000"
            && fixture_stripe.generated_live_authority.minimum
                == std::optional<std::string>{"1000"}
            && fixture_stripe.generated_live_authority.maximum
                == std::optional<std::string>{"100000000"},
        "source-only fixture did not resolve integer constant authority");
    const collected_guc& fixture_column_cache =
        fixture_guc(production, "bioseq.column_cache_size");
    require(
        fixture_column_cache.generated_live_authority.boot_value == "200"
            && fixture_column_cache.generated_live_authority.minimum
                == std::optional<std::string>{"20"}
            && fixture_column_cache.generated_live_authority.maximum
                == std::optional<std::string>{"20000"}
            && fixture_column_cache.generated_live_authority.unit
                == std::optional<std::string>{"MB"}
            && fixture_column_cache.generated_live_authority.raw_flags
                == 0x04000000,
        "integer suffix or unit authority changed");
    const collected_guc& fixture_real =
        fixture_guc(production, "bioseq.aligner.chain_gap_scale");
    require(
        fixture_real.generated_live_authority.boot_value == "0.8"
            && fixture_real.generated_live_authority.minimum
                == std::optional<std::string>{"0"}
            && fixture_real.generated_live_authority.maximum
                == std::optional<std::string>{"100"},
        "real live-authority resolution changed");
    const collected_guc& fixture_hidden =
        fixture_guc(production, "bioseq.test_force_chunked_scanner");
    require(
        fixture_hidden.generated_live_authority.raw_flags == 0x84
            && !fixture_hidden
                    .generated_live_authority.visible_in_show_all
            && !fixture_hidden
                    .generated_live_authority.included_in_sample
            && !fixture_hidden
                    .generated_live_authority.hook_presence[0]
            && !fixture_hidden
                    .generated_live_authority.hook_presence[1]
            && !fixture_hidden
                    .generated_live_authority.hook_presence[2]
            && !fixture_hidden.short_description.empty(),
        "description, flag, visibility, or hook authority changed");
    const collected_guc& fixture_test_enum = fixture_guc(
        test_indexes, "bioseq.test_force_paper_index_failure");
    require(
        fixture_test_enum
                .generated_live_authority.visible_enum_values
            == std::vector<std::string>{
                "none", "unsupported_vector_shape"}
            && fixture_test_enum
                    .generated_live_authority.boot_value
                == "none",
        "test-profile enum live authority changed");
    {
        collected_program profiled_fixture = program;
        profiled_fixture.compiler_profile = "production";
        collected_entity& compression_level =
            guc_contract_detail::annotated_guc(
                profiled_fixture,
                "bioseq::settings::compression_level");
        guc_contract_detail::require_rejected(
            [&profiled_fixture, &compression_level] {
                static_cast<void>(
                    guc_detail::resolve_integer_expression(
                        profiled_fixture,
                        compression_level,
                        "compression_level_minimum"));
            },
            {"cannot resolve generated integer GUC identifier",
             "compression_level_minimum"},
            "profiled integer authority used the source-only fallback");
        guc_contract_detail::require_rejected(
            [&profiled_fixture, &ledger] {
                static_cast<void>(collect_guc_inventory(
                    profiled_fixture,
                    ledger,
                    guc_profile::production));
            },
            {"cannot resolve generated enum values",
             "bioseq.compression"},
            "profiled enum authority used the source-only fallback");
    }
    require(
        guc_detail::canonical_feature_predicate(
            "((defined(FEATURE_OWNER)))")
                == "defined(FEATURE_OWNER)"
            && guc_detail::effective_feature_predicate(
                "(defined(FEATURE_OWNER))",
                "(defined(FEATURE_MEMBER))")
                == "(defined(FEATURE_OWNER)) && "
                   "(defined(FEATURE_MEMBER))"
            && guc_detail::effective_feature_predicate(
                "(defined(FEATURE_OWNER))",
                "((defined(FEATURE_OWNER)))")
                == "defined(FEATURE_OWNER)",
        "feature-predicate canonicalization or owner/member "
        "composition changed");

    {
        collected_program mutation = program;
        guc_contract_detail::annotated_guc(
            mutation,
            "bioseq::settings::logging").cpp_qualified_name =
                "bioseq::logging";
        guc_contract_detail::require_rejected(
            [&mutation, &ledger] {
                static_cast<void>(collect_guc_inventory(
                    mutation, ledger, guc_profile::production));
            },
            {"backing cell", "bioseq::settings::logging"},
            "derived C++ backing-cell drift was accepted");
    }
    {
        collected_program mutation = program;
        guc_contract_detail::annotated_guc(
            mutation,
            "bioseq::settings::logging").source_module =
                "bioseq.mutated_sql_guc_settings";
        guc_contract_detail::require_rejected(
            [&mutation, &ledger] {
                static_cast<void>(collect_guc_inventory(
                    mutation, ledger, guc_profile::production));
            },
            {"candidate module", "bioseq.sql_guc_settings"},
            "derived GUC source-owner module drift was accepted");
    }
    {
        collected_program mutation = program;
        guc_contract_detail::annotated_guc(
            mutation,
            "bioseq::settings::logging").source_path =
                root / "main/bioseq/backend/pg/mutated_guc_owner.cppm";
        guc_contract_detail::require_rejected(
            [&mutation, &ledger] {
                static_cast<void>(collect_guc_inventory(
                    mutation, ledger, guc_profile::production));
            },
            {"candidate module", "outside the PostgreSQL adapter"},
            "PostgreSQL-adapter GUC ownership drift was accepted");
    }
    {
        collected_program mutation = program;
        guc_contract_detail::annotated_guc(
            mutation,
            "bioseq::settings::logging").cpp_return_type =
                "std::uint64_t";
        guc_contract_detail::require_rejected(
            [&mutation, &ledger] {
                static_cast<void>(collect_guc_inventory(
                    mutation, ledger, guc_profile::production));
            },
            {"unsupported C++ storage type", "std::uint64_t"},
            "unsupported candidate GUC storage ABI was accepted");
    }

    const std::string ledger_content =
        guc_contract_detail::read_text(ledger);
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.logging",
                "\"true\"",
                "\"\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"bioseq.logging", "empty feature predicate"},
            "empty ledger GUC feature predicate was accepted as true");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.compression",
                "\"lz4\",\"2\",0,"
                "\"defined(HAVE_CITUS_LIBLZ4) && "
                "HAVE_CITUS_LIBLZ4\"",
                "\"lz4\",\"2\",0,\"\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"bioseq.compression.lz4", "empty feature predicate"},
            "empty ledger enum feature predicate was accepted as true");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.test_force_paper_index_failure",
                "[null,null,null],"
                "\"defined(BIOSEQ_TEST_INDEXES)\","
                "[\"test_indexes\"]",
                "[null,null,null],\"true\",[\"test_indexes\"]");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::test_indexes));
            },
            {"bioseq.test_force_paper_index_failure",
             "feature gating differs"},
            "GUC owner feature-predicate drift was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.compression",
                "\"lz4\",\"2\",0,"
                "\"defined(HAVE_CITUS_LIBLZ4) && "
                "HAVE_CITUS_LIBLZ4\"",
                "\"lz4\",\"2\",0,"
                "\"defined(HAVE_CITUS_LIBLZ4)\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"bioseq.compression", "lz4",
             "feature gating differs"},
            "optional enum-value feature-predicate drift was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.planner_debug_level",
                "\"debug\",\"DEBUG2\",1,\"true\",\"debug2\"",
                "\"debug\",\"DEBUG2\",1,"
                "\"defined(MUTATED_ALIAS_FEATURE)\",\"debug2\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"bioseq.planner_debug_level", "alias", "debug",
             "feature gating differs"},
            "enum-alias feature-predicate drift was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.planner_debug_level",
                "\"debug\",\"DEBUG2\",1,\"true\",\"debug2\"",
                "\"debug\",\"INFO\",1,\"true\",\"debug2\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"bioseq.planner_debug_level", "alias", "debug",
             "wire value", "debug2"},
            "enum-alias wire drift was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.logging",
                "\"true\"",
                "\"()\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"feature predicate", "empty parenthesized expression"},
            "empty GUC feature predicate was accepted as true");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.logging",
                "\"bool\",\"bool\",\"enable_logging\"",
                "\"bool\",\"int32\",\"enable_logging\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"invalid kind/storage ABI pair", "bioseq.logging"},
            "parity-ledger GUC storage ABI drift was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.logging",
                "\"bool\",\"bool\",\"enable_logging\"",
                "\"bool\",\"bool\","
                "\"bioseq_test_force_chunked_scanner\"");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"duplicate GUC parity ledger legacy storage",
             "bioseq_test_force_chunked_scanner"},
            "duplicate legacy GUC storage provenance was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.logging",
                "[null,null,null]",
                "[\"mutated_check_hook\",null,null]");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"schema version 1 requires null hooks"},
            "non-null GUC hook-triple drift was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.logging",
                "[null,null,null]",
                "[null,null]");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"hooks must contain three fields"},
            "malformed GUC hook-triple cardinality was accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.compression",
                "[[\"invalid\",\"-1\"],[\"count\",\"4\"]]",
                "[]");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"enum GUC 'bioseq.compression'",
             "unaccounted value"},
            "unconsumed enum GUC omissions were accepted");
    }
    {
        const std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.compression",
                "[\"invalid\",\"-1\"]",
                "[\"invalid\",\"-2\"]");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"omitted value 'invalid'", "changed wire value"},
            "enum GUC omission wire drift was accepted");
    }
    {
        std::string mutation =
            guc_contract_detail::replace_in_ledger_row(
                ledger_content,
                "bioseq.logging",
                "[\"production\",\"test_indexes\"]",
                "[\"test_indexes\"]");
        mutation = guc_contract_detail::replace_in_ledger_row(
            std::move(mutation),
            "bioseq.test_force_paper_index_failure",
            "[\"test_indexes\"]",
            "[\"production\",\"test_indexes\"]");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"bioseq.logging", "unexpected profile selection"},
            "equal-count GUC profile-identity drift was accepted");
    }
    {
        collected_program mutation = program;
        collected_entity extra = guc_contract_detail::annotated_guc(
            mutation, "bioseq::settings::logging");
        extra.name = "unexpected_registration";
        extra.cpp_qualified_name =
            "bioseq::settings::unexpected_registration";
        mutation.entities.push_back(std::move(extra));
        guc_contract_detail::require_rejected(
            [&mutation, &ledger] {
                static_cast<void>(collect_guc_inventory(
                    mutation, ledger, guc_profile::production));
            },
            {"annotated GUC profile 'production' has 77 rows",
             "parity ledger selects 76"},
            "extra annotated GUC registration row was accepted");
    }
    {
        std::string mutation = guc_contract_detail::replace_once(
            ledger_content,
            "\"production\":{\"row_count\":76}",
            "\"production\":{\"row_count\":77}");
        mutation = guc_contract_detail::replace_once(
            std::move(mutation),
            "\"test_indexes\":{\"row_count\":77}",
            "\"test_indexes\":{\"row_count\":78}");
        const std::size_t rows_end = mutation.rfind("\n  ]\n}");
        require(
            rows_end != std::string::npos,
            "cannot append the unconsumed GUC ledger row");
        mutation.insert(
            rows_end,
            ",\n    [77,\"column\","
            "\"bioseq.unconsumed_registration\",\"bioseq\","
            "\"unconsumed_registration\",\"bool\",\"bool\","
            "\"legacy_unconsumed_registration\",\"false\",null,null,"
            "\"user_set\",[],null,[null,null,null],\"true\","
            "[\"production\",\"test_indexes\"],"
            "\"00000000000000000000000000000000"
            "00000000000000000000000000000000\","
            "null,[],[]]");
        const guc_contract_detail::temporary_ledger fixture{mutation};
        guc_contract_detail::require_rejected(
            [&program, &fixture] {
                static_cast<void>(collect_guc_inventory(
                    program,
                    fixture.path(),
                    guc_profile::production));
            },
            {"annotated GUC profile 'production' has 76 rows",
             "parity ledger selects 77"},
            "unconsumed GUC parity-ledger registration row was accepted");
    }

    const rendered_guc_package production_package =
        render_guc_package(production);
    const rendered_guc_package test_package =
        render_guc_package(test_indexes);
    guc_contract_detail::validate_package(
        production, production_package, 29);
    guc_contract_detail::validate_package(
        test_indexes, test_package, 30);

    collected_guc string_probe{
        .name = "bioseq.string_probe",
        .owner_key = "bioseq",
        .member = "string_probe",
        .kind = guc_kind::string,
        .cpp_qualified_name = "bioseq::settings::string_probe",
        .default_expression = "nullptr",
        .context = "user_set",
        .short_description = "Exercises string boot-value rendering.",
    };
    std::string rendered_string =
        render_primitive_guc_registration_contract_probe(string_probe);
    require(
        rendered_string.contains(
            "&::bioseq::settings::string_probe, nullptr, PGC_USERSET")
            && !rendered_string.contains(
                "&::bioseq::settings::string_probe, "
                "::bioseq::settings::string_probe"),
        "null string boot value depends on the live backing cell");
    string_probe.default_expression = "\"stable-default\"";
    rendered_string =
        render_primitive_guc_registration_contract_probe(string_probe);
    require(
        rendered_string.contains(
            "&::bioseq::settings::string_probe, \"stable-default\", "
            "PGC_USERSET")
            && !rendered_string.contains(
                "&::bioseq::settings::string_probe, "
                "::bioseq::settings::string_probe"),
        "string-literal boot value lacks immutable static-lifetime authority");

    string_probe.reset_plan_cache = true;
    string_probe.authored_assign_hook =
        "bioseq::settings::assign_string_probe";
    string_probe.authority.hooks[1] =
        "assign_string_probe_and_reset_plan_cache";
    rendered_string =
        render_primitive_guc_registration_contract_probe(string_probe);
    const std::size_t authored_assign =
        rendered_string.find(
            "::bioseq::settings::assign_string_probe(new_value, extra);");
    const std::size_t reset_call =
        rendered_string.find("if (changed) ResetPlanCache();");
    require(
        rendered_string.contains(
            "assign_string_probe_and_reset_plan_cache")
            && rendered_string.contains("std::strcmp(")
            && authored_assign != std::string::npos
            && reset_call != std::string::npos
            && authored_assign < reset_call,
        "plan-cache reset wrapper does not compare, chain, then reset");

    require(
        !production_package.registry.content.contains(
            "test_force_paper_index_failure"),
        "production registry contains the test-only delta");
    require(
        production_package.registry.content.contains(
            "inline constexpr unsigned registration_count = 76U;")
            && production_package.registry.content.contains(
                "inline constexpr bool test_indexes_profile = false;")
            && test_package.registry.content.contains(
                "inline constexpr unsigned registration_count = 77U;")
            && test_package.registry.content.contains(
                "inline constexpr bool test_indexes_profile = true;"),
        "generated registry compile-time profile API changed");
    require(
        guc_contract_detail::owner(
            production_package, "bioseq").content.contains(
                "inline constexpr unsigned registration_count = 29U;")
            && guc_contract_detail::owner(
                test_package, "bioseq").content.contains(
                    "inline constexpr unsigned registration_count = 30U;"),
        "generated owner compile-time inventory API changed");
    require(
        test_package.registry.content.contains(
            "test_force_paper_index_failure"),
        "test-index registry lost its single test-only delta");
    const guc_profile_delta_proof delta =
        prove_guc_profile_delta(production, test_indexes);
    require(
        delta.added_in_test_indexes
            == std::vector<std::string>{
                "bioseq.test_force_paper_index_failure"}
            && delta.removed_from_test_indexes.empty()
            && delta.changed_in_test_indexes.empty(),
        "production/test-index delta proof changed");
    const guc_profile_projection production_projection =
        make_guc_profile_projection(
            production, production_package);
    const guc_profile_projection test_projection =
        make_guc_profile_projection(
            test_indexes, test_package);
    require(
        production_projection.profile == guc_profile::production
            && test_projection.profile
                == guc_profile::test_indexes
            && production_projection.ledger_sha256
                == test_projection.ledger_sha256
            && production_projection.registrations.size()
                == production.gucs.size()
            && test_projection.registrations.size()
                == test_indexes.gucs.size()
            && production_projection.owner_content_sha256.size() == 4
            && test_projection.owner_content_sha256.size() == 4,
        "sealed GUC profile projections lost inventory provenance");
    const guc_profile_delta_proof projected_delta =
        prove_guc_profile_delta(
            production_projection, test_projection);
    require(
        projected_delta.added_in_test_indexes
            == delta.added_in_test_indexes
            && projected_delta.removed_from_test_indexes.empty()
            && projected_delta.changed_in_test_indexes.empty()
            && !projected_delta
                    .owner_descriptor_profile_invariant.at("bioseq")
            && projected_delta
                    .owner_descriptor_profile_invariant.at("scheduler")
            && projected_delta
                    .owner_descriptor_profile_invariant.at("storage")
            && projected_delta
                    .owner_descriptor_profile_invariant.at("aligner"),
        "sealed GUC projection delta differs from inventory authority");
    const guc_profile_delta_evidence sealed_evidence =
        make_guc_profile_delta_evidence(
            production_projection, test_projection);
    require(
        sealed_evidence.production_registry_content_sha256
                == production_projection.registry_content_sha256
            && sealed_evidence.test_indexes_registry_content_sha256
                == test_projection.registry_content_sha256
            && sealed_evidence.owner_content_sha256.size() == 4
            && sealed_evidence.owner_content_sha256.at("bioseq").first
                == production_projection.owner_content_sha256.at(
                    "bioseq")
            && sealed_evidence.owner_content_sha256.at("bioseq").second
                == test_projection.owner_content_sha256.at("bioseq"),
        "sealed GUC profile evidence lost registry or owner hashes");
    const auto mutate_sha256 =
        [](std::string value) {
            guc_contract_detail::require(
                value.size() == 64,
                "projection mutation fixture lacks a SHA-256 value");
            value.front() = value.front() == '0' ? '1' : '0';
            return value;
        };
    {
        guc_profile_projection wrong_role = production_projection;
        wrong_role.profile = guc_profile::test_indexes;
        guc_contract_detail::require_rejected(
            [&wrong_role, &test_projection] {
                static_cast<void>(prove_guc_profile_delta(
                    wrong_role, test_projection));
            },
            {"requires production then test_indexes"},
            "sealed GUC projection role drift was accepted");
    }
    {
        guc_profile_projection ledger_drift = test_projection;
        ledger_drift.ledger_sha256 =
            mutate_sha256(ledger_drift.ledger_sha256);
        guc_contract_detail::require_rejected(
            [&production_projection, &ledger_drift] {
                static_cast<void>(prove_guc_profile_delta(
                    production_projection, ledger_drift));
            },
            {"different parity ledgers"},
            "sealed GUC projection ledger drift was accepted");
    }
    {
        guc_profile_projection source_drift = test_projection;
        source_drift.registrations.front().source_sha256 =
            mutate_sha256(
                source_drift.registrations.front().source_sha256);
        guc_contract_detail::require_rejected(
            [&production_projection, &source_drift] {
                static_cast<void>(prove_guc_profile_delta(
                    production_projection, source_drift));
            },
            {"single documented test-only setting"},
            "sealed GUC projection source drift was accepted");
    }
    {
        guc_profile_projection descriptor_drift = test_projection;
        descriptor_drift.registrations.front().descriptor_sha256 =
            mutate_sha256(
                descriptor_drift.registrations.front()
                    .descriptor_sha256);
        guc_contract_detail::require_rejected(
            [&production_projection, &descriptor_drift] {
                static_cast<void>(prove_guc_profile_delta(
                    production_projection, descriptor_drift));
            },
            {"single documented test-only setting"},
            "sealed GUC projection descriptor drift was accepted");
    }
    {
        guc_inventory resolved_live_drift = test_indexes;
        resolved_live_drift.gucs.front()
            .generated_live_authority.boot_value = "on";
        const rendered_guc_package resolved_live_package =
            render_guc_package(resolved_live_drift);
        const guc_profile_projection resolved_live_projection =
            make_guc_profile_projection(
                resolved_live_drift, resolved_live_package);
        require(
            resolved_live_projection.registrations.front()
                    .descriptor_sha256
                != test_projection.registrations.front()
                    .descriptor_sha256,
            "resolved live GUC property mutation did not alter the "
            "sealed descriptor");
        guc_contract_detail::require_rejected(
            [&production_projection, &resolved_live_projection] {
                static_cast<void>(prove_guc_profile_delta(
                    production_projection,
                    resolved_live_projection));
            },
            {"single documented test-only setting"},
            "resolved live GUC property drift was accepted by the "
            "sealed profile contract");
    }
    {
        guc_profile_projection owner_drift = test_projection;
        owner_drift.owner_content_sha256.at("scheduler") =
            "malformed-owner-seal";
        guc_contract_detail::require_rejected(
            [&production_projection, &owner_drift] {
                static_cast<void>(make_guc_profile_delta_evidence(
                    production_projection, owner_drift));
            },
            {"mismatched owner artifact seals"},
            "malformed sealed GUC owner hash was accepted");
    }
    {
        guc_inventory wrong_profile = production;
        wrong_profile.profile = guc_profile::test_indexes;
        guc_contract_detail::require_rejected(
            [&wrong_profile, &test_indexes] {
                static_cast<void>(prove_guc_profile_delta(
                    wrong_profile, test_indexes));
            },
            {"requires production then test_indexes"},
            "GUC profile-role identity drift was accepted");
    }
    {
        guc_inventory equal_count_drift = production;
        equal_count_drift.gucs.front().name =
            "bioseq.equal_count_identity_drift";
        require(
            equal_count_drift.gucs.size() == production.gucs.size(),
            "equal-count profile mutation changed row count");
        guc_contract_detail::require_rejected(
            [&equal_count_drift, &test_indexes] {
                static_cast<void>(prove_guc_profile_delta(
                    equal_count_drift, test_indexes));
            },
            {"single documented test-only setting"},
            "equal-count GUC registration identity drift was accepted");
    }

    const std::string& root_facet =
        guc_contract_detail::owner(
            test_package, "bioseq").content;
    require(
        root_facet.contains(
            "{\"debug\", static_cast<int>(::bioseq::"
            "PlannerDebugLevel::debug2), true}"),
        "hidden planner alias was not generated");
    require(
        root_facet.contains(
            "static_assert(static_cast<int>(::bioseq::"
            "PlannerDebugLevel::debug5) == DEBUG5"),
        "PostgreSQL planner-level wire assertions were not generated");
    require(
        root_facet.contains(
            "&::bioseq::settings::logging, false,")
            && root_facet.contains("PGC_SUSET")
            && !root_facet.contains("::bioseq::false")
            && !root_facet.contains("::bioseq::true"),
        "C++ literal boot values were qualified as owner constants");
    require(
        root_facet.contains(
            "#if (defined(HAVE_CITUS_LIBLZ4) && "
            "HAVE_CITUS_LIBLZ4)")
            && root_facet.contains(
                "#if (defined(HAVE_LIBZSTD) && HAVE_LIBZSTD)"),
        "compression feature predicate was not propagated");

    const std::string manifest = merge_guc_manifest(
        "{\n  \"manifest_version\": 1\n}\n",
        production,
        production_package,
        &production,
        &production_package,
        &test_indexes,
        &test_package);
    const std::string sealed_manifest = merge_guc_manifest(
        "{\n  \"manifest_version\": 1\n}\n",
        production,
        production_package,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &sealed_evidence);
    require(
        sealed_manifest == manifest,
        "sealed GUC delta evidence renders differently from the "
        "inventory-derived authority");
    guc_contract_detail::require_rejected(
        [&production,
         &production_package,
         &test_indexes,
         &test_package,
         &sealed_evidence] {
            static_cast<void>(merge_guc_manifest(
                "{\n  \"manifest_version\": 1\n}\n",
                production,
                production_package,
                &production,
                &production_package,
                &test_indexes,
                &test_package,
                &sealed_evidence));
        },
        {"two authorities"},
        "GUC manifest accepted derived and sealed delta authorities");
    guc_contract_detail::require_rejected(
        [&production, &production_package] {
            static_cast<void>(merge_guc_manifest(
                "{\n  \"manifest_version\": 1\n}\n",
                production,
                production_package,
                &production));
        },
        {"incomplete", "inventory/package pair"},
        "GUC manifest accepted an incomplete delta authority");
    require(
        manifest.contains("\"registration_count\": 76"),
        "manifest lacks the production registration count");
    require(
        manifest.contains(
            "\"authority\": {\"registration_ordinal\": 0, "
            "\"source_owner\": \"column\", \"name\": "
            "\"bioseq.logging\", \"group\": \"bioseq\", "
            "\"member\": \"logging\", \"kind\": \"bool\", "
            "\"storage_kind\": \"bool\", \"storage\": "
            "\"enable_logging\"")
            && manifest.contains(
                "\"hooks\": [null, null, null]")
            && manifest.contains(
                "\"candidate\": {\"owner\": \"bioseq\", "
                "\"cpp_storage\": \"bioseq::settings::logging\", "
                "\"storage_kind\": \"bool\", \"source_module\": "
                "\"bioseq.sql_guc_settings\"")
            && manifest.contains(
                "\"enum_omissions_name_wire\": "
                "[[\"invalid\", \"-1\"], [\"count\", \"4\"]]"),
        "manifest lacks exact GUC authority/candidate provenance");
    require(
        manifest.contains(
            "\"generated_live_authority\": {\"vartype\": \"bool\", "
            "\"context\": \"superuser\", \"unit\": null, "
            "\"boot_value\": \"off\", \"reset_value\": \"off\", "
            "\"current_value\": \"off\"")
            && manifest.contains(
                "\"raw_flags\": 0, \"visible_in_show_all\": true, "
                "\"included_in_sample\": true, \"hooks\": "
                "{\"check\": {\"symbol\": null, \"present\": false, "
                "\"behavior\": \"none\"}"),
        "manifest lacks exact generated live GUC authority");
    require(
        manifest.contains("\"description_authority\": "
                          "\"cleaned_doxygen_only\""),
        "manifest lacks Doxygen description authority");
    require(
        manifest.contains("\"parity_ledger\"")
            && manifest.contains(production.ledger_sha256)
            && manifest.contains(
                "\"path\": "
                "\"main/bioseq/testdata/"
                "doc0388_guc_parity_ledger.json\"")
            && !manifest.contains(root.generic_string()),
        "manifest lacks parity-ledger provenance");
    guc_inventory relocated_production = production;
    relocated_production.ledger_path =
        "/tmp/relocated/main/bioseq/testdata/"
        "doc0388_guc_parity_ledger.json";
    require(
        manifest
            == merge_guc_manifest(
                "{\n  \"manifest_version\": 1\n}\n",
                relocated_production,
                production_package,
                &production,
                &production_package,
                &test_indexes,
                &test_package),
        "GUC manifest changes under a relocated repository root");
    require(
        manifest.contains("\"registry_content_sha256\"")
            && manifest.contains("\"content_sha256\"")
            && manifest.contains("\"profile_delta_proof\"")
            && manifest.contains(
                "\"added_in_test_indexes\": "
                "[\"bioseq.test_force_paper_index_failure\"]"),
        "manifest lacks generated-artifact hashes or profile delta proof");
    require(
        !manifest.contains("legacy_description")
            && !manifest.contains(
                "baseline_legacy_description_sha256"),
        "manifest exposes a legacy description hash as authority");
    {
        guc_inventory provenance_mutation = production;
        provenance_mutation.gucs.front().authority.legacy_storage =
            "mutated_legacy_storage_provenance";
        const rendered_guc_package provenance_package =
            render_guc_package(provenance_mutation);
        const std::string provenance_manifest = merge_guc_manifest(
            "{\n  \"manifest_version\": 1\n}\n",
            provenance_mutation,
            provenance_package,
            &provenance_mutation,
            &provenance_package,
            &test_indexes,
            &test_package);
        require(
            provenance_manifest != manifest
                && provenance_manifest.contains(
                    "\"storage\": "
                    "\"mutated_legacy_storage_provenance\"")
                && provenance_manifest.contains(
                    "\"cpp_storage\": "
                    "\"bioseq::settings::logging\""),
            "manifest did not bind legacy storage provenance separately "
            "from the derived candidate backing cell");
    }
}

}  // namespace bioseq::codegen::sql
