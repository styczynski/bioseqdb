/**
 * @file
 * @brief Declares planner feature analysis without importing its implementation.
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

/** @brief Serialization constants for an extracted strict edit predicate. */
export inline constexpr int k_bioseq_edit_predicate_magic = 0x42534531; /* BSE1 */
export inline constexpr int k_bioseq_edit_predicate_version = 1;

/** @brief Describe an executor-owned strict upper bound on edit distance. */
export struct strict_edit_predicate {
    std::int64_t exclusive_upper_bound = 0;
    AttrNumber sequence_attno = InvalidAttrNumber;
};

/** @brief Describe a plan-time sequence storage-width coalescing decision. */
export struct explain_typmod_coalesce {
    bool active = false;
    uint8_t col_alpha = 0;
    uint8_t col_byte_width = 0;
};

/** @brief Accumulates executor feature requirements discovered in expression trees. */
export struct bioseq_feature_detector {
    const bioseq_stub_oids* oids = nullptr;
    scan_feature_flags flags;
};

/** @brief Maps mmsearch and projection calls to executor feature flags. */
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

/** @brief Check that an edit helper names the base scan's aligned column. */
export [[nodiscard]] bool edit_sequence_argument_matches_target(
    const FuncExpr* function,
    Index target_relid,
    AttrNumber target_sequence_attno,
    const bioseq_stub_oids& oids) noexcept;

/** @brief Detect executor features referenced by target-list and qual expressions. */
export [[nodiscard]] scan_feature_flags detect_feature_flags(
    List* targetlist,
    List* qual,
    const bioseq_stub_oids& oids);

/** @brief Test whether an expression contains a bioseq.edit call. */
export [[nodiscard]] bool contains_bioseq_edit(
    Node* expression,
    const bioseq_stub_oids& oids);

/** @brief Unwrap the scan-owned placeholder form admitted for an edit operand. */
export [[nodiscard]] Node* strict_edit_operand(Node* operand, Index scan_relid) noexcept;

/** @brief Decode an executor-owned strict bioseq.edit upper-bound predicate. */
export [[nodiscard]] std::optional<strict_edit_predicate> decode_strict_edit_upper_bound(
    Node* expression,
    Index scan_relid,
    const bioseq_stub_oids& oids);

/** @brief Copy qualifiers while removing those containing bioseq.edit. */
export [[nodiscard]] List* quals_without_bioseq_edit(
    List* quals,
    const bioseq_stub_oids& oids);

/** @brief Detect feature flags in RestrictInfo clauses, rejecting malformed entries. */
export [[nodiscard]] std::optional<scan_feature_flags>
detect_restrictinfo_feature_flags(List* restrictions, const bioseq_stub_oids& oids);

/** @brief Derive typmod coalescing metadata from plan qualifiers. */
export [[nodiscard]] explain_typmod_coalesce explain_detect_typmod_coalesce(
    List* qual,
    const bioseq_stub_oids& oids);
