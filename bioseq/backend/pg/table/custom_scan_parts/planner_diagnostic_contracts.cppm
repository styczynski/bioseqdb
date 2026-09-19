/**
 * @file
 * @brief Exposes planner diagnostics without importing callback implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_planner_diagnostic_contracts;

import bioseq.postgres.configuration.core;

/** @brief Return the configured PostgreSQL planner diagnostic severity. */
export [[nodiscard]] int planner_debug_level() noexcept {
    return static_cast<int>(bioseq::settings::planner::diagnostics::log_level.get_value());
}
