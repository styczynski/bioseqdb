/**
 * @file
 * @brief Exposes the configured log level for planner diagnostics.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_planner_diagnostic_contracts;

import bioseq.postgres.configuration.core;

/** @brief Returns the PostgreSQL log level selected for planner diagnostics. */
export [[nodiscard]] int planner_debug_level() noexcept {
    return static_cast<int>(bioseq::settings::planner::diagnostics::log_level.get_value());
}
