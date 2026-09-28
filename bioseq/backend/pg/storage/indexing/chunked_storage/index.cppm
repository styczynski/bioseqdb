/**
 * @file index.cppm
 * @brief Read and write indexes stored in PostgreSQL chunk tables.
 *
 * This module exports the catalog helpers, generation readers and writers,
 * and storage adapters used by the PostgreSQL backend. It  also exports the
 * mapped index image code and its transaction cleanup helpers.
 */
export module bioseq.postgres.storage.indexing.chunked_storage;

export import :catalog;
export import :catalog_row_io;
export import :file_actions;
export import :fragment_image;
export import :generation_writer;
export import :generation_session;
export import :hash_major_writer;
export import :storage_adapters;
