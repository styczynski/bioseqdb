/*-------------------------------------------------------------------------
 *
 * reusable_adapter.cppm
 *     PostgreSQL planner/executor adapters for provider-neutral reuse.
 *
 * Provider callbacks run in disposable child contexts.  PostgreSQL ERROR is
 * converted to an optional-candidate rejection, while cancellation and
 * shutdown retain their normal semantics.  No C++ destructor is relied upon
 * for a reference, gate, or MemoryContext correctness action.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

export module bioseq.cache.pg.reusable_adapter;

import bioseq.cache.core;
import bioseq.cache.pg.provider_registry;
import bioseq.cache.pg.reusable_planning;

export namespace bioseq::cache::pg::reuse {

enum class ProviderCallFailureV1 : std::uint16_t {
    None = 0,
    ProviderUnavailable,
    Unsupported,
    CallbackError,
    MalformedOutput,
    NodeBudgetExceeded,
    SchemaMismatch,
    RuntimeDigestMismatch,
};

struct ProviderCallDiagnosticV1 {
    ProviderCallFailureV1 failure = ProviderCallFailureV1::None;
    BioseqCacheStatusV1 provider_status = BIOSEQ_CACHE_STATUS_OK;
    int sqlstate = 0;
};

/** Accepted data lives in this child until its parent planner context dies. */
struct PlannerOwnedIntentV1 {
    MemoryContext owner_context = nullptr;
    BioseqCacheQueryIntentV1 value{};
};

struct PlannerOwnedDomainV1 {
    MemoryContext owner_context = nullptr;
    BioseqCachePathDomainV1 value{};
};

struct PlannerOwnedSourceMappingV1 {
    MemoryContext owner_context = nullptr;
    BioseqCacheSourceMappingV1 value{};
};

struct PlannerOwnedDecoratedPathV1 {
    MemoryContext owner_context = nullptr;
    BioseqCacheDecoratedPathV1 value{};
};

struct PlannerOwnedDigestRecipeV1 {
    MemoryContext owner_context = nullptr;
    BioseqCachePlanDigestRecipeV1 value{};
    std::uint64_t serialized_bytes = 0;
    std::uint32_t node_count = 0;
};

struct PlannerGateNodesV1 {
    int param_id = -1;
    Param* validity_param = nullptr;
    Expr* residual_gate = nullptr;
};

struct CoveragePathCostsV1 {
    Cost startup_cost = 0;
    Cost total_cost = 0;
};

enum class SemanticDigestKindV1 : std::uint8_t {
    Domain = 1,
    Generation = 2,
};

/** Register this before a provisional reference is activated. */
struct CandidateContextCleanupV1 {
    MemoryContextCallback callback{};
    PlannerCandidateCleanupRecordV1* record = nullptr;
};

[[nodiscard]] bool invoke_analyze_query_fail_open(
    const provider_registry::ProviderV1* provider,
    const Query* query,
    MemoryContext planner_context,
    PlannerOwnedIntentV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

[[nodiscard]] bool invoke_describe_path_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCacheQueryIntentV1* intent,
    MemoryContext planner_context,
    PlannerOwnedDomainV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

[[nodiscard]] bool invoke_map_source_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    const BioseqCachePathDomainV1* consumer_domain,
    const BioseqCacheReusableSourceV1* source,
    MemoryContext planner_context,
    PlannerOwnedSourceMappingV1* out,
    ProviderCallDiagnosticV1* diagnostic,
    PlannerCandidateCleanupRecordV1* candidate_reference = nullptr) noexcept;

[[nodiscard]] bool invoke_decorate_path_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCachePathDomainV1* domain,
    List* gated_qual_exprs,
    MemoryContext planner_context,
    PlannerOwnedDecoratedPathV1* out,
    ProviderCallDiagnosticV1* diagnostic,
    PlannerCandidateCleanupRecordV1* candidate_reference = nullptr) noexcept;

[[nodiscard]] bool invoke_build_recipe_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    const Plan* provider_plan,
    MemoryContext planner_context,
    PlannerOwnedDigestRecipeV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Capture provider-defined canonical semantic bytes under a PG error boundary. */
