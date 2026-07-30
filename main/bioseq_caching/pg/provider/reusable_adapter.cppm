/*-------------------------------------------------------------------------
 *
 * provider/reusable_adapter.cppm
 *     PostgreSQL planner/executor adapters for provider-neutral reuse.
 *
 * Provider callbacks run in disposable child contexts. Explicit provider
 * status failures reject an optional candidate; every PostgreSQL ERROR is
 * rethrown with its original SQLSTATE after callback/reference cleanup. No
 * C++ destructor is relied upon for a reference, gate, or MemoryContext
 * correctness action.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/executor.h"
#include "commands/explain.h"
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
#include "utils/datum.h"
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
#include <type_traits>

export module bioseq.cache.pg.provider:reusable_adapter;

import bioseq.cache.core;
import :registry;
import bioseq.pg_support;

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
    BioseqCacheStatusV2 provider_status = BIOSEQ_CACHE_STATUS_OK;
    int sqlstate = 0;
};

/** Identity of the sole raw callback selected inside the cache trampoline. */
enum class provider_callback_identity : std::uint32_t {
    analyze_query = 0,
    describe_path = 1,
    append_domain_semantic_digest = 2,
    append_generation_semantic_digest = 3,
    append_runtime_generation_semantic_digest = 4,
    map_reusable_source = 5,
    decorate_path = 6,
    build_plan_digest_recipe = 7,
    extract_occurrence = 8,
    late_fetch_batch = 9,
    explain_domain = 10,
};

using provider_call_prepare = bool (*)(void* opaque);
using provider_call_accept = bool (*)(
    void* opaque, BioseqCacheStatusV2 status);

/** Trivial call description consumed by the single callback trampoline. */
struct provider_call_pod {
    const provider_registry::ProviderV2* provider = nullptr;
    const provider_registry::ProviderLeaseV2* lease = nullptr;
    MemoryContext parent_context = nullptr;
    const char* context_name = nullptr;
    void* opaque = nullptr;
    provider_call_prepare prepare = nullptr;
    provider_call_accept accept = nullptr;
    PlannerCandidateCleanupRecordV1* candidate_reference = nullptr;
};

/** Result of one contained provider callback and output transaction. */
struct provider_call_result {
    bool accepted = false;
    BioseqCacheStatusV2 provider_status = BIOSEQ_CACHE_STATUS_ERROR;
    int sqlstate = 0;
    MemoryContext output_context = nullptr;
};

/**
 * Invoke exactly one provider callback behind the PostgreSQL/C++ boundary.
 *
 * Successful output remains owned by output_context.  All other output is
 * discarded and caller-visible sinks remain unchanged.
 */
[[nodiscard]] provider_call_result invoke_provider_callback(
    provider_callback_identity callback,
    const provider_call_pod& input) noexcept;

/** Accepted data lives in this child until its parent planner context dies. */
struct PlannerOwnedIntentV1 {
    MemoryContext owner_context = nullptr;
    BioseqCacheQueryIntentV2 value{};
};

struct PlannerOwnedDomainV1 {
    MemoryContext owner_context = nullptr;
    BioseqCachePathDomainV2 value{};
};

struct PlannerOwnedSourceMappingV1 {
    MemoryContext owner_context = nullptr;
    BioseqCacheSourceMappingV2 value{};
};

struct PlannerOwnedDecoratedPathV1 {
    MemoryContext owner_context = nullptr;
    BioseqCacheDecoratedPathV2 value{};
};

