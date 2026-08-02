module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

export module bioseq.pg.cache_provider:plan_recipe;

import bioseq.cache.provider_api;
import bioseq.table;

export namespace bioseq::pg::cache_provider {

inline constexpr std::uint32_t kBioseqScanProviderId = 0x42535153U;
inline constexpr char kBioseqScanProviderName[] = "bioseq.scan.exact.v1";
inline constexpr std::uint32_t kBioseqScanRecipeVersion = 1U;
inline constexpr std::uint32_t kBioseqBatchJoinProviderId = 0x4253514aU;
inline constexpr char kBioseqBatchJoinProviderName[] =
    "bioseq.batch_join.exact.v1";
inline constexpr std::uint32_t kBioseqBatchJoinRecipeVersion = 1U;

namespace detail {

inline constexpr std::int32_t kRecipeMagic = 0x42535231;
inline constexpr std::int32_t kRecipeFieldCount = 5;
inline constexpr int kRecipeNodeCount = 11;
inline constexpr int kRecipePlanNodeId = 3;
inline constexpr int kRecipeScanRelid = 4;
inline constexpr int kRecipeRelationOid = 5;
inline constexpr int kRecipeCustomPrivate = 6;
inline constexpr int kRecipeCustomExprs = 7;
inline constexpr int kRecipeTargetList = 8;
inline constexpr int kRecipeQual = 9;
inline constexpr int kRecipeCustomPlans = 10;
inline constexpr std::int32_t kBatchJoinRecipeMagic = 0x42534a31;
inline constexpr std::int32_t kBatchJoinRecipeFieldCount = 2;
inline constexpr int kBatchJoinRecipeNodeCount = 6;
inline constexpr int kBatchJoinRecipePlanNodeId = 3;
inline constexpr int kBatchJoinRecipePlanPayload = 4;
inline constexpr int kBatchJoinRecipeSemanticSnapshot = 5;
inline constexpr std::size_t kMaximumPlanWalkNodes = 4'096;
inline constexpr std::uint32_t kMaximumPlanWalkDepth = 256;

[[nodiscard]] bool append_bytes(
        BioseqCacheDigestSinkV2* sink,
        const void* bytes,
        std::size_t size) noexcept {
    return sink != nullptr
        && sink->struct_size >= sizeof(BioseqCacheDigestSinkV2)
        && sink->append != nullptr
        && (size == 0 || bytes != nullptr)
        && sink->append(sink->opaque, bytes, size);
}

template <class Integer>
void encode_little_endian(
        Integer value,
        std::array<std::uint8_t, sizeof(Integer)>& bytes) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned remaining = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            remaining & static_cast<Unsigned>(0xffU));
        remaining >>= 8U;
    }
}

void mix_digest_byte(
        std::array<std::uint64_t, 4>& state,
        std::uint8_t byte,
        std::uint64_t ordinal) noexcept {
    for (std::size_t lane = 0; lane < state.size(); ++lane) {
        state[lane] ^= static_cast<std::uint64_t>(byte)
            + (ordinal << ((lane + 1U) & 7U))
            + (0x9eU * (lane + 1U));
        state[lane] *= 0x100000001b3ULL + (lane * 0x100ULL);
        state[lane] ^= state[lane] >> (17U + lane);
    }
}

[[nodiscard]] bool digest_recipe(
        const List* recipe,
        std::uint32_t provider_id,
        std::uint32_t recipe_version,
        std::array<std::uint8_t, bioseq::cache::kProviderDigestBytes>*
            output) {
    if (recipe == nullptr || output == nullptr) return false;
    char* serialized = nodeToString(recipe);
    if (serialized == nullptr) return false;
    const std::size_t byte_count = std::strlen(serialized);
    std::array<std::uint64_t, 4> state{
        0xcbf29ce484222325ULL ^ provider_id,
        0x84222325cbf29ce4ULL ^ recipe_version,
        0x9e3779b185ebca87ULL,
        0xd6e8feb86659fd93ULL,
    };
    std::uint64_t ordinal = 0;
    for (const char byte : std::string_view{serialized, byte_count}) {
        mix_digest_byte(
            state, static_cast<std::uint8_t>(byte), ordinal++);
    }
    pfree(serialized);
    for (std::size_t lane = 0; lane < state.size(); ++lane) {
        std::array<std::uint8_t, sizeof(std::uint64_t)> encoded{};
        encode_little_endian(state[lane], encoded);
        std::memcpy(
            output->data() + lane * encoded.size(),
            encoded.data(),
            encoded.size());
    }
    return true;
}