[[nodiscard]] bool invoke_semantic_digest_fail_open(
    const provider_registry::ProviderV1* provider,
    const BioseqCachePathDomainV1* domain,
    SemanticDigestKindV1 kind,
    std::span<std::byte> output,
    std::size_t* output_size,
    MemoryContext parent_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Validate runtime bytes before any coverage registry action or child init. */
[[nodiscard]] bool validate_runtime_recipe_fail_open(
    const provider_registry::ProviderV1* provider,
    const BioseqCachePlanDigestRecipeV1* recipe,
    EState* estate,
    MemoryContext executor_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Allocate one PostgreSQL-owned Boolean PARAM_EXEC and its 3VL fail-open OR. */
[[nodiscard]] bool build_planner_gate_nodes(
    PlannerInfo* root,
    Expr* residual_source_predicate,
    PlannerGateNodesV1* out);

[[nodiscard]] bool provider_plan_matches(
    const Plan* plan,
    const provider_registry::ProviderV1* provider) noexcept;

/** Build a serial, unordered wrapper around one already-decorated residual. */
[[nodiscard]] CustomPath* make_coverage_cache_path(
    RelOptInfo* rel,
    Path* original_provider_path,
    Path* decorated_residual_path,
    List* private_nodes,
    const CustomPathMethods* methods,
    CoveragePathCostsV1 costs);

/** Remove the exact original pointer before ordinary add_path dominance. */
[[nodiscard]] bool replace_provider_path(
    RelOptInfo* rel,
    Path* original_provider_path,
    CustomPath* wrapper);

[[nodiscard]] CandidateContextCleanupV1*
register_candidate_context_cleanup(
    MemoryContext planner_context,
    PlannerCandidateCleanupRecordV1* record);

/** Validate every target first, then publish the frozen gate vector. */
[[nodiscard]] bool publish_frozen_gates(
    EState* estate,
    std::span<const int> gate_param_ids,
    const ReuseGateFreezeV1& decisions) noexcept;

} // namespace bioseq::cache::pg::reuse

namespace bioseq::cache::pg::reuse::detail {

using BoundaryBodyV1 = bool (*)(void* opaque);

[[nodiscard]] bool interrupt_error(int sqlstate) noexcept {
    return sqlstate == ERRCODE_QUERY_CANCELED
        || sqlstate == ERRCODE_ADMIN_SHUTDOWN
        || sqlstate == ERRCODE_CRASH_SHUTDOWN
        || ProcDiePending;
}

void set_diagnostic(
    ProviderCallDiagnosticV1* diagnostic,
    ProviderCallFailureV1 failure,
    BioseqCacheStatusV1 provider_status = BIOSEQ_CACHE_STATUS_OK,
    int sqlstate = 0) noexcept {
    if (diagnostic != nullptr) {
        diagnostic->failure = failure;
        diagnostic->provider_status = provider_status;
        diagnostic->sqlstate = sqlstate;
    }
}

/**
 * The body executes with child as CurrentMemoryContext.  A true result retains
 * child under parent; false or an optimization ERROR deletes it completely.
 */
[[nodiscard]] bool run_provider_boundary(
    MemoryContext parent,
    const char* context_name,
    BoundaryBodyV1 body,
    void* opaque,
    MemoryContext* accepted_context,
    ProviderCallDiagnosticV1* diagnostic,
    PlannerCandidateCleanupRecordV1* candidate_reference = nullptr) noexcept {
    if (parent == nullptr || context_name == nullptr || body == nullptr
        || accepted_context == nullptr) {
        set_diagnostic(diagnostic, ProviderCallFailureV1::MalformedOutput);
        return false;
    }

    *accepted_context = nullptr;
    MemoryContext old_context = CurrentMemoryContext;
    volatile MemoryContext child_context = nullptr;
    volatile bool accepted = false;

    PG_TRY();
    {
        child_context = AllocSetContextCreate(
            parent,
            "bioseq cache provider callback",
            ALLOCSET_SMALL_SIZES);
        MemoryContextSetIdentifier(child_context, context_name);
        MemoryContextSwitchTo(child_context);
        accepted = body(opaque);
        MemoryContextSwitchTo(old_context);
        if (candidate_reference != nullptr) {
            release_candidate_reference_noerror(*candidate_reference);
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (candidate_reference != nullptr) {
            release_candidate_reference_noerror(*candidate_reference);
        }
        if (interrupt_error(sqlstate)) {
            if (child_context != nullptr) {
                MemoryContextDelete(child_context);
            }
            PG_RE_THROW();
        }
        FlushErrorState();
        if (child_context != nullptr) {
            MemoryContextDelete(child_context);
            child_context = nullptr;
        }
        set_diagnostic(
            diagnostic,
            ProviderCallFailureV1::CallbackError,
            BIOSEQ_CACHE_STATUS_ERROR,
            sqlstate);
        accepted = false;
    }
    PG_END_TRY();

    if (!accepted) {
        if (child_context != nullptr) {
            MemoryContextDelete(child_context);
        }
        return false;
    }
    *accepted_context = child_context;
    set_diagnostic(diagnostic, ProviderCallFailureV1::None);
    return true;
}

[[nodiscard]] bool provider_available(
        const provider_registry::ProviderV1* provider) noexcept {
    return provider != nullptr
        && provider_registry::validate_provider_v1(provider)
        && provider_registry::find_provider_v1(provider->provider_id)
            == provider;
}

[[nodiscard]] bool provider_private_version(
        const List* nodes,
        std::uint32_t* version) noexcept {
    if (version == nullptr) {
        return false;
    }
    *version = 0;
    if (nodes == nullptr) {
        return true;
    }
    if (list_length(nodes) <= 0 || !IsA(list_nth(nodes, 0), Integer)) {
        return false;
    }
    const int value = intVal(list_nth(nodes, 0));
    if (value <= 0) {
        return false;
    }
    *version = static_cast<std::uint32_t>(value);
    return true;
}

/** Mapping-private v1 is NIL or a bounded opaque list with a version head. */
[[nodiscard]] bool mapping_private_nodes_valid(
        const List* nodes,
        std::uint32_t expected_version) noexcept {
    if (nodes == nullptr) {
        return true;
    }
    const int count = list_length(nodes);
    if (count <= 0
        || static_cast<std::uint32_t>(count)
            > kMaxProviderBoundaryColumns) {
        return false;
    }
    std::uint32_t version = 0;
    return provider_private_version(nodes, &version)
        && expected_version != 0
        && version == expected_version;
}

[[nodiscard]] bool copy_node_bounded(
    const void* source,
    bool allow_null,
    void** copied,
    std::uint64_t* serialized_bytes = nullptr,
    std::uint32_t* node_count = nullptr) {
    if (copied == nullptr) {
        return false;
    }
    *copied = nullptr;
    if (source == nullptr) {
        return allow_null;
    }

    char* serialized = nodeToString(source);
    if (serialized == nullptr) {
        return false;
    }
    const std::size_t byte_count = strnlen(
        serialized,
        static_cast<std::size_t>(kMaxProviderRecipeBytes) + 1U);
    if (byte_count > kMaxProviderRecipeBytes) {
        pfree(serialized);
        return false;
    }
    if (serialized_bytes != nullptr) {
        *serialized_bytes = byte_count;
    }
    if (node_count != nullptr) {
        if (!IsA(source, List)) {
            pfree(serialized);
            return false;
        }
        const int count = list_length(static_cast<const List*>(source));
        if (count < 0
            || static_cast<std::uint32_t>(count)
                > kMaxProviderBoundaryColumns) {
            pfree(serialized);
            return false;
        }
        *node_count = static_cast<std::uint32_t>(count);
    }
    *copied = copyObject(const_cast<void*>(source));
    pfree(serialized);
    return *copied != nullptr;
}

template <class Value>
[[nodiscard]] const Value* copy_array(
    const Value* source,
    std::size_t count) {
    if (count == 0) {
        return nullptr;
    }
    if (source == nullptr
        || count > kMaxProviderBoundaryColumns
        || count > (std::numeric_limits<std::size_t>::max)()
            / sizeof(Value)) {
        return nullptr;
    }
    auto* destination = static_cast<Value*>(
        palloc(count * sizeof(Value)));
    std::memcpy(destination, source, count * sizeof(Value));
    return destination;
}

struct AnalyzeBodyV1 {
    const provider_registry::ProviderV1* provider;
    const Query* query;
    PlannerOwnedIntentV1* out;
    ProviderCallDiagnosticV1* diagnostic;
};

[[nodiscard]] bool analyze_body(void* opaque) {
    auto& body = *static_cast<AnalyzeBodyV1*>(opaque);
    BioseqCacheQueryIntentV1 raw{};
    raw.struct_size = sizeof(raw);
    const BioseqCacheStatusV1 status =
        body.provider->analyze_query(body.query, &raw);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic,
            status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? ProviderCallFailureV1::Unsupported
                : ProviderCallFailureV1::CallbackError,
            status);
        return false;
    }
    if (!validate_query_intent(body.provider->provider_id, &raw).has_value()) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::MalformedOutput, status);
        return false;
    }
    void* private_nodes = nullptr;
    if (!copy_node_bounded(
            raw.provider_private_nodes,
            true,
            &private_nodes)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::NodeBudgetExceeded);
        return false;
    }
    body.out->value = raw;
    body.out->value.provider_private_nodes =
        static_cast<List*>(private_nodes);
    return true;
}