struct PlannerOwnedDigestRecipeV1 {
    MemoryContext owner_context = nullptr;
    BioseqCachePlanDigestRecipeV2 value{};
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
    const provider_registry::ProviderV2* provider,
    const Query* query,
    MemoryContext planner_context,
    PlannerOwnedIntentV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

[[nodiscard]] bool invoke_describe_path_fail_open(
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCacheQueryIntentV2* intent,
    MemoryContext planner_context,
    PlannerOwnedDomainV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

[[nodiscard]] bool invoke_map_source_fail_open(
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    const BioseqCachePathDomainV2* consumer_domain,
    const BioseqCacheReusableSourceV2* source,
    MemoryContext planner_context,
    PlannerOwnedSourceMappingV1* out,
    ProviderCallDiagnosticV1* diagnostic,
    PlannerCandidateCleanupRecordV1* candidate_reference = nullptr) noexcept;

[[nodiscard]] bool invoke_decorate_path_fail_open(
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCachePathDomainV2* domain,
    List* gated_qual_exprs,
    MemoryContext planner_context,
    PlannerOwnedDecoratedPathV1* out,
    ProviderCallDiagnosticV1* diagnostic,
    PlannerCandidateCleanupRecordV1* candidate_reference = nullptr) noexcept;

[[nodiscard]] bool invoke_build_recipe_fail_open(
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    const Plan* provider_plan,
    MemoryContext planner_context,
    PlannerOwnedDigestRecipeV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Capture provider-defined canonical semantic bytes under a PG error boundary. */
[[nodiscard]] bool invoke_semantic_digest_fail_open(
    const provider_registry::ProviderV2* provider,
    const BioseqCachePathDomainV2* domain,
    SemanticDigestKindV1 kind,
    std::span<std::byte> output,
    std::size_t* output_size,
    MemoryContext parent_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Validate runtime bytes before any coverage registry action or child init. */
[[nodiscard]] bool validate_runtime_recipe_fail_open(
    const provider_registry::ProviderV2* provider,
    const provider_registry::ProviderLeaseV2* lease,
    const BioseqCachePlanDigestRecipeV2* recipe,
    EState* estate,
    MemoryContext executor_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Extract one occurrence through the execution lease and trampoline. */
[[nodiscard]] bool invoke_extract_occurrence_fail_open(
    const provider_registry::ProviderV2* provider,
    const provider_registry::ProviderLeaseV2* lease,
    List* plan_domain_nodes,
    TupleTableSlot* slot,
    BioseqCacheOccurrenceV2* occurrence,
    MemoryContext executor_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/**
 * Stage a complete late-fetch batch and publish it only after provider OK.
 */
[[nodiscard]] bool invoke_late_fetch_batch_fail_open(
    const provider_registry::ProviderV2* provider,
    const provider_registry::ProviderLeaseV2* lease,
    List* plan_domain_nodes,
    EState* estate,
    const BioseqCacheOccurrenceV2* occurrences,
    std::size_t occurrence_count,
    const std::int16_t* attribute_numbers,
    std::size_t attribute_count,
    LateFetchSinkStateV1* output,
    TupleDesc output_descriptor,
    MemoryContext output_context,
    Datum* output_values,
    bool* output_nulls,
    MemoryContext executor_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Stage provider EXPLAIN properties, then commit them behind the boundary. */
[[nodiscard]] bool invoke_explain_domain_fail_open(
    const provider_registry::ProviderV2* provider,
    const provider_registry::ProviderLeaseV2* lease,
    List* plan_domain_nodes,
    ExplainState* explain,
    MemoryContext executor_context,
    ProviderCallDiagnosticV1* diagnostic) noexcept;

/** Allocate one PostgreSQL-owned Boolean PARAM_EXEC and its 3VL fail-open OR. */
[[nodiscard]] bool build_planner_gate_nodes(
    PlannerInfo* root,
    Expr* residual_source_predicate,
    PlannerGateNodesV1* out);

[[nodiscard]] bool provider_plan_matches(
    const Plan* plan,
    const provider_registry::ProviderV2* provider) noexcept;

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

void set_diagnostic(
    ProviderCallDiagnosticV1* diagnostic,
    ProviderCallFailureV1 failure,
    BioseqCacheStatusV2 provider_status = BIOSEQ_CACHE_STATUS_OK,
    int sqlstate = 0) noexcept {
    if (diagnostic != nullptr) {
        diagnostic->failure = failure;
        diagnostic->provider_status = provider_status;
        diagnostic->sqlstate = sqlstate;
    }
}

/** Classify a provider's explicit non-OK status before inspecting its output. */
[[nodiscard]] constexpr ProviderCallFailureV1 provider_status_failure(
        BioseqCacheStatusV2 status) noexcept {
    return status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
        ? ProviderCallFailureV1::Unsupported
        : ProviderCallFailureV1::CallbackError;
}

/** Zero one imported C ABI record without Clang aggregate-assignment codegen. */
template<class Value>
void reset_abi_pod(Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memset(&value, 0, sizeof(value));
}

[[nodiscard]] bool provider_available(
        const provider_registry::ProviderV2* provider) noexcept {
    return provider != nullptr
        && provider_registry::validate_provider_v2(provider)
        && provider_registry::find_provider_v2(provider->provider_id)
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
    const provider_registry::ProviderV2* provider;
    const Query* query;
    PlannerOwnedIntentV1* out;
    ProviderCallDiagnosticV1* diagnostic;
    BioseqCacheQueryIntentV2 raw{};
};

[[nodiscard]] bool analyze_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<AnalyzeBodyV1*>(opaque);
    const BioseqCacheQueryIntentV2& raw = body.raw;
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
    const provider_registry::ProviderV2* provider;
    PlannerInfo* root;
    RelOptInfo* rel;
    Path* provider_path;
    const BioseqCacheQueryIntentV2* intent;
    PlannerOwnedDomainV1* out;
    ProviderCallDiagnosticV1* diagnostic;
    BioseqCachePathDomainV2 raw{};
};

[[nodiscard]] bool describe_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<DescribeBodyV1*>(opaque);
    const BioseqCachePathDomainV2& raw = body.raw;
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
    const BioseqCachePathDomainV2* consumer,
    const BioseqCacheReusableSourceV2* source) noexcept {
    if (consumer == nullptr || source == nullptr
        || source->struct_size < sizeof(BioseqCacheReusableSourceV2)
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
    const provider_registry::ProviderV2* provider;
    PlannerInfo* root;
    const BioseqCachePathDomainV2* consumer;
    const BioseqCacheReusableSourceV2* source;
    PlannerOwnedSourceMappingV1* out;
    ProviderCallDiagnosticV1* diagnostic;
    BioseqCacheSourceMappingV2 raw{};
};

[[nodiscard]] bool map_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<MapBodyV1*>(opaque);
    const BioseqCacheSourceMappingV2& raw = body.raw;
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
    const provider_registry::ProviderV2* provider;
    PlannerInfo* root;
    RelOptInfo* rel;
    Path* provider_path;
    const BioseqCachePathDomainV2* domain;
    List* gated_quals;
    PlannerOwnedDecoratedPathV1* out;
    ProviderCallDiagnosticV1* diagnostic;
    BioseqCacheDecoratedPathV2 raw{};
};

[[nodiscard]] bool decorate_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<DecorateBodyV1*>(opaque);
    const BioseqCacheDecoratedPathV2& raw = body.raw;
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
    const provider_registry::ProviderV2* provider;
    PlannerInfo* root;
    const Plan* plan;
    PlannerOwnedDigestRecipeV1* out;
    ProviderCallDiagnosticV1* diagnostic;
    BioseqCachePlanDigestRecipeV2 raw{};
};

[[nodiscard]] bool recipe_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<RecipeBodyV1*>(opaque);
    const BioseqCachePlanDigestRecipeV2& raw = body.raw;
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
    const provider_registry::ProviderV2* provider;
    const BioseqCachePlanDigestRecipeV2* recipe;
    EState* estate;
    ProviderCallDiagnosticV1* diagnostic;
    List* copied_nodes = nullptr;
    std::array<std::byte, kProviderDigestBytes> actual{};
    BoundedDigestSinkBufferV1 buffer{};
    BioseqCacheDigestSinkV2 sink{};
};

struct SemanticDigestBodyV1 {
    const provider_registry::ProviderV2* provider;
    const BioseqCachePathDomainV2* domain;
    SemanticDigestKindV1 kind;
    std::span<std::byte> output;
    std::size_t* output_size;
    ProviderCallDiagnosticV1* diagnostic;
    std::byte* staging_bytes = nullptr;
    BoundedDigestSinkBufferV1 buffer{};
    BioseqCacheDigestSinkV2 sink{};
};

[[nodiscard]] bool prepare_semantic_digest_body(void* opaque) {
    auto& body = *static_cast<SemanticDigestBodyV1*>(opaque);
    if (body.output.empty()) {
        return false;
    }
    body.staging_bytes = static_cast<std::byte*>(
        palloc(body.output.size()));
    body.buffer = BoundedDigestSinkBufferV1{
        .bytes = std::span<std::byte>{
            body.staging_bytes, body.output.size()},
    };
    body.sink = make_digest_sink(body.buffer);
    return true;
}

[[nodiscard]] bool semantic_digest_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<SemanticDigestBodyV1*>(opaque);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic, provider_status_failure(status), status);
        return false;
    }
    if (body.buffer.failed || body.buffer.size == 0) {
        set_diagnostic(
            body.diagnostic,
            ProviderCallFailureV1::MalformedOutput,
            status);
        return false;
    }
    std::memcpy(
        body.output.data(), body.staging_bytes, body.buffer.size);
    *body.output_size = body.buffer.size;
    return true;
}