[[nodiscard]] bool integer_node_value(
        const List* nodes,
        int index,
        std::int32_t* output) noexcept {
    if (nodes == nullptr || output == nullptr || index < 0
        || index >= list_length(nodes)) {
        return false;
    }
    const Node* node = reinterpret_cast<const Node*>(list_nth(nodes, index));
    if (node == nullptr || !IsA(node, Integer)) return false;
    *output = intVal(node);
    return true;
}

[[nodiscard]] bool list_or_nil_node(
        const List* nodes,
        int index,
        const List** output) noexcept {
    if (nodes == nullptr || output == nullptr || index < 0
        || index >= list_length(nodes)) {
        return false;
    }
    const Node* node = reinterpret_cast<const Node*>(list_nth(nodes, index));
    if (node != nullptr && !IsA(node, List)) return false;
    *output = reinterpret_cast<const List*>(node);
    return true;
}

[[nodiscard]] Oid planner_relation_oid(
        PlannerInfo* root,
        Index scan_relid) noexcept {
    if (root == nullptr || root->parse == nullptr || scan_relid == 0
        || scan_relid > static_cast<Index>(
            list_length(root->parse->rtable))) {
        return InvalidOid;
    }
    const RangeTblEntry* rte = rt_fetch(scan_relid, root->parse->rtable);
    return rte != nullptr && rte->rtekind == RTE_RELATION
        ? rte->relid : InvalidOid;
}

[[nodiscard]] Oid statement_relation_oid(
        const PlannedStmt* statement,
        Index scan_relid) noexcept {
    if (statement == nullptr || scan_relid == 0
        || scan_relid > static_cast<Index>(
            list_length(statement->rtable))) {
        return InvalidOid;
    }
    const RangeTblEntry* rte = rt_fetch(scan_relid, statement->rtable);
    return rte != nullptr && rte->rtekind == RTE_RELATION
        ? rte->relid : InvalidOid;
}

[[nodiscard]] List* make_recipe(
        const CustomScan& scan,
        Oid relation_oid) {
    List* private_payload = bioseq_customscan_cache_private_payload(&scan);
    if (private_payload == NIL) return NIL;
    List* recipe = list_make5(
        makeInteger(kRecipeMagic),
        makeInteger(static_cast<int>(kBioseqScanRecipeVersion)),
        makeInteger(kRecipeFieldCount),
        makeInteger(scan.scan.plan.plan_node_id),
        makeInteger(static_cast<int>(scan.scan.scanrelid)));
    recipe = lappend(
        recipe, makeInteger(static_cast<int>(relation_oid)));
    recipe = lappend(recipe, private_payload);
    recipe = lappend(
        recipe, copyObject(scan.custom_exprs));
    recipe = lappend(
        recipe, copyObject(scan.scan.plan.targetlist));
    recipe = lappend(
        recipe, copyObject(scan.scan.plan.qual));
    recipe = lappend(
        recipe, copyObject(scan.custom_plans));
    Assert(list_length(recipe) == kRecipeNodeCount);
    return recipe;
}

[[nodiscard]] List* make_batch_join_recipe(
        const CustomScan& scan,
        List* semantic_snapshot) {
    if (semantic_snapshot == NIL) return NIL;
    List* plan_payload = bioseq_batch_join_cache_plan_payload(&scan);
    if (plan_payload == NIL) return NIL;
    List* recipe = list_make5(
        makeInteger(kBatchJoinRecipeMagic),
        makeInteger(static_cast<int>(kBioseqBatchJoinRecipeVersion)),
        makeInteger(kBatchJoinRecipeFieldCount),
        makeInteger(scan.scan.plan.plan_node_id),
        plan_payload);
    recipe = lappend(recipe, copyObject(semantic_snapshot));
    Assert(list_length(recipe) == kBatchJoinRecipeNodeCount);
    return recipe;
}

