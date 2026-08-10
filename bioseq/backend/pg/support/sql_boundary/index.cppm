/*-------------------------------------------------------------------------
 *
 * support/sql_boundary/index.cppm
 *
 * Bioseq facade over the common generated SQL boundary plus Bioseq-specific
 * aggregate, GUC-storage, row-codec, and vector facets.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.postgres.support.sql_boundary;

export import bioseq.postgres.sql_boundary;
export import :aggregate;
export import :guc_storage;
export import :row_codec;
export import :vector;