[[nodiscard]] bool prepare_runtime_body(void* opaque) {
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
    body.copied_nodes = static_cast<List*>(copied_nodes);
    body.buffer = BoundedDigestSinkBufferV1{.bytes = body.actual};
    body.sink = make_digest_sink(body.buffer);
    return true;
}

[[nodiscard]] bool runtime_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<RuntimeBodyV1*>(opaque);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic, provider_status_failure(status), status);
        return false;
    }
    if (body.buffer.failed || body.buffer.size != body.actual.size()) {
        set_diagnostic(
            body.diagnostic,
            ProviderCallFailureV1::MalformedOutput,
            status);
        return false;
    }
    const auto expected = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(
            body.recipe->expected_generation_digest),
        kProviderDigestBytes};
    if (!digest_bytes_equal(body.actual, expected)) {
        set_diagnostic(
            body.diagnostic,
            ProviderCallFailureV1::RuntimeDigestMismatch,
            status);
        return false;
    }
    return true;
}

struct ExtractOccurrenceBodyV2 {
    List* plan_domain_nodes = NIL;
    TupleTableSlot* slot = nullptr;
    BioseqCacheOccurrenceV2* output = nullptr;
    ProviderCallDiagnosticV1* diagnostic = nullptr;
    BioseqCacheOccurrenceV2 raw{};
};

[[nodiscard]] bool extract_occurrence_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<ExtractOccurrenceBodyV2*>(opaque);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic, provider_status_failure(status), status);
        return false;
    }
    if (body.raw.struct_size
            < offsetof(BioseqCacheOccurrenceV2, capability_mask)
                + sizeof(body.raw.capability_mask)
        || body.raw.reserved != 0) {
        set_diagnostic(
            body.diagnostic,
            ProviderCallFailureV1::MalformedOutput,
            status);
        return false;
    }
    *body.output = body.raw;
    return true;
}

struct LateFetchBodyV2 {
    List* plan_domain_nodes = NIL;
    EState* estate = nullptr;
    const BioseqCacheOccurrenceV2* occurrences = nullptr;
    std::size_t occurrence_count = 0;
    const std::int16_t* attribute_numbers = nullptr;
    std::size_t attribute_count = 0;
    LateFetchSinkStateV1* output = nullptr;
    TupleDesc output_descriptor = nullptr;
    MemoryContext output_context = nullptr;
    Datum* output_values = nullptr;
    bool* output_nulls = nullptr;
    ProviderCallDiagnosticV1* diagnostic = nullptr;
    std::uint8_t* terminal_status = nullptr;
    std::uint8_t* value_seen = nullptr;
    std::uintptr_t* values = nullptr;
    std::uint8_t* nulls = nullptr;
    Datum* copied_values = nullptr;
    bool* copied_nulls = nullptr;
    LateFetchSinkStateV1 staging{};
    BioseqCacheLateFetchSinkV2 sink{};
};

/**
 * Consume provider Datums synchronously while slot-owned by-reference storage
 * is still valid. The copies remain private to the disposable callback
 * context until the provider reports OK and every terminal is validated.
 */