struct PlanFinder {
    const CustomScanMethods* methods = nullptr;
    std::int32_t plan_node_id = -1;
    Index scan_relid = 0;
    const CustomScan* match = nullptr;
    std::size_t match_count = 0;
    std::array<const Plan*, kMaximumPlanWalkNodes> visited{};
    std::size_t visited_count = 0;
    bool valid = true;

    [[nodiscard]] bool visit(const Plan* plan, std::uint32_t depth) {
        if (!valid || plan == nullptr) return valid;
        if (depth > kMaximumPlanWalkDepth
            || visited_count >= visited.size()) {
            valid = false;
            return false;
        }
        for (std::size_t index = 0; index < visited_count; ++index) {
            if (visited[index] == plan) return true;
        }
        visited[visited_count++] = plan;
        if (IsA(plan, CustomScan)) {
            const auto* custom = reinterpret_cast<const CustomScan*>(plan);
            if (methods != nullptr && custom->methods == methods
                && custom->scan.plan.plan_node_id == plan_node_id
                && custom->scan.scanrelid == scan_relid) {
                match = custom;
                ++match_count;
            }
        }
        if (!visit(outerPlan(plan), depth + 1U)
            || !visit(innerPlan(plan), depth + 1U)) {
            return false;
        }
        switch (nodeTag(plan)) {
            case T_SubqueryScan:
                return visit(
                    reinterpret_cast<const SubqueryScan*>(plan)->subplan,
                    depth + 1U);
            case T_CustomScan:
                return visit_list(
                    reinterpret_cast<const CustomScan*>(plan)->custom_plans,
                    depth + 1U);
            case T_Append:
                return visit_list(
                    reinterpret_cast<const Append*>(plan)->appendplans,
                    depth + 1U);
            case T_MergeAppend:
                return visit_list(
                    reinterpret_cast<const MergeAppend*>(plan)->mergeplans,
                    depth + 1U);
            case T_BitmapAnd:
                return visit_list(
                    reinterpret_cast<const BitmapAnd*>(plan)->bitmapplans,
                    depth + 1U);
            case T_BitmapOr:
                return visit_list(
                    reinterpret_cast<const BitmapOr*>(plan)->bitmapplans,
                    depth + 1U);
            case T_Agg:
                return visit_list(
                    reinterpret_cast<const Agg*>(plan)->chain,
                    depth + 1U);
            case T_Result:
            case T_ProjectSet:
            case T_ModifyTable:
            case T_RecursiveUnion:
            case T_SeqScan:
            case T_SampleScan:
            case T_IndexScan:
            case T_IndexOnlyScan:
            case T_BitmapIndexScan:
            case T_BitmapHeapScan:
            case T_TidScan:
            case T_TidRangeScan:
            case T_FunctionScan:
            case T_ValuesScan:
            case T_TableFuncScan:
            case T_CteScan:
            case T_NamedTuplestoreScan:
            case T_WorkTableScan:
            case T_ForeignScan:
            case T_NestLoop:
            case T_MergeJoin:
            case T_HashJoin:
            case T_Material:
            case T_Memoize:
            case T_Sort:
            case T_IncrementalSort:
            case T_Group:
            case T_WindowAgg:
            case T_Unique:
            case T_Gather:
            case T_GatherMerge:
            case T_Hash:
            case T_SetOp:
            case T_LockRows:
            case T_Limit:
                return true;
            default:
                valid = false;
                return false;
        }
    }

    [[nodiscard]] bool visit_list(
            const List* plans,
            std::uint32_t depth) {
        const ListCell* cell = nullptr;
        foreach(cell, plans) {
            const auto* child = static_cast<const Plan*>(lfirst(cell));
            if (!visit(child, depth)) return false;
        }
        return true;
    }
};

