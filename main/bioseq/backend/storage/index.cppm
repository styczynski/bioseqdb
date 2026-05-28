export module bioseq_storage;
// DOC0263 §2.2 — sequence_size is now a standalone NO_POSTGRES module so the
// algorithm modules can import sequence_size_t without the Postgres-only
// storage-manager partition. Re-exported here so `import bioseq_storage`
// consumers (Phase A) still see `bioseq::storage::sequence_size_t`.
export import bioseq_storage_size;
export import :sequence_storage_manager;
