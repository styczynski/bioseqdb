/**
 * @file plan_recipe.cppm
 * @brief Builds and validates exact cache-provider plan recipes and digests.
 */
module;

#include <bioseq/pg_compat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

export module bioseq.postgres.extension.cache_provider:plan_recipe;

import bioseq.cache.provider_api;
import bioseq.postgres.table;

export namespace bioseq::postgres::extension::cache_provider {

/** @brief Stable provider identifier for exact Bioseq scans. */
inline constexpr std::uint32_t k_bioseq_scan_provider_id = 0x42535153U;
/** @brief Stable provider name for exact Bioseq scans. */
inline constexpr char k_bioseq_scan_provider_name[] = "bioseq.scan.exact";
/** @brief Digest recipe revision for exact Bioseq scans. */
inline constexpr std::uint32_t k_bioseq_scan_recipe_revision = 1U;
/** @brief Stable provider identifier for exact Bioseq batch joins. */
inline constexpr std::uint32_t k_bioseq_batch_join_provider_id = 0x4253514aU;
/** @brief Stable provider name for exact Bioseq batch joins. */
inline constexpr char k_bioseq_batch_join_provider_name[] =
    "bioseq.batch_join.exact";
/** @brief Digest recipe revision for exact Bioseq batch joins. */
inline constexpr std::uint32_t k_bioseq_batch_join_recipe_revision = 1U;

namespace detail {

inline constexpr std::int32_t k_recipe_magic = 0x42535231;
inline constexpr std::int32_t k_recipe_field_count = 5;
inline constexpr int k_recipe_node_count = 11;
inline constexpr int k_recipe_plan_node_id = 3;
inline constexpr int k_recipe_scan_relid = 4;
inline constexpr int k_recipe_relation_oid = 5;
inline constexpr int k_recipe_custom_private = 6;
inline constexpr int k_recipe_custom_exprs = 7;
inline constexpr int k_recipe_target_list = 8;
inline constexpr int k_recipe_qual = 9;
inline constexpr int k_recipe_custom_plans = 10;
inline constexpr std::int32_t k_batch_join_recipe_magic = 0x42534a31;
inline constexpr std::int32_t k_batch_join_recipe_field_count = 2;
inline constexpr int k_batch_join_recipe_node_count = 6;
inline constexpr int k_batch_join_recipe_plan_node_id = 3;
inline constexpr int k_batch_join_recipe_plan_payload = 4;
inline constexpr int k_batch_join_recipe_semantic_snapshot = 5;
inline constexpr std::size_t k_maximum_plan_walk_nodes = 4'096;
inline constexpr std::uint32_t k_maximum_plan_walk_depth = 256;

/** @return True when a valid digest sink accepted the supplied byte range. */
[[nodiscard]] bool append_bytes(
        bioseq_cache_digest_sink* sink,
        const void* bytes,
        std::size_t size) noexcept {
    return sink != nullptr
        && sink->struct_size >= sizeof(bioseq_cache_digest_sink)
        && sink->append != nullptr
        && (size == 0 || bytes != nullptr)
        && sink->append(sink->opaque, bytes, size);
}

/** @brief Encodes one integer into a fixed little-endian byte array. */
template <class p_integer>
void encode_little_endian(
        p_integer value,
        std::array<std::uint8_t, sizeof(p_integer)>& bytes) noexcept {
    using unsigned_integer = std::make_unsigned_t<p_integer>;
    unsigned_integer remaining = static_cast<unsigned_integer>(value);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            remaining & static_cast<unsigned_integer>(0xffU));
        remaining >>= 8U;
    }
}

/** @brief Mixes one serialized recipe byte into every digest lane. */
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

