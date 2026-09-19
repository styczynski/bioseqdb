/**
 * @file
 * @brief Owns the generated aligner-options row and codec aliases.
 */
export module bioseq.postgres.table:custom_scan_aligner_options_codec_contracts;

import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;

export {

/** @brief Catalog row carrying one query's optional aligner overrides. */
using aligner_options_catalog_row = bioseq::table::composite_rows::bioseq_aligner_options;

/** @brief Codec used to decode aligner overrides from their PostgreSQL composite value. */
using aligner_options_codec =
    bioseq::postgres::table::sql_row_codecs::bioseq_aligner_options_codec;

} // export aligner-options codec aliases