struct DescribeBodyV1 {
    const provider_registry::ProviderV1* provider;
    PlannerInfo* root;
    RelOptInfo* rel;
    Path* provider_path;
    const BioseqCacheQueryIntentV1* intent;
    PlannerOwnedDomainV1* out;
    ProviderCallDiagnosticV1* diagnostic;
};

[[nodiscard]] bool describe_body(void* opaque) {
    auto& body = *static_cast<DescribeBodyV1*>(opaque);
    BioseqCachePathDomainV1 raw{};
    raw.struct_size = sizeof(raw);
    const BioseqCacheStatusV1 status = body.provider->describe_path(
        body.root, body.rel, body.provider_path, body.intent, &raw);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic,
            status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? ProviderCallFailureV1::Unsupported
                : ProviderCallFailureV1::CallbackError,
            status);
        return false;
    }
    if (!validate_path_domain(
            body.provider->provider_id, &raw).has_value()) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::MalformedOutput, status);
        return false;
    }
    std::uint32_t private_version = 0;
    if (!provider_private_version(
            raw.provider_private_nodes, &private_version)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::MalformedOutput, status);
        return false;
    }

    body.out->value = raw;
    body.out->value.boundary_columns = copy_array(
        raw.boundary_columns, raw.boundary_column_count);
    body.out->value.lineage_columns = copy_array(
        raw.lineage_columns, raw.lineage_column_count);
    body.out->value.required_boundary_attnos = copy_array(
        raw.required_boundary_attnos, raw.required_boundary_attno_count);
    body.out->value.required_lineage_attnos = copy_array(
        raw.required_lineage_attnos, raw.required_lineage_attno_count);
    if ((raw.boundary_column_count != 0
         && body.out->value.boundary_columns == nullptr)
        || (raw.lineage_column_count != 0
            && body.out->value.lineage_columns == nullptr)
        || (raw.required_boundary_attno_count != 0
            && body.out->value.required_boundary_attnos == nullptr)
        || (raw.required_lineage_attno_count != 0
            && body.out->value.required_lineage_attnos == nullptr)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::MalformedOutput);
        return false;
    }

    void* predicate = nullptr;
    void* dependencies = nullptr;
    void* private_nodes = nullptr;
    if (!copy_node_bounded(raw.coverage_predicate, false, &predicate)
        || !copy_node_bounded(
            raw.dependency_nodes, true, &dependencies)
        || !copy_node_bounded(
            raw.provider_private_nodes, true, &private_nodes)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::NodeBudgetExceeded);
        return false;
    }
    body.out->value.coverage_predicate = static_cast<Expr*>(predicate);
    body.out->value.dependency_nodes = static_cast<List*>(dependencies);
    body.out->value.provider_private_nodes =
        static_cast<List*>(private_nodes);
    return true;
}

