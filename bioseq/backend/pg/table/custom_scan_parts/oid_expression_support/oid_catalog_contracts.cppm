/**
 * @file
 * @brief Exposes Bioseq catalog OIDs and checks for ordinary relation references.
 */
module;
extern "C" {
#include "postgres.h"
#include "nodes/primnodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.table:custom_scan_oid_expression_support_oid_catalog_contracts;

export import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

export {

    /** @brief Checks that a relation number is nonzero and is not a special executor Var number. */
    [[nodiscard]] constexpr bool bioseq_is_concrete_rte_varno(const Index varno) noexcept {
        return varno != 0 && !IS_SPECIAL_VARNO(varno);
    }
    static_assert(bioseq_is_concrete_rte_varno(1));
    static_assert(!bioseq_is_concrete_rte_varno(0));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(INNER_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(OUTER_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(INDEX_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(ROWID_VAR)));

} // export catalog contracts
