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
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/bitmapset.h"
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
#include "utils/datum.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/rel.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

export module bioseq.cache.pg.test_provider;

import bioseq.cache.provider_api;
import bioseq.pg_sql_boundary;

namespace bioseq::cache::pg::test_provider {

inline constexpr std::uint32_t kProviderId = 0x54535431U;
inline constexpr std::uint64_t kCompatibilityFamily =
    0x746573745f763031ULL;
inline constexpr std::int32_t kPlanMagic = 0x54535031;
inline constexpr std::int32_t kPlanVersion = 2;
inline constexpr std::uint32_t kGenerationSemanticVersion = 2;
inline constexpr std::size_t kPlanFixedNodeCount = 11;
inline constexpr std::size_t kMaximumPlanNodeCount = 1024;
inline constexpr std::size_t kMaximumRequiredBoundaryCount =
    kMaximumPlanNodeCount - kPlanFixedNodeCount;
inline constexpr std::size_t kDigestBytes = 32;
inline constexpr std::size_t kMaximumTargetRelations = 16;
inline constexpr std::uint32_t kPgRelFileLocatorIdentityKindV1 = 1;
inline constexpr std::size_t kPgRelFileLocatorIdentityBytesV1 =
    sizeof(Oid) + sizeof(Oid) + sizeof(RelFileNumber);

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
    ProviderUnsupported,
    ProviderStatusError,
    ProviderQueryCanceled,
    LateFetchErrorOnce,
    LateFetchPartialStatusErrorOnce,
    PersistedWrongTopTag,
    PersistedShortTop,
    PersistedLongTop,
    PersistedBadMagic,
    PersistedBadVersion,
    PersistedCountOverflow,
    PersistedWrongEntryTag,
    PersistedShortEntry,
    PersistedLongEntry,
    PersistedBadSemanticSize,
    PersistedBadFingerprintSize,
    PersistedBadEntryMetadata,
    PersistedSourceFingerprintMismatch,
    PersistedConsumerFingerprintMismatch,
    PersistedDecoratedFingerprintMismatch,
    PersistedMapFingerprintMismatch,
};

enum class DigestFault : std::uint16_t {
    None = 0,
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
    std::uint64_t late_fetch_values = 0;
    std::uint64_t late_fetch_bytes = 0;
    std::uint64_t late_fetch_errors = 0;
    std::uint64_t planner_bridge_calls = 0;
    std::uint64_t executor_rows = 0;
    std::uint64_t gated_expensive_rows = 0;
    std::uint64_t last_boundary_column_count = 0;
    std::uint64_t last_required_boundary_count = 0;
    std::uint64_t last_lineage_column_count = 0;
    std::uint64_t last_required_lineage_count = 0;
    std::uint64_t last_recipe_required_boundary_count = 0;
};

inline Oid g_target_relation_oid = InvalidOid;
inline std::array<Oid, kMaximumTargetRelations> g_additional_target_relations{};
inline std::size_t g_additional_target_relation_count = 0;
inline std::uint64_t g_requested_capabilities = 1;
inline MappingFault g_mapping_fault = MappingFault::None;
inline const PlannedStmt* g_persisted_mapping_fault_statement = nullptr;
inline DigestFault g_digest_fault = DigestFault::None;
inline ProjectionOrder g_projection_order = ProjectionOrder::Natural;
inline bool g_force_domain_digest_collision = false;
inline bool g_provider_registered = false;
inline bool g_provider_exact_source_elision_safe = true;
inline BioseqCacheProviderRegistrationV2 g_provider_registration{
    .struct_size = sizeof(BioseqCacheProviderRegistrationV2),
};
inline bool g_hook_installed = false;
inline bool g_scan_methods_registered = false;
inline set_rel_pathlist_hook_type g_previous_set_rel_pathlist_hook = nullptr;
inline CallbackCounters g_counters{};
inline std::array<std::uint8_t, kDigestBytes> g_provider_security_digest{};

[[nodiscard]] bool configured_target_relation(Oid relation_oid) noexcept {
    if (!OidIsValid(relation_oid)) {
        return false;
    }
    if (relation_oid == g_target_relation_oid) {
        return true;
    }
    for (std::size_t index = 0;
         index < g_additional_target_relation_count;
         ++index) {
        if (g_additional_target_relations[index] == relation_oid) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool add_configured_target_relation(
        Oid relation_oid) noexcept {
    if (!OidIsValid(relation_oid) || configured_target_relation(relation_oid)) {
        return OidIsValid(relation_oid);
    }
    if (g_additional_target_relation_count
        >= g_additional_target_relations.size()) {
        return false;
    }
    g_additional_target_relations[
        g_additional_target_relation_count++] = relation_oid;
    return true;
}

/** Require superuser for every mutation of the deterministic test provider. */
void require_test_superuser() {
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("bioseq_cache test-provider controls require superuser")));
    }
}

/** Encode the cache ABI's padding-free PostgreSQL locator identity. */
[[nodiscard]] bool provider_relation_physical_identity(
        Oid relation_oid,
        BioseqCachePhysicalIdentityV2* output) {
    if (!OidIsValid(relation_oid) || output == nullptr
        || kPgRelFileLocatorIdentityBytesV1 > sizeof(output->bytes)) {
        return false;
    }
    Relation relation = table_open(relation_oid, AccessShareLock);
    const RelFileLocator locator = relation->rd_locator;
    table_close(relation, AccessShareLock);
    if (!OidIsValid(locator.spcOid) || !OidIsValid(locator.dbOid)
        || !RelFileNumberIsValid(locator.relNumber)) {
        return false;
    }
    *output = BioseqCachePhysicalIdentityV2{
        .struct_size = sizeof(BioseqCachePhysicalIdentityV2),
        .kind = kPgRelFileLocatorIdentityKindV1,
        .length = static_cast<std::uint32_t>(
            kPgRelFileLocatorIdentityBytesV1),
    };
    std::size_t offset = 0;
    std::memcpy(output->bytes + offset, &locator.spcOid, sizeof(locator.spcOid));
    offset += sizeof(locator.spcOid);
    std::memcpy(output->bytes + offset, &locator.dbOid, sizeof(locator.dbOid));
    offset += sizeof(locator.dbOid);
    std::memcpy(
        output->bytes + offset, &locator.relNumber, sizeof(locator.relNumber));
    return true;
}

