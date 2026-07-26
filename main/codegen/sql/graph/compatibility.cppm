module;

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.graph:compatibility;

import :model;
import bioseq.codegen.sql.collector;

export namespace bioseq::codegen::sql {

namespace physical_ledger_detail {

[[nodiscard]] inline std::string trim_literal(
    std::string_view expression) {
    while (!expression.empty()
           && (expression.front() == ' ' || expression.front() == '\t'
               || expression.front() == '\r'
               || expression.front() == '\n')) {
        expression.remove_prefix(1);
    }
    while (!expression.empty()
           && (expression.back() == ' ' || expression.back() == '\t'
               || expression.back() == '\r'
               || expression.back() == '\n')) {
        expression.remove_suffix(1);
    }
    if (expression.size() >= 2
        && ((expression.front() == '"' && expression.back() == '"')
            || (expression.front() == '\''
                && expression.back() == '\''))) {
        expression.remove_prefix(1);
        expression.remove_suffix(1);
    }
    return std::string{expression};
}

[[nodiscard]] inline std::string physical_option(
    const collected_entity& entity) {
    const option_value* option =
        find_option(entity, "baseline_physical_id");
    if (option == nullptr) {
        throw std::runtime_error(
            entity.source_path.string() + ":"
            + std::to_string(entity.source_range.begin.line)
            + ": typed SQL install owner '" + entity.cpp_qualified_name
            + "' is missing .baseline_physical_id");
    }
    return trim_literal(option->expression);
}

[[nodiscard]] inline std::string canonical_physical_id(
    std::size_t ordinal) {
    std::string digits = std::to_string(ordinal);
    if (digits.size() < 3) {
        digits.insert(digits.begin(), 3 - digits.size(), '0');
    }
    return "P" + digits;
}

inline void write_legacy_primary_compatibility(
    collected_entity& natural,
    const physical_statement_binding& binding) {
    if (binding.facet != physical_statement_facet::primary
        || !natural.compatibility_sql_initializer.empty()) {
        return;
    }
    natural.compatibility_sql_initializer =
        binding.compatibility_sql_initializer;
    natural.compatibility_order_identity = binding.order_identity;
    natural.compatibility_source_module = binding.source_module;
    natural.compatibility_source_path = binding.source_path;
    natural.compatibility_source_sha256 = binding.source_sha256;
    natural.compatibility_source_range = binding.source_range;
    natural.compatibility_baseline_physical_id =
        binding.baseline_physical_id;
}

}  // namespace physical_ledger_detail

[[nodiscard]] inline std::size_t parse_baseline_physical_ordinal(
    std::string_view physical_id) {
    if (physical_id.size() < 4 || physical_id.front() != 'P') {
        throw std::runtime_error(
            "baseline physical ID '" + std::string{physical_id}
            + "' must use canonical PNNN decimal spelling");
    }
    const std::string_view digits = physical_id.substr(1);
    if (!std::ranges::all_of(
            digits,
            [](char value) {
                return value >= '0' && value <= '9';
            })) {
        throw std::runtime_error(
            "baseline physical ID '" + std::string{physical_id}
            + "' must use canonical PNNN decimal spelling");
    }
    std::size_t ordinal{};
    const auto [end, error] = std::from_chars(
        digits.data(), digits.data() + digits.size(), ordinal);
    if (error != std::errc{} || end != digits.data() + digits.size()
        || ordinal == 0
        || physical_ledger_detail::canonical_physical_id(ordinal)
            != physical_id) {
        throw std::runtime_error(
            "baseline physical ID '" + std::string{physical_id}
            + "' must use canonical PNNN decimal spelling");
    }
    return ordinal;
}

[[nodiscard]] inline physical_statement_binding
make_typed_install_physical_statement(
    const collected_entity& install) {
    if (!install.typed_install_owner) {
        throw std::runtime_error(
            "physical compatibility binding requires a typed install owner");
    }
    const std::string physical_id =
        physical_ledger_detail::physical_option(install);
    return physical_statement_binding{
        .kind = install.kind,
        .facet = physical_statement_facet::primary,
        .render_source =
            physical_render_source::compatibility_sql,
        .baseline_physical_id = physical_id,
        .baseline_ordinal =
            parse_baseline_physical_ordinal(physical_id),
        .order_identity = "sql:install-owner:" + install.name,
        .compatibility_sql_initializer =
            install.initializer_expression,
        .source_module = install.source_module,
        .source_path = install.source_path,
        .source_sha256 = install.source_sha256,
        .source_range = install.source_range,
    };
}

[[nodiscard]] inline std::vector<physical_statement_binding>
effective_physical_statement_bindings(
    const collected_entity& entity) {
    if (!entity.physical_statements.empty()) {
        return entity.physical_statements;
    }
    if (!entity.compatibility_sql_initializer.empty()) {
        physical_statement_binding binding{
            .kind = entity.kind,
            .facet = physical_statement_facet::primary,
            .render_source =
                physical_render_source::compatibility_sql,
            .baseline_physical_id =
                entity.compatibility_baseline_physical_id,
            .order_identity = entity.compatibility_order_identity,
            .compatibility_sql_initializer =
                entity.compatibility_sql_initializer,
            .source_module = entity.compatibility_source_module,
            .source_path = entity.compatibility_source_path,
            .source_sha256 = entity.compatibility_source_sha256,
            .source_range = entity.compatibility_source_range,
        };
        if (!binding.baseline_physical_id.empty()) {
            binding.baseline_ordinal =
                parse_baseline_physical_ordinal(
                    binding.baseline_physical_id);
        }
        return {std::move(binding)};
    }
    if (entity.typed_install_owner) {
        return {make_typed_install_physical_statement(entity)};
    }
    return {};
}

[[nodiscard]] inline std::vector<baseline_physical_statement>
capture_baseline_physical_ledger(collected_program& program) {
    std::vector<baseline_physical_statement> result;
    std::map<std::string, std::string, std::less<>> owners;
    for (collected_entity& entity : program.entities) {
        if (!entity.typed_install_owner) continue;
        if (entity.physical_statements.empty()) {
            entity.physical_statements.push_back(
                make_typed_install_physical_statement(entity));
        }
        if (entity.physical_statements.size() != 1) {
            throw std::runtime_error(
                "typed SQL install owner '" + entity.cpp_qualified_name
                + "' must own exactly one baseline physical statement");
        }
        const physical_statement_binding& binding =
            entity.physical_statements.front();
        const std::size_t parsed_ordinal =
            parse_baseline_physical_ordinal(
                binding.baseline_physical_id);
        if (binding.baseline_ordinal != parsed_ordinal) {
            throw std::runtime_error(
                "typed SQL install owner '"
                + entity.cpp_qualified_name
                + "' has an inconsistent baseline physical ordinal for '"
                + binding.baseline_physical_id + "'");
        }
        if (binding.kind != entity.kind) {
            throw std::runtime_error(
                "typed SQL install owner '"
                + entity.cpp_qualified_name
                + "' has a baseline physical statement with a different "
                  "SQL kind");
        }
        const auto [previous, inserted] = owners.emplace(
            binding.baseline_physical_id,
            entity.cpp_qualified_name);
        if (!inserted) {
            throw std::runtime_error(
                "duplicate baseline physical ID '"
                + binding.baseline_physical_id + "' on '"
                + previous->second + "' and '"
                + entity.cpp_qualified_name + "'");
        }
        result.push_back(baseline_physical_statement{
            .baseline_physical_id = binding.baseline_physical_id,
            .baseline_ordinal = binding.baseline_ordinal,
            .kind = binding.kind,
            .order_identity = binding.order_identity,
            .source_module = binding.source_module,
            .source_path = binding.source_path,
            .source_sha256 = binding.source_sha256,
            .source_range = binding.source_range,
        });
    }
    std::ranges::sort(
        result,
        {},
        &baseline_physical_statement::baseline_ordinal);
    return result;
}

inline void adopt_typed_install_statement(
    collected_entity& natural,
    const collected_entity& install,
    physical_statement_facet facet =
        physical_statement_facet::primary,
    physical_render_source render_source =
        physical_render_source::compatibility_sql) {
    std::vector<physical_statement_binding> bindings =
        effective_physical_statement_bindings(install);
    if (bindings.size() != 1) {
        throw std::runtime_error(
            "typed SQL install owner '" + install.cpp_qualified_name
            + "' must contribute exactly one physical statement");
    }
    physical_statement_binding binding = std::move(bindings.front());
    binding.facet = facet;
    binding.render_source = render_source;
    const auto duplicate = std::ranges::find(
        natural.physical_statements,
        binding.baseline_physical_id,
        &physical_statement_binding::baseline_physical_id);
    if (duplicate != natural.physical_statements.end()) {
        throw std::runtime_error(
            "natural SQL owner '" + natural.cpp_qualified_name
            + "' adopts duplicate baseline physical ID '"
            + binding.baseline_physical_id + "'");
    }
    physical_ledger_detail::write_legacy_primary_compatibility(
        natural, binding);
    natural.physical_statements.push_back(std::move(binding));
}

inline void validate_physical_statement_ownership(
    const collected_program& program,
    const std::vector<baseline_physical_statement>& expected) {
    std::map<std::string, const baseline_physical_statement*, std::less<>>
        expected_by_id;
    for (const baseline_physical_statement& statement : expected) {
        const std::size_t ordinal =
            parse_baseline_physical_ordinal(
                statement.baseline_physical_id);
        if (ordinal != statement.baseline_ordinal) {
            throw std::runtime_error(
                "baseline physical statement '"
                + statement.baseline_physical_id
                + "' has an inconsistent numeric ordinal");
        }
        if (!expected_by_id.emplace(
                statement.baseline_physical_id, &statement).second) {
            throw std::runtime_error(
                "baseline ledger contains duplicate physical ID '"
                + statement.baseline_physical_id + "'");
        }
    }
    std::map<std::string, std::string, std::less<>> owner_by_id;
    for (const collected_entity& entity : program.entities) {
        for (const physical_statement_binding& binding :
             entity.physical_statements) {
            const std::size_t ordinal =
                parse_baseline_physical_ordinal(
                    binding.baseline_physical_id);
            if (ordinal != binding.baseline_ordinal) {
                throw std::runtime_error(
                    "natural SQL owner '" + entity.cpp_qualified_name
                    + "' has an inconsistent physical ordinal for '"
                    + binding.baseline_physical_id + "'");
            }
            const auto expected_position =
                expected_by_id.find(binding.baseline_physical_id);
            if (expected_position == expected_by_id.end()) {
                throw std::runtime_error(
                    "natural SQL owner '" + entity.cpp_qualified_name
                    + "' contains unknown baseline physical ID '"
                    + binding.baseline_physical_id + "'");
            }
            if (expected_position->second->kind != binding.kind) {
                throw std::runtime_error(
                    "baseline physical ID '"
                    + binding.baseline_physical_id
                    + "' changed SQL statement kind during reconciliation");
            }
            const auto [previous, inserted] = owner_by_id.emplace(
                binding.baseline_physical_id,
                entity.cpp_qualified_name);
            if (!inserted) {
                throw std::runtime_error(
                    "baseline physical ID '"
                    + binding.baseline_physical_id
                    + "' is owned by both '" + previous->second
                    + "' and '" + entity.cpp_qualified_name + "'");
            }
        }
    }
    for (const baseline_physical_statement& statement : expected) {
        if (!owner_by_id.contains(statement.baseline_physical_id)) {
            throw std::runtime_error(
                "baseline physical ID '"
                + statement.baseline_physical_id
                + "' was lost during natural-owner reconciliation");
        }
    }
}

}  // namespace bioseq::codegen::sql
