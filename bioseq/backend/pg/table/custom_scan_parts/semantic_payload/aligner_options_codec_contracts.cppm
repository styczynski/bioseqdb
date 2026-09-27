/**
 * @file
 * @brief Names the generated SQL options row and its decoder.
 */
export module bioseq.postgres.table:custom_scan_aligner_options_codec_contracts;

import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;

export {

/** @brief C++ form of the optional settings in an SQL aligner_options value. */
using aligner_options_catalog_row = bioseq::table::composite_rows::bioseq_aligner_options;

/** @brief Decodes the PostgreSQL options record into its generated C++ row. */
using aligner_options_codec =
    bioseq::postgres::table::sql_row_codecs::bioseq_aligner_options_codec;

} // export aligner-options codec aliases