/** @return True when a PostgreSQL node recipe was serialized and digested. */
[[nodiscard]] bool digest_recipe(
        const List* recipe,
        std::uint32_t provider_id,
        std::uint32_t recipe_revision,
        std::array<std::uint8_t, bioseq::cache::k_provider_digest_bytes>*
            output) {
    if (recipe == nullptr || output == nullptr) return false;
    char* serialized = nodeToString(recipe);
    if (serialized == nullptr) return false;
    const std::size_t byte_count = std::strlen(serialized);
    std::array<std::uint64_t, 4> state{
        0xcbf29ce484222325ULL ^ provider_id,
        0x84222325cbf29ce4ULL ^ recipe_revision,
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

/** @return True when the selected recipe node is an integer value. */
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

/** @return True when the selected recipe node is a list or PostgreSQL NIL. */
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

/** @return The relation OID addressed by a planner scan index. */
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

/** @return The relation OID addressed by a planned statement scan index. */
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

/** @return A copied exact-scan recipe or PostgreSQL NIL when invalid. */
[[nodiscard]] List* make_recipe(
        const CustomScan& scan,
        Oid relation_oid) {
    List* private_payload = bioseq_customscan_cache_private_payload(&scan);
    if (private_payload == NIL) return NIL;
    List* recipe = list_make5(
        makeInteger(k_recipe_magic),
        makeInteger(static_cast<int>(k_bioseq_scan_recipe_revision)),
        makeInteger(k_recipe_field_count),
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
    Assert(list_length(recipe) == k_recipe_node_count);
    return recipe;
}

/** @return A copied exact batch-join recipe or PostgreSQL NIL when invalid. */
[[nodiscard]] List* make_batch_join_recipe(
        const CustomScan& scan,
        List* semantic_snapshot) {
    if (semantic_snapshot == NIL) return NIL;
    List* plan_payload = bioseq_batch_join_cache_plan_payload(&scan);
    if (plan_payload == NIL) return NIL;
    List* recipe = list_make5(
        makeInteger(k_batch_join_recipe_magic),
        makeInteger(static_cast<int>(k_bioseq_batch_join_recipe_revision)),
        makeInteger(k_batch_join_recipe_field_count),
        makeInteger(scan.scan.plan.plan_node_id),
        plan_payload);
    recipe = lappend(recipe, copyObject(semantic_snapshot));
    Assert(list_length(recipe) == k_batch_join_recipe_node_count);
    return recipe;
}

/** @brief Finds one exact CustomScan identity in a bounded planned-statement walk. */
struct plan_finder {
    const CustomScanMethods* methods = nullptr;
    std::int32_t plan_node_id = -1;
    Index scan_relid = 0;
    const CustomScan* match = nullptr;
    std::size_t match_count = 0;
    std::array<const Plan*, k_maximum_plan_walk_nodes> visited{};
    std::size_t visited_count = 0;
    bool valid = true;

    /** @return True while the bounded plan walk remains valid. */
    [[nodiscard]] bool visit(const Plan* plan, std::uint32_t depth) {
        if (!valid || plan == nullptr) return valid;
        if (depth > k_maximum_plan_walk_depth
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

    /** @return True when every plan in a PostgreSQL list was visited safely. */
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

/** @return True when an exact-scan recipe has the expected shape and identity. */
[[nodiscard]] bool parse_recipe_header(
        const List* recipe,
        std::int32_t* plan_node_id,
        Index* scan_relid,
        Oid* relation_oid) noexcept {
    std::int32_t magic = 0;
    std::int32_t revision = 0;
    std::int32_t field_count = 0;
    std::int32_t raw_plan_node_id = -1;
    std::int32_t raw_scan_relid = 0;
    std::int32_t raw_relation_oid = 0;
    const List* custom_private = nullptr;
    const List* custom_exprs = nullptr;
    const List* targetlist = nullptr;
    const List* qual = nullptr;
    const List* custom_plans = nullptr;
    if (recipe == nullptr || list_length(recipe) != k_recipe_node_count
        || !integer_node_value(recipe, 0, &magic)
        || !integer_node_value(recipe, 1, &revision)
        || !integer_node_value(recipe, 2, &field_count)
        || !integer_node_value(
            recipe, k_recipe_plan_node_id, &raw_plan_node_id)
        || !integer_node_value(
            recipe, k_recipe_scan_relid, &raw_scan_relid)
        || !integer_node_value(
            recipe, k_recipe_relation_oid, &raw_relation_oid)
        || !list_or_nil_node(
            recipe, k_recipe_custom_private, &custom_private)
        || !list_or_nil_node(recipe, k_recipe_custom_exprs, &custom_exprs)
        || !list_or_nil_node(recipe, k_recipe_target_list, &targetlist)
        || !list_or_nil_node(recipe, k_recipe_qual, &qual)
        || !list_or_nil_node(recipe, k_recipe_custom_plans, &custom_plans)
        || magic != k_recipe_magic
        || revision != static_cast<std::int32_t>(
            k_bioseq_scan_recipe_revision)
        || field_count != k_recipe_field_count
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

/** @return True when an exact batch-join recipe has the expected shape. */
[[nodiscard]] bool parse_batch_join_recipe_header(
        const List* recipe,
        std::int32_t* plan_node_id) noexcept {
    std::int32_t magic = 0;
    std::int32_t revision = 0;
    std::int32_t field_count = 0;
    std::int32_t raw_plan_node_id = -1;
    const List* plan_payload = nullptr;
    const List* semantic_snapshot = nullptr;
    if (recipe == nullptr
        || list_length(recipe) != k_batch_join_recipe_node_count
        || !integer_node_value(recipe, 0, &magic)
        || !integer_node_value(recipe, 1, &revision)
        || !integer_node_value(recipe, 2, &field_count)
        || !integer_node_value(
            recipe, k_batch_join_recipe_plan_node_id, &raw_plan_node_id)
        || !list_or_nil_node(
            recipe, k_batch_join_recipe_plan_payload, &plan_payload)
        || !list_or_nil_node(
            recipe, k_batch_join_recipe_semantic_snapshot, &semantic_snapshot)
        || magic != k_batch_join_recipe_magic
        || revision != static_cast<std::int32_t>(
            k_bioseq_batch_join_recipe_revision)
        || field_count != k_batch_join_recipe_field_count
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

/**
 * @brief Builds an exact-scan plan recipe and expected generation digest.
 * @return Provider ABI status describing acceptance or failure.
 */
[[nodiscard]] bioseq_cache_status bioseq_build_plan_digest_recipe(
        PlannerInfo* root,
        const Plan* provider_plan,
        bioseq_cache_plan_digest_recipe* output) {
    if (provider_plan == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || !IsA(provider_plan, CustomScan)) {
        return bioseq_cache_status_unsupported;
    }
    const auto* scan = reinterpret_cast<const CustomScan*>(provider_plan);
    const Oid relation_oid = detail::planner_relation_oid(
        root, scan->scan.scanrelid);
    if (scan->methods != bioseq_customscan_methods()
        || scan->scan.plan.plan_node_id < 0
        || !bioseq_customscan_has_valid_semantic_payload(scan)) {
        return bioseq_cache_status_unsupported;
    }
    List* recipe = detail::make_recipe(*scan, relation_oid);
    std::array<std::uint8_t, bioseq::cache::k_provider_digest_bytes> digest{};
    if (recipe == NIL || !detail::digest_recipe(
            recipe,
            k_bioseq_scan_provider_id,
            k_bioseq_scan_recipe_revision,
            &digest)) {
        return bioseq_cache_status_error;
    }
    output->struct_size = sizeof(*output);
    output->flags = 0;
    output->digest_format_revision = k_bioseq_scan_recipe_revision;
    output->reserved = 0;
    output->plan_domain_nodes = recipe;
    std::memcpy(
        output->expected_generation_digest,
        digest.data(),
        digest.size());
    return bioseq_cache_status_ok;
}

/**
 * @brief Revalidates an exact-scan recipe against the runtime plan tree.
 * @return Provider ABI status after appending the runtime generation digest.
 */
[[nodiscard]] bioseq_cache_status
bioseq_append_runtime_generation_digest(
        const List* plan_domain_nodes,
        EState* estate,
        bioseq_cache_digest_sink* sink) {
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
        return bioseq_cache_status_unsupported;
    }
    detail::plan_finder finder{
        .methods = bioseq_customscan_methods(),
        .plan_node_id = plan_node_id,
        .scan_relid = scan_relid,
    };
    if (!finder.visit(estate->es_plannedstmt->planTree, 0)
        || !finder.visit_list(estate->es_plannedstmt->subplans, 0)
        || !finder.valid || finder.match_count != 1
        || finder.match == nullptr
        || !bioseq_customscan_has_valid_semantic_payload(finder.match)) {
        return bioseq_cache_status_unsupported;
    }
    List* actual_recipe = detail::make_recipe(
        *finder.match, relation_oid);
    if (actual_recipe == NIL || !equal(actual_recipe, plan_domain_nodes)) {
        return bioseq_cache_status_unsupported;
    }
    std::array<std::uint8_t, bioseq::cache::k_provider_digest_bytes> digest{};
    if (!detail::digest_recipe(
            actual_recipe,
            k_bioseq_scan_provider_id,
            k_bioseq_scan_recipe_revision,
            &digest)) {
        return bioseq_cache_status_error;
    }
    return detail::append_bytes(sink, digest.data(), digest.size())
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

/**
 * @brief Builds an exact batch-join plan recipe and expected digest.
 * @return Provider ABI status describing acceptance or failure.
 */
[[nodiscard]] bioseq_cache_status
bioseq_batch_join_build_plan_digest_recipe(
        PlannerInfo* root,
        const Plan* provider_plan,
        bioseq_cache_plan_digest_recipe* output) {
    static_cast<void>(root);
    if (provider_plan == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || !IsA(provider_plan, CustomScan)) {
        return bioseq_cache_status_unsupported;
    }
    const auto* scan = reinterpret_cast<const CustomScan*>(provider_plan);
    if (scan->methods != bioseq_batch_join_customscan_methods()
        || !bioseq_batch_join_customscan_has_valid_semantic_payload(scan)) {
        return bioseq_cache_status_unsupported;
    }
    List* semantic_snapshot =
        bioseq_batch_join_cache_semantic_snapshot(scan);
    if (semantic_snapshot == NIL) {
        return bioseq_cache_status_unsupported;
    }
    List* recipe = detail::make_batch_join_recipe(
        *scan, semantic_snapshot);
    std::array<std::uint8_t, bioseq::cache::k_provider_digest_bytes> digest{};
    if (recipe == NIL || !detail::digest_recipe(
            recipe,
            k_bioseq_batch_join_provider_id,
            k_bioseq_batch_join_recipe_revision,
            &digest)) {
        return bioseq_cache_status_error;
    }
    output->struct_size = sizeof(*output);
    output->flags = 0;
    output->digest_format_revision = k_bioseq_batch_join_recipe_revision;
    output->reserved = 0;
    output->plan_domain_nodes = recipe;
    std::memcpy(
        output->expected_generation_digest,
        digest.data(),
        digest.size());
    return bioseq_cache_status_ok;
}

/**
 * @brief Revalidates a batch-join recipe against the runtime plan tree.
 * @return Provider ABI status after appending the runtime generation digest.
 */
[[nodiscard]] bioseq_cache_status
bioseq_batch_join_append_runtime_generation_digest(
        const List* plan_domain_nodes,
        EState* estate,
        bioseq_cache_digest_sink* sink) {
    std::int32_t plan_node_id = -1;
    if (!detail::parse_batch_join_recipe_header(
            plan_domain_nodes,
            &plan_node_id)
        || estate == nullptr || estate->es_plannedstmt == nullptr) {
        return bioseq_cache_status_unsupported;
    }
    detail::plan_finder finder{
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
        return bioseq_cache_status_unsupported;
    }
    List* runtime_semantics =
        bioseq_batch_join_cache_semantic_snapshot(finder.match);
    if (runtime_semantics == NIL) {
        return bioseq_cache_status_unsupported;
    }
    List* actual_recipe = detail::make_batch_join_recipe(
        *finder.match, runtime_semantics);
    if (actual_recipe == NIL || !equal(actual_recipe, plan_domain_nodes)) {
        return bioseq_cache_status_unsupported;
    }
    std::array<std::uint8_t, bioseq::cache::k_provider_digest_bytes> digest{};
    if (!detail::digest_recipe(
            actual_recipe,
            k_bioseq_batch_join_provider_id,
            k_bioseq_batch_join_recipe_revision,
            &digest)) {
        return bioseq_cache_status_error;
    }
    return detail::append_bytes(sink, digest.data(), digest.size())
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

/** @return Provider ABI status after appending the exact-scan domain identity. */
[[nodiscard]] bioseq_cache_status bioseq_append_domain_digest(
        const bioseq_cache_path_domain* domain,
        bioseq_cache_digest_sink* sink) {
    if (domain == nullptr
        || domain->provider_id != k_bioseq_scan_provider_id) {
        return bioseq_cache_status_unsupported;
    }
    constexpr std::array<std::uint8_t, 8> identity{
        0x53, 0x51, 0x53, 0x42,  // provider id, little endian
        0x01, 0x00, 0x00, 0x00,  // recipe revision
    };
    return detail::append_bytes(sink, identity.data(), identity.size())
            && detail::append_bytes(
                sink,
                k_bioseq_scan_provider_name,
                sizeof(k_bioseq_scan_provider_name) - 1U)
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

/** @return Provider ABI status after appending exact-scan generation inputs. */
[[nodiscard]] bioseq_cache_status bioseq_append_generation_digest(
        const bioseq_cache_path_domain* domain,
        bioseq_cache_digest_sink* sink) {
    const bioseq_cache_status domain_status =
        bioseq_append_domain_digest(domain, sink);
    if (domain_status != bioseq_cache_status_ok) {
        return domain_status;
    }
    char* dependency = nodeToString(domain->dependency_nodes);
    char* private_nodes = nodeToString(domain->provider_private_nodes);
    if (dependency == nullptr || private_nodes == nullptr) {
        if (dependency != nullptr) pfree(dependency);
        if (private_nodes != nullptr) pfree(private_nodes);
        return bioseq_cache_status_error;
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
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

/** @return Provider ABI status after appending the batch-join domain identity. */
[[nodiscard]] bioseq_cache_status
bioseq_batch_join_append_domain_digest(
        const bioseq_cache_path_domain* domain,
        bioseq_cache_digest_sink* sink) {
    if (domain == nullptr
        || domain->provider_id != k_bioseq_batch_join_provider_id) {
        return bioseq_cache_status_unsupported;
    }
    constexpr std::array<std::uint8_t, 8> identity{
        0x4a, 0x51, 0x53, 0x42,  // provider id, little endian
        0x01, 0x00, 0x00, 0x00,  // recipe revision
    };
    return detail::append_bytes(sink, identity.data(), identity.size())
            && detail::append_bytes(
                sink,
                k_bioseq_batch_join_provider_name,
                sizeof(k_bioseq_batch_join_provider_name) - 1U)
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

/** @return Provider ABI status after appending batch-join generation inputs. */
[[nodiscard]] bioseq_cache_status
bioseq_batch_join_append_generation_digest(
        const bioseq_cache_path_domain* domain,
        bioseq_cache_digest_sink* sink) {
    const bioseq_cache_status domain_status =
        bioseq_batch_join_append_domain_digest(domain, sink);
    if (domain_status != bioseq_cache_status_ok) {
        return domain_status;
    }
    char* dependency = nodeToString(domain->dependency_nodes);
    char* private_nodes = nodeToString(domain->provider_private_nodes);
    if (dependency == nullptr || private_nodes == nullptr) {
        if (dependency != nullptr) pfree(dependency);
        if (private_nodes != nullptr) pfree(private_nodes);
        return bioseq_cache_status_error;
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
        ? bioseq_cache_status_ok
        : bioseq_cache_status_error;
}

/** @brief Emits cache-domain EXPLAIN properties for an exact batch join. */
void bioseq_batch_join_explain_exact_domain(
        List* plan_domain_nodes,
        bioseq_cache_explain_sink* sink) {
    if (sink == nullptr || sink->struct_size < sizeof(*sink)) return;
    std::int32_t plan_node_id = -1;
    const bool valid = detail::parse_batch_join_recipe_header(
        plan_domain_nodes,
        &plan_node_id);
    if (sink->property_text != nullptr) {
        sink->property_text(
            sink->opaque,
            "Bioseq Cache Provider",
            k_bioseq_batch_join_provider_name);
    }
    if (sink->property_uint64 != nullptr) {
        sink->property_uint64(
            sink->opaque,
            "Bioseq Provider Recipe Revision",
            k_bioseq_batch_join_recipe_revision);
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

/** @brief Emits cache-domain EXPLAIN properties for an exact Bioseq scan. */
void bioseq_explain_exact_domain(
        List* plan_domain_nodes,
        bioseq_cache_explain_sink* sink) {
    if (sink == nullptr || sink->struct_size < sizeof(*sink)) return;
    std::int32_t plan_node_id = -1;
    Index scan_relid = 0;
    Oid relation_oid = InvalidOid;
    const bool valid = detail::parse_recipe_header(
        plan_domain_nodes, &plan_node_id, &scan_relid, &relation_oid);
    if (sink->property_text != nullptr) {
        sink->property_text(
            sink->opaque,
            "Bioseq Cache Provider",
            k_bioseq_scan_provider_name);
    }
    if (sink->property_uint64 != nullptr) {
        sink->property_uint64(
            sink->opaque,
            "Bioseq Provider Recipe Revision",
            k_bioseq_scan_recipe_revision);
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

} // namespace bioseq::postgres::extension::cache_provider
