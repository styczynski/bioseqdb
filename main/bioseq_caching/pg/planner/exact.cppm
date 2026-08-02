/*-------------------------------------------------------------------------
 *
 * Exact-result planner/executor dependency evidence.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
}

#include <cstddef>
#include <cstdint>

export module bioseq.cache.pg.planner:exact;

import bioseq.cache.pg.provider;

export namespace bioseq::cache::pg::planner {

/** Provider-completeness evidence associated with one relation dependency. */
struct ExactDependencyEvidenceV1 {
    bool references_relation = false;
    bool complete_provider = false;
    std::uint32_t provider_id = 0;
    std::uint32_t provider_abi_version = 0;
    bool provider_ambiguous = false;
};

namespace detail {

/**
 * Bounded recursive plan walker which associates a complete provider wrapper
 * with every relation in its subtree.
 */
class ExactDependencyEvidenceWalkerV1 {
public:
    ExactDependencyEvidenceWalkerV1(
        EState* estate,
        Oid relation_oid,
        const provider_registry::ProviderSnapshotV2& providers,
        std::size_t& visited,
        std::size_t maximum_nodes) noexcept
        : estate_(estate),
          relation_oid_(relation_oid),
          providers_(providers),
          visited_(visited),
          maximum_nodes_(maximum_nodes) {}

    /** Collect evidence for one plan subtree. */
    [[nodiscard]] ExactDependencyEvidenceV1 collect(
            const Plan* plan) noexcept {
        ExactDependencyEvidenceV1 result{};
        if (plan == nullptr || visited_ >= maximum_nodes_) {
            return result;
        }
        ++visited_;
        result.references_relation = scan_references_relation(plan);

        merge(result, collect(outerPlan(plan)));
        merge(result, collect(innerPlan(plan)));
        switch (nodeTag(plan)) {
            case T_SubqueryScan:
                merge(
                    result,
                    collect(reinterpret_cast<const SubqueryScan*>(plan)
                        ->subplan));
                break;
            case T_CustomScan:
                collect_list(
                    result,
                    reinterpret_cast<const CustomScan*>(plan)->custom_plans);
                break;
            case T_Append:
                collect_list(
                    result,
                    reinterpret_cast<const Append*>(plan)->appendplans);
                break;
            case T_MergeAppend:
                collect_list(
                    result,
                    reinterpret_cast<const MergeAppend*>(plan)->mergeplans);
                break;
            case T_BitmapAnd:
                collect_list(
                    result,
                    reinterpret_cast<const BitmapAnd*>(plan)->bitmapplans);
                break;
            case T_BitmapOr:
                collect_list(
                    result,
                    reinterpret_cast<const BitmapOr*>(plan)->bitmapplans);
                break;
            case T_Agg:
                collect_list(
                    result,
                    reinterpret_cast<const Agg*>(plan)->chain);
                break;
            default:
                break;
        }

        const provider_registry::ProviderV2* matched_provider = nullptr;
        for (std::uint32_t index = 0;
             index < providers_.provider_count;
             ++index) {
            const provider_registry::ProviderV2* provider =
                providers_.providers[index];
            if (provider == nullptr
                || (provider->provider_flags
                        & BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION)
                    == 0
                || !reuse::provider_plan_matches(plan, provider)) {
                continue;
            }
            if (matched_provider != nullptr
                && matched_provider->provider_id != provider->provider_id) {
                result.provider_ambiguous = true;
            } else {
                matched_provider = provider;
            }
        }
        if (matched_provider != nullptr && result.references_relation) {
            result.complete_provider = true;
            if (result.provider_id != 0
                && result.provider_id != matched_provider->provider_id) {
                result.provider_ambiguous = true;
            } else {
                result.provider_id = matched_provider->provider_id;
                result.provider_abi_version =
                    matched_provider->abi_version;
            }
        }
        return result;
    }

private:
    /** Recognize a relation-bearing scan node without following its children. */
    [[nodiscard]] bool scan_references_relation(
            const Plan* plan) const noexcept {
        bool scan_node = false;
        switch (nodeTag(plan)) {
            case T_SeqScan:
            case T_SampleScan:
            case T_IndexScan:
            case T_IndexOnlyScan:
            case T_BitmapIndexScan:
            case T_BitmapHeapScan:
            case T_TidScan:
            case T_TidRangeScan:
            case T_SubqueryScan:
            case T_FunctionScan:
            case T_TableFuncScan:
            case T_ValuesScan:
            case T_CteScan:
            case T_NamedTuplestoreScan:
            case T_WorkTableScan:
            case T_ForeignScan:
            case T_CustomScan:
                scan_node = true;
                break;
            default:
                break;
        }
        if (!scan_node || estate_ == nullptr
            || !OidIsValid(relation_oid_)) {
            return false;
        }
        const Index range_index =
            reinterpret_cast<const Scan*>(plan)->scanrelid;
        if (range_index == 0 || estate_->es_range_table == NIL
            || range_index > static_cast<Index>(
                list_length(estate_->es_range_table))) {
            return false;
        }
        const RangeTblEntry* range = exec_rt_fetch(range_index, estate_);
        return range != nullptr && range->rtekind == RTE_RELATION
            && range->relid == relation_oid_;
    }

    /** Merge one child result while rejecting conflicting provider ownership. */
    static void merge(
            ExactDependencyEvidenceV1& result,
            const ExactDependencyEvidenceV1& child) noexcept {
        result.references_relation =
            result.references_relation || child.references_relation;
        result.complete_provider =
            result.complete_provider || child.complete_provider;
        result.provider_ambiguous =
            result.provider_ambiguous || child.provider_ambiguous;
        if (child.provider_id == 0) {
            return;
        }
        if (result.provider_id != 0
            && result.provider_id != child.provider_id) {
            result.provider_ambiguous = true;
            return;
        }
        result.provider_id = child.provider_id;
        result.provider_abi_version = child.provider_abi_version;
    }

    /** Merge every plan in one PostgreSQL list. */
    void collect_list(
            ExactDependencyEvidenceV1& result,
            const List* plans) noexcept {
        const ListCell* cell = nullptr;
        foreach(cell, plans) {
            merge(
                result,
                collect(static_cast<const Plan*>(lfirst(cell))));
        }
    }

    EState* estate_ = nullptr;
    Oid relation_oid_ = InvalidOid;
    const provider_registry::ProviderSnapshotV2& providers_;
    std::size_t& visited_;
    std::size_t maximum_nodes_ = 0;
};

} // namespace detail

/** Collect bounded provider/write evidence for one relation dependency. */
[[nodiscard]] inline ExactDependencyEvidenceV1 exact_dependency_evidence(
        const Plan* plan,
        EState* estate,
        Oid relation_oid,
        const provider_registry::ProviderSnapshotV2& providers,
        std::size_t& visited,
        std::size_t maximum_nodes) noexcept {
    return detail::ExactDependencyEvidenceWalkerV1{
        estate, relation_oid, providers, visited, maximum_nodes}.collect(plan);
}

} // namespace bioseq::cache::pg::planner
