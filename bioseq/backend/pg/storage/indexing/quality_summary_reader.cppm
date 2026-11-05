/** @file Register the snapshot-bound quality index reader at extension startup. */
export module bioseq.postgres.storage.indexing.quality_summary_reader;

export namespace bioseq::storage::indexing {

// Startup needs only this declaration, not the index/storage implementation graph.
void install_quality_source_summary_reader() noexcept;

} // namespace bioseq::storage::indexing
