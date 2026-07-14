/**
 * @file bioseq_caching/pg/test_provider.cppm
 * @brief Deterministic PostgreSQL-only provider used by cache integration tests.
 *
 * This module is compiled only with BIOSEQ_CACHE_TESTING.  It deliberately
 * models no Bioseq object: any ordinary heap table with the six documented
 * test columns can act as a provider relation.  The provider is backend-local,
 * has no GUCs, and is installed explicitly by a superuser test function.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_namespace_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

export module bioseq.cache.pg.test_provider;

import bioseq.cache.provider_api;

namespace bioseq::cache::pg::test_provider {

inline constexpr std::uint32_t kProviderId = 0x54535431U;
inline constexpr std::uint64_t kCompatibilityFamily =
    0x746573745f763031ULL;
inline constexpr std::int32_t kPlanMagic = 0x54535031;
inline constexpr std::int32_t kPlanVersion = 1;
inline constexpr std::size_t kDigestBytes = 32;
inline constexpr char kApiRendezvousName[] = "bioseq_cache.api.v1";
inline constexpr char kBridgeRendezvousName[] =
    "bioseq_cache.planner_bridge.v1";

enum class MappingFault : std::uint16_t {
    None = 0,
    NullPointerNonzeroCount,
    ShortBoundaryMap,
    LongBoundaryMap,
    ShortLineageMap,
    LongLineageMap,
    ZeroConsumerAttno,
    ZeroSourceAttno,
    SourceAttnoOutOfRange,
    DuplicateConsumerAttno,
    MissingRequiredAttno,
    WrongType,
    WrongTypmod,
    WrongCollation,
    WrongSemanticId,
    SameTypedColumnPermutation,
    IllegalRepeatedSourceAttno,
    BadProviderPrivateVersion,
    NonIntegerPersistedMapNode,
    UnknownPersistedMapVersion,
    DecoratedBoundarySchemaMismatch,
    DecoratedLineageSchemaMismatch,
    ProviderError,
};

enum class DigestFault : std::uint16_t {
    None = 0,
    RecipeMismatch,
    RuntimeMismatch,
};

enum class ProjectionOrder : std::uint16_t {
    Natural = 0,
    Reverse,
};

struct CallbackCounters {
    std::uint64_t analyze_query = 0;
    std::uint64_t describe_path = 0;
    std::uint64_t domain_digest = 0;
    std::uint64_t generation_digest = 0;
    std::uint64_t runtime_digest = 0;
    std::uint64_t map_source = 0;
    std::uint64_t decorate_path = 0;
    std::uint64_t build_recipe = 0;
    std::uint64_t extract_occurrence = 0;
    std::uint64_t late_fetch_batch = 0;
    std::uint64_t late_fetch_references = 0;
    std::uint64_t planner_bridge_calls = 0;
    std::uint64_t executor_rows = 0;
    std::uint64_t gated_expensive_rows = 0;
};

inline Oid g_target_relation_oid = InvalidOid;
inline std::uint64_t g_requested_capabilities = 1;
inline MappingFault g_mapping_fault = MappingFault::None;
inline DigestFault g_digest_fault = DigestFault::None;
inline ProjectionOrder g_projection_order = ProjectionOrder::Natural;
inline bool g_force_domain_digest_collision = false;
inline bool g_provider_registered = false;
inline bool g_hook_installed = false;
inline set_rel_pathlist_hook_type g_previous_set_rel_pathlist_hook = nullptr;
inline CallbackCounters g_counters{};

/** Require superuser for every mutation of the deterministic test provider. */
void require_test_superuser() {
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("bioseq_cache test-provider controls require superuser")));
    }
}

/** Reconstruct a uint64 from two PostgreSQL Integer Nodes. */
[[nodiscard]] std::uint64_t integer_pair_value(
    const Node* low_node,
    const Node* high_node) noexcept {
    if (low_node == nullptr || high_node == nullptr
        || !IsA(low_node, Integer) || !IsA(high_node, Integer)) {
        return 0;
    }
    const auto low = static_cast<std::uint32_t>(intVal(low_node));
    const auto high = static_cast<std::uint32_t>(intVal(high_node));
    return static_cast<std::uint64_t>(low)
        | (static_cast<std::uint64_t>(high) << 32U);
}