[[nodiscard]] BioseqCacheStatusV2 stage_late_fetch_value(
        void* opaque,
        std::size_t reference_index,
        std::size_t requested_attribute_index,
        std::uintptr_t datum,
        bool is_null) noexcept {
    auto* body = static_cast<LateFetchBodyV2*>(opaque);
    if (body == nullptr) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const BioseqCacheStatusV2 staged = late_fetch_emit_value(
        &body->staging,
        reference_index,
        requested_attribute_index,
        datum,
        is_null);
    if (staged != BIOSEQ_CACHE_STATUS_OK || is_null) {
        return staged;
    }
    if (requested_attribute_index >= body->attribute_count
        || body->attribute_numbers == nullptr
        || body->output_descriptor == nullptr) {
        body->staging.malformed = true;
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const AttrNumber attno =
        body->attribute_numbers[requested_attribute_index];
    if (attno <= 0 || attno > body->output_descriptor->natts) {
        body->staging.malformed = true;
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const Form_pg_attribute column =
        TupleDescAttr(body->output_descriptor, attno - 1);
    const Datum source = static_cast<Datum>(datum);
    if (column->attisdropped
        || (!column->attbyval && DatumGetPointer(source) == nullptr)) {
        body->staging.malformed = true;
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::size_t value_index =
        reference_index * body->attribute_count
        + requested_attribute_index;
    body->values[value_index] = static_cast<std::uintptr_t>(
        datumCopy(source, column->attbyval, column->attlen));
    return BIOSEQ_CACHE_STATUS_OK;
}

/** Record one terminal against the same private staging ledger. */
void stage_late_fetch_reference_status(
        void* opaque,
        std::size_t reference_index,
        BioseqCacheStatusV2 status) noexcept {
    auto* body = static_cast<LateFetchBodyV2*>(opaque);
    if (body != nullptr) {
        late_fetch_emit_reference_status(
            &body->staging, reference_index, status);
    }
}

[[nodiscard]] bool prepare_late_fetch_body(void* opaque) {
    auto& body = *static_cast<LateFetchBodyV2*>(opaque);
    if (body.occurrence_count == 0 || body.attribute_count == 0
        || body.output == nullptr
        || body.occurrence_count
            > (std::numeric_limits<std::size_t>::max)()
                / body.attribute_count) {
        return false;
    }
    const std::size_t value_count =
        body.occurrence_count * body.attribute_count;
    if (body.output->terminal_status.size() < body.occurrence_count
        || body.output->value_seen.size() < value_count
        || body.output->values.size() < value_count
        || body.output->nulls.size() < value_count) {
        return false;
    }
    for (std::size_t index = 0; index < body.attribute_count; ++index) {
        const AttrNumber attno = body.attribute_numbers[index];
        if (attno <= 0 || body.output_descriptor == nullptr
            || attno > body.output_descriptor->natts
            || TupleDescAttr(body.output_descriptor, attno - 1)
                ->attisdropped) {
            return false;
        }
    }
    body.terminal_status = static_cast<std::uint8_t*>(
        palloc(body.occurrence_count * sizeof(std::uint8_t)));
    body.value_seen = static_cast<std::uint8_t*>(
        palloc0(value_count * sizeof(std::uint8_t)));
    body.values = static_cast<std::uintptr_t*>(
        palloc0(value_count * sizeof(std::uintptr_t)));
    body.nulls = static_cast<std::uint8_t*>(
        palloc0(value_count * sizeof(std::uint8_t)));
    body.copied_values = static_cast<Datum*>(
        palloc0(value_count * sizeof(Datum)));
    body.copied_nulls = static_cast<bool*>(
        palloc0(value_count * sizeof(bool)));
    body.staging = LateFetchSinkStateV1{
        .reference_count = body.occurrence_count,
        .attribute_count = body.attribute_count,
        .terminal_status = std::span<std::uint8_t>{
            body.terminal_status, body.occurrence_count},
        .value_seen = std::span<std::uint8_t>{
            body.value_seen, value_count},
        .values = std::span<std::uintptr_t>{
            body.values, value_count},
        .nulls = std::span<std::uint8_t>{
            body.nulls, value_count},
    };
    if (!initialize_late_fetch_sink_state(body.staging)) {
        return false;
    }
    reset_abi_pod(body.sink);
    body.sink.struct_size = sizeof(BioseqCacheLateFetchSinkV2);
    body.sink.opaque = &body;
    body.sink.emit_value = &stage_late_fetch_value;
    body.sink.emit_reference_status =
        &stage_late_fetch_reference_status;
    return true;
}

[[nodiscard]] bool late_fetch_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<LateFetchBodyV2*>(opaque);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic, provider_status_failure(status), status);
        return false;
    }
    if (!late_fetch_complete_and_valid(body.staging)) {
        set_diagnostic(
            body.diagnostic,
            ProviderCallFailureV1::MalformedOutput,
            status);
        return false;
    }
    const std::size_t value_count =
        body.occurrence_count * body.attribute_count;
    MemoryContext previous = MemoryContextSwitchTo(
        body.output_context);
    for (std::size_t reference = 0;
         reference < body.occurrence_count;
         ++reference) {
        for (std::size_t attribute_index = 0;
             attribute_index < body.attribute_count;
             ++attribute_index) {
            const std::size_t value_index =
                reference * body.attribute_count + attribute_index;
            const std::int16_t attno =
                body.attribute_numbers[attribute_index];
            const bool is_null = body.nulls[value_index] != 0;
            body.copied_nulls[value_index] = is_null;
            if (is_null) {
                body.copied_values[value_index] =
                    static_cast<Datum>(0);
                body.values[value_index] = std::uintptr_t{0};
                continue;
            }
            const Form_pg_attribute column = TupleDescAttr(
                body.output_descriptor, attno - 1);
            const Datum staged_datum = static_cast<Datum>(
                body.values[value_index]);
            body.copied_values[value_index] = datumCopy(
                staged_datum, column->attbyval, column->attlen);
            body.values[value_index] = static_cast<std::uintptr_t>(
                body.copied_values[value_index]);
        }
    }
    MemoryContextSwitchTo(previous);
    std::memcpy(
        body.output->terminal_status.data(),
        body.terminal_status,
        body.occurrence_count * sizeof(std::uint8_t));
    std::memcpy(
        body.output->value_seen.data(),
        body.value_seen,
        value_count * sizeof(std::uint8_t));
    std::memcpy(
        body.output->values.data(),
        body.values,
        value_count * sizeof(std::uintptr_t));
    std::memcpy(
        body.output->nulls.data(),
        body.nulls,
        value_count * sizeof(std::uint8_t));
    std::memcpy(
        body.output_values,
        body.copied_values,
        value_count * sizeof(Datum));
    std::memcpy(
        body.output_nulls,
        body.copied_nulls,
        value_count * sizeof(bool));
    body.output->malformed = false;
    body.output->any_reference_failure = false;
    return true;
}

