/**
 * @file
 * @brief Collects match owners and finalizes stable helper bindings after setrefs.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_match_finalization;

import std;
import bioseq.postgres.table.io;
import :access_method;
import :access_common;
import :utils;
import bioseq.postgres.column.table_access;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.execution;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.external.path_builder_slot;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.table.types;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.stats;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.sequences.cigar;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.postgres.storage.scanner;
import bioseq.indexing.type;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.instrumentation.summary_writer;
import bioseq.common.seqan_adapter;
import bioseq.postgres.table.batch_sort_scan;

import :custom_scan_support;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;

using namespace bioseq::postgres::support;

export {

    /** @brief Captures one match-producing plan node and its planner-side binding metadata. */
    struct match_owner_finalization {
        CustomScan* scan = nullptr;
        List* descriptor = NIL;
        List* sites = NIL;
        Node* target_recipe = nullptr;
        List* base_lineage = NIL;
        Index base_relid = 0;
        Oid target_relation_oid = InvalidOid;
        int stable_match_id = 0;
        /* custom_private is deliberately opaque to core setrefs, so Vars stored in
         * recipes retain query-local varnos.  This finalization-only scope identity
         * prevents equal local spellings in unrelated subqueries/initplans from
         * becoming lineage candidates for one another.  It is never serialized: the
         * final executor ABI remains the stable Plan.plan_node_id. */
        std::uint64_t query_scope_id = 0u;
        /* Non-NIL only for a base BioseqScan owner.  Its pre-setrefs descriptor and
         * sites remain above for query-scope resolution; these tag cells receive a
         * distinct post-setrefs executor image after resolution succeeds. */
        List* base_owner_tag = NIL;
    };
    static_assert(std::is_trivially_destructible_v<match_owner_finalization>);
    static_assert(std::is_trivially_destructible_v<std::array<match_owner_finalization, 4>>);
    static_assert(std::is_trivially_destructible_v<std::span<match_owner_finalization>>);

#ifdef BIOSEQ_TEST_INDEXES
    /* Backend-local audit that a PostgreSQL ERROR cannot jump across the heap-
     * backed owner vector.  The test hook reads the balance after
     * a user-triggered ambiguous-owner error in the same backend. */
    /** @brief Counts heap-owner scopes entered by the test-only finalization audit. */
    std::uint64_t match_owner_finalization_scopes_entered = 0u;
    /** @brief Counts heap-owner scopes exited by the test-only finalization audit. */
    std::uint64_t match_owner_finalization_scopes_exited = 0u;

    /** @brief Records balanced entry and exit of a test-only finalization scope. */
    struct match_owner_finalization_scope_guard {
        /** @brief Records scope entry without allocating or invoking PostgreSQL. */
        match_owner_finalization_scope_guard() noexcept {
            ++match_owner_finalization_scopes_entered;
        }
        match_owner_finalization_scope_guard(const match_owner_finalization_scope_guard&) = delete;
        match_owner_finalization_scope_guard&
        operator=(const match_owner_finalization_scope_guard&) = delete;
        /** @brief Records scope exit without allocating or invoking PostgreSQL. */
        ~match_owner_finalization_scope_guard() noexcept {
            ++match_owner_finalization_scopes_exited;
        }
    };
#endif

    /** @brief Classifies the outcome of resolving one helper site to a match owner. */
    enum class match_site_resolution_status : std::uint8_t {
        bound,
        ambiguous,
        invalid,
    };

    /** @brief Reports the selected owner, binding form, and number of candidates. */
    struct match_site_resolution {
        match_site_resolution_status status = match_site_resolution_status::invalid;
        match_helper_binding_kind kind = match_helper_binding_kind::unresolved;
        match_owner_finalization* owner = nullptr;
        std::size_t candidate_count = 0u;
    };

    /**
     * @brief Reads and validates an integer cell from a serialized binding list.
     * @throws PostgreSQL ERROR when the requested cell is absent or has the wrong node type.
     */
    [[nodiscard]] Integer* match_binding_integer(List* values, const int index, const char* field) {
        Node* node = values == nullptr || index < 0 || index >= list_length(values)
                         ? nullptr
                         : reinterpret_cast<Node*>(list_nth(values, index));
        if (node == nullptr || !IsA(node, Integer)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq match binding integer ABI is invalid"),
                     errdetail("field=%s index=%d", field, index)));
        }
        return reinterpret_cast<Integer*>(node);
    }

    /** @brief Tests whether a canonical helper argument belongs to an owner's query-local lineage. */
    [[nodiscard]] bool match_owner_lineage_contains(const match_owner_finalization& site_owner,
                                                    const match_owner_finalization& owner,
                                                    Node* argument,
                                                    const bioseq_stub_oids& oids) noexcept {
        if (site_owner.query_scope_id == 0u || owner.query_scope_id != site_owner.query_scope_id)
            return false;
        Var* variable = try_cast_pg_node<Var>(argument);
        if (variable == nullptr || !bioseq_is_concrete_rte_varno(variable->varno) ||
            variable->varattno <= 0 || variable->varlevelsup != 0 ||
            !bms_is_empty(variable->varnullingrels))
            return false;
        if (variable->vartype == oids.bioseq_seq_type_oid) {
            for (Var* base : postgres_list<Var>(owner.base_lineage)) {
                if (base != nullptr && equal(base, variable))
                    return true;
            }
            return false;
        }
        /* Query-local Var identity is the alias boundary.  Relation OIDs are not:
         * inheritance, partitions, and composite recipes may legitimately expose
         * distinct physical relations while retaining one SQL-visible lineage,
         * whereas two aliases of one relation must never be conflated. */
        if (variable->varno == owner.base_relid)
            return true;
        for (Var* base : postgres_list<Var>(owner.base_lineage)) {
            if (base != nullptr && base->varno == variable->varno &&
                base->varlevelsup == variable->varlevelsup)
                return true;
        }
        return false;
    }

    /** @brief Resolves one canonical helper argument against owners in the registering query scope. */
    [[nodiscard]] match_site_resolution
    resolve_match_helper_site(match_owner_finalization& site_owner,
                              Node* canonical_argument,
                              std::span<match_owner_finalization> owners,
                              const bioseq_stub_oids& oids) noexcept {
        match_site_resolution result{};
        /* Resolve exact recipes across the complete query-local owner set before
         * honoring the registering site.  Two equal recipes owned by distinct plan
         * nodes are not distinguishable from SQL and must fail closed instead of
         * letting traversal/registration order choose one. */
        for (match_owner_finalization& candidate : owners) {
            if (candidate.query_scope_id != site_owner.query_scope_id)
                continue;
            if (canonical_argument != nullptr && candidate.target_recipe != nullptr &&
                equal(canonical_argument, candidate.target_recipe)) {
                ++result.candidate_count;
                result.owner = &candidate;
            }
        }
        if (result.candidate_count != 0u) {
            if (result.candidate_count > 1u) {
                result.status = match_site_resolution_status::ambiguous;
                return result;
            }
            if (result.owner != &site_owner) {
                result.status = match_site_resolution_status::invalid;
                return result;
            }
            result.status = match_site_resolution_status::bound;
            result.kind = match_helper_binding_kind::exact_recipe;
            return result;
        }

        result.owner = nullptr;
        result.candidate_count = 0u;
        for (match_owner_finalization& candidate : owners) {
            if (!match_owner_lineage_contains(site_owner, candidate, canonical_argument, oids))
                continue;
            ++result.candidate_count;
            result.owner = &candidate;
        }
        if (result.candidate_count > 1u) {
            result.status = match_site_resolution_status::ambiguous;
            return result;
        }
        if (result.candidate_count != 1u || result.owner != &site_owner) {
            result.status = match_site_resolution_status::invalid;
            return result;
        }
        result.status = match_site_resolution_status::bound;
        result.kind = match_helper_binding_kind::unique_lineage;
        return result;
    }

