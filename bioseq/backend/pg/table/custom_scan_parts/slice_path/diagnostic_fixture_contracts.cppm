/**
 * @file
 * @brief Owns the backend-local SliceProject DSM diagnostic fixture selection.
 */
export module bioseq.postgres.table:custom_scan_slice_path_diagnostic_fixture_contracts;

/** @brief Selects a one-shot malformed DSM condition for integration diagnostics. */
export enum slice_project_dsm_test_fixture : int {
    k_slice_dsm_test_none = 0,
    k_slice_dsm_test_incomplete = 1,
    k_slice_dsm_test_malformed = 2,
};

/* Backend-local, one-shot diagnostic state. The next analyzed SliceProject
 * consumes the selected mode before any fixture code can raise an error. */
export int pending_slice_dsm_test_fixture = k_slice_dsm_test_none;