enum class ExplainPropertyKindV2 : std::uint8_t {
    Text = 0,
    UInt64 = 1,
    Bool = 2,
};

inline constexpr std::size_t kMaximumExplainPropertiesV2 = 32;
inline constexpr std::size_t kMaximumExplainPropertyNameBytesV2 = 96;
inline constexpr std::size_t kMaximumExplainPropertyTextBytesV2 = 256;

struct ExplainPropertyV2 {
    ExplainPropertyKindV2 kind = ExplainPropertyKindV2::Text;
    bool bool_value = false;
    std::uint64_t uint64_value = 0;
    char name[kMaximumExplainPropertyNameBytesV2]{};
    char text[kMaximumExplainPropertyTextBytesV2]{};
};

struct ExplainLedgerV2 {
    ExplainPropertyV2 properties[kMaximumExplainPropertiesV2]{};
    std::size_t count = 0;
    bool failed = false;
};

[[nodiscard]] bool copy_sink_string(
        char* destination,
        std::size_t capacity,
        const char* source) noexcept {
    if (destination == nullptr || capacity == 0 || source == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        destination[index] = source[index];
        if (source[index] == '\0') {
            return index != 0;
        }
    }
    destination[capacity - 1] = '\0';
    return false;
}

[[nodiscard]] ExplainPropertyV2* next_explain_property(
        ExplainLedgerV2* ledger,
        const char* name,
        ExplainPropertyKindV2 kind) noexcept {
    if (ledger == nullptr || ledger->failed
        || ledger->count >= kMaximumExplainPropertiesV2) {
        if (ledger != nullptr) {
            ledger->failed = true;
        }
        return nullptr;
    }
    ExplainPropertyV2& property = ledger->properties[ledger->count];
    property = {};
    property.kind = kind;
    if (!copy_sink_string(
            property.name, sizeof(property.name), name)) {
        ledger->failed = true;
        return nullptr;
    }
    ++ledger->count;
    return &property;
}

void explain_property_text_sink(
        void* opaque,
        const char* name,
        const char* value) noexcept {
    auto* ledger = static_cast<ExplainLedgerV2*>(opaque);
    ExplainPropertyV2* property = next_explain_property(
        ledger, name, ExplainPropertyKindV2::Text);
    if (property == nullptr
        || !copy_sink_string(
            property->text, sizeof(property->text), value)) {
        if (ledger != nullptr) {
            ledger->failed = true;
        }
    }
}

void explain_property_uint64_sink(
        void* opaque,
        const char* name,
        std::uint64_t value) noexcept {
    ExplainPropertyV2* property = next_explain_property(
        static_cast<ExplainLedgerV2*>(opaque),
        name,
        ExplainPropertyKindV2::UInt64);
    if (property != nullptr) {
        property->uint64_value = value;
    }
}

void explain_property_bool_sink(
        void* opaque,
        const char* name,
        bool value) noexcept {
    ExplainPropertyV2* property = next_explain_property(
        static_cast<ExplainLedgerV2*>(opaque),
        name,
        ExplainPropertyKindV2::Bool);
    if (property != nullptr) {
        property->bool_value = value;
    }
}

struct ExplainBodyV2 {
    List* plan_domain_nodes = NIL;
    ExplainState* explain = nullptr;
    ProviderCallDiagnosticV1* diagnostic = nullptr;
    ExplainLedgerV2 ledger{};
    BioseqCacheExplainSinkV2 sink{};
};

[[nodiscard]] bool prepare_explain_body(void* opaque) {
    auto& body = *static_cast<ExplainBodyV2*>(opaque);
    body.ledger = {};
    body.sink = BioseqCacheExplainSinkV2{
        .struct_size = sizeof(BioseqCacheExplainSinkV2),
        .opaque = &body.ledger,
        .property_text = &explain_property_text_sink,
        .property_uint64 = &explain_property_uint64_sink,
        .property_bool = &explain_property_bool_sink,
    };
    return true;
}

[[nodiscard]] bool explain_body(
        void* opaque, BioseqCacheStatusV2 status) {
    auto& body = *static_cast<ExplainBodyV2*>(opaque);
    if (status != BIOSEQ_CACHE_STATUS_OK) {
        set_diagnostic(
            body.diagnostic, provider_status_failure(status), status);
        return false;
    }
    if (body.ledger.failed) {
        set_diagnostic(
            body.diagnostic,
            ProviderCallFailureV1::MalformedOutput,
            status);
        return false;
    }
    for (std::size_t index = 0; index < body.ledger.count; ++index) {
        const ExplainPropertyV2& property =
            body.ledger.properties[index];
        switch (property.kind) {
            case ExplainPropertyKindV2::Text:
                ExplainPropertyText(
                    property.name, property.text, body.explain);
                break;
            case ExplainPropertyKindV2::UInt64:
                ExplainPropertyUInteger(
                    property.name,
                    nullptr,
                    property.uint64_value,
                    body.explain);
                break;
            case ExplainPropertyKindV2::Bool:
                ExplainPropertyBool(
                    property.name, property.bool_value, body.explain);
                break;
        }
    }
    return true;
}

} // namespace bioseq::cache::pg::reuse::detail