/** Emit a provider-complete write marker before transaction PRE_COMMIT. */
[[nodiscard]] bool record_provider_relation_write(Oid relation_oid) {
    BioseqCachePhysicalIdentityV2 identity{};
    void** slot = find_rendezvous_variable(
        bioseq::cache::kCacheApiRendezvousName);
    if (!provider_relation_physical_identity(relation_oid, &identity)
        || slot == nullptr || *slot == nullptr) {
        return false;
    }
    const auto* api = static_cast<const BioseqCacheApiV2*>(*slot);
    if (api->struct_size < sizeof(*api)
        || api->abi_version != bioseq::cache::kProviderAbiVersion
        || api->postgres_major
            != static_cast<std::uint32_t>(PG_VERSION_NUM / 10000)
        || api->reserved != 0
        || api->instance_cookie == 0
        || api->record_relation_write == nullptr) {
        return false;
    }
    api->record_relation_write(
        static_cast<std::uint32_t>(MyDatabaseId),
        static_cast<std::uint32_t>(relation_oid),
        &identity);
    return true;
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

struct ProjectedBoundaryWalkV1 {
    Index relation_index = 0;
    Bitmapset* attnos = nullptr;
    bool whole_row = false;
    bool invalid = false;
};

/** Collect only output-boundary Vars; base restriction Vars stay late-fetchable. */
[[nodiscard]] bool projected_boundary_walker(Node* node, void* opaque) {
    if (node == nullptr) {
        return false;
    }
    auto* context = static_cast<ProjectedBoundaryWalkV1*>(opaque);
    if (context == nullptr) {
        return true;
    }
    if (IsA(node, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(node);
        if (variable->varlevelsup != 0
            || variable->varno != context->relation_index) {
            return false;
        }
        if (variable->varattno == InvalidAttrNumber) {
            context->whole_row = true;
        } else if (variable->varattno > 0) {
            context->attnos = bms_add_member(
                context->attnos, variable->varattno);
        } else {
            context->invalid = true;
        }
        return false;
    }
    return expression_tree_walker(node, projected_boundary_walker, opaque);
}

/** Freeze projected physical attributes in stable table order. */
[[nodiscard]] bool collect_projected_boundary_attnos(
    RelOptInfo* relation_info,
    Index relation_index,
    Oid relation_oid,
    std::uint32_t** output,
    std::size_t* output_count) {
    if (relation_info == nullptr || relation_info->reltarget == nullptr
        || relation_index == 0 || !OidIsValid(relation_oid)
        || output == nullptr || output_count == nullptr) {
        return false;
    }
    *output = nullptr;
    *output_count = 0;
    ProjectedBoundaryWalkV1 context{.relation_index = relation_index};
    static_cast<void>(projected_boundary_walker(
        reinterpret_cast<Node*>(relation_info->reltarget->exprs), &context));
    if (context.invalid) {
        bms_free(context.attnos);
        return false;
    }

    Relation relation = table_open(relation_oid, NoLock);
    TupleDesc descriptor = RelationGetDescr(relation);
    std::size_t count = 0;
    for (int index = 0; index < descriptor->natts; ++index) {
        if (!TupleDescAttr(descriptor, index)->attisdropped
            && (context.whole_row
                || bms_is_member(index + 1, context.attnos))) {
            ++count;
        }
    }
    if (count > kMaximumRequiredBoundaryCount) {
        table_close(relation, NoLock);
        bms_free(context.attnos);
        return false;
    }
    auto* attnos = count == 0
        ? nullptr
        : static_cast<std::uint32_t*>(
            palloc0(count * sizeof(std::uint32_t)));
    std::size_t output_index = 0;
    for (int index = 0; index < descriptor->natts; ++index) {
        if (!TupleDescAttr(descriptor, index)->attisdropped
            && (context.whole_row
                || bms_is_member(index + 1, context.attnos))) {
            attnos[output_index++] = static_cast<std::uint32_t>(index + 1);
        }
    }
    table_close(relation, NoLock);
    bms_free(context.attnos);
    *output = attnos;
    *output_count = count;
    return true;
}

/** Provider raw-plan metadata; every value is a copyable PostgreSQL Node. */
[[nodiscard]] List* make_plan_nodes(
    Oid relation_oid,
    AttrNumber row_identity_attno,
    AttrNumber base_identity_attno,
    AttrNumber occurrence_attno,
    AttrNumber capability_attno,
    const std::uint32_t* required_boundary_attnos,
    std::size_t required_boundary_count) {
    List* nodes = list_make3(
        makeInteger(kPlanVersion),
        makeInteger(kPlanMagic),
        makeInteger(static_cast<std::int32_t>(relation_oid)));
    nodes = append_integer_pair(nodes, g_requested_capabilities);
    nodes = lappend(nodes, makeInteger(
        static_cast<std::int32_t>(g_projection_order)));
    nodes = lappend(nodes, makeInteger(row_identity_attno));
    nodes = lappend(nodes, makeInteger(base_identity_attno));
    nodes = lappend(nodes, makeInteger(occurrence_attno));
    nodes = lappend(nodes, makeInteger(capability_attno));
    nodes = lappend(nodes, makeInteger(
        static_cast<std::int32_t>(required_boundary_count)));
    for (std::size_t index = 0; index < required_boundary_count; ++index) {
        const std::size_t source_index =
            g_projection_order == ProjectionOrder::Natural
            ? index
            : required_boundary_count - index - 1;
        nodes = lappend(nodes, makeInteger(static_cast<std::int32_t>(
            required_boundary_attnos[source_index])));
    }
    return nodes;
}

[[nodiscard]] std::uint32_t plan_required_boundary_attno(
    const List* nodes, std::size_t index) noexcept {
    return static_cast<std::uint32_t>(intVal(list_nth(
        nodes, static_cast<int>(kPlanFixedNodeCount + index))));
}

/** Parse and validate the fixed test-provider plan recipe. */
[[nodiscard]] bool parse_plan_nodes(
    const List* nodes,
    Oid* relation_oid,
    AttrNumber* row_identity_attno,
    AttrNumber* base_identity_attno,
    AttrNumber* occurrence_attno,
    AttrNumber* capability_attno,
    std::size_t* required_boundary_count) noexcept {
    if (nodes == nullptr
        || list_length(nodes) < static_cast<int>(kPlanFixedNodeCount)) {
        return false;
    }
    for (int index = 0; index < list_length(nodes); ++index) {
        const auto* node = static_cast<const Node*>(list_nth(nodes, index));
        if (node == nullptr || !IsA(node, Integer)) {
            return false;
        }
    }
    if (intVal(list_nth(nodes, 0)) != kPlanVersion
        || intVal(list_nth(nodes, 1)) != kPlanMagic) {
        return false;
    }
    const auto relation = static_cast<Oid>(
        static_cast<std::uint32_t>(intVal(list_nth(nodes, 2))));
    const int raw_row_attno = intVal(list_nth(nodes, 6));
    const int raw_base_attno = intVal(list_nth(nodes, 7));
    const int raw_occurrence_attno = intVal(list_nth(nodes, 8));
    const int raw_capability_attno = intVal(list_nth(nodes, 9));
    const int required_count = intVal(list_nth(nodes, 10));
    if (!OidIsValid(relation)
        || raw_row_attno <= 0
        || raw_row_attno > MaxTupleAttributeNumber
        || raw_base_attno <= 0
        || raw_base_attno > MaxTupleAttributeNumber
        || raw_occurrence_attno <= 0
        || raw_occurrence_attno > MaxTupleAttributeNumber
        || raw_capability_attno <= 0
        || raw_capability_attno > MaxTupleAttributeNumber
        || required_count < 0
        || required_count
            > static_cast<int>(kMaximumRequiredBoundaryCount)
        || list_length(nodes)
            != static_cast<int>(kPlanFixedNodeCount) + required_count) {
        return false;
    }
    const auto row_attno = static_cast<AttrNumber>(raw_row_attno);
    const auto base_attno = static_cast<AttrNumber>(raw_base_attno);
    const auto occurrence_column = static_cast<AttrNumber>(
        raw_occurrence_attno);
    const auto capability_column = static_cast<AttrNumber>(
        raw_capability_attno);
    for (int index = 0; index < required_count; ++index) {
        const std::uint32_t attno = plan_required_boundary_attno(
            nodes, static_cast<std::size_t>(index));
        if (attno == 0
            || attno > static_cast<std::uint32_t>(MaxTupleAttributeNumber)) {
            return false;
        }
        for (int prior = 0; prior < index; ++prior) {
            if (plan_required_boundary_attno(
                    nodes, static_cast<std::size_t>(prior)) == attno) {
                return false;
            }
        }
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
    if (required_boundary_count != nullptr) {
        *required_boundary_count = static_cast<std::size_t>(required_count);
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
    List* all_clauses = list_concat(
        list_copy(clauses), list_copy(best_path->custom_restrictinfo));
    scan->scan.plan.qual = extract_actual_clauses(all_clauses, false);
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
    BioseqCacheColumnSchemaV2& output,
    AttrNumber attribute_number,
    Form_pg_attribute attribute,
    bool lineage) noexcept {
    const auto semantic = column_semantic_id(
        attribute_number, attribute, lineage);
    output = {
        .struct_size = sizeof(BioseqCacheColumnSchemaV2),
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
[[nodiscard]] const BioseqCacheColumnSchemaV2* find_schema_column(
    const BioseqCacheColumnSchemaV2* columns,
    std::size_t count,
    std::uint32_t attribute_number) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (columns[index].attno == attribute_number) {
            return &columns[index];
        }
    }
    return nullptr;
}

struct NormalizedPredicateContextV1 {
    const BioseqCachePathDomainV2* domain = nullptr;
    bool valid = true;
};

/** Semantic callbacks reject any query-global Var leaked by the adapter. */
bool normalized_predicate_walker(Node* node, void* opaque) {
    if (node == nullptr) {
        return false;
    }
    auto* context = static_cast<NormalizedPredicateContextV1*>(opaque);
    if (context == nullptr || context->domain == nullptr
        || !context->valid) {
        return true;
    }
    if (IsA(node, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(node);
        const auto* column = variable->varattno > 0
            ? find_schema_column(
                context->domain->boundary_columns,
                context->domain->boundary_column_count,
                static_cast<std::uint32_t>(variable->varattno))
            : nullptr;
        context->valid = variable->varno == INDEX_VAR
            && variable->varnosyn == INDEX_VAR
            && variable->varattnosyn == variable->varattno
            && variable->varlevelsup == 0
            && bms_is_empty(variable->varnullingrels)
            && variable->location == -1
            && column != nullptr
            && variable->vartype == column->type_oid
            && variable->vartypmod == column->typmod
            && variable->varcollid == column->collation_oid;
        return !context->valid;
    }
    return expression_tree_walker(
        node, normalized_predicate_walker, opaque);
}

[[nodiscard]] bool provider_received_normalized_predicate(
        const BioseqCachePathDomainV2& domain) {
    NormalizedPredicateContextV1 context{.domain = &domain};
    static_cast<void>(normalized_predicate_walker(
        reinterpret_cast<Node*>(domain.coverage_predicate), &context));
    return context.valid;
}

/** Query analysis admits only statements containing the configured table. */
[[nodiscard]] BioseqCacheStatusV2 provider_analyze_query(
    const Query* query,
    BioseqCacheQueryIntentV2* output) {
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
            && configured_target_relation(entry->relid)) {
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
[[nodiscard]] BioseqCacheStatusV2 provider_describe_path(
    PlannerInfo* root,
    RelOptInfo* relation_info,
    Path* provider_path,
    const BioseqCacheQueryIntentV2* intent,
    BioseqCachePathDomainV2* output) {
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

    const bool index_valid = root->simple_rte_array != nullptr
        ? relation_info->relid
            < static_cast<Index>(root->simple_rel_array_size)
        : relation_info->relid
            <= static_cast<Index>(list_length(root->parse->rtable));
    const auto* range_entry = index_valid
        ? planner_rt_fetch(relation_info->relid, root)
        : nullptr;
    if (range_entry == nullptr || range_entry->rtekind != RTE_RELATION
        || !configured_target_relation(range_entry->relid)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    Oid recipe_relation_oid = InvalidOid;
    std::size_t required_boundary_count = 0;
    const auto* provider_custom = reinterpret_cast<CustomPath*>(provider_path);
    if (!parse_plan_nodes(
            provider_custom->custom_private,
            &recipe_relation_oid,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &required_boundary_count)
        || recipe_relation_oid != range_entry->relid) {
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

    Relation relation = table_open(range_entry->relid, NoLock);
    TupleDesc descriptor = RelationGetDescr(relation);
    std::size_t boundary_count = 0;
    for (int index = 0; index < descriptor->natts; ++index) {
        if (!TupleDescAttr(descriptor, index)->attisdropped) {
            ++boundary_count;
        }
    }
    if (boundary_count == 0 || boundary_count > 1024) {
        table_close(relation, NoLock);
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    auto* boundary = static_cast<BioseqCacheColumnSchemaV2*>(
        palloc0(boundary_count * sizeof(BioseqCacheColumnSchemaV2)));
    auto* required_boundary = required_boundary_count == 0
        ? nullptr
        : static_cast<std::uint32_t*>(palloc0(
            required_boundary_count * sizeof(std::uint32_t)));
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
    for (std::size_t index = 0; index < required_boundary_count; ++index) {
        const std::uint32_t attno = plan_required_boundary_attno(
            provider_custom->custom_private, index);
        if (find_schema_column(boundary, boundary_count, attno) == nullptr) {
            table_close(relation, NoLock);
            return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
        }
        required_boundary[index] = attno;
    }

    constexpr std::array<const char*, 3> lineage_names{
        "row_id", "base_id", "occurrence_no"};
    auto* lineage = static_cast<BioseqCacheColumnSchemaV2*>(
        palloc0(lineage_names.size() * sizeof(BioseqCacheColumnSchemaV2)));
    auto* required_lineage = static_cast<std::uint32_t*>(
        palloc0(lineage_names.size() * sizeof(std::uint32_t)));
    for (std::size_t index = 0; index < lineage_names.size(); ++index) {
        const AttrNumber attno = get_attnum(
            range_entry->relid, lineage_names[index]);
        if (attno <= 0 || attno > descriptor->natts) {
            table_close(relation, NoLock);
            return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
        }
        fill_column_schema(
            lineage[index],
            attno,
            TupleDescAttr(descriptor, attno - 1),
            true);
        required_lineage[index] = static_cast<std::uint32_t>(attno);
    }
    table_close(relation, NoLock);

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
    output->required_boundary_attno_count = required_boundary_count;
    output->required_lineage_attnos = required_lineage;
    output->required_lineage_attno_count = lineage_names.size();
    output->dependency_nodes = list_make1(
        makeInteger(static_cast<std::int32_t>(range_entry->relid)));
    output->provider_private_nodes = static_cast<List*>(
        copyObject(reinterpret_cast<CustomPath*>(provider_path)->custom_private));
    output->provider_security_digest_format_version =
        BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1;
    output->provider_security_flags = 0;
    std::memcpy(
        output->provider_security_digest,
        g_provider_security_digest.data(),
        sizeof(output->provider_security_digest));
    g_counters.last_boundary_column_count = boundary_count;
    g_counters.last_required_boundary_count = required_boundary_count;
    g_counters.last_lineage_column_count = lineage_names.size();
    g_counters.last_required_lineage_count = lineage_names.size();
    return BIOSEQ_CACHE_STATUS_OK;
}

/** Append stable bytes to a cache-owned bounded digest sink. */
[[nodiscard]] bool append_digest_bytes(
    BioseqCacheDigestSinkV2* sink,
    const void* data,
    std::size_t size) noexcept {
    return sink != nullptr && sink->struct_size >= sizeof(*sink)
        && sink->append != nullptr && (size == 0 || data != nullptr)
        && sink->append(sink->opaque, data, size);
}

/**
 * Domain bytes identify the stable provider relation contract.  Predicates
 * deliberately do not participate: different predicates over one relation
 * must meet in the same coverage directory before the generation key and
 * coverage algebra distinguish them.
 */
[[nodiscard]] BioseqCacheStatusV2 provider_append_domain_digest(
    const BioseqCachePathDomainV2* domain,
    BioseqCacheDigestSinkV2* sink) {
    ++g_counters.domain_digest;
    if (domain == nullptr || domain->provider_id != kProviderId
        || domain->coverage_predicate == nullptr
        || !provider_received_normalized_predicate(*domain)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    Oid relation_oid = InvalidOid;
    AttrNumber row_attno = 0;
    AttrNumber base_attno = 0;
    AttrNumber occurrence_attno = 0;
    AttrNumber capability_attno = 0;
    if (!parse_plan_nodes(
            domain->provider_private_nodes,
            &relation_oid,
            &row_attno,
            &base_attno,
            &occurrence_attno,
            &capability_attno,
            nullptr)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    const std::uint64_t header = g_force_domain_digest_collision
        ? 0x434f4c4c4953494fULL
        : kCompatibilityFamily;
    const std::array<std::uint32_t, 5> identity{
        static_cast<std::uint32_t>(relation_oid),
        static_cast<std::uint32_t>(row_attno),
        static_cast<std::uint32_t>(base_attno),
        static_cast<std::uint32_t>(occurrence_attno),
        static_cast<std::uint32_t>(capability_attno),
    };
    return append_digest_bytes(sink, &header, sizeof(header))
            && append_digest_bytes(
                sink, identity.data(), identity.size() * sizeof(identity[0]))
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

/** Generation bytes add the exact predicate, capability, and ordered schemas. */
[[nodiscard]] BioseqCacheStatusV2 provider_append_generation_digest(
    const BioseqCachePathDomainV2* domain,
    BioseqCacheDigestSinkV2* sink) {
    ++g_counters.generation_digest;
    if (provider_append_domain_digest(domain, sink)
        != BIOSEQ_CACHE_STATUS_OK) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    char* serialized = nodeToString(domain->coverage_predicate);
    if (serialized == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::size_t predicate_size = std::strlen(serialized);
    const std::uint64_t stable_predicate_size = predicate_size;
    const bool predicate_appended = append_digest_bytes(
            sink, &stable_predicate_size, sizeof(stable_predicate_size))
        && append_digest_bytes(sink, serialized, predicate_size);
    pfree(serialized);
    if (!predicate_appended) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    if (!append_digest_bytes(
            sink, &domain->capability_mask, sizeof(domain->capability_mask))) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::array<std::uint32_t, 5> semantic_shape{
        kGenerationSemanticVersion,
        static_cast<std::uint32_t>(domain->boundary_column_count),
        static_cast<std::uint32_t>(domain->lineage_column_count),
        static_cast<std::uint32_t>(domain->required_boundary_attno_count),
        static_cast<std::uint32_t>(domain->required_lineage_attno_count),
    };
    if (!append_digest_bytes(
            sink,
            semantic_shape.data(),
            semantic_shape.size() * sizeof(semantic_shape[0]))) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    for (std::size_t index = 0; index < domain->boundary_column_count; ++index) {
        if (!append_digest_bytes(
                sink, &domain->boundary_columns[index],
                sizeof(BioseqCacheColumnSchemaV2))) {
            return BIOSEQ_CACHE_STATUS_ERROR;
        }
    }
    for (std::size_t index = 0; index < domain->lineage_column_count; ++index) {
        if (!append_digest_bytes(
                sink, &domain->lineage_columns[index],
                sizeof(BioseqCacheColumnSchemaV2))) {
            return BIOSEQ_CACHE_STATUS_ERROR;
        }
    }
    if (!append_digest_bytes(
            sink,
            domain->required_boundary_attnos,
            domain->required_boundary_attno_count
                * sizeof(std::uint32_t))
        || !append_digest_bytes(
            sink,
            domain->required_lineage_attnos,
            domain->required_lineage_attno_count
                * sizeof(std::uint32_t))) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    return BIOSEQ_CACHE_STATUS_OK;
}

} // namespace bioseq::cache::pg::test_provider

namespace bioseq::cache::pg::test_provider {

/*
 * Genuine ordering cycle: the relation hook calls the provider table, while
 * the table must be defined after every callback it stores.
 */
[[nodiscard]] const BioseqCacheProviderV2& provider_v2() noexcept;

/** Resolve the four required occurrence columns on the configured table. */
[[nodiscard]] bool relation_contract_attnos(
    Oid relation_oid,
    AttrNumber* row_identity_attno,
    AttrNumber* base_identity_attno,
    AttrNumber* occurrence_attno,
    AttrNumber* capability_attno) noexcept {
    if (!OidIsValid(relation_oid)) {
        return false;
    }
    const AttrNumber row = get_attnum(relation_oid, "row_id");
    const AttrNumber base = get_attnum(relation_oid, "base_id");
    const AttrNumber occurrence = get_attnum(relation_oid, "occurrence_no");
    const AttrNumber capability = get_attnum(relation_oid, "capability_mask");
    if (row <= 0 || base <= 0 || occurrence <= 0 || capability <= 0
        || get_atttype(relation_oid, row) != INT8OID
        || get_atttype(relation_oid, base) != INT8OID
        || get_atttype(relation_oid, occurrence) != INT4OID
        || get_atttype(relation_oid, capability) != INT8OID) {
        return false;
    }
    if (row_identity_attno != nullptr) {
        *row_identity_attno = row;
    }
    if (base_identity_attno != nullptr) {
        *base_identity_attno = base;
    }
    if (occurrence_attno != nullptr) {
        *occurrence_attno = occurrence;
    }
    if (capability_attno != nullptr) {
        *capability_attno = capability;
    }
    return true;
}

/** Build an unregistered child that add_path cannot evict from under us. */
[[nodiscard]] Path* make_provider_child(
        PlannerInfo* root, RelOptInfo* relation) noexcept {
    if (root == nullptr || relation == nullptr
        || !bms_is_empty(relation->lateral_relids)) {
        return nullptr;
    }
    return create_seqscan_path(root, relation, nullptr, 0);
}

/** Add every physical user attribute to the provider's raw PathTarget. */
[[nodiscard]] PathTarget* make_provider_raw_target(
    PlannerInfo* root,
    RelOptInfo* relation,
    Index range_table_index,
    Oid relation_oid,
    PathTarget* original_target) {
    Relation opened = table_open(relation_oid, NoLock);
    TupleDesc descriptor = RelationGetDescr(opened);
    PathTarget* target = copy_pathtarget(original_target);
    for (int index = 0; index < descriptor->natts; ++index) {
        Form_pg_attribute attribute = TupleDescAttr(descriptor, index);
        if (attribute->attisdropped) {
            continue;
        }
        auto* variable = makeVar(
            range_table_index,
            static_cast<AttrNumber>(index + 1),
            attribute->atttypid,
            attribute->atttypmod,
            attribute->attcollation,
            0);
        add_new_column_to_pathtarget(target, reinterpret_cast<Expr*>(variable));
    }
    table_close(opened, NoLock);
    set_pathtarget_cost_width(root, target);
    return target;
}

/** Resolve and call the published planner bridge for every provider path set. */
void call_planner_bridge(
    PlannerInfo* root,
    RelOptInfo* relation,
    Index range_table_index,
    RangeTblEntry* range_entry) noexcept {
    void** slot = find_rendezvous_variable(
        bioseq::cache::kCacheApiRendezvousName);
    if (slot == nullptr || *slot == nullptr) {
        return;
    }
    const auto* api = static_cast<const BioseqCacheApiV2*>(*slot);
    if (api->struct_size
            < offsetof(BioseqCacheApiV2, planner_bridge)
                + sizeof(api->planner_bridge)
        || api->abi_version != bioseq::cache::kProviderAbiVersion
        || api->postgres_major
            != static_cast<std::uint32_t>(PG_VERSION_NUM / 10000)
        || api->reserved != 0 || api->instance_cookie == 0) {
        return;
    }
    const BioseqCachePlannerBridgeV2* bridge = api->planner_bridge;
    if (bridge == nullptr) {
        return;
    }
    if (bridge->struct_size < sizeof(*bridge)
        || bridge->abi_version != bioseq::cache::kProviderAbiVersion
        || bridge->postgres_major
            != static_cast<std::uint32_t>(PG_VERSION_NUM / 10000)
        || bridge->after_provider_rel_paths == nullptr) {
        return;
    }
    ++g_counters.planner_bridge_calls;
    bridge->after_provider_rel_paths(
        root,
        relation,
        range_table_index,
        range_entry,
        &provider_v2());
}

/** Chained relation hook: add the provider path, then invoke the bridge. */
void provider_set_rel_pathlist_hook(
    PlannerInfo* root,
    RelOptInfo* relation,
    Index range_table_index,
    RangeTblEntry* range_entry) {
    if (g_previous_set_rel_pathlist_hook != nullptr) {
        g_previous_set_rel_pathlist_hook(
            root, relation, range_table_index, range_entry);
    }
    if (root == nullptr || relation == nullptr || range_entry == nullptr
        || range_entry->rtekind != RTE_RELATION
        || (range_entry->inh && has_subclass(range_entry->relid))
        || !configured_target_relation(range_entry->relid)
        || !g_provider_registered) {
        return;
    }
    AttrNumber row_attno = 0;
    AttrNumber base_attno = 0;
    AttrNumber occurrence_attno = 0;
    AttrNumber capability_attno = 0;
    std::uint32_t* required_boundary_attnos = nullptr;
    std::size_t required_boundary_count = 0;
    if (!relation_contract_attnos(
            range_entry->relid,
            &row_attno,
            &base_attno,
            &occurrence_attno,
            &capability_attno)
        || !collect_projected_boundary_attnos(
            relation,
            range_table_index,
            range_entry->relid,
            &required_boundary_attnos,
            &required_boundary_count)) {
        return;
    }
    Path* child = make_provider_child(root, relation);
    if (child == nullptr || child->pathtarget == nullptr) {
        return;
    }
    PathTarget* raw_target = make_provider_raw_target(
        root,
        relation,
        range_table_index,
        range_entry->relid,
        child->pathtarget);
    auto* projection = create_projection_path(
        root, relation, child, raw_target);
    if (projection == nullptr) {
        return;
    }
    auto* provider_path = makeNode(CustomPath);
    provider_path->path.pathtype = T_CustomScan;
    provider_path->path.parent = relation;
    provider_path->path.pathtarget = raw_target;
    provider_path->path.param_info = nullptr;
    provider_path->path.parallel_aware = false;
    provider_path->path.parallel_safe = false;
    provider_path->path.parallel_workers = 0;
    provider_path->path.rows = child->rows;
    provider_path->path.startup_cost = projection->path.startup_cost;
    const Cost run_cost = projection->path.total_cost
        - projection->path.startup_cost;
    provider_path->path.total_cost = projection->path.startup_cost
        + run_cost * 0.98;
    provider_path->path.pathkeys = projection->path.pathkeys;
    provider_path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    provider_path->custom_paths = list_make1(&projection->path);
    provider_path->custom_restrictinfo = NIL;
    provider_path->custom_private = make_plan_nodes(
        range_entry->relid,
        row_attno,
        base_attno,
        occurrence_attno,
        capability_attno,
        required_boundary_attnos,
        required_boundary_count);
    provider_path->methods = &provider_path_methods();
    add_path(relation, &provider_path->path);
    set_cheapest(relation);
    if (list_member_ptr(relation->pathlist, provider_path)) {
        call_planner_bridge(
            root, relation, range_table_index, range_entry);
    }
}

/** Register the provider and its backend-local relation hook idempotently. */
[[nodiscard]] bool initialize_provider() {
    if (!g_scan_methods_registered) {
        RegisterCustomScanMethods(&provider_scan_methods());
        g_scan_methods_registered = true;
    }
    if (!g_provider_registered) {
        void** slot = find_rendezvous_variable(
            bioseq::cache::kCacheApiRendezvousName);
        if (slot == nullptr || *slot == nullptr) {
            return false;
        }
        auto* api = static_cast<BioseqCacheApiV2*>(*slot);
        if (api->struct_size < sizeof(*api)
            || api->abi_version != bioseq::cache::kProviderAbiVersion
            || api->postgres_major
                != static_cast<std::uint32_t>(PG_VERSION_NUM / 10000)
            || api->reserved != 0 || api->instance_cookie == 0
            || api->register_provider == nullptr) {
            return false;
        }
        g_provider_registration = {
            .struct_size = sizeof(BioseqCacheProviderRegistrationV2),
        };
        if (api->register_provider(
                &provider_v2(), &g_provider_registration)
            != BIOSEQ_CACHE_STATUS_OK) {
            return false;
        }
        g_provider_registered = true;
    }
    if (!g_hook_installed) {
        g_previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
        set_rel_pathlist_hook = provider_set_rel_pathlist_hook;
        g_hook_installed = true;
    }
    return true;
}

/** Restore only the hook slot and provider registration owned by this module. */
[[nodiscard]] bool shutdown_provider() noexcept {
    if (g_provider_registered) {
        void** slot = find_rendezvous_variable(
            bioseq::cache::kCacheApiRendezvousName);
        if (slot == nullptr || *slot == nullptr) {
            return false;
        }
        auto* api = static_cast<BioseqCacheApiV2*>(*slot);
        if (api->struct_size < sizeof(*api)
            || api->unregister_provider == nullptr
            || !api->unregister_provider(&g_provider_registration)) {
            return false;
        }
    }
    if (g_hook_installed && set_rel_pathlist_hook
        == provider_set_rel_pathlist_hook) {
        set_rel_pathlist_hook = g_previous_set_rel_pathlist_hook;
    }
    g_hook_installed = false;
    g_previous_set_rel_pathlist_hook = nullptr;
    g_provider_registered = false;
    g_provider_registration = {
        .struct_size = sizeof(BioseqCacheProviderRegistrationV2),
    };
    g_persisted_mapping_fault_statement = nullptr;
    return true;
}

/** Parse the normative mapping-fault control vocabulary. */
[[nodiscard]] MappingFault parse_mapping_fault(const char* name) {
    struct NamedFault {
        const char* name;
        MappingFault value;
    };
    static constexpr std::array<NamedFault, 44> faults{{
        {"none", MappingFault::None},
        {"null_pointer_nonzero_count", MappingFault::NullPointerNonzeroCount},
        {"short_boundary_map", MappingFault::ShortBoundaryMap},
        {"long_boundary_map", MappingFault::LongBoundaryMap},
        {"short_lineage_map", MappingFault::ShortLineageMap},
        {"long_lineage_map", MappingFault::LongLineageMap},
        {"zero_consumer_attno", MappingFault::ZeroConsumerAttno},
        {"zero_source_attno", MappingFault::ZeroSourceAttno},
        {"source_attno_out_of_range", MappingFault::SourceAttnoOutOfRange},
        {"duplicate_consumer_attno", MappingFault::DuplicateConsumerAttno},
        {"missing_required_attno", MappingFault::MissingRequiredAttno},
        {"wrong_type", MappingFault::WrongType},
        {"wrong_typmod", MappingFault::WrongTypmod},
        {"wrong_collation", MappingFault::WrongCollation},
        {"wrong_semantic_id", MappingFault::WrongSemanticId},
        {"same_typed_column_permutation", MappingFault::SameTypedColumnPermutation},
        {"illegal_repeated_source_attno", MappingFault::IllegalRepeatedSourceAttno},
        {"bad_provider_private_version", MappingFault::BadProviderPrivateVersion},
        {"non_integer_persisted_map_node", MappingFault::NonIntegerPersistedMapNode},
        {"unknown_persisted_map_version", MappingFault::UnknownPersistedMapVersion},
        {"decorated_boundary_schema_mismatch", MappingFault::DecoratedBoundarySchemaMismatch},
        {"decorated_lineage_schema_mismatch", MappingFault::DecoratedLineageSchemaMismatch},
        {"provider_error", MappingFault::ProviderError},
        {"provider_unsupported", MappingFault::ProviderUnsupported},
        {"provider_status_error", MappingFault::ProviderStatusError},
        {"provider_query_canceled", MappingFault::ProviderQueryCanceled},
        {"late_fetch_error_once", MappingFault::LateFetchErrorOnce},
        {"late_fetch_partial_status_error_once", MappingFault::LateFetchPartialStatusErrorOnce},
        {"persisted_wrong_top_tag", MappingFault::PersistedWrongTopTag},
        {"persisted_short_top", MappingFault::PersistedShortTop},
        {"persisted_long_top", MappingFault::PersistedLongTop},
        {"persisted_bad_magic", MappingFault::PersistedBadMagic},
        {"persisted_bad_version", MappingFault::PersistedBadVersion},
        {"persisted_count_overflow", MappingFault::PersistedCountOverflow},
        {"persisted_wrong_entry_tag", MappingFault::PersistedWrongEntryTag},
        {"persisted_short_entry", MappingFault::PersistedShortEntry},
        {"persisted_long_entry", MappingFault::PersistedLongEntry},
        {"persisted_bad_semantic_size", MappingFault::PersistedBadSemanticSize},
        {"persisted_bad_fingerprint_size", MappingFault::PersistedBadFingerprintSize},
        {"persisted_bad_entry_metadata", MappingFault::PersistedBadEntryMetadata},
        {"persisted_source_fingerprint_mismatch", MappingFault::PersistedSourceFingerprintMismatch},
        {"persisted_consumer_fingerprint_mismatch", MappingFault::PersistedConsumerFingerprintMismatch},
        {"persisted_decorated_fingerprint_mismatch", MappingFault::PersistedDecoratedFingerprintMismatch},
        {"persisted_map_fingerprint_mismatch", MappingFault::PersistedMapFingerprintMismatch},
    }};
    for (const auto& fault : faults) {
        if (std::strcmp(name, fault.name) == 0) {
            return fault.value;
        }
    }
    ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("unknown generic test-provider mapping fault: %s", name)));
    pg_unreachable();
}

} // namespace bioseq::cache::pg::test_provider

namespace bioseq::cache::pg::test_provider {

export void provider_init() {
    bioseq::cache::pg::test_provider::require_test_superuser();
    if (!bioseq::cache::pg::test_provider::initialize_provider()) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq_cache test provider could not register")));
    }
}

export void provider_shutdown() {
    bioseq::cache::pg::test_provider::require_test_superuser();
    if (!bioseq::cache::pg::test_provider::shutdown_provider()) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
             errmsg("bioseq_cache test provider is still in use")));
    }
}

export void provider_set_exact_source_elision_safe(bool enabled) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    if (g_provider_registered || g_hook_installed) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq_cache test provider must be shut down before "
                    "changing exact source-elision capability")));
    }
    g_provider_exact_source_elision_safe = enabled;
    ResetPlanCache();
}

export void provider_use_relation(std::uint32_t relation_oid) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    AttrNumber row_attno = 0;
    AttrNumber base_attno = 0;
    AttrNumber occurrence_attno = 0;
    AttrNumber capability_attno = 0;
    Relation relation = table_open(relation_oid, AccessShareLock);
    const bool compatible = relation->rd_rel->relkind == RELKIND_RELATION
        && relation_contract_attnos(
            relation_oid,
            &row_attno,
            &base_attno,
            &occurrence_attno,
            &capability_attno);
    table_close(relation, AccessShareLock);
    if (!compatible) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("generic test-provider relation has the wrong contract"),
             errdetail("Required columns: row_id bigint, base_id bigint, occurrence_no integer, capability_mask bigint.")));
    }
    g_target_relation_oid = relation_oid;
    g_additional_target_relations.fill(InvalidOid);
    g_additional_target_relation_count = 0;
}

