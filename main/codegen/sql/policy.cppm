module;

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

export module bioseq.codegen.sql:policy;

import :model;
import bioseq.codegen.safety;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

enum class final_physical_ledger_state : std::uint8_t {
    missing,
    checked_input,
    transition_output,
};

[[nodiscard]] inline final_physical_ledger_state
classify_final_physical_ledger_state(
    const command_line& command) {
    if (!command.physical_ledger.empty()
        && !command.physical_ledger_output.empty()) {
        throw std::runtime_error(
            "FINAL physical ledger input and transition output are "
            "mutually exclusive");
    }
    if (!command.physical_ledger.empty()) {
        return final_physical_ledger_state::checked_input;
    }
    if (!command.physical_ledger_output.empty()) {
        return final_physical_ledger_state::transition_output;
    }
    return final_physical_ledger_state::missing;
}

/**
 * Require an explicit physical-ledger authority state for complete FINAL
 * generation of the main extension, whose annotated owners reconcile the
 * legacy install ledger. Sibling extensions have no inherited physical
 * ledger: complete natural generation is their terminal state unless a
 * checked-ledger cutover is explicitly requested. Transition output remains
 * available only when selected explicitly; the cutover switch accepts checked
 * input only.
 */
inline void validate_final_physical_ledger_policy(
    const command_line& command) {
    if (command.require_checked_physical_ledger
        && (!command.require_complete_codegen
            || command.stage != generation_stage::final
            || command.action == command_action::prepare
            || command.action == command_action::migrate_baseline)) {
        throw std::runtime_error(
            "--require-checked-physical-ledger is valid only for "
            "complete FINAL generation");
    }
    if (command.stage != generation_stage::final
        || !command.require_complete_codegen
        || command.action == command_action::prepare
        || command.action == command_action::migrate_baseline) {
        return;
    }

    const final_physical_ledger_state state =
        classify_final_physical_ledger_state(command);
    if (command.extension_name != "bioseq"
        && !command.require_checked_physical_ledger
        && state == final_physical_ledger_state::missing) {
        return;
    }
    if (state == final_physical_ledger_state::missing) {
        throw std::runtime_error(
            "complete FINAL generation requires an explicit physical "
            "ledger state: checked --physical-ledger input or bounded "
            "transition --physical-ledger-output");
    }
    if (command.require_checked_physical_ledger
        && state != final_physical_ledger_state::checked_input) {
        throw std::runtime_error(
            "checked-ledger cutover requires exactly --physical-ledger "
            "input and rejects transition output");
    }
}

/**
 * Enforce the final migration state when the auditable completion switch is
 * enabled. Transitional install owners and main-extension macro wrappers are
 * allowed while the switch is off; the external-extension handler is outside
 * this policy.
 */
inline void validate_codegen_completion_policy(
        const sql_graph& graph,
        bool require_complete_codegen) {
    if (!require_complete_codegen) return;

    std::vector<std::string> transitional;
    for (const normalized_entity& entity : graph.entities) {
        if (entity.source.typed_install_owner) {
            transitional.push_back(entity.stable_identity);
        }
    }
    if (!transitional.empty()) {
        std::string identities;
        for (std::size_t index = 0;
             index < transitional.size();
             ++index) {
            if (!identities.empty()) identities += ", ";
            identities += "'" + transitional[index] + "'";
        }
        throw std::runtime_error(
            "complete SQL codegen requires zero transitional install "
            "owners; remaining count is "
            + std::to_string(transitional.size()) + ": "
            + identities);
    }

    for (const normalized_entity& entity : graph.entities) {
        const auto bindings =
            effective_physical_statement_bindings(entity.source);
        const auto compatibility = std::ranges::find_if(
            bindings,
            [](const auto& binding) {
                return binding.render_source
                    == physical_render_source::compatibility_sql;
            });
        if (compatibility != bindings.end()) {
            throw std::runtime_error(
                "complete SQL codegen requires every physical statement "
                "to use a natural projection; first remaining "
                "compatibility source is '"
                + compatibility->baseline_physical_id + "' on '"
                + entity.stable_identity + "'");
        }
    }

    const auto main_legacy = std::ranges::find_if(
        graph.legacy_fmgr_macro_owners,
        [](const legacy_fmgr_macro_owner& owner) {
            return !std::ranges::contains(
                safety::doc0388_external_fmgr_symbols,
                owner.symbol);
        });
    if (main_legacy != graph.legacy_fmgr_macro_owners.end()) {
        throw std::runtime_error(
            "complete SQL codegen requires zero main-extension legacy "
            "fmgr macro owners; first remaining symbol is '"
            + main_legacy->symbol + "'");
    }
}

}  // namespace bioseq::codegen::sql