/* BIOSEQ_TEST_INDEXES has no SQL-visible helper for this invariant on purpose:
 * exposing one would create another executable match-helper surface.  The
 * first real owner finalization in a test backend runs this resolver seam.  It
 * validates both halves of the scope contract with the exact pre-setrefs collision
 * that nested PlannerInfos can produce: an equal local Var in another scope is
 * ignored, while two owners in one scope remain ambiguous for a lineage key
 * Var even when their physical relation OIDs differ.  A second alias of the
 * same physical relation remains distinct by Var identity.  Duplicate exact
 * recipes fail closed rather than selecting by traversal order. */
#ifdef BIOSEQ_TEST_INDEXES
    /** @brief Runs the backend-local query-scope and ambiguity resolver self-test once. */
    void verify_match_owner_scope_resolution(const bioseq_stub_oids& oids) {
        static bool verified = false;
        if (verified)
            return;

        Var* source = makeVar(
            /*varno=*/1,
            /*varattno=*/1,
            oids.bioseq_seq_type_oid,
            /*vartypmod=*/-1,
            /*varcollid=*/InvalidOid,
            /*varlevelsup=*/0);
        Var* lineage_key = makeVar(
            /*varno=*/1,
            /*varattno=*/2,
            INT4OID,
            /*vartypmod=*/-1,
            /*varcollid=*/InvalidOid,
            /*varlevelsup=*/0);
        constexpr Oid fixture_relation_oid = 4242;

        std::array<match_owner_finalization, 4> fixture{};
        match_owner_finalization first{};
        first.target_recipe = reinterpret_cast<Node*>(copyObject(source));
        first.base_lineage = list_make1(copyObject(source));
        first.base_relid = 1;
        first.target_relation_oid = fixture_relation_oid;
        first.stable_match_id = 101;
        first.query_scope_id = 11u;
        fixture[0] = first;

        match_owner_finalization unrelated_scope{};
        unrelated_scope.target_recipe = reinterpret_cast<Node*>(copyObject(source));
        unrelated_scope.base_lineage = list_make1(copyObject(source));
        unrelated_scope.base_relid = 1;
        unrelated_scope.target_relation_oid = fixture_relation_oid;
        unrelated_scope.stable_match_id = 102;
        unrelated_scope.query_scope_id = 12u;
        fixture[1] = unrelated_scope;

        const match_site_resolution scoped =
            resolve_match_helper_site(fixture[0],
                                      reinterpret_cast<Node*>(lineage_key),
                                      std::span<match_owner_finalization>{fixture.data(), 2u},
                                      oids);
        if (scoped.status != match_site_resolution_status::bound ||
            scoped.kind != match_helper_binding_kind::unique_lineage ||
            scoped.owner != &fixture[0] || scoped.candidate_count != 1u) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq match binding query-scope self-test failed"),
                     errdetail("unrelated subplan scope was not isolated")));
        }

        match_owner_finalization same_scope{};
        same_scope.target_recipe = reinterpret_cast<Node*>(copyObject(source));
        same_scope.base_lineage = list_make1(copyObject(source));
        same_scope.base_relid = 1;
        same_scope.target_relation_oid = fixture_relation_oid + 1;
        same_scope.stable_match_id = 103;
        same_scope.query_scope_id = 11u;
        fixture[2] = same_scope;

        Var* alias_source = makeVar(
            /*varno=*/2,
            /*varattno=*/1,
            oids.bioseq_seq_type_oid,
            /*vartypmod=*/-1,
            /*varcollid=*/InvalidOid,
            /*varlevelsup=*/0);
        match_owner_finalization same_relation_alias{};
        same_relation_alias.target_recipe = reinterpret_cast<Node*>(copyObject(alias_source));
        same_relation_alias.base_lineage = list_make1(copyObject(alias_source));
        same_relation_alias.base_relid = 2;
        same_relation_alias.target_relation_oid = fixture_relation_oid;
        same_relation_alias.stable_match_id = 104;
        same_relation_alias.query_scope_id = 11u;
        fixture[3] = same_relation_alias;

        const match_site_resolution ambiguous =
            resolve_match_helper_site(fixture[0],
                                      reinterpret_cast<Node*>(lineage_key),
                                      std::span<match_owner_finalization>{fixture},
                                      oids);
        const match_site_resolution duplicate_exact =
            resolve_match_helper_site(fixture[0],
                                      fixture[0].target_recipe,
                                      std::span<match_owner_finalization>{fixture},
                                      oids);
        if (ambiguous.status != match_site_resolution_status::ambiguous ||
            ambiguous.candidate_count != 2u ||
            duplicate_exact.status != match_site_resolution_status::ambiguous ||
            duplicate_exact.candidate_count != 2u) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq match binding query-scope self-test failed"),
                     errdetail("same-scope lineage, alias, or duplicate-exact binding changed")));
        }

        Var* secondary_key = makeVar(
            /*varno=*/2,
            /*varattno=*/2,
            INT4OID,
            /*vartypmod=*/-1,
            /*varcollid=*/InvalidOid,
            /*varlevelsup=*/0);
        std::array<match_owner_finalization, 2> composite_fixture{};
        match_owner_finalization composite{};
        composite.target_recipe = reinterpret_cast<Node*>(copyObject(source));
        composite.base_lineage = list_make2(copyObject(source), copyObject(alias_source));
        composite.base_relid = 1;
        composite.stable_match_id = 201;
        composite.query_scope_id = 21u;
        composite_fixture[0] = composite;
        match_owner_finalization secondary{};
        secondary.target_recipe = reinterpret_cast<Node*>(copyObject(alias_source));
        secondary.base_lineage = list_make1(copyObject(alias_source));
        secondary.base_relid = 2;
        secondary.stable_match_id = 202;
        secondary.query_scope_id = 21u;
        composite_fixture[1] = secondary;
        const match_site_resolution secondary_ambiguous =
            resolve_match_helper_site(composite_fixture[0],
                                      reinterpret_cast<Node*>(secondary_key),
                                      std::span<match_owner_finalization>{composite_fixture},
                                      oids);
        if (secondary_ambiguous.status != match_site_resolution_status::ambiguous ||
            secondary_ambiguous.candidate_count != 2u) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq match binding composite-lineage self-test failed"),
                     errdetail("secondary base lineage did not remain ambiguous")));
        }
        verified = true;
    }
