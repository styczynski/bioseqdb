/**
 * @file metadata_age_log_test_support.cppm
 * @brief Test repeated writes to the index metadata-age catalog.
 */
export module bioseq.postgres.storage.indexing.metadata_age_log_test_support;

export namespace bioseq::storage::indexing {

/// Repeatedly update one metadata-age row, then check that deleting it removes one row.
void exercise_metadata_age_log_context_stress();

} // namespace bioseq::storage::indexing
