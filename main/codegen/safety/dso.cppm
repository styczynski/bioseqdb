module;

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.safety:dso;

import :model;
import :validation;

export namespace bioseq::codegen::safety {

[[nodiscard]] inline dso_contract make_doc0388_dso_contract(
    std::vector<std::string> retired_symbols,
    std::vector<std::string> legacy_macro_symbols,
    bool require_complete_codegen = true) {
    return {
        .expected_main_identity_count = 270,
        .expected_main_symbol_count = 268,
        .expected_external_symbol_count = 1,
        .expected_retired_symbol_count = 117,
        .expected_shared_symbols = {
            "from_array",
            "mmsearch",
        },
        .external_symbols = {
            "bioseq_external_handler",
        },
        .retired_symbols = std::move(retired_symbols),
        .legacy_macro_symbols = std::move(legacy_macro_symbols),
        .require_complete_codegen = require_complete_codegen,
    };
}

namespace detail {

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

[[nodiscard]] inline std::map<
    std::string,
    std::vector<const dso_symbol_occurrence*>,
    std::less<>>
defined_dynamic_symbols(const dso_inventory& inventory) {
    std::map<
        std::string,
        std::vector<const dso_symbol_occurrence*>,
        std::less<>>
        result;
    for (const dso_symbol_occurrence& symbol : inventory.symbols) {
        if (symbol.defined && symbol.dynamic_visible) {
            result[symbol.name].push_back(&symbol);
        }
    }
    return result;
}

inline void require_one_export(
    const std::map<
        std::string,
        std::vector<const dso_symbol_occurrence*>,
        std::less<>>& symbols,
    std::string_view name) {
    const auto found = symbols.find(name);
    if (found == symbols.end() || found->second.size() != 1) {
        safety_failure(
            name,
            "DSO symbol must be defined and dynamically visible exactly once");
    }
    const dso_symbol_occurrence& occurrence = *found->second.front();
    if (occurrence.binding != "GLOBAL"
        || occurrence.source_tool.empty()) {
        safety_failure(name, "DSO symbol does not have global provenance");
    }
}

}  // namespace detail

inline void validate_dso_closure(
    const safety_ledger& production,
    const dso_inventory& inventory,
    const dso_contract& contract) {
    if (production.profile != compilation_profile::production) {
        detail::safety_failure(
            "DSO", "closure must use the production SQL manifest");
    }
    if (production.parents.size()
            != contract.expected_main_identity_count
        || production.symbol_groups.size()
            != contract.expected_main_symbol_count) {
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
                || detail::dso_routine_leaf(identity)
                    != group.public_symbol) {
                detail::safety_failure(
                    group.public_symbol,
                    "main symbol is shared across different SQL names");
            }
        }
    }
    if (main_identities.size()
        != contract.expected_main_identity_count) {
        detail::safety_failure(
            "DSO", "main symbol groups do not cover every SQL identity");
    }
    const std::set<std::string, std::less<>>
        expected_shared_symbols{
            contract.expected_shared_symbols.begin(),
            contract.expected_shared_symbols.end()};
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
            != contract.expected_external_symbol_count
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
    if (contract.retired_symbols.size()
        != contract.expected_retired_symbol_count) {
        detail::safety_failure(
            "DSO", "retired-symbol ownership ledger is incomplete");
    }

    const std::set<std::string, std::less<>> legacy_macro_symbols{
        contract.legacy_macro_symbols.begin(),
        contract.legacy_macro_symbols.end()};
    if (legacy_macro_symbols.size()
        != contract.legacy_macro_symbols.size()) {
        detail::safety_failure(
            "DSO", "legacy fmgr macro inventory contains duplicates");
    }
    for (const std::string& legacy : legacy_macro_symbols) {
        if (!main_symbols.contains(legacy)) {
            detail::safety_failure(
                legacy,
                "legacy fmgr macro is not owned by the main extension");
        }
        if (generated_symbols.contains(legacy)) {
            detail::safety_failure(
                legacy,
                "generated and legacy fmgr ownership overlap");
        }
    }
    std::set<std::string, std::less<>> owned_main_symbols =
        generated_symbols;
    owned_main_symbols.insert(
        legacy_macro_symbols.begin(), legacy_macro_symbols.end());
    if (owned_main_symbols != main_symbols) {
        detail::safety_failure(
            "DSO",
            "generated plus legacy fmgr ownership does not exactly cover "
            "the main-extension symbols");
    }
    if (contract.require_complete_codegen
        && (!legacy_macro_symbols.empty()
            || generated_symbols != main_symbols)) {
        detail::safety_failure(
            "DSO",
            "final code-generation closure still contains legacy fmgr "
            "owners");
    }

    const auto symbols = detail::defined_dynamic_symbols(inventory);
    std::set<std::string, std::less<>> actual_fmgr_symbols;
    for (const auto& [name, occurrences] : symbols) {
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
            "fmgr/finfo exports are not exactly 268 bioseq symbols plus "
            "the bioseq_external handler");
    }
    for (const std::string& symbol : expected_fmgr_symbols) {
        detail::require_one_export(symbols, symbol);
        detail::require_one_export(symbols, "pg_finfo_" + symbol);
    }

    std::set<std::string, std::less<>> retired{
        contract.retired_symbols.begin(),
        contract.retired_symbols.end()};
    if (retired.size() != contract.retired_symbols.size()) {
        detail::safety_failure(
            "DSO", "retired-symbol ledger contains duplicates");
    }
    for (const std::string& symbol : retired) {
        if (symbols.contains(symbol)
            || symbols.contains("pg_finfo_" + symbol)) {
            detail::safety_failure(
                symbol, "retired symbol remains exported");
        }
    }
    for (const std::string_view symbol : {
             "vchareq",
             "vcharne",
             "vchargt",
             "vcharlt",
             "vcharle",
             "vcharge",
         }) {
        if (symbols.contains(symbol)
            || symbols.contains("pg_finfo_" + std::string{symbol})) {
            detail::safety_failure(
                symbol, "legacy VARCHAR comparison export remains");
        }
    }
    for (const std::string_view symbol : {
             "vbpchareq",
             "vbpcharne",
             "vbpchargt",
             "vbpcharlt",
             "vbpcharle",
             "vbpcharge",
         }) {
        if (!main_symbols.contains(symbol)) {
            detail::safety_failure(
                symbol, "BPCHAR comparison owner is absent");
        }
        detail::require_one_export(symbols, symbol);
        detail::require_one_export(
            symbols, "pg_finfo_" + std::string{symbol});
    }
    for (const std::string_view symbol :
         {"seq_concat", "array_cat"}) {
        if (!main_symbols.contains(symbol)) {
            detail::safety_failure(
                symbol, "distinct concat entrypoint is absent");
        }
    }
}

}  // namespace bioseq::codegen::safety
