/**
 * @file index.cppm
 * @brief PostgreSQL chunked-index storage module aggregation interface.
 *
 * The semantic partitions keep catalog and row codecs, transaction-owned file
 * actions, mapped fragment images, read sessions, hash-major persistence, and
 * concrete storage adapters independently owned while preserving one public
 * module for existing consumers.
 * Implementation partitions remain reachable only through their stable
 * facade partitions.
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
