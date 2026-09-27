/**
 * @file admin/inspection_policy.cppm
 * @brief Inspect stored minimizer indexes, remove index tables, and select read policies.
 */
module;
#include <fmt/format.h>
#include <bioseq/pg_compat.h>

#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "commands/tablespace.h"
#include "executor/tuptable.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

extern "C" {
#include "storage/buffile.h"
}
export module bioseq.postgres.storage.indexing.admin:inspection_policy;
import std;
import bioseq.pipeline.runtime_contract;
import :inspection_storage_support;
import :grouped_soa;
import :hash_major;
import :rebuild_api;
import bioseq.postgres.storage.indexing.admin.diagnostic_support;
import bioseq.postgres.support;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.indexing.admin.index_sketch_backend_policy;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.postgres.types.compact_sequence;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata; // column_table_row_count
import bioseq.postgres.table.io;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

/// Inspect a READY grouped-SoA generation under one registered snapshot.
/// Check its source and directory against the current k/w settings and storage
/// format before returning metadata. This inspection reads no index payload extents.
[[nodiscard]] ready_minimizer_generation_inspection inspect_ready_minimizer_generation_impl(
    Oid rel_oid, std::string_view index_name, std::uint64_t sequence_rowno) {
    const bool is_bioseq_table =
        OidIsValid(rel_oid) && bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return relation_oid_uses_column_table_access_method(rel_oid);
        });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq minimizer generation inspection requires a USING bioseq table");
    }
    const Oid user_id = GetUserId();
    if (!bioseq::postgres::support::invoke_pg_as_cpp([rel_oid, user_id] {
            return object_ownercheck(RelationRelationId, rel_oid, user_id);
        })) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(rel_oid)); });
    }
    if (index_name != k_minimizers_per_fragment_index_name &&
        index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection requires a minimizer index",
            fmt::format("index_name={}", index_name));
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "bioseq minimizer generation inspection query key exceeds BIGINT");
    }
    if (bioseq::aligner::settings::minimizer::kmer_size <= 0 ||
        bioseq::aligner::settings::minimizer::kmer_size >
            static_cast<int>((std::numeric_limits<std::uint16_t>::max)()) ||
        bioseq::aligner::settings::minimizer::window_size <= 0 ||
        bioseq::aligner::settings::minimizer::window_size >
            static_cast<int>((std::numeric_limits<std::uint16_t>::max)())) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection has invalid active k/w parameters");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.tablespace_oid) ||
        !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq minimizer generation inspection source relation is unavailable");
    }
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq minimizer generation inspection requires an active MVCC snapshot");
    }

    struct inspection_state {
        Relation relation = nullptr;
        column_read_state* read_state = nullptr;
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new inspection_state{};
    ready_minimizer_generation_inspection result{};
    const MemoryContext caller_context = CurrentMemoryContext;
    try {
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (state->registered_snapshot == nullptr || state->registered_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "bioseq minimizer generation inspection could not register its snapshot");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([caller_context] {
            return AllocSetContextCreate(
                caller_context, "bioseq ready generation SQL inspection", ALLOCSET_DEFAULT_SIZES);
        });
        state->relation = bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
        });
        if (state->relation == nullptr || state->relation->rd_locator.dbOid != MyDatabaseId ||
            static_cast<Oid>(state->relation->rd_locator.relNumber) !=
                relation_identity.relfilenode) {
            ready_minimizer_error_as_cpp(
                ERRCODE_UNDEFINED_TABLE,
                "bioseq minimizer generation inspection source relation changed");
        }

        TupleDesc source_desc = RelationGetDescr(state->relation);
        MemoryContext previous_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return MemoryContextSwitchTo(state->work_context); });
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [seq_attno] { return list_make1_int(seq_attno); });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<Datum*>(
                palloc0(sizeof(Datum) * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<bool*>(
                palloc0(sizeof(bool) * static_cast<std::size_t>(source_desc->natts)));
        });
        state->read_state =
            bioseq::postgres::support::invoke_pg_as_cpp([state, source_desc, projected] {
                return begin_column_read(state->relation,
                                         source_desc,
                                         projected,
                                         NIL,
                                         state->work_context,
                                         state->registered_snapshot,
                                         /*randomAccess=*/true,
                                         nullptr);
            });
        const bool row_visible =
            bioseq::postgres::support::invoke_pg_as_cpp([state, sequence_rowno, values, nulls] {
                return read_column_row_by_number(state->read_state, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[seq_attno - 1]) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] { MemoryContextSwitchTo(previous_context); });
            ready_minimizer_error_as_cpp(
                ERRCODE_NO_DATA_FOUND,
                "bioseq minimizer generation inspection source row is not visible",
                fmt::format("sequence_rowno={}", sequence_rowno));
        }
        bioseq::pg_types::seq* header =
            bioseq::postgres::support::invoke_pg_as_cpp([values, seq_attno] {
                return reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM(values[seq_attno - 1]));
            });
        if (!is_locator_stub(header)) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] { MemoryContextSwitchTo(previous_context); });
            ready_minimizer_error_as_cpp(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                         "bioseq minimizer generation source has no source epoch");
        }
        const sequence_locator locator = decode_locator_from_stub(header);
        if (locator.source_epoch == 0) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] { MemoryContextSwitchTo(previous_context); });
            ready_minimizer_error_as_cpp(ERRCODE_DATA_CORRUPTED,
                                         "bioseq minimizer generation source epoch is invalid");
        }
        result.source = minimizer_generation_source_locator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = static_cast<std::uint8_t>(header->alphabet_code),
            .storage_unit_byte_width = logical_storage_unit_byte_width(header),
            .padding_letters = header->padding_letters,
            .storage_contract_checksum = 0,
        };
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->read_state); });
        state->read_state = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [previous_context] { MemoryContextSwitchTo(previous_context); });
        Relation finished_relation = std::exchange(state->relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_relation] { relation_close(finished_relation, AccessShareLock); });

        const std::string table_name = format_seqidx_name(index_name,
                                                          relation_identity.db_oid,
                                                          relation_identity.tablespace_oid,
                                                          relation_identity.relfilenode,
                                                          seq_attno,
                                                          result.source.locator.bucket_index,
                                                          result.source.locator.shard_index);
        const minimizer_generation_administration_identity admin_identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = rel_oid,
            .column_attno = seq_attno,
            .index_name = std::string{index_name},
            .table_name = table_name,
        };

        minimizer_generation_head preliminary_head{};
        std::uint64_t expected_source_identity = 0;
        const auto requested_algorithm =
            static_cast<bioseq::simd::minimizer_algorithm>(
                static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()));
        const auto algorithm_identity =
            bioseq::indexing::select_minimizer_generation_algorithm_identity_runtime(
                requested_algorithm,
                index_name == bioseq::indexing::k_minimizers_per_fragment_index_name
                    ? bioseq::indexing::k_minimizer_fragment_algorithm_position_extent_bp
                    : result.source.locator.logical_length,
                result.source.alphabet_code,
                static_cast<std::uint8_t>(result.source.storage_unit_byte_width * 8u),
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size),
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size));
        if (!algorithm_identity) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation algorithm identity is not READY");
        }
        const auto backend =
            resolve_index_sketch_backend(algorithm_identity->effective_algorithm);
        const auto discovery_expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
                backend,
                *algorithm_identity,
                result.source.alphabet_code,
                result.source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size),
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size),
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!discovery_expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation discovery storage contract is invalid");
        }
        {
            index_storage_fragment_generation_read_session discovery{
                table_name,
                sequence_rowno,
                state->registered_snapshot,
                *discovery_expected,
                relation_identity.tablespace_oid,
                minimizer_generation_snapshot_registration::borrow_registered};
            preliminary_head = discovery.discover_active_head_layout_and_close();
        }
        if (preliminary_head.layout !=
            bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local) {
            ready_minimizer_error_as_cpp(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                         "bioseq minimizer generation wire layout is unsupported");
        }
        const auto expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                preliminary_head.layout,
                backend,
                *algorithm_identity,
                result.source.alphabet_code,
                result.source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size),
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size),
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation expected storage contract is invalid");
        }
        {
            index_storage_fragment_generation_read_session preliminary{
                table_name,
                sequence_rowno,
                state->registered_snapshot,
                *expected,
                relation_identity.tablespace_oid,
                minimizer_generation_snapshot_registration::borrow_registered};
            const minimizer_generation_head exact_head = preliminary.read_active_head_once();
            if (exact_head != preliminary_head) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation head changed during layout binding");
            }
            const minimizer_generation_read_metadata& metadata =
                preliminary.read_manifest_directory_once(exact_head);
            result.manifest = metadata.manifest;
            result.source.storage_contract_checksum =
                bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    result.manifest);
            expected_source_identity =
                minimizer_generation_source_identity(relation_identity.relfilenode, result.source);
            preliminary.close_metadata_read();
        }
        if (expected_source_identity == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation inspection source identity is invalid");
        }
        if (result.manifest.kmer_size !=
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size) ||
            result.manifest.window_size !=
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size) ||
            result.manifest.alphabet_code != result.source.alphabet_code ||
            result.manifest.storage_unit_byte_width != result.source.storage_unit_byte_width ||
            result.manifest.target_length != result.source.locator.logical_length ||
            result.manifest.gpu_abi_revision != expected->gpu_abi_revision ||
            result.manifest.segment_geometry_revision !=
                bioseq::indexing::k_minimizer_segment_geometry_revision ||
            result.source.storage_contract_checksum !=
                bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    result.manifest)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation does not match the active session storage contract",
                fmt::format("manifest k={} w={} active k={} w={}",
                            static_cast<unsigned>(result.manifest.kmer_size),
                            static_cast<unsigned>(result.manifest.window_size),
                            bioseq::aligner::settings::minimizer::kmer_size,
                            bioseq::aligner::settings::minimizer::window_size));
        }
        result.identity = inspect_ready_minimizer_generation_identity_pg(
            admin_identity, sequence_rowno, state->registered_snapshot, *expected);
        if (result.identity.generation_id != preliminary_head.generation_id ||
            result.identity.source_identity != preliminary_head.source_identity ||
            result.identity.storage_contract_checksum != result.source.storage_contract_checksum) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation identity changed during inspection");
        }
        result.source_content_checksum = result.identity.source_content_checksum;
        result.database_oid = relation_identity.db_oid;
        result.main_relfilenode = relation_identity.relfilenode;
        result.column_attno = seq_attno;

        Snapshot finished_snapshot = std::exchange(state->registered_snapshot, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_snapshot] { UnregisterSnapshot(finished_snapshot); });
        MemoryContext finished_context = std::exchange(state->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
        delete state;
        state = nullptr;
    } catch (...) {
        MemoryContextSwitchTo(caller_context);
        state->read_state = nullptr;
        state->relation = nullptr;
        state->registered_snapshot = nullptr;
        state->work_context = nullptr;
        delete state;
        state = nullptr;
        throw;
    }
    return result;
}

