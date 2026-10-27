/**
 * @file
 * @brief Declares edit-predicate checks and alignment-feature  detection for scan expressions.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_oid_expression_support_semantic_analysis_contracts;

import std;
import bioseq.postgres.support;
import :custom_scan_scan_feature_contracts;
import :custom_scan_oid_expression_support_expression_analysis_contracts;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
export import :custom_scan_oid_expression_support_edit_sequence_recipe_contracts;

using namespace bioseq::postgres::support;

/** @brief Identifies the saved edit-predicate record and its format version. */
export inline constexpr int k_bioseq_edit_predicate_magic = 0x42534531; /* BSE1 */
export inline constexpr int k_bioseq_edit_predicate_version = 1;

/** @brief Stores the sequence column and exclusive edit-distance, limit checked after alignment. */
export struct strict_edit_predicate {
    std::int64_t exclusive_upper_bound = 0;
    AttrNumber sequence_attno = InvalidAttrNumber;
};

/** @brief Stores the target alphabet and storage-unit width for an EXPLAIN conversion hint. */
export struct explain_typmod_coalesce {
    bool active = false;
    uint8_t col_alpha = 0;
    uint8_t col_byte_width = 0;
};

/** @brief Collects the alignmqnt features requested by mmsearch and match helpers. */
export struct bioseq_feature_detector {
    const bioseq_stub_oids* oids = nullptr;
    scan_feature_flags flags;
};

/** @brief Marks which alignment features the expression's function calls need */
export struct feature_flag_visitor {
    /** @brief Updates feature flags for the current node and continues traversal. */
    [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                          pg_node_view node,
                                          bioseq_feature_detector& detector) const {
        if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
            const bioseq_stub_oids* o = detector.oids;
            if (is_mmsearch_funcid(fe->funcid, *o)) {
                detector.flags.has_mmsearch = true;
            }
            // The full CIGAR seq projection requests the same base-alignment
            // stage as has_cigar; its result alphabet is not a reference filter.
            if (fe->funcid == o->cigar_oid || fe->funcid == o->has_cigar_oid) {
                detector.flags.uses_cigar = true;
            }
            if (fe->funcid == o->edit_oid) {
                detector.flags.uses_edit = true;
            }
            if (fe->funcid == o->divergence_oid) {
                detector.flags.uses_divergence = true;
            }
            if (fe->funcid == o->mapq_oid) {
                detector.flags.uses_mapq = true;
            }
            if (is_bioseq_projection_funcid(fe->funcid, *o)) {
                detector.flags.any_match_projection = true;
            }
        }
        return pg_walk_step::descend;
    }
};

/** @brief Checks that a sequence-valued edit argument names the aligned base column. */
export [[nodiscard]] bool edit_sequence_argument_matches_target(
    const FuncExpr* function,
    Index target_relid,
    AttrNumber target_sequence_attno,
    const bioseq_stub_oids& oids) noexcept;

/** @brief Finds the  alignment features requested by output expressions and filters. */
export [[nodiscard]] scan_feature_flags detect_feature_flags(
    List* targetlist,
    List* qual,
    const bioseq_stub_oids& oids);

/** @brief Checks whether an expression contains a bioseq.edit call. */
export [[nodiscard]] bool contains_bioseq_edit(
    Node* expression,
    const bioseq_stub_oids& oids);

/** @brief Unwraps a PlaceHolderVar owned only by this scan, rejecting unsafe wrappers. */
export [[nodiscard]] Node* strict_edit_operand(Node* operand, Index scan_relid) noexcept;

/** @brief Extracts a supported bioseq.edit(sequence_column) < integer_constant predicate. */
export [[nodiscard]] std::optional<strict_edit_predicate> decode_strict_edit_upper_bound(
    Node* expression,
    Index scan_relid,
    const bioseq_stub_oids& oids);

/** @brief Builds a filter list excluding whole expressions that contain bioseq.edit. */
export [[nodiscard]] List* quals_without_bioseq_edit(
    List* quals,
    const bioseq_stub_oids& oids);

/** @brief Finds requested alignment features, returning no value for malformed restrictions. */
export [[nodiscard]] std::optional<scan_feature_flags>
detect_restrictinfo_feature_flags(List* restrictions, const bioseq_stub_oids& oids);

/** @brief Finds a sequence storage-width conversion to report in EXPLAIN. */
export [[nodiscard]] explain_typmod_coalesce explain_detect_typmod_coalesce(
    List* qual,
    const bioseq_stub_oids& oids);