[[nodiscard]] bool parse_recipe_header(
        const List* recipe,
        std::int32_t* plan_node_id,
        Index* scan_relid,
        Oid* relation_oid) noexcept {
    std::int32_t magic = 0;
    std::int32_t version = 0;
    std::int32_t field_count = 0;
    std::int32_t raw_plan_node_id = -1;
    std::int32_t raw_scan_relid = 0;
    std::int32_t raw_relation_oid = 0;
    const List* custom_private = nullptr;
    const List* custom_exprs = nullptr;
    const List* targetlist = nullptr;
    const List* qual = nullptr;
    const List* custom_plans = nullptr;
    if (recipe == nullptr || list_length(recipe) != kRecipeNodeCount
        || !integer_node_value(recipe, 0, &magic)
        || !integer_node_value(recipe, 1, &version)
        || !integer_node_value(recipe, 2, &field_count)
        || !integer_node_value(
            recipe, kRecipePlanNodeId, &raw_plan_node_id)
        || !integer_node_value(
            recipe, kRecipeScanRelid, &raw_scan_relid)
        || !integer_node_value(
            recipe, kRecipeRelationOid, &raw_relation_oid)
        || !list_or_nil_node(
            recipe, kRecipeCustomPrivate, &custom_private)
        || !list_or_nil_node(recipe, kRecipeCustomExprs, &custom_exprs)
        || !list_or_nil_node(recipe, kRecipeTargetList, &targetlist)
        || !list_or_nil_node(recipe, kRecipeQual, &qual)
        || !list_or_nil_node(recipe, kRecipeCustomPlans, &custom_plans)
        || magic != kRecipeMagic
        || version != static_cast<std::int32_t>(
            kBioseqScanRecipeVersion)
        || field_count != kRecipeFieldCount
        || raw_plan_node_id < 0
        || raw_scan_relid <= 0
        || custom_private == nullptr
        || custom_exprs == nullptr
        || targetlist == nullptr
        || plan_node_id == nullptr
        || scan_relid == nullptr
        || relation_oid == nullptr) {
        return false;
    }
    *plan_node_id = raw_plan_node_id;
    *scan_relid = static_cast<Index>(raw_scan_relid);
    *relation_oid = static_cast<Oid>(
        static_cast<std::uint32_t>(raw_relation_oid));
    return true;
}

[[nodiscard]] bool parse_batch_join_recipe_header(
        const List* recipe,
        std::int32_t* plan_node_id) noexcept {
    std::int32_t magic = 0;
    std::int32_t version = 0;
    std::int32_t field_count = 0;
    std::int32_t raw_plan_node_id = -1;
    const List* plan_payload = nullptr;
    const List* semantic_snapshot = nullptr;
    if (recipe == nullptr
        || list_length(recipe) != kBatchJoinRecipeNodeCount
        || !integer_node_value(recipe, 0, &magic)
        || !integer_node_value(recipe, 1, &version)
        || !integer_node_value(recipe, 2, &field_count)
        || !integer_node_value(
            recipe, kBatchJoinRecipePlanNodeId, &raw_plan_node_id)
        || !list_or_nil_node(
            recipe, kBatchJoinRecipePlanPayload, &plan_payload)
        || !list_or_nil_node(
            recipe, kBatchJoinRecipeSemanticSnapshot, &semantic_snapshot)
        || magic != kBatchJoinRecipeMagic
        || version != static_cast<std::int32_t>(
            kBioseqBatchJoinRecipeVersion)
        || field_count != kBatchJoinRecipeFieldCount
        || raw_plan_node_id < 0
        || plan_node_id == nullptr
        || plan_payload == nullptr
        || semantic_snapshot == nullptr) {
        return false;
    }
    std::int32_t flags = 0;
    const List* custom_private = nullptr;
    const List* target_list = nullptr;
    const List* qual = nullptr;
    const List* custom_scan_tlist = nullptr;
    if (list_length(plan_payload) != 5
        || !integer_node_value(plan_payload, 0, &flags)
        || !list_or_nil_node(plan_payload, 1, &custom_private)
        || !list_or_nil_node(plan_payload, 2, &target_list)
        || !list_or_nil_node(plan_payload, 3, &qual)
        || !list_or_nil_node(plan_payload, 4, &custom_scan_tlist)
        || custom_private == nullptr || target_list == nullptr
        || custom_scan_tlist == nullptr) {
        return false;
    }
    static_cast<void>(flags);
    static_cast<void>(qual);
    *plan_node_id = raw_plan_node_id;
    return true;
}

} // namespace detail