export void provider_record_write(std::uint32_t relation_oid) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    if (!record_provider_relation_write(relation_oid)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("generic test-provider write marker could not be recorded")));
    }
}

export void provider_add_relation(std::uint32_t relation_oid) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    AttrNumber row_attno = 0;
    AttrNumber base_attno = 0;
    AttrNumber occurrence_attno = 0;
    AttrNumber capability_attno = 0;
    Relation relation = table_open(relation_oid, AccessShareLock);
    const bool compatible =
        (relation->rd_rel->relkind == RELKIND_RELATION
            || relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
        && relation_contract_attnos(
            relation_oid,
            &row_attno,
            &base_attno,
            &occurrence_attno,
            &capability_attno);
    table_close(relation, AccessShareLock);
    if (!compatible) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("generic test-provider relation has the wrong contract"),
             errdetail("Required columns: row_id bigint, base_id bigint, occurrence_no integer, capability_mask bigint.")));
    }
    if (!add_configured_target_relation(relation_oid)) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("generic test-provider relation target limit exceeded")));
    }
}

export void provider_set_mapping_fault(const std::string& name) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    g_mapping_fault = parse_mapping_fault(name.c_str());
    g_persisted_mapping_fault_statement = nullptr;
}

export void provider_set_digest_fault(const std::string& name) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    if (name == "none") {
        g_digest_fault = DigestFault::None;
    } else if (name == "runtime_mismatch") {
        g_digest_fault = DigestFault::RuntimeMismatch;
    } else {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown generic test-provider digest fault: %s",
                 name.c_str())));
    }
}