/** Append a uint64 as two copyObject-safe Integer Nodes. */
[[nodiscard]] List* append_integer_pair(List* list, std::uint64_t value) {
    list = lappend(list, makeInteger(
        static_cast<std::int32_t>(value & 0xffffffffULL)));
    return lappend(list, makeInteger(
        static_cast<std::int32_t>(value >> 32U)));
}

/** Deterministic non-cryptographic digest used only as provider test bytes. */
[[nodiscard]] std::array<std::uint8_t, kDigestBytes>
digest_plan_nodes(const List* nodes) noexcept {
    std::array<std::uint64_t, 4> state{
        0xcbf29ce484222325ULL,
        0x84222325cbf29ce4ULL,
        0x9e3779b185ebca87ULL,
        0xd6e8feb86659fd93ULL,
    };
    if (nodes == nullptr) {
        return {};
    }
    std::size_t ordinal = 0;
    ListCell* cell = nullptr;
    foreach(cell, const_cast<List*>(nodes)) {
        const auto* node = static_cast<const Node*>(lfirst(cell));
        const std::uint32_t value = node != nullptr && IsA(node, Integer)
            ? static_cast<std::uint32_t>(intVal(node))
            : 0xffffffffU;
        for (std::size_t index = 0; index < state.size(); ++index) {
            state[index] ^= static_cast<std::uint64_t>(value)
                + (ordinal << 16U) + index;
            state[index] *= 0x100000001b3ULL + (index * 2U);
            state[index] ^= state[index] >> (17U + index);
        }
        ++ordinal;
    }
    std::array<std::uint8_t, kDigestBytes> result{};
    for (std::size_t word = 0; word < state.size(); ++word) {
        for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
            result[word * sizeof(std::uint64_t) + byte] =
                static_cast<std::uint8_t>(state[word] >> (byte * 8U));
        }
    }
    return result;
}

/** Provider raw-plan metadata; every value is a copyable PostgreSQL Node. */
[[nodiscard]] List* make_plan_nodes(
    Oid relation_oid,
    AttrNumber row_identity_attno,
    AttrNumber base_identity_attno,
    AttrNumber occurrence_attno,
    AttrNumber capability_attno) {
    List* nodes = list_make3(
        makeInteger(kPlanMagic),
        makeInteger(kPlanVersion),
        makeInteger(static_cast<std::int32_t>(relation_oid)));
    nodes = append_integer_pair(nodes, g_requested_capabilities);
    nodes = lappend(nodes, makeInteger(
        static_cast<std::int32_t>(g_projection_order)));
    nodes = lappend(nodes, makeInteger(row_identity_attno));
    nodes = lappend(nodes, makeInteger(base_identity_attno));
    nodes = lappend(nodes, makeInteger(occurrence_attno));
    nodes = lappend(nodes, makeInteger(capability_attno));
    return nodes;
}

/** Parse and validate the fixed test-provider plan recipe. */
[[nodiscard]] bool parse_plan_nodes(
    const List* nodes,
    Oid* relation_oid,
    AttrNumber* row_identity_attno,
    AttrNumber* base_identity_attno,
    AttrNumber* occurrence_attno,
    AttrNumber* capability_attno) noexcept {
    if (nodes == nullptr || list_length(nodes) != 10) {
        return false;
    }
    for (int index = 0; index < 10; ++index) {
        const auto* node = static_cast<const Node*>(list_nth(nodes, index));
        if (node == nullptr || !IsA(node, Integer)) {
            return false;
        }
    }
    if (intVal(list_nth(nodes, 0)) != kPlanMagic
        || intVal(list_nth(nodes, 1)) != kPlanVersion) {
        return false;
    }
    const auto relation = static_cast<Oid>(
        static_cast<std::uint32_t>(intVal(list_nth(nodes, 2))));
    const auto row_attno = static_cast<AttrNumber>(
        intVal(list_nth(nodes, 6)));
    const auto base_attno = static_cast<AttrNumber>(
        intVal(list_nth(nodes, 7)));
    const auto occurrence_column = static_cast<AttrNumber>(
        intVal(list_nth(nodes, 8)));
    const auto capability_column = static_cast<AttrNumber>(
        intVal(list_nth(nodes, 9)));
    if (!OidIsValid(relation) || row_attno <= 0 || base_attno <= 0
        || occurrence_column <= 0 || capability_column <= 0) {
        return false;
    }
    if (relation_oid != nullptr) {
        *relation_oid = relation;
    }
    if (row_identity_attno != nullptr) {
        *row_identity_attno = row_attno;
    }
    if (base_identity_attno != nullptr) {
        *base_identity_attno = base_attno;
    }
    if (occurrence_attno != nullptr) {
        *occurrence_attno = occurrence_column;
    }
    if (capability_attno != nullptr) {
        *capability_attno = capability_column;
    }
    return true;
}

