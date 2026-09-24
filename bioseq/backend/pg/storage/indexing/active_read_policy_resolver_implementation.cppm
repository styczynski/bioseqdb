/**
 * @file active_read_policy_resolver_implementation.cppm
 * @brief Implements executor-scoped active-read policy resolution.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/genam.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "utils/rls.h"
}

module bioseq.postgres.storage.indexing.active_read_policy_resolver;

import bioseq.common.logging;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.support;

namespace bioseq::storage::indexing {

namespace {

using index_policy_codec =
    bioseq::postgres::storage::indexing::sql_row_codecs::index_policy_codec;
using index_policy_row = index_policy_codec::row_type;

[[noreturn]] void
active_read_policy_resolver_error_as_cpp(std::string_view message,
                                         std::string_view detail = {}) {
    bioseq::postgres::support::invoke_pg_as_cpp([message, detail] {
        if (detail.empty()) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq active-read policy: %.*s",
                            static_cast<int>(message.size()),
                            message.data())));
        }
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq active-read policy: %.*s",
                        static_cast<int>(message.size()),
                        message.data()),
                 errdetail("%.*s", static_cast<int>(detail.size()), detail.data())));
    });
    pg_unreachable();
}

} // namespace

/**
 * @brief Resolves the active read index for an executor-owned relation column.
 *
 * The policy heap and primary index are read under `AccessShareLock` with the
 * executor's registered MVCC snapshot. A single enabled active row selects its
 * index, while an authoritative empty result selects the declared default.
 *
 * @param[in] executor_read Borrowed executor relation and snapshot context.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in,out] cold_stats Optional counters updated for catalog and lock work.
 * @return Selected index name and its policy source.
 */