[[nodiscard]] bool source_shape_valid(
    const BioseqCachePathDomainV1* consumer,
    const BioseqCacheReusableSourceV1* source) noexcept {
    if (consumer == nullptr || source == nullptr
        || source->struct_size < sizeof(BioseqCacheReusableSourceV1)
        || source->provider_id != consumer->provider_id
        || source->compatibility_family_id
            != consumer->compatibility_family_id
        || !capabilities_satisfy(
            source->capability_mask, consumer->capability_mask)) {
        return false;
    }
    return validate_provider_schema(ProviderSchemaView{
               .columns = source->boundary_columns,
               .column_count = source->boundary_column_count,
           }).has_value()
        && validate_provider_schema(ProviderSchemaView{
               .columns = source->lineage_columns,
               .column_count = source->lineage_column_count,
           }).has_value();
}

struct MapBodyV1 {
    const provider_registry::ProviderV1* provider;
    PlannerInfo* root;
    const BioseqCachePathDomainV1* consumer;
    const BioseqCacheReusableSourceV1* source;
    PlannerOwnedSourceMappingV1* out;
    ProviderCallDiagnosticV1* diagnostic;
};

[[nodiscard]] bool map_body(void* opaque) {
    auto& body = *static_cast<MapBodyV1*>(opaque);
    BioseqCacheSourceMappingV1 raw{};
    raw.struct_size = sizeof(raw);
    const BioseqCacheStatusV1 status = body.provider->map_reusable_source(
        body.root, body.consumer, body.source, &raw);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic,
            status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? ProviderCallFailureV1::Unsupported
                : ProviderCallFailureV1::CallbackError,
            status);
        return false;
    }
    const ProviderContractView consumer_contract{
        .boundary = boundary_schema_view(*body.consumer),
        .lineage = lineage_schema_view(*body.consumer),
        .capability_mask = body.consumer->capability_mask,
    };
    const ProviderContractView source_contract{
        .boundary = {
            .columns = body.source->boundary_columns,
            .column_count = body.source->boundary_column_count,
        },
        .lineage = {
            .columns = body.source->lineage_columns,
            .column_count = body.source->lineage_column_count,
        },
        .capability_mask = body.source->capability_mask,
    };
    std::uint32_t consumer_private_version = 0;
    std::uint32_t source_private_version = 0;
    if (!validate_source_map(
            consumer_contract, source_contract, raw).has_value()
        || !provider_private_version(
            body.consumer->provider_private_nodes,
            &consumer_private_version)
        || !provider_private_version(
            body.source->provider_private_nodes,
            &source_private_version)
        || consumer_private_version != source_private_version
        || !mapping_private_nodes_valid(
            raw.provider_private_nodes, consumer_private_version)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::SchemaMismatch, status);
        return false;
    }
    body.out->value = raw;
    body.out->value.boundary_map = copy_array(
        raw.boundary_map, raw.boundary_map_count);
    body.out->value.lineage_map = copy_array(
        raw.lineage_map, raw.lineage_map_count);
    if ((raw.boundary_map_count != 0
         && body.out->value.boundary_map == nullptr)
        || (raw.lineage_map_count != 0
            && body.out->value.lineage_map == nullptr)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::MalformedOutput);
        return false;
    }
    void* private_nodes = nullptr;
    if (!copy_node_bounded(
            raw.provider_private_nodes, true, &private_nodes)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::NodeBudgetExceeded);
        return false;
    }
    body.out->value.provider_private_nodes =
        static_cast<List*>(private_nodes);
    return true;
}

