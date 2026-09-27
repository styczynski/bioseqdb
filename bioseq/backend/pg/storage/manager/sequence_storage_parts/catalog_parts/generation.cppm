/**
 * @file generation.cppm
 * @brief Minimizer-generation catalog schema validation.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/sequence.h"
#include "commands/vacuum.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.manager:sequence_storage_catalog_generation;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.storage;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.table.io;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.indexing.storage;
import bioseq.common.seqan_adapter; // ceil_log2 used in letters_per_unit_for
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
// The PG-free bucket math and sequence_locator value live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
import bioseq.storage.layout;

export import :sequence_storage_catalog_source;

export namespace bioseq::storage {
// The descriptor catalog owns required relation locator attributes and two
// partial indexes, so the generic eight-column sidecar validator cannot
// describe it. Keep this cold schema authority next to the existing
// PostgreSQL-owned sequence-source catalog contracts; generation-image
// lifecycle code consumes the exported entry point without importing a second
// tuple-shape authority.
namespace minimizer_generation_catalog_detail {

/// Supported canonical defaults in generation catalog descriptors.
enum class default_kind : std::uint8_t {
    none,
    zero_int2,
    zero_int4,
    zero_int8,
    empty_text,
};

inline constexpr std::array<default_kind, minimizer_generation_storage_codec::field_count>
    k_storage_defaults{};

inline constexpr std::array<default_kind, minimizer_generation_reaper_codec::field_count>
    k_reaper_defaults{{
        default_kind::none,
        default_kind::zero_int4,
        default_kind::zero_int2,
        default_kind::empty_text,
        default_kind::zero_int8,
    }};

inline constexpr std::array<std::string_view, 13> k_storage_check_expressions{{
    "database_oid <> 0",
    "tablespace_oid <> 0",
    "main_relfilenode <> 0",
    "user_relid <> 0",
    "column_attno > 0",
    "index_name = 'minimizers_per_fragment'",
    "query_key >= 0",
    "generation_id > 0",
    "provider = 0",
    "image_bytes >= 4096",
    "state IN (1, 2)",
    "(payload_relation_oid <> 0 "
    "AND payload_database_oid = database_oid "
    "AND payload_tablespace_oid <> 0 "
    "AND payload_relfilenode <> 0)",
    "((state = 1 AND retire_full_xid IS NULL) "
    "OR (state = 2 AND retire_full_xid IS NOT NULL))",
}};

inline constexpr std::array<std::string_view, 4> k_reaper_check_expressions{{
    "database_oid <> 0",
    "tablespace_root_ordinal >= 0",
    "shard_ordinal BETWEEN 0 AND 255",
    "pass >= 0",
}};

/// Raise one generation-catalog schema contract violation.
/// @param catalog Logical catalog name copied into the error detail.
/// @param detail Contract description copied into the error detail.
/// @throws PostgreSQL ERROR unconditionally.
[[noreturn]] inline void contract_error(const std::string_view catalog,
                                        const std::string_view detail) {
    const std::string owned_catalog{catalog};
    const std::string owned_detail{detail};
    bioseq::postgres::support::invoke_pg_as_cpp([&owned_catalog, &owned_detail] {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq minimizer generation catalog contract changed"),
             errdetail("catalog=%s invariant=%s", owned_catalog.c_str(), owned_detail.c_str())));
    });
    pg_unreachable();
}

/// Check an attribute default against its canonical expression.
/// @param relation Open relation containing the attribute.
/// @param expected Expected attribute descriptor.
/// @param expected_default_kind Canonical default category.
/// @return Whether the analyzed default has the required constant form.
[[nodiscard]] inline bool
exact_default(Relation relation,
              const bioseq::postgres::support::row_descriptor_field& expected,
              const default_kind expected_default_kind) {
    Node* node = bioseq::postgres::support::invoke_pg_as_cpp([relation, &expected] {
        return build_column_default(relation, expected.attribute_number);
    });
    if (expected_default_kind == default_kind::none) {
        return node == nullptr;
    }
    if (node == nullptr || !IsA(node, Const))
        return false;
    const auto* constant = castNode(Const, node);
    if (constant->constisnull)
        return false;
    switch (expected_default_kind) {
    case default_kind::none:
        return false;
    case default_kind::zero_int2:
        return constant->consttype == INT2OID && DatumGetInt16(constant->constvalue) == 0;
    case default_kind::zero_int4:
        return constant->consttype == INT4OID && DatumGetInt32(constant->constvalue) == 0;
    case default_kind::zero_int8:
        return constant->consttype == INT8OID && DatumGetInt64(constant->constvalue) == 0;
    case default_kind::empty_text: {
        if (constant->consttype != TEXTOID)
            return false;
        const text* value = DatumGetTextPP(constant->constvalue);
        return VARSIZE_ANY_EXHDR(value) == 0;
    }
    }
    return false;
}

/// Validate relation identity, access method and every attribute descriptor.
/// @tparam p_attribute_count Number of fixed attributes in the catalog.
/// @param relation Open catalog relation held by the caller.
/// @param expected_relation_oid Required relation OID.
/// @param expected_name Required unqualified relation name.
/// @param attributes Fixed attribute descriptors in physical order.
/// @param defaults Required default category for each attribute.
/// @param expected_check_count Exact number of CHECK constraints.
/// @throws PostgreSQL ERROR when any relation or attribute contract differs.
template <std::size_t p_attribute_count>
inline void
validate_relation_shape(Relation relation,
                        const Oid expected_relation_oid,
                        const std::string_view expected_name,
                        const std::array<bioseq::postgres::support::row_descriptor_field,
                                         p_attribute_count>& attributes,
                        const std::array<default_kind, p_attribute_count>& defaults,
                        const std::uint16_t expected_check_count) {
    if (relation == nullptr || RelationGetRelid(relation) != expected_relation_oid ||
        RelationGetNamespace(relation) != bioseq::postgres::support::invoke_pg_as_cpp([] {
            return get_namespace_oid("bioseq_internal", true);
        }) ||
        relation->rd_rel == nullptr || relation->rd_rel->relkind != RELKIND_RELATION ||
        relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
        relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        contract_error(expected_name, "relation identity, UNLOGGED persistence, or heap AM");
    }
    TupleDesc descriptor = RelationGetDescr(relation);
    if (descriptor == nullptr || descriptor->natts != static_cast<int>(attributes.size())) {
        contract_error(expected_name, "attribute count");
    }
    std::uint16_t observed_defaults = 0;
    for (std::size_t index = 0; index < attributes.size(); ++index) {
        const bioseq::postgres::support::row_descriptor_field& expected = attributes[index];
        const default_kind expected_default_kind = defaults[index];
        const Form_pg_attribute attribute =
            bioseq::postgres::support::tuple_descriptor_attribute(descriptor, expected.attribute_number - 1);
        const bool expects_default = expected_default_kind != default_kind::none;
        if (attribute->attnum != expected.attribute_number || attribute->attisdropped ||
            attribute->attinhcount != 0 || !attribute->attislocal ||
            attribute->attnotnull != !expected.nullable ||
            attribute->atttypid != expected.type_oid || attribute->atttypmod != -1 ||
            attribute->attcollation != bioseq::postgres::support::invoke_pg_as_cpp([&expected] {
                return get_typcollation(expected.type_oid);
            }) ||
            attribute->atthasdef != expects_default || attribute->atthasmissing ||
            attribute->attidentity != '\0' || attribute->attgenerated != '\0' ||
            std::string_view{NameStr(attribute->attname)} != expected.name) {
            contract_error(expected_name, "attribute ordinal/name/type/typmod/nullability/default");
        }
        if (expects_default)
            ++observed_defaults;
    }
    const TupleConstr* constraints = descriptor->constr;
    if (constraints == nullptr || !constraints->has_not_null || constraints->has_generated_stored ||
        constraints->num_defval != observed_defaults ||
        constraints->num_check != expected_check_count ||
        (expected_check_count != 0 && constraints->check == nullptr)) {
        contract_error(expected_name, "NOT NULL/default/CHECK constraint counts");
    }
    for (std::uint16_t index = 0; index < constraints->num_check; ++index) {
        const ConstrCheck& check = constraints->check[index];
        if (check.ccname == nullptr || check.ccname[0] == '\0' || check.ccbin == nullptr ||
            check.ccbin[0] == '\0' || !check.ccvalid || check.ccnoinherit) {
            contract_error(expected_name, "CHECK constraint identity, validation, or inheritance");
        }
    }
}

/// Parse and analyze one canonical CHECK expression for comparison.
/// @param relation_name Unqualified catalog name used for name resolution.
/// @param expression SQL expression text owned by the compiled contract.
/// @return PostgreSQL expression node allocated in the current memory context.
/// @throws PostgreSQL ERROR when parsing or analysis does not yield one expression.
[[nodiscard]] inline Node* analyze_expected_check_expression(const std::string_view relation_name,
                                                             const std::string_view expression) {
    const std::string sql =
        fmt::format("SELECT ({}) FROM ONLY bioseq_internal.{};", expression, relation_name);
    return bioseq::postgres::support::invoke_pg_as_cpp([&sql] {
        List* raw_statements = raw_parser(sql.c_str(), RAW_PARSE_DEFAULT);
        if (list_length(raw_statements) != 1) {
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq generation schema expectation did not parse as one statement")));
        }
        auto* raw = castNode(RawStmt, linitial(raw_statements));
        Query* query = parse_analyze_fixedparams(raw, sql.c_str(), nullptr, 0, nullptr);
        if (query == nullptr || query->commandType != CMD_SELECT ||
            list_length(query->targetList) != 1) {
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq generation schema expectation did not analyze as one expression")));
        }
        auto* target = castNode(TargetEntry, linitial(query->targetList));
        return reinterpret_cast<Node*>(target->expr);
    });
}

/// Require exactly the configured canonical CHECK expressions.
/// @tparam p_check_count Number of required expressions.
/// @param relation Open catalog relation held by the caller.
/// @param relation_name Unqualified relation name used for expression analysis.
/// @param expected_checks Canonical SQL expressions, independent of catalog order.
/// @throws PostgreSQL ERROR when the set of analyzed expressions differs.
template <std::size_t p_check_count>
inline void
validate_check_expressions(Relation relation,
                           const std::string_view relation_name,
                           const std::array<std::string_view, p_check_count>& expected_checks) {
    const TupleConstr* constraints = RelationGetDescr(relation)->constr;
    if (constraints == nullptr || constraints->num_check != expected_checks.size()) {
        contract_error(relation_name, "exact CHECK expression count");
    }
    std::array<Node*, p_check_count> expected_nodes{};
    std::array<bool, p_check_count> matched{};
    for (std::size_t expected_index = 0; expected_index < expected_checks.size();
         ++expected_index) {
        expected_nodes[expected_index] =
            analyze_expected_check_expression(relation_name, expected_checks[expected_index]);
    }
    for (std::uint16_t actual_index = 0; actual_index < constraints->num_check; ++actual_index) {
        const ConstrCheck& check = constraints->check[actual_index];
        Node* actual = static_cast<Node*>(stringToNode(check.ccbin));
        bool found = false;
        for (std::size_t expected_index = 0; expected_index < expected_nodes.size();
             ++expected_index) {
            if (!matched[expected_index] && equal(actual, expected_nodes[expected_index])) {
                matched[expected_index] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            contract_error(relation_name, "CHECK expression differs from the frozen contract");
        }
    }
    if (std::find(matched.begin(), matched.end(), false) != matched.end()) {
        contract_error(relation_name, "required CHECK expression is missing");
    }
}

/// Match a partial-index predicate against one exact state constant.
/// @param index Open index relation held by the caller.
/// @param expected_state Required `state` value in the predicate.
/// @return Whether the predicate is the canonical integer equality.
[[nodiscard]] inline bool exact_state_predicate(Relation index, const std::int16_t expected_state) {
    List* predicate = bioseq::postgres::support::invoke_pg_as_cpp(
        [index] { return RelationGetIndexPredicate(index); });
    if (predicate == NIL || list_length(predicate) != 1)
        return false;
    const Node* node = static_cast<const Node*>(linitial(predicate));
    if (node == nullptr || !IsA(node, OpExpr))
        return false;
    const auto* equality = reinterpret_cast<const OpExpr*>(node);
    if (equality->opresulttype != BOOLOID || list_length(equality->args) != 2 ||
        bioseq::postgres::support::invoke_pg_as_cpp(
            [equality] { return get_opcode(equality->opno); }) != F_INT2EQ) {
        return false;
    }
    const Node* left = static_cast<const Node*>(linitial(equality->args));
    const Node* right = static_cast<const Node*>(lsecond(equality->args));
    if (left == nullptr || right == nullptr || !IsA(left, Var) || !IsA(right, Const))
        return false;
    const auto* variable = reinterpret_cast<const Var*>(left);
    const auto* constant = reinterpret_cast<const Const*>(right);
    return variable->varattno == minimizer_generation_storage_codec::attribute_number<
                                     &minimizer_generation_storage_catalog_row::state>() &&
           variable->vartype == INT2OID && variable->varlevelsup == 0 &&
           constant->consttype == INT2OID && !constant->constisnull &&
           DatumGetInt16(constant->constvalue) == expected_state;
}

/// Validate one authoritative index identity, key and predicate contract.
/// @tparam p_key_count Number of fixed index key attributes.
/// @param index Open index relation held with `AccessShareLock` by the caller.
/// @param expected_table_oid Required base-table OID.
/// @param expected_name Required index relation name.
/// @param expected_primary Required primary-index flag.
/// @param expected_unique Required uniqueness flag.
/// @param keys Required attribute numbers in key order.
/// @param key_types PostgreSQL types used to derive canonical operator classes.
/// @param expected_state Required partial-predicate state, or no predicate.
/// @return Whether identity, flags, keys, operator classes, and predicate match.
template <std::size_t p_key_count>
[[nodiscard]] inline bool exact_index(Relation index,
                                      const Oid expected_table_oid,
                                      const std::string_view expected_name,
                                      const bool expected_primary,
                                      const bool expected_unique,
                                      const std::array<AttrNumber, p_key_count>& keys,
                                      const std::array<Oid, p_key_count>& key_types,
                                      const std::optional<std::int16_t> expected_state) {
    const Form_pg_index shape = index == nullptr ? nullptr : index->rd_index;
    if (shape == nullptr || RelationGetRelid(index) == InvalidOid ||
        std::string_view{RelationGetRelationName(index)} != expected_name ||
        shape->indrelid != expected_table_oid || index->rd_rel == nullptr ||
        index->rd_rel->relkind != RELKIND_INDEX || index->rd_rel->relam != BTREE_AM_OID ||
        index->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED ||
        shape->indisprimary != expected_primary || shape->indisunique != expected_unique ||
        !shape->indimmediate || !shape->indisvalid || !shape->indisready || !shape->indislive ||
        shape->indisexclusion || shape->indisclustered || shape->indisreplident ||
        shape->indnullsnotdistinct || shape->indnkeyatts != static_cast<int>(keys.size()) ||
        shape->indnatts != static_cast<int>(keys.size()) ||
        bioseq::postgres::support::invoke_pg_as_cpp(
            [index] { return RelationGetIndexExpressions(index); }) != NIL) {
        return false;
    }
    const List* predicate = bioseq::postgres::support::invoke_pg_as_cpp(
        [index] { return RelationGetIndexPredicate(index); });
    if ((!expected_state.has_value() && predicate != NIL) ||
        (expected_state.has_value() && !exact_state_predicate(index, *expected_state))) {
        return false;
    }
    for (std::size_t key = 0; key < keys.size(); ++key) {
        const Oid expected_opclass = bioseq::postgres::support::invoke_pg_as_cpp(
            [&key_types, key] { return GetDefaultOpClass(key_types[key], BTREE_AM_OID); });
        if (!OidIsValid(expected_opclass) || shape->indkey.values[key] != keys[key] ||
            get_index_column_opclass(RelationGetRelid(index), static_cast<int>(key + 1)) !=
                expected_opclass ||
            index->rd_indcollation == nullptr ||
            index->rd_indcollation[key] != get_typcollation(key_types[key]) ||
            index->rd_indoption == nullptr || index->rd_indoption[key] != 0 ||
            (index->rd_opcoptions != nullptr && index->rd_opcoptions[key] != nullptr)) {
            return false;
        }
    }
    return true;
}

/// Validate every authoritative generation-storage index.
/// @param storage Open descriptor catalog held with `AccessShareLock`.
/// @throws PostgreSQL ERROR unless exactly the three canonical indexes exist.
inline void validate_storage_indexes(Relation storage) {
    using row = minimizer_generation_storage_catalog_row;
    using codec = minimizer_generation_storage_codec;
    static constexpr std::array<AttrNumber, 8> primary_keys{
        codec::attribute_number<&row::database_oid>(),
        codec::attribute_number<&row::tablespace_oid>(),
        codec::attribute_number<&row::main_relfilenode>(),
        codec::attribute_number<&row::user_relid>(),
        codec::attribute_number<&row::column_attno>(),
        codec::attribute_number<&row::index_name>(),
        codec::attribute_number<&row::query_key>(),
        codec::attribute_number<&row::generation_id>()};
    static constexpr std::array<Oid, 8> primary_types{
        codec::field<&row::database_oid>::type_oid,
        codec::field<&row::tablespace_oid>::type_oid,
        codec::field<&row::main_relfilenode>::type_oid,
        codec::field<&row::user_relid>::type_oid,
        codec::field<&row::column_attno>::type_oid,
        codec::field<&row::index_name>::type_oid,
        codec::field<&row::query_key>::type_oid,
        codec::field<&row::generation_id>::type_oid};
    static constexpr std::array<AttrNumber, 7> live_keys{
        codec::attribute_number<&row::database_oid>(),
        codec::attribute_number<&row::tablespace_oid>(),
        codec::attribute_number<&row::main_relfilenode>(),
        codec::attribute_number<&row::user_relid>(),
        codec::attribute_number<&row::column_attno>(),
        codec::attribute_number<&row::index_name>(),
        codec::attribute_number<&row::query_key>()};
    static constexpr std::array<Oid, 7> live_types{codec::field<&row::database_oid>::type_oid,
                                                   codec::field<&row::tablespace_oid>::type_oid,
                                                   codec::field<&row::main_relfilenode>::type_oid,
                                                   codec::field<&row::user_relid>::type_oid,
                                                   codec::field<&row::column_attno>::type_oid,
                                                   codec::field<&row::index_name>::type_oid,
                                                   codec::field<&row::query_key>::type_oid};
    static constexpr std::array<AttrNumber, 9> retired_keys{
        codec::attribute_number<&row::retire_full_xid>(),
        codec::attribute_number<&row::database_oid>(),
        codec::attribute_number<&row::tablespace_oid>(),
        codec::attribute_number<&row::main_relfilenode>(),
        codec::attribute_number<&row::user_relid>(),
        codec::attribute_number<&row::column_attno>(),
        codec::attribute_number<&row::index_name>(),
        codec::attribute_number<&row::query_key>(),
        codec::attribute_number<&row::generation_id>()};
    static constexpr std::array<Oid, 9> retired_types{
        codec::field<&row::retire_full_xid>::type_oid,
        codec::field<&row::database_oid>::type_oid,
        codec::field<&row::tablespace_oid>::type_oid,
        codec::field<&row::main_relfilenode>::type_oid,
        codec::field<&row::user_relid>::type_oid,
        codec::field<&row::column_attno>::type_oid,
        codec::field<&row::index_name>::type_oid,
        codec::field<&row::query_key>::type_oid,
        codec::field<&row::generation_id>::type_oid};

    List* indexes = bioseq::postgres::support::invoke_pg_as_cpp(
        [storage] { return RelationGetIndexList(storage); });
    std::size_t exact_count = 0;
    ListCell* cell = nullptr;
    foreach (cell, indexes) {
        const Oid index_oid = lfirst_oid(cell);
        Relation index = bioseq::postgres::support::invoke_pg_as_cpp(
            [index_oid] { return index_open(index_oid, AccessShareLock); });
        const std::string_view name{RelationGetRelationName(index)};
        const bool exact =
            (name == "minimizer_generation_storage_pkey" && exact_index(index,
                                                                        RelationGetRelid(storage),
                                                                        name,
                                                                        true,
                                                                        true,
                                                                        primary_keys,
                                                                        primary_types,
                                                                        std::nullopt)) ||
            (name == "minimizer_generation_storage_one_live" &&
             exact_index(index,
                         RelationGetRelid(storage),
                         name,
                         false,
                         true,
                         live_keys,
                         live_types,
                         std::optional<std::int16_t>{1})) ||
            (name == "minimizer_generation_storage_retired" &&
             exact_index(index,
                         RelationGetRelid(storage),
                         name,
                         false,
                         false,
                         retired_keys,
                         retired_types,
                         std::optional<std::int16_t>{2}));
        bioseq::postgres::support::invoke_pg_as_cpp(
            [index] { index_close(index, AccessShareLock); });
        if (!exact) {
            contract_error("minimizer_generation_storage",
                           "primary/one-LIVE/retired index contract");
        }
        ++exact_count;
    }
    bioseq::postgres::support::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (exact_count != 3) {
        contract_error("minimizer_generation_storage", "exactly three authoritative indexes");
    }
}

/// Validate every authoritative generation-reaper index.
/// @param reaper Open reaper-state catalog held with `AccessShareLock`.
/// @throws PostgreSQL ERROR unless the canonical primary index is the only index.
inline void validate_reaper_indexes(Relation reaper) {
    using row = minimizer_generation_reaper_catalog_row;
    using codec = minimizer_generation_reaper_codec;
    static constexpr std::array<AttrNumber, 1> primary_keys{
        codec::attribute_number<&row::database_oid>()};
    static constexpr std::array<Oid, 1> primary_types{codec::field<&row::database_oid>::type_oid};
    List* indexes = bioseq::postgres::support::invoke_pg_as_cpp(
        [reaper] { return RelationGetIndexList(reaper); });
    if (list_length(indexes) != 1) {
        contract_error("minimizer_generation_reaper_state", "exactly one primary index");
    }
    const Oid index_oid = lfirst_oid(list_head(indexes));
    Relation index = bioseq::postgres::support::invoke_pg_as_cpp(
        [index_oid] { return index_open(index_oid, AccessShareLock); });
    const bool exact = exact_index(index,
                                   RelationGetRelid(reaper),
                                   "minimizer_generation_reaper_state_pkey",
                                   true,
                                   true,
                                   primary_keys,
                                   primary_types,
                                   std::nullopt);
    bioseq::postgres::support::invoke_pg_as_cpp([index] { index_close(index, AccessShareLock); });
    bioseq::postgres::support::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (!exact) {
        contract_error("minimizer_generation_reaper_state", "primary index order/opclass/validity");
    }
}

} // namespace minimizer_generation_catalog_detail

/// Fail closed when either installation-local catalog authority drifts. This
/// is intentionally a cold publication/open guard, not a per-anchor check.
/// @throws PostgreSQL ERROR when either catalog, constraint set, or index set
/// differs from the compiled schema contract.
///
/// Both catalog relations and each inspected index are held with
/// `AccessShareLock` only for the duration of validation.
inline void validate_minimizer_generation_storage_catalogs_pg() {
    using namespace minimizer_generation_catalog_detail;
    const Oid namespace_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return get_namespace_oid("bioseq_internal", true); });
    if (!OidIsValid(namespace_oid)) {
        contract_error("bioseq_internal", "internal schema is missing");
    }
    const Oid storage_oid = bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
        return get_relname_relid("minimizer_generation_storage", namespace_oid);
    });
    const Oid reaper_oid = bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
        return get_relname_relid("minimizer_generation_reaper_state", namespace_oid);
    });
    if (!OidIsValid(storage_oid) || !OidIsValid(reaper_oid)) {
        contract_error("bioseq_internal", "generation descriptor or reaper catalog is missing");
    }
    Relation storage = bioseq::postgres::support::invoke_pg_as_cpp(
        [storage_oid] { return relation_open(storage_oid, AccessShareLock); });
    Relation reaper = bioseq::postgres::support::invoke_pg_as_cpp(
        [reaper_oid] { return relation_open(reaper_oid, AccessShareLock); });
    validate_relation_shape(storage,
                            storage_oid,
                            "minimizer_generation_storage",
                            minimizer_generation_storage_codec::expected_descriptor,
                            k_storage_defaults,
                            13);
    validate_check_expressions(
        storage, "minimizer_generation_storage", k_storage_check_expressions);
    validate_storage_indexes(storage);
    validate_relation_shape(reaper,
                            reaper_oid,
                            "minimizer_generation_reaper_state",
                            minimizer_generation_reaper_codec::expected_descriptor,
                            k_reaper_defaults,
                            4);
    validate_check_expressions(
        reaper, "minimizer_generation_reaper_state", k_reaper_check_expressions);
    validate_reaper_indexes(reaper);
    bioseq::postgres::support::invoke_pg_as_cpp(
        [reaper] { relation_close(reaper, AccessShareLock); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [storage] { relation_close(storage, AccessShareLock); });
}

} // namespace bioseq::storage