/// Count persisted rows across every shard of one logical index.
[[nodiscard]] std::int64_t index_table_row_count_impl(Oid rel_oid, std::string_view name) {
    std::int64_t total = 0;
    for (const std::string& pfx : seqidx_prefixes_for(rel_oid, name))
        for (const std::string& sh : shard_relnames_with_prefix(pfx)) {
            const std::int64_t rows = count_in_shard(sh, "");
            if (rows > (std::numeric_limits<std::int64_t>::max)() - total) {
                index_audit_error("persisted row count overflow");
            }
            total += rows;
        }
    return total;
}

/// Count source rows and stored index data for a no-index test. Source row
/// counts come from stripe metadata, so this audit does not read or decode
/// sequence values. The caller checks the returned counts for index absence.
[[nodiscard]] cold_no_index_absence_audit
cold_no_index_absence_audit_impl(Oid rel_oid, std::string_view name) {
    if (name != "minimizers_per_fragment" && name != "minimizers_per_sequence") {
        index_audit_error("audit requires a minimizer fragment or sequence policy");
    }
    Relation target = table_open(rel_oid, AccessShareLock);
    if (!relation_oid_uses_column_table_access_method(rel_oid)) {
        table_close(target, AccessShareLock);
        index_audit_error("target is not a USING bioseq relation");
    }
    cold_no_index_absence_audit audit{
        .target_rows = column_table_row_count(target),
    };
    table_close(target, AccessShareLock);

    for (const std::string& prefix : seqidx_prefixes_for(rel_oid, name)) {
        for (const std::string& shard : shard_relnames_with_prefix(prefix)) {
            ++audit.physical_index_relations;
            const std::int64_t rows = count_in_shard(shard, "");
            if (static_cast<std::uint64_t>(rows) >
                (std::numeric_limits<std::uint64_t>::max)() - audit.persisted_index_rows) {
                index_audit_error("persisted row count overflow");
            }
            audit.persisted_index_rows += static_cast<std::uint64_t>(rows);
            if (shard_has_minimizer_generation_key(shard)) {
                const std::int64_t generations =
                    count_in_shard(shard,
                                   fmt::format("WHERE generation_id=0 AND position_segment=-1 "
                                               "AND sub_key={} AND chunk_no=0",
                                               static_cast<long long>(k_generation_head_sub_key)));
                if (static_cast<std::uint64_t>(generations) >
                    (std::numeric_limits<std::uint64_t>::max)() - audit.generation_objects) {
                    index_audit_error("generation object count overflow");
                }
                audit.generation_objects += static_cast<std::uint64_t>(generations);
            }
        }
    }
    return audit;
}