struct DecorateBodyV1 {
    const provider_registry::ProviderV1* provider;
    PlannerInfo* root;
    RelOptInfo* rel;
    Path* provider_path;
    const BioseqCachePathDomainV1* domain;
    List* gated_quals;
    PlannerOwnedDecoratedPathV1* out;
    ProviderCallDiagnosticV1* diagnostic;
};

[[nodiscard]] bool decorate_body(void* opaque) {
    auto& body = *static_cast<DecorateBodyV1*>(opaque);
    BioseqCacheDecoratedPathV1 raw{};
    raw.struct_size = sizeof(raw);
    const BioseqCacheStatusV1 status = body.provider->decorate_path(
        body.root,
        body.rel,
        body.provider_path,
        body.domain,
        body.gated_quals,
        &raw);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic,
            status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? ProviderCallFailureV1::Unsupported
                : ProviderCallFailureV1::CallbackError,
            status);
        return false;
    }
    if (!validate_decorated_path(body.domain, &raw).has_value()
        || raw.path->parent != body.rel
        || raw.path->param_info != nullptr) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::SchemaMismatch, status);
        return false;
    }
    std::uint32_t domain_private_version = 0;
    std::uint32_t decorated_private_version = 0;
    if (!provider_private_version(
            body.domain->provider_private_nodes,
            &domain_private_version)
        || !provider_private_version(
            raw.provider_private_nodes,
            &decorated_private_version)
        || domain_private_version != decorated_private_version) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::SchemaMismatch, status);
        return false;
    }
    body.out->value = raw;
    body.out->value.actual_boundary_columns = copy_array(
        raw.actual_boundary_columns, raw.actual_boundary_column_count);
    body.out->value.actual_lineage_columns = copy_array(
        raw.actual_lineage_columns, raw.actual_lineage_column_count);
    if ((raw.actual_boundary_column_count != 0
         && body.out->value.actual_boundary_columns == nullptr)
        || (raw.actual_lineage_column_count != 0
            && body.out->value.actual_lineage_columns == nullptr)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::MalformedOutput);
        return false;
    }
    void* private_nodes = nullptr;
    if (!copy_node_bounded(
            raw.provider_private_nodes, true, &private_nodes)) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::NodeBudgetExceeded);
        return false;
    }
    body.out->value.provider_private_nodes =
        static_cast<List*>(private_nodes);
    return true;
}

struct RecipeBodyV1 {
    const provider_registry::ProviderV1* provider;
    PlannerInfo* root;
    const Plan* plan;
    PlannerOwnedDigestRecipeV1* out;
    ProviderCallDiagnosticV1* diagnostic;
};

[[nodiscard]] bool recipe_body(void* opaque) {
    auto& body = *static_cast<RecipeBodyV1*>(opaque);
    BioseqCachePlanDigestRecipeV1 raw{};
    raw.struct_size = sizeof(raw);
    const BioseqCacheStatusV1 status =
        body.provider->build_plan_digest_recipe(
            body.root, body.plan, &raw);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic,
            status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? ProviderCallFailureV1::Unsupported
                : ProviderCallFailureV1::CallbackError,
            status);
        return false;
    }
    void* copied_nodes = nullptr;
    std::uint64_t serialized_bytes = 0;
    std::uint32_t node_count = 0;
    if (!copy_node_bounded(
            raw.plan_domain_nodes,
            false,
            &copied_nodes,
            &serialized_bytes,
            &node_count)
        || !validate_plan_digest_recipe(
            &raw, 1, serialized_bytes, node_count).has_value()) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::NodeBudgetExceeded);
        return false;
    }
    body.out->value = raw;
    body.out->value.plan_domain_nodes = static_cast<List*>(copied_nodes);
    body.out->serialized_bytes = serialized_bytes;
    body.out->node_count = node_count;
    return true;
}

struct RuntimeBodyV1 {
    const provider_registry::ProviderV1* provider;
    const BioseqCachePlanDigestRecipeV1* recipe;
    EState* estate;
    ProviderCallDiagnosticV1* diagnostic;
};