/** Evaluate provider gates before counting the simulated expensive work. */
[[nodiscard]] TupleTableSlot* provider_exec_custom_scan(
    CustomScanState* node) {
    PlanState* child = outerPlanState(node);
    ExprContext* expression_context = node->ss.ps.ps_ExprContext;
    for (;;) {
        ResetExprContext(expression_context);
        TupleTableSlot* child_slot = ExecProcNode(child);
        if (TupIsNull(child_slot)) {
            return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
        }
        ExecCopySlot(node->ss.ss_ScanTupleSlot, child_slot);
        expression_context->ecxt_scantuple = node->ss.ss_ScanTupleSlot;
        if (node->ss.ps.qual != nullptr
            && !ExecQual(node->ss.ps.qual, expression_context)) {
            continue;
        }
        ++g_counters.gated_expensive_rows;
        ++g_counters.executor_rows;
        if (node->ss.ps.ps_ProjInfo != nullptr) {
            return ExecProject(node->ss.ps.ps_ProjInfo);
        }
        return node->ss.ss_ScanTupleSlot;
    }
}

/** Initialize the one ordinary child plan retained by the provider wrapper. */
void provider_begin_custom_scan(
    CustomScanState* node,
    EState* estate,
    int execution_flags) {
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    if (scan == nullptr || list_length(scan->custom_plans) != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq_cache test provider has a malformed child plan")));
    }
    auto* child_plan = static_cast<Plan*>(linitial(scan->custom_plans));
    outerPlanState(node) = ExecInitNode(child_plan, estate, execution_flags);
}

/** Release the ordinary child plan. */
void provider_end_custom_scan(CustomScanState* node) {
    if (outerPlanState(node) != nullptr) {
        ExecEndNode(outerPlanState(node));
        outerPlanState(node) = nullptr;
    }
}

/** Rewind both scan expression state and the ordinary child. */
void provider_rescan_custom_scan(CustomScanState* node) {
    ExecScanReScan(&node->ss);
    if (outerPlanState(node) != nullptr) {
        ExecReScan(outerPlanState(node));
    }
}

/** Expose deterministic executor counters in EXPLAIN. */
void provider_explain_custom_scan(
    CustomScanState*,
    List*,
    ExplainState* explain) {
    ExplainPropertyText("Test Provider", "generic nullable occurrence", explain);
    ExplainPropertyInteger(
        "Test Provider Rows",
        nullptr,
        static_cast<std::int64_t>(g_counters.executor_rows),
        explain);
    ExplainPropertyInteger(
        "Test Provider Gated Work",
        nullptr,
        static_cast<std::int64_t>(g_counters.gated_expensive_rows),
        explain);
}

/** Stable registered executor table. */
[[nodiscard]] const CustomExecMethods& provider_exec_methods() noexcept {
    static const CustomExecMethods methods{
        .CustomName = "BioseqCacheGenericTestProvider",
        .BeginCustomScan = provider_begin_custom_scan,
        .ExecCustomScan = provider_exec_custom_scan,
        .EndCustomScan = provider_end_custom_scan,
        .ReScanCustomScan = provider_rescan_custom_scan,
        .ExplainCustomScan = provider_explain_custom_scan,
    };
    return methods;
}

/** Allocate a plain CustomScanState; all ownership remains PostgreSQL-owned. */
[[nodiscard]] Node* provider_create_custom_scan_state(CustomScan*) {
    auto* state = reinterpret_cast<CustomScanState*>(
        newNode(sizeof(CustomScanState), T_CustomScanState));
    state->methods = &provider_exec_methods();
    return reinterpret_cast<Node*>(state);
}

/** Stable provider scan identity advertised through the cache ABI. */
[[nodiscard]] const CustomScanMethods& provider_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqCacheGenericTestProvider",
        .CreateCustomScanState = provider_create_custom_scan_state,
    };
    return methods;
}