namespace bioseq::cache::pg::reuse {

provider_call_result invoke_provider_callback(
        provider_callback_identity callback,
        const provider_call_pod& input) noexcept {
    provider_call_result result{};
    if (input.provider == nullptr || input.parent_context == nullptr
        || input.context_name == nullptr || input.opaque == nullptr
        || input.accept == nullptr
        || !detail::provider_available(input.provider)) {
        return result;
    }

    provider_registry::ProviderCallbackTokenV2 callback_token{};
    if (!provider_registry::begin_provider_callback_v2(
            input.provider, input.lease, &callback_token)) {
        return result;
    }

    MemoryContext old_context = CurrentMemoryContext;
    volatile MemoryContext child_context = nullptr;
    volatile bool accepted = false;
    volatile BioseqCacheStatusV2 callback_status =
        BIOSEQ_CACHE_STATUS_ERROR;
    volatile int callback_sqlstate = 0;
    ErrorData* postgres_error_data = nullptr;
    bool postgres_error_capture_failed = false;

    try {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [callback,
             &input,
             &child_context,
             &accepted,
             &callback_status] {
                child_context = AllocSetContextCreateInternal(
                    input.parent_context,
                    "bioseq cache provider callback",
                    ALLOCSET_SMALL_MINSIZE,
                    ALLOCSET_SMALL_INITSIZE,
                    ALLOCSET_SMALL_MAXSIZE);
                MemoryContextSetIdentifier(
                    child_context, input.context_name);
                MemoryContextSwitchTo(child_context);
                try {
            const bool prepared =
                input.prepare == nullptr || input.prepare(input.opaque);
            if (prepared) {
                switch (callback) {
                    case provider_callback_identity::analyze_query: {
                        auto& body =
                            *static_cast<detail::AnalyzeBodyV1*>(
                                input.opaque);
                        body.raw = {};
                        body.raw.struct_size = sizeof(body.raw);
                        callback_status = input.provider->analyze_query(
                            body.query, &body.raw);
                        break;
                    }
                    case provider_callback_identity::describe_path: {
                        auto& body =
                            *static_cast<detail::DescribeBodyV1*>(
                                input.opaque);
                        body.raw = {};
                        body.raw.struct_size = sizeof(body.raw);
                        callback_status = input.provider->describe_path(
                            body.root,
                            body.rel,
                            body.provider_path,
                            body.intent,
                            &body.raw);
                        break;
                    }
                    case provider_callback_identity::
                            append_domain_semantic_digest: {
                        auto& body =
                            *static_cast<detail::SemanticDigestBodyV1*>(
                                input.opaque);
                        callback_status =
                            input.provider
                                ->append_domain_semantic_digest(
                                    body.domain, &body.sink);
                        break;
                    }
                    case provider_callback_identity::
                            append_generation_semantic_digest: {
                        auto& body =
                            *static_cast<detail::SemanticDigestBodyV1*>(
                                input.opaque);
                        callback_status =
                            input.provider
                                ->append_generation_semantic_digest(
                                    body.domain, &body.sink);
                        break;
                    }
                    case provider_callback_identity::
                            append_runtime_generation_semantic_digest: {
                        auto& body =
                            *static_cast<detail::RuntimeBodyV1*>(
                                input.opaque);
                        callback_status =
                            input.provider
                                ->append_runtime_generation_semantic_digest(
                                    body.copied_nodes,
                                    body.estate,
                                    &body.sink);
                        break;
                    }
                    case provider_callback_identity::map_reusable_source: {
                        auto& body =
                            *static_cast<detail::MapBodyV1*>(
                                input.opaque);
                        body.raw = {};
                        body.raw.struct_size = sizeof(body.raw);
                        callback_status =
                            input.provider->map_reusable_source(
                                body.root,
                                body.consumer,
                                body.source,
                                &body.raw);
                        break;
                    }
                    case provider_callback_identity::decorate_path: {
                        auto& body =
                            *static_cast<detail::DecorateBodyV1*>(
                                input.opaque);
                        body.raw = {};
                        body.raw.struct_size = sizeof(body.raw);
                        callback_status = input.provider->decorate_path(
                            body.root,
                            body.rel,
                            body.provider_path,
                            body.domain,
                            body.gated_quals,
                            &body.raw);
                        break;
                    }
                    case provider_callback_identity::
                            build_plan_digest_recipe: {
                        auto& body =
                            *static_cast<detail::RecipeBodyV1*>(
                                input.opaque);
                        body.raw = {};
                        body.raw.struct_size = sizeof(body.raw);
                        callback_status =
                            input.provider->build_plan_digest_recipe(
                                body.root, body.plan, &body.raw);
                        break;
                    }
                    case provider_callback_identity::extract_occurrence: {
                        auto& body =
                            *static_cast<detail::ExtractOccurrenceBodyV2*>(
                                input.opaque);
                        body.raw = {};
                        body.raw.struct_size = sizeof(body.raw);
                        callback_status =
                            input.provider->extract_occurrence == nullptr
                            ? BIOSEQ_CACHE_STATUS_UNSUPPORTED
                            : input.provider->extract_occurrence(
                                body.plan_domain_nodes,
                                body.slot,
                                &body.raw);
                        break;
                    }
                    case provider_callback_identity::late_fetch_batch: {
                        auto& body =
                            *static_cast<detail::LateFetchBodyV2*>(
                                input.opaque);
                        callback_status =
                            input.provider->late_fetch_batch == nullptr
                            ? BIOSEQ_CACHE_STATUS_UNSUPPORTED
                            : input.provider->late_fetch_batch(
                                body.plan_domain_nodes,
                                body.estate,
                                body.occurrences,
                                body.occurrence_count,
                                body.attribute_numbers,
                                body.attribute_count,
                                &body.sink);
                        break;
                    }
                    case provider_callback_identity::explain_domain: {
                        auto& body =
                            *static_cast<detail::ExplainBodyV2*>(
                                input.opaque);
                        if (input.provider->explain_domain == nullptr) {
                            callback_status =
                                BIOSEQ_CACHE_STATUS_UNSUPPORTED;
                        } else {
                            input.provider->explain_domain(
                                body.plan_domain_nodes, &body.sink);
                            callback_status = BIOSEQ_CACHE_STATUS_OK;
                        }
                        break;
                    }
                }
            }
            const auto status =
                static_cast<BioseqCacheStatusV2>(callback_status);
            if (status != BIOSEQ_CACHE_STATUS_OK
                && status != BIOSEQ_CACHE_STATUS_UNSUPPORTED
                && status != BIOSEQ_CACHE_STATUS_ERROR) {
                callback_status = BIOSEQ_CACHE_STATUS_ERROR;
            }
            accepted = prepared
                && input.accept(
                    input.opaque,
                    static_cast<BioseqCacheStatusV2>(callback_status));
        } catch (...) {
            callback_status = BIOSEQ_CACHE_STATUS_ERROR;
            accepted = false;
        }
            });
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        callback_sqlstate = error.sqlerrcode();
        postgres_error_data = error.release_error_data();
        callback_status = BIOSEQ_CACHE_STATUS_ERROR;
        accepted = false;
    } catch (const bioseq::pg_utils::PostgresErrorCaptureFailure&) {
        postgres_error_capture_failed = true;
        callback_status = BIOSEQ_CACHE_STATUS_ERROR;
        accepted = false;
    } catch (...) {
        callback_status = BIOSEQ_CACHE_STATUS_ERROR;
        accepted = false;
    }

