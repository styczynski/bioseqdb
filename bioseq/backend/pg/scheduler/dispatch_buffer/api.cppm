/**
 * @file
 * @brief Declares the custom-scan API for buffered sequence dispatch.
 */
/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer/api.cppm
 *
 * Narrow custom-scan-facing API for tuple_dispatch_buffer.  The primary
 * goal is to keep table/custom_scan.cppm from importing the full
 * bioseq.postgres.scheduler.dispatch_buffer BMI: GCC 16.1 crashes while loading
 * that heavy exported module from custom_scan, but the executor only
 * needs an opaque buffer handle plus a small row/match view.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_buffer.api;

export import bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;