/** Convert the provider path into a passthrough CustomScan over one child. */
[[nodiscard]] Plan* provider_plan_custom_path(
    PlannerInfo*,
    RelOptInfo* relation,
    CustomPath* best_path,
    List* target_list,
    List* clauses,
    List* custom_plans) {
    if (relation == nullptr || best_path == nullptr
        || list_length(custom_plans) != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq_cache test provider path is malformed")));
    }
    auto* scan = makeNode(CustomScan);
    scan->methods = &provider_scan_methods();
    scan->flags = best_path->flags;
    scan->scan.scanrelid = 0;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = extract_actual_clauses(clauses, false);
    scan->custom_exprs = static_cast<List*>(
        copyObject(scan->scan.plan.qual));
    scan->custom_private = static_cast<List*>(
        copyObject(best_path->custom_private));
    scan->custom_plans = custom_plans;
    const auto* child_plan = static_cast<const Plan*>(linitial(custom_plans));
    scan->custom_scan_tlist = static_cast<List*>(
        copyObject(child_plan->targetlist));
    scan->custom_relids = bms_copy(relation->relids);
    return &scan->scan.plan;
}

/** Stable provider path table. */
[[nodiscard]] const CustomPathMethods& provider_path_methods() noexcept {
    static const CustomPathMethods methods{
        .CustomName = "BioseqCacheGenericTestProvider",
        .PlanCustomPath = provider_plan_custom_path,
    };
    return methods;
}

/** True when an expression is outside the provider's reviewed total subset. */
[[nodiscard]] bool unsafe_predicate_walker(Node* node, void* opaque) {
    const Index relation_index = *static_cast<const Index*>(opaque);
    if (node == nullptr) {
        return false;
    }
    if (IsA(node, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(node);
        return variable->varlevelsup != 0
            || variable->varno != relation_index
            || variable->varattno <= 0
            || !bms_is_empty(variable->varnullingrels);
    }
    if (IsA(node, Const)) {
        return false;
    }
    if (IsA(node, Param) || IsA(node, FuncExpr)
        || IsA(node, SubLink) || IsA(node, Aggref)
        || IsA(node, WindowFunc) || IsA(node, SetToDefault)) {
        return true;
    }
    if (IsA(node, OpExpr)) {
        const auto* operation = reinterpret_cast<const OpExpr*>(node);
        const Oid function_oid = get_opcode(operation->opno);
        if (!OidIsValid(function_oid)
            || get_func_namespace(function_oid) != PG_CATALOG_NAMESPACE) {
            return true;
        }
        return expression_tree_walker(
            node, unsafe_predicate_walker, opaque);
    }
    if (IsA(node, BoolExpr) || IsA(node, NullTest)
        || IsA(node, BooleanTest) || IsA(node, RelabelType)) {
        return expression_tree_walker(
            node, unsafe_predicate_walker, opaque);
    }
    return true;
}

/** Stable semantic identity for one provider column meaning. */
[[nodiscard]] std::array<std::uint64_t, 2> column_semantic_id(
    AttrNumber attribute_number,
    Form_pg_attribute attribute,
    bool lineage) noexcept {
    std::uint64_t high = lineage
        ? 0x4c494e4541474500ULL
        : 0x424f554e44415200ULL;
    high ^= static_cast<std::uint64_t>(
        static_cast<std::uint16_t>(attribute_number));
    std::uint64_t low = 0xcbf29ce484222325ULL;
    low ^= static_cast<std::uint64_t>(attribute->atttypid);
    low *= 0x100000001b3ULL;
    low ^= static_cast<std::uint32_t>(attribute->atttypmod);
    low *= 0x100000001b3ULL;
    low ^= static_cast<std::uint64_t>(attribute->attcollation);
    low *= 0x100000001b3ULL;
    low ^= static_cast<std::uint16_t>(attribute_number);
    return {high, low};
}

/** Fill one ABI schema element from a physical table attribute. */
void fill_column_schema(
    BioseqCacheColumnSchemaV1& output,
    AttrNumber attribute_number,
    Form_pg_attribute attribute,
    bool lineage) noexcept {
    const auto semantic = column_semantic_id(
        attribute_number, attribute, lineage);
    output = {
        .struct_size = sizeof(BioseqCacheColumnSchemaV1),
        .attno = static_cast<std::uint32_t>(attribute_number),
        .semantic_id_high = semantic[0],
        .semantic_id_low = semantic[1],
        .type_oid = attribute->atttypid,
        .typmod = attribute->atttypmod,
        .collation_oid = attribute->attcollation,
        .flags = lineage ? 1U : 0U,
    };
}

/** Find a schema element by its 1-based provider attribute number. */
[[nodiscard]] const BioseqCacheColumnSchemaV1* find_schema_column(
    const BioseqCacheColumnSchemaV1* columns,
    std::size_t count,
    std::uint32_t attribute_number) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (columns[index].attno == attribute_number) {
            return &columns[index];
        }
    }
    return nullptr;
}