#endif

    /** @brief Allocates a nonzero query-scope identifier and advances its counter. */
    [[nodiscard]] std::uint64_t allocate_match_owner_scope(std::uint64_t& next_scope_id) {
        if (next_scope_id == 0u || next_scope_id == (std::numeric_limits<std::uint64_t>::max)()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("Bioseq match owner query-scope limit exceeded")));
        }
        return next_scope_id++;
    }

    /** @brief Finds and validates the optional base-scan match-owner tag. */
    [[nodiscard]] List* find_base_match_owner_plan_tag(CustomScan* scan) {
        if (scan == nullptr || scan->methods != bioseq_column_scan_scan_methods()) {
            return NIL;
        }
        List* result = NIL;
        for (Node* private_node : postgres_list<Node>(scan->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            Node* magic_node = tag == NIL ? nullptr : reinterpret_cast<Node*>(linitial(tag));
            if (magic_node == nullptr || !IsA(magic_node, Integer) ||
                intVal(magic_node) != k_bioseq_base_match_owner_plan_magic)
                continue;
            Node* version_node =
                list_length(tag) > k_base_match_plan_version
                    ? reinterpret_cast<Node*>(list_nth(tag, k_base_match_plan_version))
                    : nullptr;
            Node* descriptor_node =
                list_length(tag) > k_base_match_plan_descriptor
                    ? reinterpret_cast<Node*>(list_nth(tag, k_base_match_plan_descriptor))
                    : nullptr;
            Node* sites_node = list_length(tag) > k_base_match_plan_sites
                                   ? reinterpret_cast<Node*>(list_nth(tag, k_base_match_plan_sites))
                                   : nullptr;
            Node* relation_node =
                list_length(tag) > k_base_match_plan_relation_oid
                    ? reinterpret_cast<Node*>(list_nth(tag, k_base_match_plan_relation_oid))
                    : nullptr;
            Node* executor_descriptor_node =
                list_length(tag) > k_base_match_plan_executor_descriptor
                    ? reinterpret_cast<Node*>(list_nth(tag, k_base_match_plan_executor_descriptor))
                    : nullptr;
            Node* executor_sites_node =
                list_length(tag) > k_base_match_plan_executor_sites
                    ? reinterpret_cast<Node*>(list_nth(tag, k_base_match_plan_executor_sites))
                    : nullptr;
            if (result != NIL || list_length(tag) != k_bioseq_base_match_owner_plan_fields ||
                version_node == nullptr || !IsA(version_node, Integer) ||
                intVal(version_node) != k_bioseq_base_match_owner_plan_version ||
                descriptor_node == nullptr || !IsA(descriptor_node, List) ||
                (sites_node != nullptr && !IsA(sites_node, List)) || relation_node == nullptr ||
                !IsA(relation_node, Integer) ||
                !OidIsValid(static_cast<Oid>(intVal(relation_node))) ||
                (executor_descriptor_node != nullptr && !IsA(executor_descriptor_node, List)) ||
                (executor_sites_node != nullptr && !IsA(executor_sites_node, List))) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqScan match owner plan tag is invalid")));
            }
            result = tag;
        }
        return result;
    }

    /**
     * @brief Collects match owners recursively while assigning distinct scopes to subplans.
     * @throws PostgreSQL ERROR when serialized owner metadata violates its plan ABI.
     */
    void collect_match_owner_plan(Plan* plan,
                                  std::vector<match_owner_finalization>& owners,
                                  const std::uint64_t query_scope_id,
                                  std::uint64_t& next_scope_id) {
        if (plan == nullptr)
            return;
        if (query_scope_id == 0u) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq match owner query scope is invalid")));
        }
        check_stack_depth();
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            const char* name = custom->methods == nullptr ? nullptr : custom->methods->CustomName;
            if (name != nullptr && strcmp(name, "BioseqBatchJoin") == 0 &&
                custom->custom_private != NIL) {
                Node* version_node = reinterpret_cast<Node*>(linitial(custom->custom_private));
                if (version_node != nullptr && IsA(version_node, Integer)) {
                    const int version = intVal(version_node);
                    const int descriptor_index =
                        version == k_bioseq_batch_join_plan_version
                            ? k_batch_join_match_owner_descriptor
                            : (version == k_bioseq_per_row_join_plan_version
                                   ? k_per_row_match_owner_descriptor
                                   : -1);
                    const int sites_index = version == k_bioseq_batch_join_plan_version
                                                ? k_batch_join_placeholder_sites
                                                : (version == k_bioseq_per_row_join_plan_version
                                                       ? k_per_row_placeholder_sites
                                                       : -1);
                    const int relation_oid_index =
                        version == k_bioseq_batch_join_plan_version
                            ? k_batch_join_target_relation_oid
                            : (version == k_bioseq_per_row_join_plan_version
                                   ? k_per_row_target_relation_oid
                                   : -1);
                    if (descriptor_index >= 0 &&
                        descriptor_index < list_length(custom->custom_private) &&
                        sites_index >= 0 && relation_oid_index >= 0 &&
                        sites_index < list_length(custom->custom_private)) {
                        Node* descriptor_node = reinterpret_cast<Node*>(
                            list_nth(custom->custom_private, descriptor_index));
                        Node* sites_node =
                            reinterpret_cast<Node*>(list_nth(custom->custom_private, sites_index));
                        if (descriptor_node == nullptr || !IsA(descriptor_node, List) ||
                            (sites_node != nullptr && !IsA(sites_node, List))) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_INTERNAL_ERROR),
                                     errmsg("Bioseq match owner finalization ABI is invalid")));
                        }
                        List* descriptor = reinterpret_cast<List*>(descriptor_node);
                        if (list_length(descriptor) != k_bioseq_match_owner_descriptor_fields ||
                            intVal(match_binding_integer(
                                descriptor, k_match_owner_magic, "owner.magic")) !=
                                k_bioseq_match_owner_descriptor_magic ||
                            intVal(match_binding_integer(
                                descriptor, k_match_owner_version, "owner.version")) !=
                                k_bioseq_match_owner_descriptor_version) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_INTERNAL_ERROR),
                                     errmsg("Bioseq match owner descriptor is invalid")));
                        }
                        Node* lineage_node = reinterpret_cast<Node*>(
                            list_nth(descriptor, k_match_owner_base_lineage));
                        if (lineage_node != nullptr && !IsA(lineage_node, List)) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_INTERNAL_ERROR),
                                     errmsg("Bioseq match owner lineage is invalid")));
                        }
                        owners.push_back(match_owner_finalization{
                            custom,
                            descriptor,
                            sites_node == nullptr ? NIL : reinterpret_cast<List*>(sites_node),
                            reinterpret_cast<Node*>(
                                list_nth(descriptor, k_match_owner_target_recipe)),
                            lineage_node == nullptr ? NIL : reinterpret_cast<List*>(lineage_node),
                            static_cast<Index>(intVal(match_binding_integer(
                                descriptor, k_match_owner_base_relid, "owner.base_relid"))),
                            static_cast<Oid>(
                                intVal(match_binding_integer(custom->custom_private,
                                                             relation_oid_index,
                                                             "owner.target_relation_oid"))),
                            plan->plan_node_id,
                            query_scope_id,
                            NIL});
                    }
                }
            }
            List* base_owner_tag = find_base_match_owner_plan_tag(custom);
            if (base_owner_tag != NIL) {
                Node* descriptor_node =
                    reinterpret_cast<Node*>(list_nth(base_owner_tag, k_base_match_plan_descriptor));
                Node* sites_node =
                    reinterpret_cast<Node*>(list_nth(base_owner_tag, k_base_match_plan_sites));
                List* descriptor = reinterpret_cast<List*>(descriptor_node);
                if (list_length(descriptor) != k_bioseq_match_owner_descriptor_fields ||
                    intVal(match_binding_integer(descriptor, k_match_owner_magic, "owner.magic")) !=
                        k_bioseq_match_owner_descriptor_magic ||
                    intVal(match_binding_integer(
                        descriptor, k_match_owner_version, "owner.version")) !=
                        k_bioseq_match_owner_descriptor_version) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqScan match owner descriptor is invalid")));
                }
                Node* lineage_node =
                    reinterpret_cast<Node*>(list_nth(descriptor, k_match_owner_base_lineage));
                if (lineage_node != nullptr && !IsA(lineage_node, List)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqScan match owner lineage is invalid")));
                }
                owners.push_back(match_owner_finalization{
                    custom,
                    descriptor,
                    sites_node == nullptr ? NIL : reinterpret_cast<List*>(sites_node),
                    reinterpret_cast<Node*>(list_nth(descriptor, k_match_owner_target_recipe)),
                    lineage_node == nullptr ? NIL : reinterpret_cast<List*>(lineage_node),
                    static_cast<Index>(intVal(match_binding_integer(
                        descriptor, k_match_owner_base_relid, "owner.base_relid"))),
                    static_cast<Oid>(intVal(match_binding_integer(base_owner_tag,
                                                                  k_base_match_plan_relation_oid,
                                                                  "owner.target_relation_oid"))),
                    plan->plan_node_id,
                    query_scope_id,
                    base_owner_tag});
            }
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                collect_match_owner_plan(child, owners, query_scope_id, next_scope_id);
            }
            return;
        }
        if (IsA(plan, Append)) {
            for (Plan* child : postgres_list<Plan>(reinterpret_cast<Append*>(plan)->appendplans)) {
                collect_match_owner_plan(child, owners, query_scope_id, next_scope_id);
            }
            return;
        }
        if (IsA(plan, MergeAppend)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<MergeAppend*>(plan)->mergeplans)) {
                collect_match_owner_plan(child, owners, query_scope_id, next_scope_id);
            }
            return;
        }
        if (IsA(plan, BitmapAnd)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans)) {
                collect_match_owner_plan(child, owners, query_scope_id, next_scope_id);
            }
            return;
        }
        if (IsA(plan, BitmapOr)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapOr*>(plan)->bitmapplans)) {
                collect_match_owner_plan(child, owners, query_scope_id, next_scope_id);
            }
            return;
        }
        if (IsA(plan, SubqueryScan)) {
            const std::uint64_t subquery_scope = allocate_match_owner_scope(next_scope_id);
            collect_match_owner_plan(reinterpret_cast<SubqueryScan*>(plan)->subplan,
                                     owners,
                                     subquery_scope,
                                     next_scope_id);
            return;
        }
        collect_match_owner_plan(outerPlan(plan), owners, query_scope_id, next_scope_id);
        if (innerPlan(plan) != outerPlan(plan)) {
            collect_match_owner_plan(innerPlan(plan), owners, query_scope_id, next_scope_id);
        }
    }

    /** @brief Checks that setrefs preserved one base-lineage Var's physical attribute identity. */
    [[nodiscard]] bool
    base_match_target_var_translation_agrees(const Var* planner_target,
                                             const Var* executor_target,
                                             const Index executor_scanrelid,
                                             const bioseq_stub_oids& oids) noexcept {
        return planner_target != nullptr && executor_target != nullptr &&
               bioseq_is_concrete_rte_varno(planner_target->varno) &&
               planner_target->varattno > 0 && planner_target->varlevelsup == 0 &&
               bms_is_empty(planner_target->varnullingrels) &&
               planner_target->vartype == oids.bioseq_seq_type_oid &&
               bioseq_is_concrete_rte_varno(executor_scanrelid) &&
               executor_target->varno == executor_scanrelid && executor_target->varattno > 0 &&
               executor_target->varlevelsup == 0 && bms_is_empty(executor_target->varnullingrels) &&
               executor_target->vartype == oids.bioseq_seq_type_oid &&
               planner_target->varattno == executor_target->varattno &&
               planner_target->vartype == executor_target->vartype &&
               planner_target->vartypmod == executor_target->vartypmod &&
               planner_target->varcollid == executor_target->varcollid;
    }

    /** @brief Checks planner and executor Vars for the selected helper binding form. */
    [[nodiscard]] bool
    base_match_site_var_translation_agrees(const Var* planner_argument,
                                           const Var* executor_argument,
                                           const Var* planner_target,
                                           const Var* executor_target,
                                           const match_helper_binding_kind binding_kind,
                                           const bioseq_stub_oids& oids) noexcept {
        if (planner_argument == nullptr || executor_argument == nullptr ||
            planner_target == nullptr || executor_target == nullptr ||
            planner_argument->varlevelsup != 0 || executor_argument->varlevelsup != 0 ||
            !bms_is_empty(planner_argument->varnullingrels) ||
            !bms_is_empty(executor_argument->varnullingrels))
            return false;
        if (binding_kind == match_helper_binding_kind::exact_recipe) {
            return planner_argument->vartype == oids.bioseq_seq_type_oid &&
                   executor_argument->vartype == oids.bioseq_seq_type_oid &&
                   equal(planner_argument, planner_target) &&
                   equal(executor_argument, executor_target);
        }
        if (binding_kind != match_helper_binding_kind::unique_lineage ||
            planner_argument->vartype == oids.bioseq_seq_type_oid ||
            executor_argument->vartype == oids.bioseq_seq_type_oid ||
            planner_argument->varno != planner_target->varno ||
            executor_argument->varno != executor_target->varno)
            return false;
        /* setrefs may offset the query-local RTE index, but it cannot change which
         * physical attribute, type, typmod, or collation the row marker names. */
        return planner_argument->varattno == executor_argument->varattno &&
               planner_argument->vartype == executor_argument->vartype &&
               planner_argument->vartypmod == executor_argument->vartypmod &&
               planner_argument->varcollid == executor_argument->varcollid;
    }

    /** @brief Checks whether setrefs translated an expression only by its scan relation index. */
    [[nodiscard]] bool base_match_expression_translation_agrees(Node* planner_expression,
                                                                Node* executor_expression,
                                                                const Index planner_relid,
                                                                const Index executor_relid) {
        if (planner_expression == nullptr || executor_expression == nullptr ||
            !bioseq_is_concrete_rte_varno(planner_relid) ||
            !bioseq_is_concrete_rte_varno(executor_relid) ||
            exprType(planner_expression) != exprType(executor_expression) ||
            exprTypmod(planner_expression) != exprTypmod(executor_expression) ||
            exprCollation(planner_expression) != exprCollation(executor_expression))
            return false;
        Node* translated = reinterpret_cast<Node*>(copyObject(planner_expression));
        ChangeVarNodes(translated,
                       static_cast<int>(planner_relid),
                       static_cast<int>(executor_relid),
                       /*sublevels_up=*/0);
        return equal(translated, executor_expression);
    }

    /** @brief Checks the post-setrefs target recipe and each canonical base-lineage source. */
    [[nodiscard]] bool
    base_match_target_expression_translation_agrees(Node* planner_target,
                                                    Node* executor_target,
                                                    const Index planner_relid,
                                                    const Index executor_scanrelid,
                                                    const bioseq_stub_oids& oids) {
        if (planner_target == nullptr || executor_target == nullptr ||
            exprType(planner_target) != oids.bioseq_seq_type_oid ||
            exprType(executor_target) != oids.bioseq_seq_type_oid ||
            !base_match_expression_translation_agrees(
                planner_target, executor_target, planner_relid, executor_scanrelid))
            return false;
        List* planner_lineage = canonical_match_base_lineage(planner_target, oids);
        List* executor_lineage = canonical_match_base_lineage(executor_target, oids);
        if (planner_lineage == NIL ||
            list_length(planner_lineage) != list_length(executor_lineage)) {
            return false;
        }
        for (int index = 0; index < list_length(planner_lineage); ++index) {
            Var* planner_source = reinterpret_cast<Var*>(list_nth(planner_lineage, index));
            Var* executor_source = reinterpret_cast<Var*>(list_nth(executor_lineage, index));
            if (!base_match_target_var_translation_agrees(
                    planner_source, executor_source, executor_scanrelid, oids)) {
                return false;
            }
        }
        return true;
    }

    /** @brief Checks one helper argument's planner-to-executor translation. */
    [[nodiscard]] bool
    base_match_site_expression_translation_agrees(Node* planner_argument,
                                                  Node* executor_argument,
                                                  Node* planner_target,
                                                  Node* executor_target,
                                                  const Index planner_relid,
                                                  const Index executor_relid,
                                                  const match_helper_binding_kind binding_kind,
                                                  const bioseq_stub_oids& oids) {
        if (planner_argument == nullptr || executor_argument == nullptr ||
            planner_target == nullptr || executor_target == nullptr)
            return false;
        if (binding_kind == match_helper_binding_kind::exact_recipe) {
            return equal(planner_argument, planner_target) &&
                   equal(executor_argument, executor_target) &&
                   base_match_expression_translation_agrees(
                       planner_argument, executor_argument, planner_relid, executor_relid);
        }
        if (binding_kind != match_helper_binding_kind::unique_lineage) {
            return false;
        }
        Var* planner_var = try_cast_pg_node<Var>(planner_argument);
        Var* executor_var = try_cast_pg_node<Var>(executor_argument);
        if (planner_var == nullptr || executor_var == nullptr ||
            planner_var->varno != planner_relid || executor_var->varno != executor_relid ||
            planner_var->varattno <= 0 || executor_var->varattno <= 0 ||
            planner_var->varlevelsup != 0 || executor_var->varlevelsup != 0 ||
            !bms_is_empty(planner_var->varnullingrels) ||
            !bms_is_empty(executor_var->varnullingrels) ||
            planner_var->varattno != executor_var->varattno ||
            planner_var->vartype != executor_var->vartype ||
            planner_var->vartypmod != executor_var->vartypmod ||
            planner_var->varcollid != executor_var->varcollid)
            return false;
        if (planner_var->vartype != oids.bioseq_seq_type_oid)
            return true;
        bool planner_member = false;
        for (Var* source : postgres_list<Var>(canonical_match_base_lineage(planner_target, oids))) {
            if (equal(source, planner_var)) {
                planner_member = true;
                break;
            }
        }
        bool executor_member = false;
        for (Var* source :
             postgres_list<Var>(canonical_match_base_lineage(executor_target, oids))) {
            if (equal(source, executor_var)) {
                executor_member = true;
                break;
            }
        }
        return planner_member && executor_member;
    }

    /** @brief Tests a finalized helper argument against its canonical planner expression. */
    [[nodiscard]] bool finalized_match_helper_argument_translation_agrees(CustomScan* scan,
                                                                          Node* translated,
                                                                          Node* canonical) noexcept;

    /**
     * @brief Builds the post-setrefs descriptor and helper sites for a base scan owner.
     * @throws PostgreSQL ERROR when planner and executor identities do not agree.
     */
    void finalize_base_match_owner_executor_image(match_owner_finalization& owner,
                                                  const bioseq_stub_oids& oids) {
        if (owner.base_owner_tag == NIL)
            return;
        CustomScan* scan = owner.scan;
        Node* existing_descriptor = reinterpret_cast<Node*>(
            list_nth(owner.base_owner_tag, k_base_match_plan_executor_descriptor));
        Node* existing_sites = reinterpret_cast<Node*>(
            list_nth(owner.base_owner_tag, k_base_match_plan_executor_sites));
        Node* owned_quals_node = scan == nullptr || list_length(scan->custom_exprs) <= 3
                                     ? nullptr
                                     : reinterpret_cast<Node*>(list_nth(scan->custom_exprs, 3));
        List* owned_quals = owned_quals_node != nullptr && IsA(owned_quals_node, List)
                                ? reinterpret_cast<List*>(owned_quals_node)
                                : NIL;
        FuncExpr* mmsearch =
            list_length(owned_quals) >= 1
                ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(linitial(owned_quals)))
                : nullptr;
        Node* executor_target = mmsearch != nullptr && is_mmsearch_funcid(mmsearch->funcid, oids) &&
                                        list_length(mmsearch->args) >= 1
                                    ? reinterpret_cast<Node*>(linitial(mmsearch->args))
                                    : nullptr;
        Node* planner_target = owner.target_recipe;
        if (scan == nullptr || existing_descriptor != nullptr || existing_sites != nullptr ||
            planner_target == nullptr || executor_target == nullptr ||
            !base_match_target_expression_translation_agrees(
                planner_target, executor_target, owner.base_relid, scan->scan.scanrelid, oids)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqScan post-setrefs match owner identity disagrees with its "
                            "planner descriptor")));
        }

        List* executor_descriptor =
            make_match_owner_descriptor(executor_target, scan->scan.scanrelid, oids);
        match_binding_integer(
            executor_descriptor, k_match_owner_stable_id, "executor_owner.stable_id")
            ->ival = owner.stable_match_id;
        List* executor_lineage = canonical_match_base_lineage(executor_target, oids);
        Var* executor_primary_source =
            executor_lineage == NIL ? nullptr : reinterpret_cast<Var*>(linitial(executor_lineage));
        if (executor_primary_source == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqScan post-setrefs match owner lineage is empty")));
        }
        List* executor_sites = collect_base_match_helper_sites(scan->scan.plan.targetlist,
                                                               executor_target,
                                                               scan->scan.scanrelid,
                                                               executor_primary_source->varattno,
                                                               oids);
        if (list_length(executor_sites) != list_length(owner.sites)) {
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqScan post-setrefs match helper sites disagree with planner sites"),
                 errdetail("planner_sites=%d executor_sites=%d",
                           list_length(owner.sites),
                           list_length(executor_sites))));
        }
        for (int index = 0; index < list_length(owner.sites); ++index) {
            List* planner_site = reinterpret_cast<List*>(list_nth(owner.sites, index));
            List* executor_site = reinterpret_cast<List*>(list_nth(executor_sites, index));
            if (planner_site == nullptr || executor_site == nullptr || !IsA(planner_site, List) ||
                !IsA(executor_site, List) ||
                list_length(planner_site) != k_bioseq_match_helper_site_fields ||
                list_length(executor_site) != k_bioseq_match_helper_site_fields) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan post-setrefs match helper site codec is invalid")));
            }
            for (const int integer_index : {k_match_site_resno,
                                            k_match_site_funcid,
                                            k_match_site_typmod,
                                            k_match_site_collation,
                                            k_match_site_binding_kind,
                                            k_match_site_phid,
                                            k_match_site_stable_id}) {
                (void)match_binding_integer(
                    planner_site, integer_index, "planner_helper.site_integer");
                (void)match_binding_integer(
                    executor_site, integer_index, "executor_helper.site_integer");
            }
            const auto binding_kind = static_cast<match_helper_binding_kind>(
                intVal(list_nth(planner_site, k_match_site_binding_kind)));
            Node* planner_argument =
                reinterpret_cast<Node*>(list_nth(planner_site, k_match_site_canonical_argument));
            Node* executor_argument =
                reinterpret_cast<Node*>(list_nth(executor_site, k_match_site_canonical_argument));
            const bool integer_identity_agrees =
                intVal(list_nth(planner_site, k_match_site_resno)) ==
                    intVal(list_nth(executor_site, k_match_site_resno)) &&
                intVal(list_nth(planner_site, k_match_site_funcid)) ==
                    intVal(list_nth(executor_site, k_match_site_funcid)) &&
                intVal(list_nth(planner_site, k_match_site_typmod)) ==
                    intVal(list_nth(executor_site, k_match_site_typmod)) &&
                intVal(list_nth(planner_site, k_match_site_collation)) ==
                    intVal(list_nth(executor_site, k_match_site_collation)) &&
                intVal(list_nth(planner_site, k_match_site_phid)) ==
                    intVal(list_nth(executor_site, k_match_site_phid));
            if (!integer_identity_agrees ||
                intVal(list_nth(planner_site, k_match_site_stable_id)) != owner.stable_match_id ||
                !base_match_site_expression_translation_agrees(planner_argument,
                                                               executor_argument,
                                                               planner_target,
                                                               executor_target,
                                                               owner.base_relid,
                                                               scan->scan.scanrelid,
                                                               binding_kind,
                                                               oids)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan post-setrefs match helper identity disagrees with its "
                                "planner site"),
                         errdetail("site_index=%d", index)));
            }
            match_binding_integer(
                executor_site, k_match_site_binding_kind, "executor_helper.binding_kind")
                ->ival = static_cast<int>(binding_kind);
            match_binding_integer(
                executor_site, k_match_site_stable_id, "executor_helper.stable_id")
                ->ival = owner.stable_match_id;
        }

        lfirst(list_nth_cell(owner.base_owner_tag, k_base_match_plan_executor_descriptor)) =
            executor_descriptor;
        lfirst(list_nth_cell(owner.base_owner_tag, k_base_match_plan_executor_sites)) =
            executor_sites;
    }

    /**
     * @brief Assigns stable owner identifiers and resolves every registered helper site.
     * @throws PostgreSQL ERROR for invalid, ambiguous, or cross-owner bindings.
     */
    void finalize_match_owner_bindings(std::span<match_owner_finalization> owners) {
        const bioseq_stub_oids& oids = get_cached_stub_oids();
#ifdef BIOSEQ_TEST_INDEXES
        verify_match_owner_scope_resolution(oids);
#endif
        Bitmapset* stable_ids = nullptr;
        std::size_t owner_index = 0u;
        for (match_owner_finalization& owner : owners) {
            /* set_plan_references() may assign zero to the root Plan.  The stable
             * owner ABI is the exact plan_node_id, so only negative ids are invalid
             * and zero must participate in duplicate detection like every other id. */
            const bool duplicate_stable_id =
                owner.stable_match_id >= 0 && bms_is_member(owner.stable_match_id, stable_ids);
            if (owner.scan == nullptr || owner.descriptor == NIL ||
                owner.target_recipe == nullptr || !bioseq_is_concrete_rte_varno(owner.base_relid) ||
                owner.stable_match_id < 0 || owner.query_scope_id == 0u || duplicate_stable_id) {
                const char* custom_name = owner.scan == nullptr || owner.scan->methods == nullptr
                                              ? "<none>"
                                              : owner.scan->methods->CustomName;
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("Bioseq final match owner identity is invalid"),
                         errdetail("owner_index=%zu custom_name=%s scan_present=%s "
                                   "descriptor_present=%s target_recipe_present=%s base_relid=%u "
                                   "stable_match_id=%d query_scope_id=" UINT64_FORMAT
                                   " duplicate_stable_id=%s",
                                   owner_index,
                                   custom_name == nullptr ? "<none>" : custom_name,
                                   owner.scan == nullptr ? "false" : "true",
                                   owner.descriptor == NIL ? "false" : "true",
                                   owner.target_recipe == nullptr ? "false" : "true",
                                   static_cast<unsigned int>(owner.base_relid),
                                   owner.stable_match_id,
                                   static_cast<uint64>(owner.query_scope_id),
                                   duplicate_stable_id ? "true" : "false")));
            }
            stable_ids = bms_add_member(stable_ids, owner.stable_match_id);
            match_binding_integer(owner.descriptor, k_match_owner_stable_id, "owner.stable_id")
                ->ival = owner.stable_match_id;
            ++owner_index;
        }
        if (stable_ids != nullptr)
            bms_free(stable_ids);

        for (match_owner_finalization& site_owner : owners) {
            for (List* site : postgres_list<List>(site_owner.sites)) {
                if (site == nullptr || !IsA(site, List) ||
                    list_length(site) != k_bioseq_match_helper_site_fields) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("Bioseq match helper site codec is invalid")));
                }
                for (const int integer_index : {k_match_site_resno,
                                                k_match_site_funcid,
                                                k_match_site_typmod,
                                                k_match_site_collation,
                                                k_match_site_binding_kind,
                                                k_match_site_phid,
                                                k_match_site_stable_id}) {
                    (void)match_binding_integer(site, integer_index, "helper.site_integer");
                }
                Node* argument =
                    reinterpret_cast<Node*>(list_nth(site, k_match_site_canonical_argument));
                const match_site_resolution resolution =
                    resolve_match_helper_site(site_owner, argument, owners, oids);
                if (resolution.status == match_site_resolution_status::ambiguous) {
                    const Oid funcid = static_cast<Oid>(
                        intVal(match_binding_integer(site, k_match_site_funcid, "helper.funcid")));
                    const char* function_name = get_func_name(funcid);
                    ereport(ERROR,
                            (errcode(ERRCODE_AMBIGUOUS_COLUMN),
                             errmsg("bioseq match-result helper binding is ambiguous"),
                             errdetail("helper=%s candidate_match_owners=%zu",
                                       function_name == nullptr ? "<unknown>" : function_name,
                                       resolution.candidate_count),
                             errhint("Pass the exact transformed target recipe to each helper when "
                                     "more than one mmsearch owner shares a base lineage.")));
                }
                if (resolution.status != match_site_resolution_status::bound ||
                    resolution.owner != &site_owner) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                         errmsg("bioseq match-result helper does not identify its mmsearch owner"),
                         errhint("Use the exact mmsearch target recipe or its unique base source "
                                 "lineage.")));
                }
                match_binding_integer(site, k_match_site_binding_kind, "helper.binding_kind")
                    ->ival = static_cast<int>(resolution.kind);
                match_binding_integer(site, k_match_site_stable_id, "helper.stable_id")->ival =
                    site_owner.stable_match_id;
            }
        }
        for (match_owner_finalization& owner : owners) {
            finalize_base_match_owner_executor_image(owner, oids);
        }
    }

    /** @brief Rewrites helper placeholders owned by ancestor match plans to inert expressions. */
    void neutralize_ancestor_owned_dummy_match_helpers(PlannedStmt* statement,
                                                       std::span<match_owner_finalization> owners);

    /** @brief Resolve every match helper to one finalized owner in a planned statement. */
    void bioseq_finalize_match_helper_bindings(PlannedStmt* statement) {
        if (statement == nullptr || statement->planTree == nullptr ||
            !OidIsValid(get_extension_oid("bioseq", /*missing_ok=*/true))) {
            return;
        }
        ErrorData* exact_error = nullptr;
        {
#ifdef BIOSEQ_TEST_INDEXES
            [[maybe_unused]] match_owner_finalization_scope_guard scope_guard;
#endif
            std::vector<match_owner_finalization> owners;
            try {
                /* This callback is below PostgreSQL's sigsetjmp boundary.  Every local
                 * it reaches is either PostgreSQL-owned (List/Bitmapset) or statically
                 * trivially destructible (span/fixed fixture/scalars).  The sole heap-
                * owning C++ container is this outer vector, which remains above the
                 * boundary and therefore unwinds after a translated PG error. */
                // Collect and finalize all query-local owners inside the translated error boundary.
                bioseq::postgres::support::invoke_pg_as_cpp([statement, &owners] {
                    std::uint64_t next_scope_id = 1u;
                    const std::uint64_t statement_scope = allocate_match_owner_scope(next_scope_id);
                    collect_match_owner_plan(
                        statement->planTree, owners, statement_scope, next_scope_id);
                    for (Plan* subplan : postgres_list<Plan>(statement->subplans)) {
                        const std::uint64_t subplan_scope =
                            allocate_match_owner_scope(next_scope_id);
                        collect_match_owner_plan(subplan, owners, subplan_scope, next_scope_id);
                    }
                    if (!owners.empty()) {
                        finalize_match_owner_bindings(std::span<match_owner_finalization>{owners});
                        neutralize_ancestor_owned_dummy_match_helpers(
                            statement, std::span<match_owner_finalization>{owners});
                    }
                });
            } catch (bioseq::postgres::support::postgres_error& error) {
                exact_error = error.release_error_data();
            }
        }
        /* Re-enter PostgreSQL's error ABI only after the vector and every other C++
         * owner in the finalization scope have been destroyed. */
        if (exact_error != nullptr)
            ReThrowError(exact_error);
    }

} // export custom-scan partition declarations