struct SemanticDigestBodyV1 {
    const provider_registry::ProviderV1* provider;
    const BioseqCachePathDomainV1* domain;
    SemanticDigestKindV1 kind;
    std::span<std::byte> output;
    std::size_t* output_size;
    ProviderCallDiagnosticV1* diagnostic;
};

[[nodiscard]] bool semantic_digest_body(void* opaque) {
    auto& body = *static_cast<SemanticDigestBodyV1*>(opaque);
    BoundedDigestSinkBufferV1 buffer{.bytes = body.output};
    BioseqCacheDigestSinkV1 sink = make_digest_sink(buffer);
    const BioseqCacheStatusV1 status =
        body.kind == SemanticDigestKindV1::Domain
        ? body.provider->append_domain_semantic_digest(
            body.domain, &sink)
        : body.provider->append_generation_semantic_digest(
            body.domain, &sink);
    if (status != BIOSEQ_CACHE_STATUS_OK || buffer.failed
        || buffer.size == 0) {
        set_diagnostic(
            body.diagnostic,
            status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? ProviderCallFailureV1::Unsupported
                : ProviderCallFailureV1::MalformedOutput,
            status);
        return false;
    }
    *body.output_size = buffer.size;
    return true;
}

[[nodiscard]] bool runtime_body(void* opaque) {
    auto& body = *static_cast<RuntimeBodyV1*>(opaque);
    void* copied_nodes = nullptr;
    std::uint64_t serialized_bytes = 0;
    std::uint32_t node_count = 0;
    if (!copy_node_bounded(
            body.recipe->plan_domain_nodes,
            false,
            &copied_nodes,
            &serialized_bytes,
            &node_count)
        || !validate_plan_digest_recipe(
            body.recipe, 1, serialized_bytes, node_count).has_value()) {
        set_diagnostic(
            body.diagnostic, ProviderCallFailureV1::NodeBudgetExceeded);
        return false;
    }

    std::array<std::byte, kProviderDigestBytes> actual{};
    BoundedDigestSinkBufferV1 buffer{.bytes = actual};
    BioseqCacheDigestSinkV1 sink = make_digest_sink(buffer);
    const BioseqCacheStatusV1 status =
        body.provider->append_runtime_generation_semantic_digest(
            static_cast<List*>(copied_nodes), body.estate, &sink);
    if (status != BIOSEQ_CACHE_STATUS_OK || buffer.failed
        || buffer.size != actual.size()) {
        set_diagnostic(
            body.diagnostic,
            status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                ? ProviderCallFailureV1::Unsupported
                : ProviderCallFailureV1::MalformedOutput,
            status);
        return false;
    }
    const auto expected = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(
            body.recipe->expected_generation_digest),
        kProviderDigestBytes};
    if (!digest_bytes_equal(actual, expected)) {
        set_diagnostic(
            body.diagnostic,
            ProviderCallFailureV1::RuntimeDigestMismatch,
            status);
        return false;
    }
    return true;
}

void candidate_context_cleanup(void* opaque) noexcept {
    auto* cleanup = static_cast<CandidateContextCleanupV1*>(opaque);
    if (cleanup != nullptr && cleanup->record != nullptr) {
        release_candidate_reference_noerror(*cleanup->record);
    }
}

} // namespace bioseq::cache::pg::reuse::detail

namespace bioseq::cache::pg::reuse {

bool invoke_analyze_query_fail_open(
    const provider_registry::ProviderV1* provider,
    const Query* query,
    MemoryContext planner_context,
    PlannerOwnedIntentV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (out == nullptr || query == nullptr || !detail::provider_available(provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    out->owner_context = nullptr;
    out->value = {};
    detail::AnalyzeBodyV1 body{provider, query, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            planner_context,
            "bioseq cache provider analyze",
            &detail::analyze_body,
            &body,
            &accepted,
            diagnostic)) {
        return false;
    }
    out->owner_context = accepted;
    return true;
}

bool invoke_describe_path_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCacheQueryIntentV1* intent,
    MemoryContext planner_context,
    PlannerOwnedDomainV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (out == nullptr || root == nullptr || rel == nullptr
        || provider_path == nullptr || intent == nullptr
        || !detail::provider_available(provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    out->owner_context = nullptr;
    out->value = {};
    detail::DescribeBodyV1 body{
        provider, root, rel, provider_path, intent, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            planner_context,
            "bioseq cache provider describe",
            &detail::describe_body,
            &body,
            &accepted,
            diagnostic)) {
        return false;
    }
    out->owner_context = accepted;
    return true;
}

bool invoke_map_source_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    const BioseqCachePathDomainV1* consumer_domain,
    const BioseqCacheReusableSourceV1* source,
    MemoryContext planner_context,
    PlannerOwnedSourceMappingV1* out,
    ProviderCallDiagnosticV1* diagnostic,
    PlannerCandidateCleanupRecordV1* candidate_reference) noexcept {
    if (out == nullptr || root == nullptr
        || !detail::provider_available(provider)
        || !detail::source_shape_valid(consumer_domain, source)) {
        if (candidate_reference != nullptr) {
            release_candidate_reference_noerror(*candidate_reference);
        }
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::MalformedOutput);
        return false;
    }
    out->owner_context = nullptr;
    out->value = {};
    detail::MapBodyV1 body{
        provider, root, consumer_domain, source, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            planner_context,
            "bioseq cache provider mapping",
            &detail::map_body,
            &body,
            &accepted,
            diagnostic,
            candidate_reference)) {
        return false;
    }
    out->owner_context = accepted;
    return true;
}