export void provider_set_security_digest(
        const bioseq::pg_utils::sql_bytea& digest) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    if (digest.bytes().size() != g_provider_security_digest.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("generic test-provider security digest must be 32 bytes")));
    }
    std::memcpy(
        g_provider_security_digest.data(),
        digest.bytes().data(),
        g_provider_security_digest.size());
    ResetPlanCache();
}

export void provider_clear_security_digest() {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    g_provider_security_digest.fill(0);
    ResetPlanCache();
}

export void provider_set_projection_order(const std::string& name) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    if (name == "natural") {
        g_projection_order = ProjectionOrder::Natural;
    } else if (name == "reverse") {
        g_projection_order = ProjectionOrder::Reverse;
    } else {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown generic test-provider projection order: %s",
                 name.c_str())));
    }
}

export void provider_set_capabilities(std::int64_t capabilities) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    if (capabilities <= 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("generic test-provider capabilities must be positive")));
    }
    g_requested_capabilities = static_cast<std::uint64_t>(capabilities);
}

export void provider_force_domain_collision(bool enabled) {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    g_force_domain_digest_collision = enabled;
}

export void provider_reset() {
    using namespace bioseq::cache::pg::test_provider;
    require_test_superuser();
    g_requested_capabilities = 1;
    g_mapping_fault = MappingFault::None;
    g_persisted_mapping_fault_statement = nullptr;
    g_digest_fault = DigestFault::None;
    g_projection_order = ProjectionOrder::Natural;
    g_force_domain_digest_collision = false;
    g_provider_security_digest.fill(0);
    g_counters = {};
    ResetPlanCache();
}

