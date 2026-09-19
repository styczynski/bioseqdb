/**
 * @file metadata_age_log_test_support.cppm
 * @brief Narrow diagnostic boundary for metadata-age catalog stress checks.
 */
export module bioseq.postgres.storage.indexing.metadata_age_log_test_support;

export namespace bioseq::storage::indexing {

/// Exercise repeated metadata-age upserts and the matching catalog delete.
void exercise_metadata_age_log_context_stress();

} // namespace bioseq::storage::indexing
