/**
 * @file
 * @brief Selects the shared-memory test case for the next SliceProject execution.
 */
export module bioseq.postgres.table:custom_scan_slice_path_diagnostic_fixture_contracts;

/** @brief Shared-memory test cases for incomplete workers and malformed records. */
export enum slice_project_dsm_test_fixture : int {
    k_slice_dsm_test_none = 0,
    k_slice_dsm_test_incomplete = 1,
    k_slice_dsm_test_malformed = 2,
};

/* Begin consumes this backend-local selection before validation, so a failed
 * test cannot oeave it armed for the next query. */
export int pending_slice_dsm_test_fixture = k_slice_dsm_test_none;