export [[nodiscard]] bioseq::pg_utils::sql_jsonb provider_stats() {
    using namespace bioseq::cache::pg::test_provider;
    StringInfoData json{};
    initStringInfo(&json);
    appendStringInfo(
        &json,
        "{\"registered\":%s,\"target_relation\":%u,"
        "\"analyze_query\":%llu,\"describe_path\":%llu,"
        "\"domain_digest\":%llu,\"generation_digest\":%llu,"
        "\"runtime_digest\":%llu,\"map_source\":%llu,"
        "\"decorate_path\":%llu,\"build_recipe\":%llu,"
        "\"extract_occurrence\":%llu,\"late_fetch_batch\":%llu,"
        "\"late_fetch_references\":%llu,\"late_fetch_values\":%llu,"
        "\"late_fetch_bytes\":%llu,\"late_fetch_errors\":%llu,"
        "\"planner_bridge_calls\":%llu,\"executor_rows\":%llu,"
        "\"gated_expensive_rows\":%llu,"
        "\"last_boundary_column_count\":%llu,"
        "\"last_required_boundary_count\":%llu,"
        "\"last_lineage_column_count\":%llu,"
        "\"last_required_lineage_count\":%llu,"
        "\"last_recipe_required_boundary_count\":%llu}",
        g_provider_registered ? "true" : "false",
        g_target_relation_oid,
        static_cast<unsigned long long>(g_counters.analyze_query),
        static_cast<unsigned long long>(g_counters.describe_path),
        static_cast<unsigned long long>(g_counters.domain_digest),
        static_cast<unsigned long long>(g_counters.generation_digest),
        static_cast<unsigned long long>(g_counters.runtime_digest),
        static_cast<unsigned long long>(g_counters.map_source),
        static_cast<unsigned long long>(g_counters.decorate_path),
        static_cast<unsigned long long>(g_counters.build_recipe),
        static_cast<unsigned long long>(g_counters.extract_occurrence),
        static_cast<unsigned long long>(g_counters.late_fetch_batch),
        static_cast<unsigned long long>(g_counters.late_fetch_references),
        static_cast<unsigned long long>(g_counters.late_fetch_values),
        static_cast<unsigned long long>(g_counters.late_fetch_bytes),
        static_cast<unsigned long long>(g_counters.late_fetch_errors),
        static_cast<unsigned long long>(g_counters.planner_bridge_calls),
        static_cast<unsigned long long>(g_counters.executor_rows),
        static_cast<unsigned long long>(g_counters.gated_expensive_rows),
        static_cast<unsigned long long>(
            g_counters.last_boundary_column_count),
        static_cast<unsigned long long>(
            g_counters.last_required_boundary_count),
        static_cast<unsigned long long>(
            g_counters.last_lineage_column_count),
        static_cast<unsigned long long>(
            g_counters.last_required_lineage_count),
        static_cast<unsigned long long>(
            g_counters.last_recipe_required_boundary_count));
    bioseq::pg_utils::sql_jsonb result{std::string{json.data}};
    pfree(json.data);
    return result;
}

} // namespace bioseq::cache::pg::test_provider

