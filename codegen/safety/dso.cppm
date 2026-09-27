module;

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.safety:dso;

import :model;
import :validation;

export namespace bioseq::codegen::safety {

/// Returns the expected SQL function counts and exported symbols.
///
/// Generated wrappers belong to the main extension.
/// `bioseq_external_handler` belongs to the external-data extension.
[[nodiscard]] inline dso_contract current_dso_contract() {
    return {
        .main_identity_count = 272,
        .main_symbol_count = 270,
        .external_symbol_count = 1,
        .shared_symbols = {
            "from_array",
            "mmsearch",
        },
        .external_symbols = {
            "bioseq_external_handler",
        },
    };
}

namespace dso_detail {

using dynamic_export_index = std::map<
    std::string,
    std::vector<const dso_symbol_occurrence*>,
    std::less<>>;

[[nodiscard]] inline std::string dso_routine_leaf(
    std::string_view identity) {
    const std::size_t arguments = identity.find('(');
    const std::string_view qualified =
        arguments == std::string_view::npos
        ? identity
        : identity.substr(0, arguments);
    const std::size_t separator = qualified.rfind('.');
    return std::string{
        separator == std::string_view::npos
            ? qualified
            : qualified.substr(separator + 1)};
}

[[nodiscard]] inline dynamic_export_index
index_dynamic_exports(const dso_inventory& inventory) {
    dynamic_export_index result;
    for (const dso_symbol_occurrence& symbol : inventory.symbols) {
        if (symbol.defined && symbol.dynamic_visible) {
            result[symbol.name].push_back(&symbol);
        }
    }
    return result;
}

inline void require_unique_global_export(
    const dynamic_export_index& symbols,
    std::string_view name) {
    const dynamic_export_index::const_iterator found =
        symbols.find(name);
    if (found == symbols.end() || found->second.size() != 1) {
        detail::safety_failure(
            name,
            "DSO symbol must be defined and dynamically visible exactly once");
    }
    const dso_symbol_occurrence& occurrence = *found->second.front();
    if (occurrence.binding != "GLOBAL"
        || occurrence.source_tool.empty()) {
        detail::safety_failure(
            name, "DSO symbol does not have global provenance");
    }
}

}  // namespace dso_detail

/// Checks that each SQL function has the expected generated wrapper and C export.
///
/// Each public symbol and its `pg_finfo_` companion must appear once as a
/// global dynamic export, with the inspection tool recorded. The external-data
/// extension must own only `bioseq_external_handler`.
inline void validate_dso_ownership(
    const safety_ledger& production,
    const dso_inventory& inventory,
    const dso_contract& contract) {
    if (production.profile != compilation_profile::production) {
        detail::safety_failure(
            "DSO", "ownership validation requires the production ledger");
    }
    if (production.parents.size()
            != contract.main_identity_count
        || production.symbol_groups.size()
            != contract.main_symbol_count) {
        detail::safety_failure(
            "DSO", "main SQL identity/symbol totals are not exact");
    }
    std::set<std::string, std::less<>> main_symbols;
    std::set<std::string, std::less<>> generated_symbols;
    std::set<std::string, std::less<>> main_identities;
    std::set<std::string, std::less<>> shared_symbols;
    for (const symbol_group& group : production.symbol_groups) {
        if (group.public_symbol.empty()
            || group.sql_identities.empty()
            || !main_symbols.insert(group.public_symbol).second
            || group.finfo_symbol
                != "pg_finfo_" + group.public_symbol) {
            detail::safety_failure(
                group.public_symbol,
                "main symbol owner is duplicated or has the wrong finfo");
        }
        if (!group.generated_wrapper.empty()) {
            generated_symbols.insert(group.public_symbol);
        }
        if (group.sql_identities.size() > 1) {
            shared_symbols.insert(group.public_symbol);
        }
        for (const std::string& identity : group.sql_identities) {
            if (!main_identities.insert(identity).second
                || dso_detail::dso_routine_leaf(identity)
                    != group.public_symbol) {
                detail::safety_failure(
                    group.public_symbol,
                    "main symbol is shared across different SQL names");
            }
        }
    }
    if (main_identities.size()
        != contract.main_identity_count) {
        detail::safety_failure(
            "DSO", "main symbol groups do not cover every SQL identity");
    }
    const std::set<std::string, std::less<>>
        expected_shared_symbols{
            contract.shared_symbols.begin(),
            contract.shared_symbols.end()};
    if (shared_symbols != expected_shared_symbols) {
        detail::safety_failure(
            "DSO",
            "same-name overload sharing is not exactly from_array/mmsearch");
    }

    std::set<std::string, std::less<>> external_symbols{
        contract.external_symbols.begin(),
        contract.external_symbols.end()};
    const std::set<std::string, std::less<>> required_external{
        "bioseq_external_handler",
    };
    if (external_symbols.size()
            != contract.external_symbol_count
        || external_symbols != required_external) {
        detail::safety_failure(
            "DSO",
            "bioseq_external disposition must contain only its handler");
    }
    for (const std::string& symbol : external_symbols) {
        if (main_symbols.contains(symbol)) {
            detail::safety_failure(
                symbol,
                "bioseq and bioseq_external symbol ownership overlaps");
        }
    }
    if (generated_symbols != main_symbols) {
        detail::safety_failure(
            "DSO",
            "generated wrappers do not exactly own the main-extension "
            "symbols");
    }

    const dso_detail::dynamic_export_index symbols =
        dso_detail::index_dynamic_exports(inventory);
    std::set<std::string, std::less<>> actual_fmgr_symbols;
    for (const dso_detail::dynamic_export_index::value_type& entry :
         symbols) {
        const std::string& name = entry.first;
        const std::vector<const dso_symbol_occurrence*>& occurrences =
            entry.second;
        if (!name.starts_with("pg_finfo_")) continue;
        if (occurrences.size() != 1) {
            detail::safety_failure(
                name, "pg_finfo symbol is defined more than once");
        }
        actual_fmgr_symbols.insert(
            name.substr(std::string_view{"pg_finfo_"}.size()));
    }
    std::set<std::string, std::less<>> expected_fmgr_symbols =
        main_symbols;
    expected_fmgr_symbols.insert(
        external_symbols.begin(), external_symbols.end());
    if (actual_fmgr_symbols != expected_fmgr_symbols) {
        detail::safety_failure(
            "DSO",
            "FMGR/finfo exports differ from current main/external "
            "symbol ownership");
    }
    for (const std::string& symbol : expected_fmgr_symbols) {
        dso_detail::require_unique_global_export(symbols, symbol);
        dso_detail::require_unique_global_export(
            symbols, "pg_finfo_" + symbol);
    }
}

}  // namespace bioseq::codegen::safety