/** Query analysis admits only statements containing the configured table. */
[[nodiscard]] BioseqCacheStatusV1 provider_analyze_query(
    const Query* query,
    BioseqCacheQueryIntentV1* output) {
    ++g_counters.analyze_query;
    if (query == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || !OidIsValid(g_target_relation_oid)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    bool found = false;
    ListCell* cell = nullptr;
    foreach(cell, query->rtable) {
        const auto* entry = static_cast<const RangeTblEntry*>(lfirst(cell));
        if (entry != nullptr && entry->rtekind == RTE_RELATION
            && entry->relid == g_target_relation_oid) {
            found = true;
            break;
        }
    }
    if (!found) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    output->struct_size = sizeof(*output);
    output->provider_id = kProviderId;
    output->flags = 0;
    output->reserved = 0;
    output->provider_private_nodes = list_make2(
        makeInteger(kPlanVersion),
        makeInteger(static_cast<std::int32_t>(g_target_relation_oid)));
    return BIOSEQ_CACHE_STATUS_OK;
}

/** Advertise a complete raw boundary and one-occurrence lineage contract. */
[[nodiscard]] BioseqCacheStatusV1 provider_describe_path(
    PlannerInfo* root,
    RelOptInfo* relation_info,
    Path* provider_path,
    const BioseqCacheQueryIntentV1* intent,
    BioseqCachePathDomainV1* output) {
    ++g_counters.describe_path;
    if (root == nullptr || relation_info == nullptr || provider_path == nullptr
        || intent == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || provider_path->parent != relation_info
        || provider_path->pathtype != T_CustomScan
        || reinterpret_cast<CustomPath*>(provider_path)->methods
            != &provider_path_methods()
        || intent->provider_id != kProviderId
        || relation_info->relid == 0) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    const auto* range_entry = rt_fetch(
        relation_info->relid, root->parse->rtable);
    if (range_entry == nullptr || range_entry->rtekind != RTE_RELATION
        || range_entry->relid != g_target_relation_oid) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    List* clauses = extract_actual_clauses(
        relation_info->baserestrictinfo, false);
    if (clauses == NIL) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    Expr* predicate = nullptr;
    if (list_length(clauses) == 1) {
        predicate = static_cast<Expr*>(copyObject(linitial(clauses)));
    } else {
        predicate = make_andclause(
            static_cast<List*>(copyObject(clauses)));
    }
    Index relation_index = relation_info->relid;
    if (predicate == nullptr
        || unsafe_predicate_walker(
            reinterpret_cast<Node*>(predicate), &relation_index)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    Relation relation = relation_open(range_entry->relid, NoLock);
    TupleDesc descriptor = RelationGetDescr(relation);
    std::size_t boundary_count = 0;
    for (int index = 0; index < descriptor->natts; ++index) {
        if (!TupleDescAttr(descriptor, index)->attisdropped) {
            ++boundary_count;
        }
    }
    if (boundary_count == 0 || boundary_count > 1024) {
        relation_close(relation, NoLock);
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    auto* boundary = static_cast<BioseqCacheColumnSchemaV1*>(
        palloc0(boundary_count * sizeof(BioseqCacheColumnSchemaV1)));
    auto* required_boundary = static_cast<std::uint32_t*>(
        palloc0(boundary_count * sizeof(std::uint32_t)));
    std::size_t boundary_index = 0;
    for (int index = 0; index < descriptor->natts; ++index) {
        Form_pg_attribute attribute = TupleDescAttr(descriptor, index);
        if (attribute->attisdropped) {
            continue;
        }
        const AttrNumber attno = static_cast<AttrNumber>(index + 1);
        fill_column_schema(
            boundary[boundary_index], attno, attribute, false);
        ++boundary_index;
    }
    for (std::size_t index = 0; index < boundary_count; ++index) {
        const std::size_t schema_index =
            g_projection_order == ProjectionOrder::Natural
            ? index
            : boundary_count - index - 1;
        required_boundary[index] = boundary[schema_index].attno;
    }

    constexpr std::array<const char*, 3> lineage_names{
        "row_id", "base_id", "occurrence_no"};
    auto* lineage = static_cast<BioseqCacheColumnSchemaV1*>(
        palloc0(lineage_names.size() * sizeof(BioseqCacheColumnSchemaV1)));
    auto* required_lineage = static_cast<std::uint32_t*>(
        palloc0(lineage_names.size() * sizeof(std::uint32_t)));
    for (std::size_t index = 0; index < lineage_names.size(); ++index) {
        const AttrNumber attno = get_attnum(
            range_entry->relid, lineage_names[index]);
        if (attno <= 0 || attno > descriptor->natts) {
            relation_close(relation, NoLock);
            return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
        }
        fill_column_schema(
            lineage[index],
            attno,
            TupleDescAttr(descriptor, attno - 1),
            true);
        required_lineage[index] = static_cast<std::uint32_t>(attno);
    }
    relation_close(relation, NoLock);

    output->struct_size = sizeof(*output);
    output->provider_id = kProviderId;
    output->compatibility_family_id = kCompatibilityFamily;
    output->capability_mask = g_requested_capabilities;
    output->coverage_predicate = predicate;
    output->boundary_columns = boundary;
    output->boundary_column_count = boundary_count;
    output->lineage_columns = lineage;
    output->lineage_column_count = lineage_names.size();
    output->required_boundary_attnos = required_boundary;
    output->required_boundary_attno_count = boundary_count;
    output->required_lineage_attnos = required_lineage;
    output->required_lineage_attno_count = lineage_names.size();
    output->dependency_nodes = list_make1(
        makeInteger(static_cast<std::int32_t>(range_entry->relid)));
    output->provider_private_nodes = static_cast<List*>(
        copyObject(reinterpret_cast<CustomPath*>(provider_path)->custom_private));
    return BIOSEQ_CACHE_STATUS_OK;
}

/** Append stable bytes to a cache-owned bounded digest sink. */
[[nodiscard]] bool append_digest_bytes(
    BioseqCacheDigestSinkV1* sink,
    const void* data,
    std::size_t size) noexcept {
    return sink != nullptr && sink->struct_size >= sizeof(*sink)
        && sink->append != nullptr && (size == 0 || data != nullptr)
        && sink->append(sink->opaque, data, size);
}

/** Domain digest excludes output-only capabilities by construction. */
[[nodiscard]] BioseqCacheStatusV1 provider_append_domain_digest(
    const BioseqCachePathDomainV1* domain,
    BioseqCacheDigestSinkV1* sink) {
    ++g_counters.domain_digest;
    if (domain == nullptr || domain->provider_id != kProviderId
        || domain->coverage_predicate == nullptr) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    const std::uint64_t header = g_force_domain_digest_collision
        ? 0x434f4c4c4953494fULL
        : kCompatibilityFamily;
    if (!append_digest_bytes(sink, &header, sizeof(header))) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    if (g_force_domain_digest_collision) {
        return BIOSEQ_CACHE_STATUS_OK;
    }
    char* serialized = nodeToString(domain->coverage_predicate);
    if (serialized == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::size_t length = std::strlen(serialized);
    const bool appended = append_digest_bytes(sink, &length, sizeof(length))
        && append_digest_bytes(sink, serialized, length);
    pfree(serialized);
    return appended ? BIOSEQ_CACHE_STATUS_OK : BIOSEQ_CACHE_STATUS_ERROR;
}

/** Generation digest includes exact capability and ordered raw schemas. */
[[nodiscard]] BioseqCacheStatusV1 provider_append_generation_digest(
    const BioseqCachePathDomainV1* domain,
    BioseqCacheDigestSinkV1* sink) {
    ++g_counters.generation_digest;
    if (provider_append_domain_digest(domain, sink)
        != BIOSEQ_CACHE_STATUS_OK) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    if (!append_digest_bytes(
            sink, &domain->capability_mask, sizeof(domain->capability_mask))) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    for (std::size_t index = 0; index < domain->boundary_column_count; ++index) {
        if (!append_digest_bytes(
                sink, &domain->boundary_columns[index],
                sizeof(BioseqCacheColumnSchemaV1))) {
            return BIOSEQ_CACHE_STATUS_ERROR;
        }
    }
    for (std::size_t index = 0; index < domain->lineage_column_count; ++index) {
        if (!append_digest_bytes(
                sink, &domain->lineage_columns[index],
                sizeof(BioseqCacheColumnSchemaV1))) {
            return BIOSEQ_CACHE_STATUS_ERROR;
        }
    }
    return BIOSEQ_CACHE_STATUS_OK;
}

} // namespace bioseq::cache::pg::test_provider
