/**
 * @file
 * @brief Defines the SQL function and type OIDs needed by custom scans.
 */
module;
extern "C" {
#include "postgres_ext.h"
}

export module bioseq.postgres.table:custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Stores exact catalog OIDs for the SQL types and functions used by custom scans. */
export struct bioseq_stub_oids {
    Oid mmsearch_oid;
    Oid mmsearch_with_opts_oid;
    Oid bioseq_seq_type_oid;
    Oid bioseq_aligner_options_type_oid;

    Oid score_oid;
    Oid dp_score_oid;
    Oid query_len_oid;
    Oid query_start_oid;
    Oid query_end_oid;
    Oid target_len_oid;
    Oid target_start_oid;
    Oid target_end_oid;
    Oid num_matches_oid;
    Oid block_length_oid;
    Oid edit_oid;
    Oid mapq_oid;
    Oid chain_score_oid;
    Oid dp_max_score_oid;
    Oid seed_count_oid;
    Oid secondary_chain_score_oid;
    Oid is_reverse_oid;
    Oid is_primary_oid;
    Oid has_cigar_oid;
    Oid divergence_oid;
    Oid cigar_oid;
    Oid symbol_count_oid;
    Oid seq_concat_oid;
    Oid seq_concat_text_oid;
    Oid text_concat_seq_oid;
    Oid seq_concat_varchar_oid;
    Oid varchar_concat_seq_oid;
    Oid seq_concat_text_array_oid;
    Oid text_array_concat_seq_oid;
    Oid seq_concat_letter_array_oid;
    Oid letter_array_concat_seq_oid;
    Oid seq_reverse_oid;
    Oid seq_reverse_complement_oid;
    Oid seq_array_cat_oid;
    Oid seq_array_length_oid;
    Oid seq_cardinality_oid;
    Oid seq_to_text_oid;
    Oid seq_to_varchar_oid;
    Oid seq_to_array_oid;
    Oid seq_from_letter_array_infer_oid;
    Oid seq_from_letter_array_named_oid;
    Oid seq_len_oid;
    Oid seq_cast_oid;
};

/** @brief Returns this backend's Bioseq OIDs, looking up required objects as needed. */
export [[nodiscard]] const bioseq_stub_oids& get_cached_stub_oids();

/** @brief Returns Bioseq OIDs, or null when required extension objects are unavailable. */
export [[nodiscard]] const bioseq_stub_oids* try_get_cached_stub_oids();

/** @brief Checks whether the function is a match-result helper such as score or cigar. */
export [[nodiscard]] constexpr bool
is_bioseq_projection_funcid(const Oid funcid, const bioseq_stub_oids& o) noexcept {
    return funcid == o.score_oid || funcid == o.dp_score_oid || funcid == o.query_len_oid ||
           funcid == o.query_start_oid || funcid == o.query_end_oid ||
           funcid == o.target_len_oid || funcid == o.target_start_oid ||
           funcid == o.target_end_oid || funcid == o.num_matches_oid ||
           funcid == o.block_length_oid || funcid == o.edit_oid || funcid == o.mapq_oid ||
           funcid == o.chain_score_oid || funcid == o.dp_max_score_oid ||
           funcid == o.seed_count_oid || funcid == o.secondary_chain_score_oid ||
           funcid == o.is_reverse_oid || funcid == o.is_primary_oid ||
           funcid == o.has_cigar_oid || funcid == o.divergence_oid || funcid == o.cigar_oid;
}
