/**
 * @file index.cppm
 * @brief Reexports the complete PostgreSQL SQL-boundary API.
 *
 * Combines the common generated boundary with Bioseq aggregate, GUC-storage,
 * row-codec, and vector facets.
 */
export module bioseq.postgres.support.sql_boundary;

export import bioseq.postgres.sql_boundary;
export import :aggregate;
export import :guc_storage;
export import :row_codec;
export import :vector;