[[nodiscard]] active_read_policy_resolution
resolve_active_read_index_outcome(
    const borrowed_executor_read_context& executor_read,
    std::int16_t attno,
    cold_index_preparation_stats* cold_stats) {
    const Oid user_relid = executor_read.relation_oid();
    const Oid relfn = executor_read.relfilenode();
    Snapshot snapshot = executor_read.snapshot();
    if (!executor_read.valid() || !OidIsValid(user_relid) || !OidIsValid(relfn) || attno <= 0) {
        active_read_policy_resolver_error_as_cpp(
            "relation-backed resolver identity is invalid",
            fmt::format("relation={} relfilenode={} attno={}",
                        static_cast<unsigned>(user_relid),
                        static_cast<unsigned>(relfn),
                        static_cast<int>(attno)));
    }
    if (cold_stats != nullptr) {
        cold_stats->add(cold_stats->target_relation_reuses, 1);
    }
    // This resolver is on every physical alignment batch. Read our heap-AM
    // catalog directly instead of recursively invoking the SQL planner and
    // executor through SPI. Besides removing parse/plan/ExecutorStart latency,
    // the direct catalog scan is safe while the outer Bioseq CustomScan is
    // already inside ExecutorRun and retains the same authoritative relation.
    // Holds the bounded result copied out of the PostgreSQL catalog scan.
    struct policy_scan_result {
        std::uint64_t rows = 0;
        std::array<char, 256> index_name{};
    } scanned{};
    std::array<ScanKeyData, 3> keys{
        index_policy_codec::scan_key<&index_policy_row::user_relid>(user_relid),
        index_policy_codec::scan_key<&index_policy_row::main_relfilenode>(relfn),
        index_policy_codec::scan_key<&index_policy_row::column_attno>(attno),
    };
    bioseq::postgres::support::invoke_pg_as_cpp(
        [user_relid, relfn, attno, snapshot, &scanned, cold_stats, &keys] {
            const Oid namespace_oid = get_namespace_oid("bioseq_internal", false);
            const Oid policy_oid = get_relname_relid("index_policy", namespace_oid);
            if (!OidIsValid(policy_oid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("bioseq active-read policy catalog is missing")));
            }
            // index_policy is trusted extension metadata.  Installation
            // deliberately revokes ordinary users' access to
            // bioseq_internal; the user-facing relation is authorized by the
            // caller before reaching this resolver.  Requiring ACL_USAGE or
            // ACL_SELECT here would make every legitimate non-owner search
            // fail while adding no authorization boundary.
            if (check_enable_rls(policy_oid, InvalidOid, true) == RLS_ENABLED) {
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("row security on bioseq active-read policy is unsupported")));
            }
            if (snapshot == nullptr || !IsMVCCSnapshot(snapshot) || snapshot->regd_count == 0) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq active-read policy requires a registered executor snapshot")));
            }
            Relation relation = table_open(policy_oid, AccessShareLock);
            if (cold_stats != nullptr) {
                cold_stats->add(cold_stats->policy_heap_opens, 1);
                cold_stats->add(cold_stats->lock_acquires, 1);
            }
            TupleDesc tuple_desc = RelationGetDescr(relation);
            constexpr auto& attributes = index_policy_codec::expected_descriptor;
            if (relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
                tuple_desc->natts != static_cast<int>(attributes.size())) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq active-read policy catalog shape is invalid")));
            }
            for (std::size_t i = 0; i < attributes.size(); ++i) {
                Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(tuple_desc, static_cast<int>(i));
                if (attribute->attisdropped || attribute->atttypid != attributes[i].type_oid ||
                    attribute->attnotnull != !attributes[i].nullable ||
                    std::string_view{NameStr(attribute->attname)} != attributes[i].name) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq active-read policy catalog schema is invalid"),
                             errdetail("attribute=%zu", i + 1u)));
                }
            }
            if (cold_stats != nullptr) {
                cold_stats->add(cold_stats->policy_schema_validations, 1);
            }
            const Oid primary_oid = get_relname_relid("index_policy_pkey", namespace_oid);
            if (!OidIsValid(primary_oid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq active-read policy primary index is missing")));
            }
            Relation primary = index_open(primary_oid, AccessShareLock);
            if (cold_stats != nullptr) {
                cold_stats->add(cold_stats->policy_index_opens, 1);
                cold_stats->add(cold_stats->lock_acquires, 1);
            }
            const Form_pg_index primary_form = primary->rd_index;
            const bool primary_valid =
                primary_form != nullptr && primary->rd_rel->relam == BTREE_AM_OID &&
                primary_form->indrelid == policy_oid &&
                bioseq::postgres::support::relation_immediate_primary_key_index(relation) ==
                    primary_oid && primary_form->indisvalid &&
                primary_form->indisready && primary_form->indislive && primary_form->indimmediate &&
                primary_form->indisunique && primary_form->indisprimary &&
                primary_form->indnatts == 4 && primary_form->indnkeyatts == 4 &&
                primary_form->indkey.values[0] == 1 && primary_form->indkey.values[1] == 2 &&
                primary_form->indkey.values[2] == 3 && primary_form->indkey.values[3] == 4 &&
                RelationGetIndexExpressions(primary) == NIL &&
                RelationGetIndexPredicate(primary) == NIL;
            if (!primary_valid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq active-read policy primary index is invalid")));
            }
            SysScanDesc scan = systable_beginscan_ordered(
                relation, primary, snapshot, static_cast<int>(keys.size()), keys.data());
            HeapTuple tuple = nullptr;
            while (HeapTupleIsValid(tuple = systable_getnext_ordered(scan, ForwardScanDirection))) {
                bool user_null = false;
                bool relfn_null = false;
                bool attno_null = false;
                bool name_null = false;
                bool enabled_null = false;
                bool active_null = false;
                const Datum row_user = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<&index_policy_row::user_relid>(),
                    tuple_desc,
                    &user_null);
                const Datum row_relfn = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<&index_policy_row::main_relfilenode>(),
                    tuple_desc,
                    &relfn_null);
                const Datum row_attno = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<&index_policy_row::column_attno>(),
                    tuple_desc,
                    &attno_null);
                const Datum row_name = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<&index_policy_row::index_name>(),
                    tuple_desc,
                    &name_null);
                const Datum row_enabled =
                    heap_getattr(tuple,
                                 index_policy_codec::attribute_number<&index_policy_row::enabled>(),
                                 tuple_desc,
                                 &enabled_null);
                const Datum row_active = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<&index_policy_row::is_active_read>(),
                    tuple_desc,
                    &active_null);
                if (user_null || relfn_null || attno_null || name_null || enabled_null ||
                    active_null) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq active-read policy row contains NULL")));
                }
                if (DatumGetObjectId(row_user) != user_relid ||
                    DatumGetObjectId(row_relfn) != relfn || DatumGetInt16(row_attno) != attno ||
                    !DatumGetBool(row_enabled) || !DatumGetBool(row_active)) {
                    continue;
                }
                ++scanned.rows;
                text* name = DatumGetTextPP(row_name);
                const bool free_name = static_cast<void*>(name) != DatumGetPointer(row_name);
                const std::size_t name_size = static_cast<std::size_t>(VARSIZE_ANY_EXHDR(name));
                if (name_size == 0u || name_size >= scanned.index_name.size()) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq active-read policy index name is invalid")));
                }
                std::memcpy(scanned.index_name.data(), VARDATA_ANY(name), name_size);
                scanned.index_name[name_size] = '\0';
                if (free_name) {
                    pfree(name);
                }
            }
            systable_endscan_ordered(scan);
            index_close(primary, AccessShareLock);
            if (cold_stats != nullptr) {
                cold_stats->add(cold_stats->lock_releases, 1);
            }
            table_close(relation, AccessShareLock);
            if (cold_stats != nullptr) {
                cold_stats->add(cold_stats->lock_releases, 1);
            }
        });
    const std::uint64_t rows = scanned.rows;
    std::optional<std::string> configured_index;
    std::optional<std::string> semantic_error;
    if (rows > 1) {
        semantic_error = fmt::format("policy query returned {} rows", rows);
    } else if (rows == 1) {
        configured_index.emplace(scanned.index_name.data());
    }
    if (semantic_error.has_value()) {
        active_read_policy_resolver_error_as_cpp("policy query result is invalid", *semantic_error);
    }
    if (configured_index.has_value()) {
        ++g_active_read_policy_resolution_counters.configured_rows;
        bioseq_log<logging_level::debug>(
            "[bioseq] active-read policy configured relation={} relfilenode={} attno={} index={}",
            static_cast<unsigned>(user_relid),
            static_cast<unsigned>(relfn),
            static_cast<int>(attno),
            *configured_index);
        return active_read_policy_resolution{
            .index_name = std::move(*configured_index),
            .source = active_read_policy_source::configured_row,
        };
    }
    ++g_active_read_policy_resolution_counters.authoritative_defaults;
    bioseq_log<logging_level::debug>("[bioseq] active-read policy authoritative no-row default "
                                     "relation={} relfilenode={} attno={} index={}",
                                     static_cast<unsigned>(user_relid),
                                     static_cast<unsigned>(relfn),
                                     static_cast<int>(attno),
                                     bioseq::indexing::k_default_active_read_index);
    return active_read_policy_resolution{
        .index_name = std::string{bioseq::indexing::k_default_active_read_index},
        .source = active_read_policy_source::authoritative_no_row_default,
    };
}

} // namespace bioseq::storage::indexing