namespace bioseq::cache::pg::test_provider {

/** Extract one stable provider-opaque binding occurrence from a raw slot. */
[[nodiscard]] BioseqCacheStatusV2 provider_extract_occurrence(
    List* plan_domain_nodes,
    TupleTableSlot* slot,
    BioseqCacheOccurrenceV2* output) {
    ++g_counters.extract_occurrence;
    Oid relation_oid = InvalidOid;
    AttrNumber row_attno = 0;
    AttrNumber base_attno = 0;
    AttrNumber occurrence_attno = 0;
    AttrNumber capability_attno = 0;
    if (slot == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || !parse_plan_nodes(
            plan_domain_nodes,
            &relation_oid,
            &row_attno,
            &base_attno,
            &occurrence_attno,
            &capability_attno,
            nullptr)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    bool row_is_null = false;
    bool base_is_null = false;
    bool occurrence_is_null = false;
    bool capability_is_null = false;
    const Datum row_value = slot_getattr(slot, row_attno, &row_is_null);
    const Datum base_value = slot_getattr(slot, base_attno, &base_is_null);
    const Datum occurrence_value = slot_getattr(
        slot, occurrence_attno, &occurrence_is_null);
    const Datum capability_value = slot_getattr(
        slot, capability_attno, &capability_is_null);
    if (row_is_null || base_is_null || occurrence_is_null
        || capability_is_null) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    output->struct_size = sizeof(*output);
    output->rte_ordinal = 1;
    output->logical_relation_oid = relation_oid;
    output->leaf_relation_oid = relation_oid;
    output->identity_kind = 3U;
    output->reserved = 0;
    output->row_identity = static_cast<std::uint64_t>(
        DatumGetInt64(row_value));
    output->occurrence_high = static_cast<std::uint64_t>(
        DatumGetInt64(base_value));
    output->occurrence_low = static_cast<std::uint64_t>(
        DatumGetInt32(occurrence_value));
    output->capability_mask = static_cast<std::uint64_t>(
        DatumGetInt64(capability_value));
    return BIOSEQ_CACHE_STATUS_OK;
}

/** Emit one terminal status for every reference on an early batch failure. */
void emit_batch_failure_statuses(
    BioseqCacheLateFetchSinkV2* sink,
    std::size_t occurrence_count,
    BioseqCacheStatusV2 status) noexcept {
    if (sink == nullptr || sink->struct_size < sizeof(*sink)
        || sink->emit_reference_status == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < occurrence_count; ++index) {
        sink->emit_reference_status(sink->opaque, index, status);
    }
}

/**
 * Fetch every provider-opaque row identity in one table scan under the
 * consumer snapshot, emitting values first and exactly one terminal status.
 */
[[nodiscard]] BioseqCacheStatusV2 provider_late_fetch_batch(
    List* plan_domain_nodes,
    EState* estate,
    const BioseqCacheOccurrenceV2* occurrences,
    std::size_t occurrence_count,
    const std::int16_t* attribute_numbers,
    std::size_t attribute_count,
    BioseqCacheLateFetchSinkV2* sink) {
    ++g_counters.late_fetch_batch;
    g_counters.late_fetch_references += occurrence_count;
    if (g_mapping_fault == MappingFault::LateFetchErrorOnce) {
        g_mapping_fault = MappingFault::None;
        ++g_counters.late_fetch_errors;
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected generic cache-provider late-fetch error")));
    }
    Oid relation_oid = InvalidOid;
    AttrNumber row_attno = 0;
    AttrNumber base_attno = 0;
    AttrNumber occurrence_attno = 0;
    AttrNumber capability_attno = 0;
    if (estate == nullptr || estate->es_snapshot == nullptr
        || occurrences == nullptr || occurrence_count == 0
        || occurrence_count > 4096
        || (attribute_count != 0 && attribute_numbers == nullptr)
        || sink == nullptr || sink->struct_size < sizeof(*sink)
        || sink->emit_value == nullptr
        || sink->emit_reference_status == nullptr
        || !parse_plan_nodes(
            plan_domain_nodes,
            &relation_oid,
            &row_attno,
            &base_attno,
            &occurrence_attno,
            &capability_attno,
            nullptr)) {
        emit_batch_failure_statuses(
            sink, occurrence_count, BIOSEQ_CACHE_STATUS_UNSUPPORTED);
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    for (std::size_t index = 0; index < occurrence_count; ++index) {
        if (occurrences[index].struct_size < sizeof(BioseqCacheOccurrenceV2)
            || occurrences[index].identity_kind != 3U
            || occurrences[index].logical_relation_oid != relation_oid
            || occurrences[index].leaf_relation_oid != relation_oid) {
            emit_batch_failure_statuses(
                sink, occurrence_count, BIOSEQ_CACHE_STATUS_UNSUPPORTED);
            return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
        }
    }

    Relation relation = table_open(relation_oid, NoLock);
    TupleDesc descriptor = RelationGetDescr(relation);
    for (std::size_t index = 0; index < attribute_count; ++index) {
        const AttrNumber attno = attribute_numbers[index];
        if (attno <= 0 || attno > descriptor->natts
            || TupleDescAttr(descriptor, attno - 1)->attisdropped) {
            table_close(relation, NoLock);
            emit_batch_failure_statuses(
                sink, occurrence_count, BIOSEQ_CACHE_STATUS_UNSUPPORTED);
            return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
        }
    }

    auto* found = static_cast<bool*>(
        palloc0(occurrence_count * sizeof(bool)));
    auto* terminal = static_cast<BioseqCacheStatusV2*>(palloc(
        occurrence_count * sizeof(BioseqCacheStatusV2)));
    bool injected_partial_status_error = false;
    for (std::size_t index = 0; index < occurrence_count; ++index) {
        terminal[index] = BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    TableScanDesc scan = table_beginscan(
        relation, estate->es_snapshot, 0, nullptr);
    TupleTableSlot* slot = table_slot_create(relation, nullptr);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        bool row_is_null = false;
        bool base_is_null = false;
        bool occurrence_is_null = false;
        bool capability_is_null = false;
        const Datum row_value = slot_getattr(slot, row_attno, &row_is_null);
        const Datum base_value = slot_getattr(slot, base_attno, &base_is_null);
        const Datum occurrence_value = slot_getattr(
            slot, occurrence_attno, &occurrence_is_null);
        const Datum capability_value = slot_getattr(
            slot, capability_attno, &capability_is_null);
        if (row_is_null || base_is_null || occurrence_is_null
            || capability_is_null) {
            ExecClearTuple(slot);
            continue;
        }
        const auto row_identity = static_cast<std::uint64_t>(
            DatumGetInt64(row_value));
        const auto base_identity = static_cast<std::uint64_t>(
            DatumGetInt64(base_value));
        const auto occurrence_identity = static_cast<std::uint64_t>(
            DatumGetInt32(occurrence_value));
        const auto capability_mask = static_cast<std::uint64_t>(
            DatumGetInt64(capability_value));
        for (std::size_t reference = 0;
             reference < occurrence_count;
             ++reference) {
            if (found[reference]
                || occurrences[reference].row_identity != row_identity) {
                continue;
            }
            found[reference] = true;
            if (occurrences[reference].occurrence_high != base_identity
                || occurrences[reference].occurrence_low
                    != occurrence_identity
                || occurrences[reference].capability_mask
                    != capability_mask) {
                terminal[reference] = BIOSEQ_CACHE_STATUS_UNSUPPORTED;
                continue;
            }
            terminal[reference] = BIOSEQ_CACHE_STATUS_OK;
            for (std::size_t requested = 0;
                 requested < attribute_count;
                 ++requested) {
                bool is_null = false;
                const AttrNumber requested_attno =
                    attribute_numbers[requested];
                const Datum value = slot_getattr(
                    slot, requested_attno, &is_null);
                std::uint64_t value_bytes = 0;
                if (!is_null) {
                    const Form_pg_attribute attribute =
                        TupleDescAttr(descriptor, requested_attno - 1);
                    const Size datum_bytes = datumGetSize(
                        value, attribute->attbyval, attribute->attlen);
                    if (datum_bytes
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - g_counters.late_fetch_bytes) {
                        terminal[reference] = BIOSEQ_CACHE_STATUS_ERROR;
                        break;
                    }
                    value_bytes = static_cast<std::uint64_t>(datum_bytes);
                }
                const BioseqCacheStatusV2 status = sink->emit_value(
                    sink->opaque,
                    reference,
                    requested,
                    static_cast<std::uintptr_t>(value),
                    is_null);
                if (status != BIOSEQ_CACHE_STATUS_OK) {
                    terminal[reference] = status;
                    break;
                }
                ++g_counters.late_fetch_values;
                g_counters.late_fetch_bytes += value_bytes;
                if (g_mapping_fault
                    == MappingFault::LateFetchPartialStatusErrorOnce) {
                    g_mapping_fault = MappingFault::None;
                    ++g_counters.late_fetch_errors;
                    injected_partial_status_error = true;
                    break;
                }
            }
            if (injected_partial_status_error) {
                break;
            }
        }
        if (injected_partial_status_error) {
            break;
        }
        ExecClearTuple(slot);
    }

    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    table_close(relation, NoLock);
    if (injected_partial_status_error) {
        pfree(terminal);
        pfree(found);
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    bool all_valid = true;
    for (std::size_t index = 0; index < occurrence_count; ++index) {
        sink->emit_reference_status(sink->opaque, index, terminal[index]);
        all_valid = all_valid && terminal[index] == BIOSEQ_CACHE_STATUS_OK;
    }
    pfree(terminal);
    pfree(found);
    return all_valid
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_UNSUPPORTED;
}

/** Provider-specific EXPLAIN fields contain no raw SQL or sensitive values. */
void provider_explain_domain(
    List* plan_domain_nodes,
    BioseqCacheExplainSinkV2* sink) {
    if (sink == nullptr || sink->struct_size < sizeof(*sink)) {
        return;
    }
    const bool valid = parse_plan_nodes(
        plan_domain_nodes,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (sink->property_text != nullptr) {
        sink->property_text(
            sink->opaque,
            "Generic Provider",
            "nullable occurrence table");
    }
    if (sink->property_bool != nullptr) {
        sink->property_bool(sink->opaque, "Provider Recipe Valid", valid);
    }
    if (sink->property_uint64 != nullptr) {
        sink->property_uint64(
            sink->opaque,
            "Provider Mapping Calls",
            g_counters.map_source);
        sink->property_uint64(
            sink->opaque,
            "Provider Late Fetch References",
            g_counters.late_fetch_references);
    }
}

} // namespace bioseq::cache::pg::test_provider

namespace bioseq::cache::pg::test_provider {

[[nodiscard]] bool persisted_mapping_fault_active() noexcept {
    return g_mapping_fault >= MappingFault::PersistedWrongTopTag
        && g_mapping_fault <= MappingFault::PersistedMapFingerprintMismatch;
}

[[nodiscard]] CustomScan* find_reusable_coverage_plan(
        Plan* plan,
        std::uint32_t depth = 0) noexcept {
    if (plan == nullptr || depth > 64) {
        return nullptr;
    }
    if (IsA(plan, CustomScan)) {
        auto* custom = reinterpret_cast<CustomScan*>(plan);
        if (custom->methods != nullptr
            && custom->methods->CustomName != nullptr
            && std::strcmp(
                custom->methods->CustomName,
                "BioseqReusableCoverageScan") == 0) {
            return custom;
        }
        ListCell* cell = nullptr;
        foreach(cell, custom->custom_plans) {
            if (CustomScan* found = find_reusable_coverage_plan(
                    static_cast<Plan*>(lfirst(cell)), depth + 1U)) {
                return found;
            }
        }
    }
    if (CustomScan* found = find_reusable_coverage_plan(
            outerPlan(plan), depth + 1U)) {
        return found;
    }
    return find_reusable_coverage_plan(innerPlan(plan), depth + 1U);
}

[[nodiscard]] Const* make_fault_bytea_node(std::size_t byte_count) {
    auto* value = static_cast<bytea*>(palloc0(VARHDRSZ + byte_count));
    SET_VARSIZE(value, VARHDRSZ + byte_count);
    return makeConst(
        BYTEAOID,
        -1,
        InvalidOid,
        -1,
        PointerGetDatum(value),
        false,
        false);
}

void replace_list_node(List* list, int index, void* node) noexcept {
    if (list == nullptr || index < 0 || index >= list_length(list)) {
        return;
    }
    ListCell* cell = list_nth_cell(list, index);
    lfirst(cell) = node;
}

[[nodiscard]] bool flip_mapping_fingerprint(
        List* mapping_plan,
        int fingerprint_ordinal) noexcept {
    if (mapping_plan == nullptr || list_length(mapping_plan) < 8
        || !IsA(list_nth(mapping_plan, 2), Integer)
        || !IsA(list_nth(mapping_plan, 3), Integer)) {
        return false;
    }
    const int boundary_count = intVal(list_nth(mapping_plan, 2));
    const int lineage_count = intVal(list_nth(mapping_plan, 3));
    const int index = 4 + boundary_count + lineage_count
        + fingerprint_ordinal;
    if (boundary_count < 0 || lineage_count < 0 || index < 0
        || index >= list_length(mapping_plan)
        || !IsA(list_nth(mapping_plan, index), Const)) {
        return false;
    }
    auto* constant = static_cast<Const*>(list_nth(mapping_plan, index));
    if (constant->constisnull || constant->consttype != BYTEAOID
        || constant->constbyval) {
        return false;
    }
    auto* value = reinterpret_cast<bytea*>(
        DatumGetPointer(constant->constvalue));
    if (value == nullptr || VARSIZE_ANY_EXHDR(value) != kDigestBytes) {
        return false;
    }
    reinterpret_cast<std::uint8_t*>(VARDATA_ANY(value))[0] ^= 0x80U;
    return true;
}

/** Corrupt one frozen cache-owned plan only after PREPARE has cached it. */
[[nodiscard]] bool mutate_persisted_mapping_plan(EState* estate) {
    if (!persisted_mapping_fault_active() || estate == nullptr
        || estate->es_plannedstmt == nullptr) {
        return false;
    }
    CustomScan* coverage = find_reusable_coverage_plan(
        estate->es_plannedstmt->planTree);
    if (coverage == nullptr || coverage->custom_private == NIL
        || list_length(coverage->custom_private) < 4) {
        return false;
    }
    void* candidates_node = list_nth(coverage->custom_private, 3);
    if (candidates_node == nullptr || !IsA(candidates_node, List)) {
        return false;
    }
    auto* candidates = static_cast<List*>(candidates_node);
    if (candidates == NIL || !IsA(linitial(candidates), List)) {
        return false;
    }
    auto* candidate = static_cast<List*>(linitial(candidates));
    if (list_length(candidate) != 7) {
        return false;
    }
    ListCell* plan_cell = list_nth_cell(candidate, 3);
    if (g_mapping_fault == MappingFault::PersistedWrongTopTag) {
        lfirst(plan_cell) = makeInteger(0);
        return true;
    }
    if (!IsA(lfirst(plan_cell), List)) {
        return false;
    }
    auto* mapping_plan = static_cast<List*>(lfirst(plan_cell));
    if (list_length(mapping_plan) < 8) {
        return false;
    }
    if (g_mapping_fault == MappingFault::PersistedShortTop) {
        lfirst(plan_cell) = list_truncate(
            mapping_plan, list_length(mapping_plan) - 1);
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedLongTop) {
        lfirst(plan_cell) = lappend(mapping_plan, makeInteger(0));
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedBadMagic) {
        replace_list_node(mapping_plan, 0, makeInteger(0));
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedBadVersion) {
        replace_list_node(mapping_plan, 1, makeInteger(999));
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedCountOverflow) {
        replace_list_node(mapping_plan, 2, makeInteger(1025));
        return true;
    }
    if (!IsA(list_nth(mapping_plan, 4), List)) {
        return false;
    }
    auto* entry = static_cast<List*>(list_nth(mapping_plan, 4));
    if (g_mapping_fault == MappingFault::PersistedWrongEntryTag) {
        replace_list_node(mapping_plan, 4, makeInteger(0));
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedShortEntry) {
        replace_list_node(mapping_plan, 4,
            list_truncate(entry, list_length(entry) - 1));
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedLongEntry) {
        replace_list_node(mapping_plan, 4, lappend(entry, makeInteger(0)));
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedBadSemanticSize) {
        replace_list_node(entry, 4, make_fault_bytea_node(15));
        return true;
    }
    if (g_mapping_fault == MappingFault::PersistedBadEntryMetadata) {
        replace_list_node(entry, 0, makeInteger(1));
        return true;
    }
    const int boundary_count = intVal(list_nth(mapping_plan, 2));
    const int lineage_count = intVal(list_nth(mapping_plan, 3));
    const int fingerprint_index = 4 + boundary_count + lineage_count;
    if (g_mapping_fault == MappingFault::PersistedBadFingerprintSize) {
        if (fingerprint_index < 4
            || fingerprint_index >= list_length(mapping_plan)) {
            return false;
        }
        replace_list_node(
            mapping_plan, fingerprint_index, make_fault_bytea_node(31));
        return true;
    }
    const int ordinal =
        g_mapping_fault == MappingFault::PersistedSourceFingerprintMismatch
            ? 0
            : g_mapping_fault
                    == MappingFault::PersistedConsumerFingerprintMismatch
                ? 1
                : g_mapping_fault
                        == MappingFault::PersistedDecoratedFingerprintMismatch
                    ? 2
                    : 3;
    return flip_mapping_fingerprint(mapping_plan, ordinal);
}

/** Runtime digest validates only the immutable Node recipe. */
[[nodiscard]] BioseqCacheStatusV2 provider_append_runtime_digest(
    const List* plan_domain_nodes,
    EState* estate,
    BioseqCacheDigestSinkV2* sink) {
    ++g_counters.runtime_digest;
    if (persisted_mapping_fault_active()) {
        if (estate == nullptr || estate->es_plannedstmt == nullptr) {
            return BIOSEQ_CACHE_STATUS_ERROR;
        }
        if (g_persisted_mapping_fault_statement
                != estate->es_plannedstmt) {
            if (!mutate_persisted_mapping_plan(estate)) {
                return BIOSEQ_CACHE_STATUS_ERROR;
            }
            g_persisted_mapping_fault_statement = estate->es_plannedstmt;
        }
    }
    if (!parse_plan_nodes(
            plan_domain_nodes,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    auto digest = digest_plan_nodes(plan_domain_nodes);
    if (g_digest_fault == DigestFault::RuntimeMismatch) {
        digest[0] ^= 0x5aU;
    }
    return append_digest_bytes(sink, digest.data(), digest.size())
        ? BIOSEQ_CACHE_STATUS_OK
        : BIOSEQ_CACHE_STATUS_ERROR;
}

/** Find a source column having exactly the same semantic contract. */
[[nodiscard]] const BioseqCacheColumnSchemaV2* find_compatible_source_column(
    const BioseqCacheColumnSchemaV2& consumer,
    const BioseqCacheColumnSchemaV2* source_columns,
    std::size_t source_count) noexcept {
    for (std::size_t index = 0; index < source_count; ++index) {
        const auto& source = source_columns[index];
        if (source.semantic_id_high == consumer.semantic_id_high
            && source.semantic_id_low == consumer.semantic_id_low
            && source.type_oid == consumer.type_oid
            && source.typmod == consumer.typmod
            && source.collation_oid == consumer.collation_oid) {
            return &source;
        }
    }
    return nullptr;
}

/** Construct one complete source-to-consumer map in requested consumer order. */
[[nodiscard]] bool fill_source_map(
    const BioseqCacheColumnSchemaV2* consumer_columns,
    std::size_t consumer_count,
    const std::uint32_t* required_attnos,
    std::size_t required_count,
    const BioseqCacheColumnSchemaV2* source_columns,
    std::size_t source_count,
    BioseqCacheColumnMapEntryV2* output) noexcept {
    if (required_count != 0
        && (required_attnos == nullptr || output == nullptr)) {
        return false;
    }
    for (std::size_t index = 0; index < required_count; ++index) {
        const auto* consumer = find_schema_column(
            consumer_columns, consumer_count, required_attnos[index]);
        if (consumer == nullptr) {
            return false;
        }
        const auto* source = find_compatible_source_column(
            *consumer, source_columns, source_count);
        if (source == nullptr) {
            return false;
        }
        output[index] = {
            .struct_size = sizeof(BioseqCacheColumnMapEntryV2),
            .consumer_attno = consumer->attno,
            .source_attno = source->attno,
            .flags = 0,
            .semantic_id_high = consumer->semantic_id_high,
            .semantic_id_low = consumer->semantic_id_low,
            .type_oid = consumer->type_oid,
            .typmod = consumer->typmod,
            .collation_oid = consumer->collation_oid,
            .reserved = 0,
        };
    }
    return true;
}

/** Apply one malformed-output mode after constructing a correct map. */
void apply_mapping_fault(
    BioseqCacheSourceMappingV2& mapping,
    std::size_t boundary_capacity,
    std::size_t lineage_capacity) {
    auto* boundary = const_cast<BioseqCacheColumnMapEntryV2*>(
        mapping.boundary_map);
    auto* lineage = const_cast<BioseqCacheColumnMapEntryV2*>(
        mapping.lineage_map);
    switch (g_mapping_fault) {
        case MappingFault::None:
        case MappingFault::DecoratedBoundarySchemaMismatch:
        case MappingFault::DecoratedLineageSchemaMismatch:
        case MappingFault::ProviderUnsupported:
        case MappingFault::ProviderStatusError:
        case MappingFault::ProviderQueryCanceled:
        case MappingFault::LateFetchErrorOnce:
        case MappingFault::LateFetchPartialStatusErrorOnce:
        case MappingFault::PersistedWrongTopTag:
        case MappingFault::PersistedShortTop:
        case MappingFault::PersistedLongTop:
        case MappingFault::PersistedBadMagic:
        case MappingFault::PersistedBadVersion:
        case MappingFault::PersistedCountOverflow:
        case MappingFault::PersistedWrongEntryTag:
        case MappingFault::PersistedShortEntry:
        case MappingFault::PersistedLongEntry:
        case MappingFault::PersistedBadSemanticSize:
        case MappingFault::PersistedBadFingerprintSize:
        case MappingFault::PersistedBadEntryMetadata:
        case MappingFault::PersistedSourceFingerprintMismatch:
        case MappingFault::PersistedConsumerFingerprintMismatch:
        case MappingFault::PersistedDecoratedFingerprintMismatch:
        case MappingFault::PersistedMapFingerprintMismatch:
            break;
        case MappingFault::NullPointerNonzeroCount:
            mapping.boundary_map = nullptr;
            break;
        case MappingFault::ShortBoundaryMap:
        case MappingFault::MissingRequiredAttno:
            if (mapping.boundary_map_count != 0) {
                --mapping.boundary_map_count;
            }
            break;
        case MappingFault::LongBoundaryMap:
            if (boundary != nullptr && boundary_capacity > mapping.boundary_map_count
                && mapping.boundary_map_count != 0) {
                boundary[mapping.boundary_map_count] = boundary[0];
                ++mapping.boundary_map_count;
            }
            break;
        case MappingFault::ShortLineageMap:
            if (mapping.lineage_map_count != 0) {
                --mapping.lineage_map_count;
            }
            break;
        case MappingFault::LongLineageMap:
            if (lineage != nullptr && lineage_capacity > mapping.lineage_map_count
                && mapping.lineage_map_count != 0) {
                lineage[mapping.lineage_map_count] = lineage[0];
                ++mapping.lineage_map_count;
            }
            break;
        case MappingFault::ZeroConsumerAttno:
            if (mapping.boundary_map_count != 0) {
                boundary[0].consumer_attno = 0;
            }
            break;
        case MappingFault::ZeroSourceAttno:
            if (mapping.boundary_map_count != 0) {
                boundary[0].source_attno = 0;
            }
            break;
        case MappingFault::SourceAttnoOutOfRange:
            if (mapping.boundary_map_count != 0) {
                boundary[0].source_attno = 0xffffffffU;
            }
            break;
        case MappingFault::DuplicateConsumerAttno:
            if (mapping.boundary_map_count > 1) {
                boundary[1].consumer_attno = boundary[0].consumer_attno;
            }
            break;
        case MappingFault::WrongType:
            if (mapping.boundary_map_count != 0) {
                boundary[0].type_oid = BOOLOID;
            }
            break;
        case MappingFault::WrongTypmod:
            if (mapping.boundary_map_count != 0) {
                ++boundary[0].typmod;
            }
            break;
        case MappingFault::WrongCollation:
            if (mapping.boundary_map_count != 0) {
                boundary[0].collation_oid = static_cast<Oid>(9999);
            }
            break;
        case MappingFault::WrongSemanticId:
            if (mapping.boundary_map_count != 0) {
                boundary[0].semantic_id_low ^= 1U;
            }
            break;
        case MappingFault::SameTypedColumnPermutation:
            if (mapping.boundary_map_count > 1) {
                boundary[0].source_attno = boundary[1].source_attno;
            }
            break;
        case MappingFault::IllegalRepeatedSourceAttno:
            if (mapping.boundary_map_count > 1) {
                boundary[1].source_attno = boundary[0].source_attno;
            }
            break;
        case MappingFault::BadProviderPrivateVersion:
            mapping.provider_private_nodes = list_make1(makeInteger(-1));
            break;
        case MappingFault::NonIntegerPersistedMapNode:
            mapping.provider_private_nodes = list_make1(makeString("bad"));
            break;
        case MappingFault::UnknownPersistedMapVersion:
            mapping.provider_private_nodes = list_make1(makeInteger(999));
            break;
        case MappingFault::ProviderError:
            break;
    }
}

/** Explicitly affirm every older source, including apparent identity maps. */
[[nodiscard]] BioseqCacheStatusV2 provider_map_reusable_source(
    PlannerInfo*,
    const BioseqCachePathDomainV2* consumer,
    const BioseqCacheReusableSourceV2* source,
    BioseqCacheSourceMappingV2* output) {
    ++g_counters.map_source;
    if (g_mapping_fault == MappingFault::ProviderError) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected generic cache-provider mapping error")));
    }
    if (g_mapping_fault == MappingFault::ProviderQueryCanceled) {
        ereport(ERROR,
            (errcode(ERRCODE_QUERY_CANCELED),
             errmsg("injected generic cache-provider query cancellation")));
    }
    if (g_mapping_fault == MappingFault::ProviderUnsupported) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    if (g_mapping_fault == MappingFault::ProviderStatusError) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    if (consumer == nullptr || source == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || consumer->provider_id != kProviderId
        || source->provider_id != kProviderId
        || source->compatibility_family_id != kCompatibilityFamily
        || (source->capability_mask & consumer->capability_mask)
            != consumer->capability_mask) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    const std::size_t boundary_capacity =
        consumer->required_boundary_attno_count + 1U;
    const std::size_t lineage_capacity =
        consumer->required_lineage_attno_count + 1U;
    auto* boundary = static_cast<BioseqCacheColumnMapEntryV2*>(palloc0(
        boundary_capacity * sizeof(BioseqCacheColumnMapEntryV2)));
    auto* lineage = static_cast<BioseqCacheColumnMapEntryV2*>(palloc0(
        lineage_capacity * sizeof(BioseqCacheColumnMapEntryV2)));
    if (!fill_source_map(
            consumer->boundary_columns,
            consumer->boundary_column_count,
            consumer->required_boundary_attnos,
            consumer->required_boundary_attno_count,
            source->boundary_columns,
            source->boundary_column_count,
            boundary)
        || !fill_source_map(
            consumer->lineage_columns,
            consumer->lineage_column_count,
            consumer->required_lineage_attnos,
            consumer->required_lineage_attno_count,
            source->lineage_columns,
            source->lineage_column_count,
            lineage)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }

    output->struct_size = sizeof(*output);
    output->flags = 0;
    output->boundary_map = boundary;
    output->boundary_map_count = consumer->required_boundary_attno_count;
    output->lineage_map = lineage;
    output->lineage_map_count = consumer->required_lineage_attno_count;
    output->provider_private_nodes = list_make1(makeInteger(kPlanVersion));
    apply_mapping_fault(*output, boundary_capacity, lineage_capacity);
    return BIOSEQ_CACHE_STATUS_OK;
}

/** Add cache-owned gate expressions to a cloned provider path only. */
[[nodiscard]] BioseqCacheStatusV2 provider_decorate_path(
    PlannerInfo* root,
    RelOptInfo* relation,
    Path* provider_path,
    const BioseqCachePathDomainV2* domain,
    List* gated_qual_exprs,
    BioseqCacheDecoratedPathV2* output) {
    ++g_counters.decorate_path;
    if (root == nullptr || relation == nullptr || provider_path == nullptr
        || domain == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || provider_path->parent != relation
        || provider_path->pathtype != T_CustomScan
        || reinterpret_cast<CustomPath*>(provider_path)->methods
            != &provider_path_methods()) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    auto* original = reinterpret_cast<CustomPath*>(provider_path);
    auto* decorated = makeNode(CustomPath);
    *decorated = *original;
    decorated->custom_paths = list_copy(original->custom_paths);
    decorated->custom_private = static_cast<List*>(
        copyObject(original->custom_private));
    decorated->custom_restrictinfo = list_copy(
        original->custom_restrictinfo);
    ListCell* cell = nullptr;
    foreach(cell, gated_qual_exprs) {
        const auto* expression = static_cast<const Expr*>(lfirst(cell));
        if (expression == nullptr) {
            return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
        }
        auto* restriction = make_restrictinfo(
            root,
            static_cast<Expr*>(copyObject(const_cast<Expr*>(expression))),
            true,
            false,
            false,
            false,
            0,
            nullptr,
            nullptr,
            nullptr);
        decorated->custom_restrictinfo = lappend(
            decorated->custom_restrictinfo, restriction);
    }

    output->struct_size = sizeof(*output);
    output->flags = 0;
    output->path = &decorated->path;
    output->actual_boundary_columns = domain->boundary_columns;
    output->actual_boundary_column_count = domain->boundary_column_count;
    output->actual_lineage_columns = domain->lineage_columns;
    output->actual_lineage_column_count = domain->lineage_column_count;
    output->provider_private_nodes = static_cast<List*>(
        copyObject(domain->provider_private_nodes));

    if (g_mapping_fault == MappingFault::DecoratedBoundarySchemaMismatch
        && domain->boundary_column_count != 0) {
        auto* columns = static_cast<BioseqCacheColumnSchemaV2*>(palloc(
            domain->boundary_column_count
            * sizeof(BioseqCacheColumnSchemaV2)));
        std::memcpy(
            columns,
            domain->boundary_columns,
            domain->boundary_column_count
                * sizeof(BioseqCacheColumnSchemaV2));
        columns[0].semantic_id_low ^= 1U;
        output->actual_boundary_columns = columns;
    }
    if (g_mapping_fault == MappingFault::DecoratedLineageSchemaMismatch
        && domain->lineage_column_count != 0) {
        auto* columns = static_cast<BioseqCacheColumnSchemaV2*>(palloc(
            domain->lineage_column_count
            * sizeof(BioseqCacheColumnSchemaV2)));
        std::memcpy(
            columns,
            domain->lineage_columns,
            domain->lineage_column_count
                * sizeof(BioseqCacheColumnSchemaV2));
        columns[0].semantic_id_low ^= 1U;
        output->actual_lineage_columns = columns;
    }
    return BIOSEQ_CACHE_STATUS_OK;
}

/** Freeze plan semantics and expected runtime digest atomically. */
[[nodiscard]] BioseqCacheStatusV2 provider_build_plan_digest_recipe(
    PlannerInfo*,
    const Plan* provider_plan,
    BioseqCachePlanDigestRecipeV2* output) {
    ++g_counters.build_recipe;
    std::size_t required_boundary_count = 0;
    if (provider_plan == nullptr || output == nullptr
        || output->struct_size < sizeof(*output)
        || !IsA(provider_plan, CustomScan)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    const auto* scan = reinterpret_cast<const CustomScan*>(provider_plan);
    if (scan->methods != &provider_scan_methods()
        || !parse_plan_nodes(
            scan->custom_private,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &required_boundary_count)) {
        return BIOSEQ_CACHE_STATUS_UNSUPPORTED;
    }
    g_counters.last_recipe_required_boundary_count =
        required_boundary_count;
    const auto digest = digest_plan_nodes(scan->custom_private);
    output->struct_size = sizeof(*output);
    output->flags = 0;
    output->digest_format_version = 1;
    output->reserved = 0;
    output->plan_domain_nodes = static_cast<List*>(
        copyObject(scan->custom_private));
    std::memcpy(
        output->expected_generation_digest, digest.data(), digest.size());
    return BIOSEQ_CACHE_STATUS_OK;
}

} // namespace bioseq::cache::pg::test_provider

namespace bioseq::cache::pg::test_provider {

[[nodiscard]] BioseqCacheProviderV2 make_provider_v2(
        std::uint32_t provider_flags) noexcept {
    return BioseqCacheProviderV2{
        .struct_size = sizeof(BioseqCacheProviderV2),
        .abi_version = bioseq::cache::kProviderAbiVersion,
        .postgres_major = static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
        .provider_id = kProviderId,
        .provider_flags = provider_flags,
        .reserved = 0,
        .provider_name = "generic_nullable_occurrence_test_provider",
        .provider_scan_methods = &provider_scan_methods(),
        .analyze_query = provider_analyze_query,
        .describe_path = provider_describe_path,
        .append_domain_semantic_digest = provider_append_domain_digest,
        .append_generation_semantic_digest = provider_append_generation_digest,
        .append_runtime_generation_semantic_digest =
            provider_append_runtime_digest,
        .map_reusable_source = provider_map_reusable_source,
        .decorate_path = provider_decorate_path,
        .build_plan_digest_recipe = provider_build_plan_digest_recipe,
        .extract_occurrence = provider_extract_occurrence,
        .late_fetch_batch = provider_late_fetch_batch,
        .explain_domain = provider_explain_domain,
    };
}

/** Complete ABI table registered only by an explicit test-only SQL call. */
[[nodiscard]] const BioseqCacheProviderV2& provider_v2() noexcept {
    constexpr std::uint32_t reusable_flags =
        BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE
        | BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION;
    static const BioseqCacheProviderV2 exact_elision_safe =
        make_provider_v2(
            reusable_flags
            | BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE);
    static const BioseqCacheProviderV2 retained_source_required =
        make_provider_v2(reusable_flags);
    return g_provider_exact_source_elision_safe
        ? exact_elision_safe
        : retained_source_required;
}

} // namespace bioseq::cache::pg::test_provider