bool invoke_decorate_path_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCachePathDomainV1* domain,
    List* gated_qual_exprs,
    MemoryContext planner_context,
    PlannerOwnedDecoratedPathV1* out,
    ProviderCallDiagnosticV1* diagnostic,
    PlannerCandidateCleanupRecordV1* candidate_reference) noexcept {
    if (out == nullptr || root == nullptr || rel == nullptr
        || provider_path == nullptr || domain == nullptr
        || !detail::provider_available(provider)) {
        if (candidate_reference != nullptr) {
            release_candidate_reference_noerror(*candidate_reference);
        }
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    out->owner_context = nullptr;
    out->value = {};
    detail::DecorateBodyV1 body{
        provider,
        root,
        rel,
        provider_path,
        domain,
        gated_qual_exprs,
        out,
        diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            planner_context,
            "bioseq cache provider decoration",
            &detail::decorate_body,
            &body,
            &accepted,
            diagnostic,
            candidate_reference)) {
        return false;
    }
    out->owner_context = accepted;
    return true;
}

bool invoke_build_recipe_fail_open(
    const provider_registry::ProviderV1* provider,
    PlannerInfo* root,
    const Plan* provider_plan,
    MemoryContext planner_context,
    PlannerOwnedDigestRecipeV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (out == nullptr || root == nullptr || provider_plan == nullptr
        || !detail::provider_available(provider)
        || !provider_plan_matches(provider_plan, provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    out->owner_context = nullptr;
    out->value = {};
    out->serialized_bytes = 0;
    out->node_count = 0;
    detail::RecipeBodyV1 body{
        provider, root, provider_plan, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            planner_context,
            "bioseq cache provider recipe",
            &detail::recipe_body,
            &body,
            &accepted,
            diagnostic)) {
        return false;
    }
    out->owner_context = accepted;
    return true;
}

bool invoke_semantic_digest_fail_open(
    const provider_registry::ProviderV1* provider,
    const BioseqCachePathDomainV1* domain,
    SemanticDigestKindV1 kind,
    std::span<std::byte> output,
    std::size_t* output_size,
    MemoryContext parent_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (domain == nullptr || output.empty() || output_size == nullptr
        || (kind != SemanticDigestKindV1::Domain
            && kind != SemanticDigestKindV1::Generation)
        || !detail::provider_available(provider)
        || !validate_path_domain(provider->provider_id, domain).has_value()) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::MalformedOutput);
        return false;
    }
    *output_size = 0;
    detail::SemanticDigestBodyV1 body{
        provider, domain, kind, output, output_size, diagnostic};
    MemoryContext disposable = nullptr;
    const bool valid = detail::run_provider_boundary(
        parent_context,
        "bioseq cache provider semantic digest",
        &detail::semantic_digest_body,
        &body,
        &disposable,
        diagnostic);
    if (disposable != nullptr) {
        MemoryContextDelete(disposable);
    }
    return valid;
}