/// Drop all index tables and translate failures after C++ cleanup has finished.
void drop_all_index_tables_impl() {
    ErrorData* postgres_error = nullptr;
    std::array<char, 2048> cpp_error_detail{};
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    try {
        drop_index_relations(drop_index_relation_selector::all_index_tables());
    } catch (bioseq::postgres::support::postgres_error& error) {
        postgres_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(cpp_error_detail.data(),
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                cpp_error_detail.size());
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }

    // Report the saved error after the SPI session and C++ owners have closed.
    if (postgres_error != nullptr)
        ReThrowError(postgres_error);
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq index relation cleanup failed"),
                 errdetail_internal("%s", cpp_error_detail.data())));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq index relation cleanup failed"),
                 errdetail_internal("unknown C++ exception")));
    }
}

/// Make this index the active read choice for every sequence column.
[[nodiscard]] int set_active_read_index_impl(Oid rel_oid, std::string_view name) {
    if (!name_is_registered(name))
        return 1;
    if (layout_of_name(name) == sequence_index_layout::table_chunks)
        return 2;
    const relation_identity rid = index_relation_identity(rel_oid);
    if (SPI_connect() != SPI_OK_CONNECT)
        return 0;
    std::string clear = fmt::format("UPDATE bioseq_internal.index_policy SET is_active_read=false "
                                    "WHERE user_relid={}::oid AND main_relfilenode={}::oid",
                                    static_cast<unsigned>(rel_oid),
                                    static_cast<unsigned>(rid.relfilenode));
    SPI_execute(clear.c_str(), false, 0);
    std::string set_ =
        fmt::format("INSERT INTO bioseq_internal.index_policy (user_relid, main_relfilenode, "
                    "column_attno, index_name, enabled, is_active_read) "
                    "SELECT {}::oid, {}::oid, a.attnum, {}, true, true FROM pg_attribute a JOIN "
                    "pg_type t ON a.atttypid=t.oid "
                    "JOIN pg_namespace n ON t.typnamespace=n.oid WHERE a.attrelid={}::oid AND "
                    "t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') "
                    "AND a.attnum>0 AND NOT a.attisdropped "
                    "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) DO "
                    "UPDATE SET is_active_read=true, enabled=true",
                    static_cast<unsigned>(rel_oid),
                    static_cast<unsigned>(rid.relfilenode),
                    quote_literal_cstr(std::string(name).c_str()),
                    static_cast<unsigned>(rel_oid));
    SPI_execute(set_.c_str(), false, 0);
    SPI_finish();
    return 0;
}

} // namespace bioseq::storage::indexing