    MemoryContextSwitchTo(old_context);
    provider_registry::end_provider_callback_v2(&callback_token);
    if (input.candidate_reference != nullptr) {
        release_candidate_reference_noerror(
            *input.candidate_reference);
    }
    if (postgres_error_data != nullptr) {
        if (child_context != nullptr) {
            MemoryContextDelete(child_context);
            child_context = nullptr;
        }
        bioseq::pg_utils::rethrow_postgres_error_after_cleanup(
            postgres_error_data);
    }
    if (postgres_error_capture_failed) {
        if (child_context != nullptr) {
            MemoryContextDelete(child_context);
            child_context = nullptr;
        }
        bioseq::pg_utils::emit_postgres_error_after_cleanup(
            ERRCODE_INTERNAL_ERROR,
            "bioseq cache could not preserve a provider PostgreSQL error");
    }

    result.accepted = accepted;
    result.provider_status =
        static_cast<BioseqCacheStatusV2>(callback_status);
    result.sqlstate = callback_sqlstate;
    if (accepted) {
        result.output_context = child_context;
    } else if (child_context != nullptr) {
        MemoryContextDelete(child_context);
    }
    return result;
}

} // namespace bioseq::cache::pg::reuse

namespace bioseq::cache::pg::reuse::detail {

[[nodiscard]] bool run_provider_boundary(
        provider_callback_identity callback,
        const provider_registry::ProviderV2* provider,
        const provider_registry::ProviderLeaseV2* lease,
        MemoryContext parent,
        const char* context_name,
        provider_call_prepare prepare,
        provider_call_accept accept,
        void* opaque,
        MemoryContext* accepted_context,
        ProviderCallDiagnosticV1* diagnostic,
        PlannerCandidateCleanupRecordV1* candidate_reference = nullptr)
        noexcept {
    if (accepted_context == nullptr) {
        set_diagnostic(
            diagnostic, ProviderCallFailureV1::MalformedOutput);
        return false;
    }
    *accepted_context = nullptr;
    const provider_call_result result = invoke_provider_callback(
        callback,
        provider_call_pod{
            .provider = provider,
            .lease = lease,
            .parent_context = parent,
            .context_name = context_name,
            .opaque = opaque,
            .prepare = prepare,
            .accept = accept,
            .candidate_reference = candidate_reference,
        });
    if (!result.accepted) {
        if (result.sqlstate != 0) {
            set_diagnostic(
                diagnostic,
                ProviderCallFailureV1::CallbackError,
                BIOSEQ_CACHE_STATUS_ERROR,
                result.sqlstate);
        } else if (diagnostic != nullptr
            && diagnostic->failure == ProviderCallFailureV1::None) {
            set_diagnostic(
                diagnostic,
                result.provider_status == BIOSEQ_CACHE_STATUS_UNSUPPORTED
                    ? ProviderCallFailureV1::Unsupported
                    : ProviderCallFailureV1::CallbackError,
                result.provider_status);
        }
        return false;
    }
    *accepted_context = result.output_context;
    set_diagnostic(diagnostic, ProviderCallFailureV1::None);
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
    const provider_registry::ProviderV2* provider,
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
    detail::reset_abi_pod(out->value);
    detail::AnalyzeBodyV1 body{provider, query, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            provider_callback_identity::analyze_query,
            provider,
            nullptr,
            planner_context,
            "bioseq cache provider analyze",
            nullptr,
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
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCacheQueryIntentV2* intent,
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
    detail::reset_abi_pod(out->value);
    detail::DescribeBodyV1 body{
        provider, root, rel, provider_path, intent, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            provider_callback_identity::describe_path,
            provider,
            nullptr,
            planner_context,
            "bioseq cache provider describe",
            nullptr,
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
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    const BioseqCachePathDomainV2* consumer_domain,
    const BioseqCacheReusableSourceV2* source,
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
    detail::reset_abi_pod(out->value);
    detail::MapBodyV1 body{
        provider, root, consumer_domain, source, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            provider_callback_identity::map_reusable_source,
            provider,
            nullptr,
            planner_context,
            "bioseq cache provider mapping",
            nullptr,
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
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    RelOptInfo* rel,
    Path* provider_path,
    const BioseqCachePathDomainV2* domain,
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
    detail::reset_abi_pod(out->value);
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
            provider_callback_identity::decorate_path,
            provider,
            nullptr,
            planner_context,
            "bioseq cache provider decoration",
            nullptr,
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
    const provider_registry::ProviderV2* provider,
    PlannerInfo* root,
    const Plan* provider_plan,
    MemoryContext planner_context,
    PlannerOwnedDigestRecipeV1* out,
    ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (out == nullptr || provider_plan == nullptr
        || !detail::provider_available(provider)
        || !provider_plan_matches(provider_plan, provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    out->owner_context = nullptr;
    detail::reset_abi_pod(out->value);
    out->serialized_bytes = 0;
    out->node_count = 0;
    detail::RecipeBodyV1 body{
        provider, root, provider_plan, out, diagnostic};
    MemoryContext accepted = nullptr;
    if (!detail::run_provider_boundary(
            provider_callback_identity::build_plan_digest_recipe,
            provider,
            nullptr,
            planner_context,
            "bioseq cache provider recipe",
            nullptr,
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
    const provider_registry::ProviderV2* provider,
    const BioseqCachePathDomainV2* domain,
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
    const provider_callback_identity callback =
        kind == SemanticDigestKindV1::Domain
        ? provider_callback_identity::append_domain_semantic_digest
        : provider_callback_identity::append_generation_semantic_digest;
    const bool valid = detail::run_provider_boundary(
        callback,
        provider,
        nullptr,
        parent_context,
        "bioseq cache provider semantic digest",
        &detail::prepare_semantic_digest_body,
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
    const provider_registry::ProviderV2* provider,
    const provider_registry::ProviderLeaseV2* lease,
    const BioseqCachePlanDigestRecipeV2* recipe,
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
        provider_callback_identity::
            append_runtime_generation_semantic_digest,
        provider,
        lease,
        executor_context,
        "bioseq cache runtime recipe",
        &detail::prepare_runtime_body,
        &detail::runtime_body,
        &body,
        &disposable,
        diagnostic);
    if (disposable != nullptr) {
        MemoryContextDelete(disposable);
    }
    return valid;
}

bool invoke_extract_occurrence_fail_open(
        const provider_registry::ProviderV2* provider,
        const provider_registry::ProviderLeaseV2* lease,
        List* plan_domain_nodes,
        TupleTableSlot* slot,
        BioseqCacheOccurrenceV2* occurrence,
        MemoryContext executor_context,
        ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (plan_domain_nodes == NIL || slot == nullptr || occurrence == nullptr
        || lease == nullptr || !detail::provider_available(provider)
        || !provider_registry::validate_provider_lease_v2(
            *lease, provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    *occurrence = {};
    detail::ExtractOccurrenceBodyV2 body{
        .plan_domain_nodes = plan_domain_nodes,
        .slot = slot,
        .output = occurrence,
        .diagnostic = diagnostic,
    };
    MemoryContext disposable = nullptr;
    const bool valid = detail::run_provider_boundary(
        provider_callback_identity::extract_occurrence,
        provider,
        lease,
        executor_context,
        "bioseq cache provider occurrence",
        nullptr,
        &detail::extract_occurrence_body,
        &body,
        &disposable,
        diagnostic);
    if (disposable != nullptr) {
        MemoryContextDelete(disposable);
    }
    return valid;
}

bool invoke_late_fetch_batch_fail_open(
        const provider_registry::ProviderV2* provider,
        const provider_registry::ProviderLeaseV2* lease,
        List* plan_domain_nodes,
        EState* estate,
        const BioseqCacheOccurrenceV2* occurrences,
        std::size_t occurrence_count,
        const std::int16_t* attribute_numbers,
        std::size_t attribute_count,
        LateFetchSinkStateV1* output,
        TupleDesc output_descriptor,
        MemoryContext output_context,
        Datum* output_values,
        bool* output_nulls,
        MemoryContext executor_context,
        ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (plan_domain_nodes == NIL || estate == nullptr
        || occurrences == nullptr || occurrence_count == 0
        || attribute_numbers == nullptr || attribute_count == 0
        || output == nullptr || output_descriptor == nullptr
        || output_context == nullptr || output_values == nullptr
        || output_nulls == nullptr || lease == nullptr
        || !detail::provider_available(provider)
        || !provider_registry::validate_provider_lease_v2(
            *lease, provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    detail::LateFetchBodyV2 body{
        .plan_domain_nodes = plan_domain_nodes,
        .estate = estate,
        .occurrences = occurrences,
        .occurrence_count = occurrence_count,
        .attribute_numbers = attribute_numbers,
        .attribute_count = attribute_count,
        .output = output,
        .output_descriptor = output_descriptor,
        .output_context = output_context,
        .output_values = output_values,
        .output_nulls = output_nulls,
        .diagnostic = diagnostic,
    };
    MemoryContext disposable = nullptr;
    const bool valid = detail::run_provider_boundary(
        provider_callback_identity::late_fetch_batch,
        provider,
        lease,
        executor_context,
        "bioseq cache provider late fetch",
        &detail::prepare_late_fetch_body,
        &detail::late_fetch_body,
        &body,
        &disposable,
        diagnostic);
    if (disposable != nullptr) {
        MemoryContextDelete(disposable);
    }
    return valid;
}

bool invoke_explain_domain_fail_open(
        const provider_registry::ProviderV2* provider,
        const provider_registry::ProviderLeaseV2* lease,
        List* plan_domain_nodes,
        ExplainState* explain,
        MemoryContext executor_context,
        ProviderCallDiagnosticV1* diagnostic) noexcept {
    if (plan_domain_nodes == NIL || explain == nullptr || lease == nullptr
        || !detail::provider_available(provider)
        || !provider_registry::validate_provider_lease_v2(
            *lease, provider)) {
        detail::set_diagnostic(
            diagnostic, ProviderCallFailureV1::ProviderUnavailable);
        return false;
    }
    detail::ExplainBodyV2 body{
        .plan_domain_nodes = plan_domain_nodes,
        .explain = explain,
        .diagnostic = diagnostic,
    };
    MemoryContext disposable = nullptr;
    const bool valid = detail::run_provider_boundary(
        provider_callback_identity::explain_domain,
        provider,
        lease,
        executor_context,
        "bioseq cache provider explain",
        &detail::prepare_explain_body,
        &detail::explain_body,
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
    const provider_registry::ProviderV2* provider) noexcept {
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