bool validate_runtime_recipe_fail_open(
    const provider_registry::ProviderV1* provider,
    const BioseqCachePlanDigestRecipeV1* recipe,
    EState* estate,
    MemoryContext executor_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (recipe == nullptr || estate == nullptr
        || !detail::provider_available(provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    detail::RuntimeBodyV1 body{provider, recipe, estate, diagnostic};
    MemoryContext disposable = nullptr;
    const bool valid = detail::run_provider_boundary(
        executor_context,
        "bioseq cache runtime recipe",
        &detail::runtime_body,
        &body,
        &disposable,
        diagnostic);
    if (disposable != nullptr) {
        MemoryContextDelete(disposable);
    }
    return valid;
}

bool build_planner_gate_nodes(
    PlannerInfo* root,
    Expr* residual_source_predicate,
    PlannerGateNodesV1* out) {
    if (root == nullptr || residual_source_predicate == nullptr
        || out == nullptr) {
        return false;
    }
    Param* valid = generate_new_exec_param(
        root, BOOLOID, -1, InvalidOid);
    if (valid == nullptr || valid->paramkind != PARAM_EXEC
        || valid->paramid < 0) {
        return false;
    }
    auto* invalid = makeNode(BooleanTest);
    invalid->arg = reinterpret_cast<Expr*>(copyObject(valid));
    invalid->booltesttype = IS_NOT_TRUE;
    invalid->location = -1;

    auto* uncovered = makeNode(BooleanTest);
    uncovered->arg = static_cast<Expr*>(
        copyObject(residual_source_predicate));
    uncovered->booltesttype = IS_NOT_TRUE;
    uncovered->location = -1;

    out->param_id = valid->paramid;
    out->validity_param = valid;
    out->residual_gate = make_orclause(
        list_make2(invalid, uncovered));
    return out->residual_gate != nullptr;
}

bool provider_plan_matches(
    const Plan* plan,
    const provider_registry::ProviderV1* provider) noexcept {
    if (plan == nullptr || provider == nullptr || !IsA(plan, CustomScan)) {
        return false;
    }
    const auto* scan = reinterpret_cast<const CustomScan*>(plan);
    return scan->methods == provider->provider_scan_methods;
}

CustomPath* make_coverage_cache_path(
    RelOptInfo* rel,
    Path* original_provider_path,
    Path* decorated_residual_path,
    List* private_nodes,
    const CustomPathMethods* methods,
    CoveragePathCostsV1 costs) {
    if (rel == nullptr || original_provider_path == nullptr
        || decorated_residual_path == nullptr || methods == nullptr
        || original_provider_path->parent != rel
        || decorated_residual_path->parent != rel
        || original_provider_path->param_info != nullptr
        || decorated_residual_path->param_info != nullptr
        || costs.startup_cost < 0
        || costs.total_cost < costs.startup_cost) {
        return nullptr;
    }
    auto* path = makeNode(CustomPath);
    path->path.pathtype = T_CustomScan;
    path->path.parent = rel;
    path->path.pathtarget = original_provider_path->pathtarget;
    path->path.param_info = nullptr;
    path->path.parallel_aware = false;
    path->path.parallel_safe = false;
    path->path.parallel_workers = 0;
    path->path.rows = original_provider_path->rows;
    path->path.startup_cost = costs.startup_cost;
    path->path.total_cost = costs.total_cost;
    path->path.pathkeys = NIL;
    path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    path->custom_paths = list_make1(decorated_residual_path);
    path->custom_restrictinfo = NIL;
    path->custom_private = private_nodes;
    path->methods = methods;
    return path;
}

bool replace_provider_path(
    RelOptInfo* rel,
    Path* original_provider_path,
    CustomPath* wrapper) {
    if (rel == nullptr || original_provider_path == nullptr
        || wrapper == nullptr || wrapper->path.parent != rel
        || !list_member_ptr(rel->pathlist, original_provider_path)
        || list_member_ptr(rel->partial_pathlist, original_provider_path)) {
        return false;
    }
    rel->pathlist = list_delete_ptr(
        rel->pathlist, original_provider_path);
    add_path(rel, &wrapper->path);
    const bool retained = list_member_ptr(rel->pathlist, &wrapper->path);
    set_cheapest(rel);
    return retained;
}

CandidateContextCleanupV1* register_candidate_context_cleanup(
    MemoryContext planner_context,
    PlannerCandidateCleanupRecordV1* record) {
    if (planner_context == nullptr || record == nullptr) {
        return nullptr;
    }
    auto* cleanup = static_cast<CandidateContextCleanupV1*>(
        MemoryContextAllocZero(
            planner_context, sizeof(CandidateContextCleanupV1)));
    cleanup->record = record;
    cleanup->callback.func = &detail::candidate_context_cleanup;
    cleanup->callback.arg = cleanup;
    MemoryContextRegisterResetCallback(
        planner_context, &cleanup->callback);
    return cleanup;
}

bool publish_frozen_gates(
    EState* estate,
    std::span<const int> gate_param_ids,
    const ReuseGateFreezeV1& decisions) noexcept {
    if (estate == nullptr || estate->es_plannedstmt == nullptr
        || estate->es_param_exec_vals == nullptr
        || decisions.state != ReusePreflightStateV1::Frozen
        || gate_param_ids.size() != decisions.source_count) {
        return false;
    }
    const int parameter_count =
        list_length(estate->es_plannedstmt->paramExecTypes);
    for (const int param_id : gate_param_ids) {
        if (param_id < 0 || param_id >= parameter_count
            || estate->es_param_exec_vals[param_id].execPlan != nullptr) {
            return false;
        }
    }
    for (std::size_t index = 0; index < gate_param_ids.size(); ++index) {
        ParamExecData& slot =
            estate->es_param_exec_vals[gate_param_ids[index]];
        slot.value = BoolGetDatum(source_gate_is_valid(decisions, index));
        slot.isnull = false;
    }
    return true;
}

} // namespace bioseq::cache::pg::reuse