[[nodiscard]] BioseqCacheStatusV2 bioseq_build_plan_digest_recipe(
        PlannerInfo* root,
        const Plan* provider_plan,
        BioseqCachePlanDigestRecipeV2* output) {
    if (provider_plan == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || !IsA(provider_plan, CustomScan)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    const auto* scan = reinterpret_cast<const CustomScan*>(provider_plan);
    const Oid relation_oid = detail::planner_relation_oid(
        root, scan->scan.scanrelid);
    if (scan->methods != bioseq_customscan_methods()
        || scan->scan.plan.plan_node_id < 0
        || !bioseq_customscan_has_valid_semantic_payload(scan)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    List* recipe = detail::make_recipe(*scan, relation_oid);
    std::array<std::uint8_t, bioseq::cache::kProviderDigestBytes> digest{};
    if (recipe == NIL || !detail::digest_recipe(
            recipe,
            kBioseqScanProviderId,
            kBioseqScanRecipeVersion,
            &digest)) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    output->struct_size = sizeof(*output);
    output->flags = 0;
    output->digest_format_version = kBioseqScanRecipeVersion;
    output->reserved = 0;
    output->plan_domain_nodes = recipe;
    std::memcpy(
        output->expected_generation_digest,
        digest.data(),
        digest.size());
    return BIOSEQ_CACHE_STATUS_OK;
}

[[nodiscard]] BioseqCacheStatusV2
bioseq_append_runtime_generation_digest(
        const List* plan_domain_nodes,
        EState* estate,
        BioseqCacheDigestSinkV2* sink) {
    std::int32_t plan_node_id = -1;
    Index scan_relid = 0;
    Oid relation_oid = InvalidOid;
    if (!detail::parse_recipe_header(
            plan_domain_nodes,
            &plan_node_id,
            &scan_relid,
            &relation_oid)
        || estate == nullptr || estate->es_plannedstmt == nullptr
        || !OidIsValid(detail::statement_relation_oid(
            estate->es_plannedstmt, scan_relid))
        || (OidIsValid(relation_oid)
            && detail::statement_relation_oid(
                estate->es_plannedstmt, scan_relid) != relation_oid)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    detail::PlanFinder finder{
        .methods = bioseq_customscan_methods(),
        .plan_node_id = plan_node_id,
        .scan_relid = scan_relid,
    };
    if (!finder.visit(estate->es_plannedstmt->planTree, 0)
        || !finder.visit_list(estate->es_plannedstmt->subplans, 0)
        || !finder.valid || finder.match_count != 1
        || finder.match == nullptr
        || !bioseq_customscan_has_valid_semantic_payload(finder.match)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    List* actual_recipe = detail::make_recipe(
        *finder.match, relation_oid);
    if (actual_recipe == NIL || !equal(actual_recipe, plan_domain_nodes)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    std::array<std::uint8_t, bioseq::cache::kProviderDigestBytes> digest{};
    if (!detail::digest_recipe(
            actual_recipe,
            kBioseqScanProviderId,
            kBioseqScanRecipeVersion,
            &digest)) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    return detail::append_bytes(sink, digest.data(), digest.size())
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

[[nodiscard]] BioseqCacheStatusV2
bioseq_batch_join_build_plan_digest_recipe(
        PlannerInfo* root,
        const Plan* provider_plan,
        BioseqCachePlanDigestRecipeV2* output) {
    static_cast<void>(root);
    if (provider_plan == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || !IsA(provider_plan, CustomScan)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    const auto* scan = reinterpret_cast<const CustomScan*>(provider_plan);
    if (scan->methods != bioseq_batch_join_customscan_methods()
        || !bioseq_batch_join_customscan_has_valid_semantic_payload(scan)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    List* semantic_snapshot =
        bioseq_batch_join_cache_semantic_snapshot(scan);
    if (semantic_snapshot == NIL) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    List* recipe = detail::make_batch_join_recipe(
        *scan, semantic_snapshot);
    std::array<std::uint8_t, bioseq::cache::kProviderDigestBytes> digest{};
    if (recipe == NIL || !detail::digest_recipe(
            recipe,
            kBioseqBatchJoinProviderId,
            kBioseqBatchJoinRecipeVersion,
            &digest)) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    output->struct_size = sizeof(*output);
    output->flags = 0;
    output->digest_format_version = kBioseqBatchJoinRecipeVersion;
    output->reserved = 0;
    output->plan_domain_nodes = recipe;
    std::memcpy(
        output->expected_generation_digest,
        digest.data(),
        digest.size());
    return BIOSEQ_CACHE_STATUS_OK;
}

[[nodiscard]] BioseqCacheStatusV2
bioseq_batch_join_append_runtime_generation_digest(
        const List* plan_domain_nodes,
        EState* estate,
        BioseqCacheDigestSinkV2* sink) {
    std::int32_t plan_node_id = -1;
    if (!detail::parse_batch_join_recipe_header(
            plan_domain_nodes,
            &plan_node_id)
        || estate == nullptr || estate->es_plannedstmt == nullptr) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    detail::PlanFinder finder{
        .methods = bioseq_batch_join_customscan_methods(),
        .plan_node_id = plan_node_id,
        .scan_relid = 0,
    };
    if (!finder.visit(estate->es_plannedstmt->planTree, 0)
        || !finder.visit_list(estate->es_plannedstmt->subplans, 0)
        || !finder.valid || finder.match_count != 1
        || finder.match == nullptr
        || !bioseq_batch_join_customscan_has_valid_semantic_payload(
            finder.match)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    List* runtime_semantics =
        bioseq_batch_join_cache_semantic_snapshot(finder.match);
    if (runtime_semantics == NIL) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    List* actual_recipe = detail::make_batch_join_recipe(
        *finder.match, runtime_semantics);
    if (actual_recipe == NIL || !equal(actual_recipe, plan_domain_nodes)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    std::array<std::uint8_t, bioseq::cache::kProviderDigestBytes> digest{};
    if (!detail::digest_recipe(
            actual_recipe,
            kBioseqBatchJoinProviderId,
            kBioseqBatchJoinRecipeVersion,
            &digest)) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    return detail::append_bytes(sink, digest.data(), digest.size())
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

[[nodiscard]] BioseqCacheStatusV2 bioseq_append_domain_digest(
        const BioseqCachePathDomainV2* domain,
        BioseqCacheDigestSinkV2* sink) {
    if (domain == nullptr || domain->provider_id != kBioseqScanProviderId) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    constexpr std::array<std::uint8_t, 8> identity{
        0x53, 0x51, 0x53, 0x42,  // provider id, little endian
        0x01, 0x00, 0x00, 0x00,  // recipe version
    };
    return detail::append_bytes(sink, identity.data(), identity.size())
            && detail::append_bytes(
                sink,
                kBioseqScanProviderName,
                sizeof(kBioseqScanProviderName) - 1U)
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

[[nodiscard]] BioseqCacheStatusV2 bioseq_append_generation_digest(
        const BioseqCachePathDomainV2* domain,
        BioseqCacheDigestSinkV2* sink) {
    const BioseqCacheStatusV2 domain_status =
        bioseq_append_domain_digest(domain, sink);
    if (domain_status != BIOSEQ_CACHE_STATUS_OK) {
        return domain_status;
    }
    char* dependency = nodeToString(domain->dependency_nodes);
    char* private_nodes = nodeToString(domain->provider_private_nodes);
    if (dependency == nullptr || private_nodes == nullptr) {
        if (dependency != nullptr) pfree(dependency);
        if (private_nodes != nullptr) pfree(private_nodes);
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::size_t dependency_size = std::strlen(dependency);
    const std::size_t private_size = std::strlen(private_nodes);
    std::array<std::uint8_t, sizeof(std::uint64_t)> encoded_size{};
    detail::encode_little_endian(
        static_cast<std::uint64_t>(dependency_size), encoded_size);
    bool appended = detail::append_bytes(
        sink, encoded_size.data(), encoded_size.size())
        && detail::append_bytes(sink, dependency, dependency_size);
    detail::encode_little_endian(
        static_cast<std::uint64_t>(private_size), encoded_size);
    appended = appended
        && detail::append_bytes(
            sink, encoded_size.data(), encoded_size.size())
        && detail::append_bytes(sink, private_nodes, private_size);
    pfree(dependency);
    pfree(private_nodes);
    return appended
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

[[nodiscard]] BioseqCacheStatusV2
bioseq_batch_join_append_domain_digest(
        const BioseqCachePathDomainV2* domain,
        BioseqCacheDigestSinkV2* sink) {
    if (domain == nullptr
        || domain->provider_id != kBioseqBatchJoinProviderId) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    constexpr std::array<std::uint8_t, 8> identity{
        0x4a, 0x51, 0x53, 0x42,  // provider id, little endian
        0x01, 0x00, 0x00, 0x00,  // recipe version
    };
    return detail::append_bytes(sink, identity.data(), identity.size())
            && detail::append_bytes(
                sink,
                kBioseqBatchJoinProviderName,
                sizeof(kBioseqBatchJoinProviderName) - 1U)
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

[[nodiscard]] BioseqCacheStatusV2
bioseq_batch_join_append_generation_digest(
        const BioseqCachePathDomainV2* domain,
        BioseqCacheDigestSinkV2* sink) {
    const BioseqCacheStatusV2 domain_status =
        bioseq_batch_join_append_domain_digest(domain, sink);
    if (domain_status != BIOSEQ_CACHE_STATUS_OK) {
        return domain_status;
    }
    char* dependency = nodeToString(domain->dependency_nodes);
    char* private_nodes = nodeToString(domain->provider_private_nodes);
    if (dependency == nullptr || private_nodes == nullptr) {
        if (dependency != nullptr) pfree(dependency);
        if (private_nodes != nullptr) pfree(private_nodes);
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::size_t dependency_size = std::strlen(dependency);
    const std::size_t private_size = std::strlen(private_nodes);
    std::array<std::uint8_t, sizeof(std::uint64_t)> encoded_size{};
    detail::encode_little_endian(
        static_cast<std::uint64_t>(dependency_size), encoded_size);
    bool appended = detail::append_bytes(
        sink, encoded_size.data(), encoded_size.size())
        && detail::append_bytes(sink, dependency, dependency_size);
    detail::encode_little_endian(
        static_cast<std::uint64_t>(private_size), encoded_size);
    appended = appended
        && detail::append_bytes(
            sink, encoded_size.data(), encoded_size.size())
        && detail::append_bytes(sink, private_nodes, private_size);
    pfree(dependency);
    pfree(private_nodes);
    return appended
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

void bioseq_batch_join_explain_exact_domain(
        List* plan_domain_nodes,
        BioseqCacheExplainSinkV2* sink) {
    if (sink == nullptr || sink->struct_size < sizeof(*sink)) return;
    std::int32_t plan_node_id = -1;
    const bool valid = detail::parse_batch_join_recipe_header(
        plan_domain_nodes,
        &plan_node_id);
    if (sink->property_text != nullptr) {
        sink->property_text(
            sink->opaque,
            "Bioseq Cache Provider",
            kBioseqBatchJoinProviderName);
    }
    if (sink->property_uint64 != nullptr) {
        sink->property_uint64(
            sink->opaque,
            "Bioseq Provider Recipe Version",
            kBioseqBatchJoinRecipeVersion);
        if (valid) {
            sink->property_uint64(
                sink->opaque,
                "Bioseq Provider Plan Node",
                static_cast<std::uint64_t>(plan_node_id));
        }
    }
    if (sink->property_bool != nullptr) {
        sink->property_bool(
            sink->opaque, "Bioseq Exact Recipe Valid", valid);
        sink->property_bool(
            sink->opaque, "Bioseq Reusable Coverage", false);
    }
}

void bioseq_explain_exact_domain(
        List* plan_domain_nodes,
        BioseqCacheExplainSinkV2* sink) {
    if (sink == nullptr || sink->struct_size < sizeof(*sink)) return;
    std::int32_t plan_node_id = -1;
    Index scan_relid = 0;
    Oid relation_oid = InvalidOid;
    const bool valid = detail::parse_recipe_header(
        plan_domain_nodes, &plan_node_id, &scan_relid, &relation_oid);
    if (sink->property_text != nullptr) {
        sink->property_text(
            sink->opaque, "Bioseq Cache Provider", kBioseqScanProviderName);
    }
    if (sink->property_uint64 != nullptr) {
        sink->property_uint64(
            sink->opaque,
            "Bioseq Provider Recipe Version",
            kBioseqScanRecipeVersion);
        if (valid && OidIsValid(relation_oid)) {
            sink->property_uint64(
                sink->opaque,
                "Bioseq Provider Relation",
                relation_oid);
        }
    }
    if (sink->property_bool != nullptr) {
        sink->property_bool(
            sink->opaque, "Bioseq Exact Recipe Valid", valid);
        sink->property_bool(
            sink->opaque, "Bioseq Reusable Coverage", false);
    }
}

} // namespace bioseq::pg::cache_provider
