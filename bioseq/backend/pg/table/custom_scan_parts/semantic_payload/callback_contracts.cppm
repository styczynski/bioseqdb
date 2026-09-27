/**
 * @file
 * @brief Collects scan callbacks and provides the SliceProject shared-memory test switch.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_semantic_payload_callback_contracts;

import std;
import bioseq.postgres.table.scan_state;

import :custom_scan_slice_path_diagnostic_fixture_contracts;
export import :custom_scan_semantic_payload_explain_callback_contracts;
export import :custom_scan_join_runtime_tuple_injection_contracts;
export import :custom_scan_semantic_payload_executor_callback_contracts;
export import :custom_scan_semantic_payload_query_sketch_route_contracts;
export import :custom_scan_scan_paths_cost_finalization_contracts;
export import :custom_scan_planner_diagnostic_contracts;
export import :custom_scan_planner_hook_state_contracts;
export import :custom_scan_planner_method_contracts;
export import :custom_scan_planner_path_contracts;

export {
    /**
     * @brief Sets a one-use shared-memory test for the next SliceProject scan.
     *
     * Accepts "incomplete", "malformed" or "none". A test must be consumed or
     * cleared before another can be armed. Available only with BIOSEQ_TEST_INDEXES.
     */
    void bioseq_arm_slice_dsm_fixture_for_test(std::string_view mode) {
#ifdef BIOSEQ_TEST_INDEXES
        int requested_mode = k_slice_dsm_test_none;
        if (mode == "incomplete") {
            requested_mode = k_slice_dsm_test_incomplete;
        } else if (mode == "malformed") {
            requested_mode = k_slice_dsm_test_malformed;
        } else if (mode != "none") {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("unknown SliceProject DSM diagnostic fixture"),
                     errdetail("fixture=%.*s", static_cast<int>(mode.size()), mode.data()),
                     errhint("Expected incomplete, malformed, or none.")));
        }
        if (requested_mode == k_slice_dsm_test_none) {
            pending_slice_dsm_test_fixture = k_slice_dsm_test_none;
            return;
        }
        if (pending_slice_dsm_test_fixture != k_slice_dsm_test_none) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("a SliceProject DSM diagnostic fixture is already armed"),
                     errhint("Run the analyzed SliceProject or disarm it with fixture 'none'.")));
        }
        pending_slice_dsm_test_fixture = requested_mode;
#else
        (void)mode;
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("SliceProject DSM diagnostics require a test build"),
                 errdetail("The extension was built without BIOSEQ_TEST_INDEXES.")));
#endif
    }

} // export custom-scan partition declarations
