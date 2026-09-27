/**
 * @file
 * @brief Reads or recomputes symbol counts and cleans up their temporary readers.
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

export module bioseq.postgres.table:custom_scan_scan_runtime_symbol_count_materialization;

export import :custom_scan_scan_runtime_symbol_count_materialization_contracts;

import std;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.storage.scanner.lazy_binding_contract;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.table.io;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.layout;

import :custom_scan_runtime_placeholder_contracts;

using namespace bioseq::postgres::support;

export {

    /** @brief Tracks temporary readers and memory used to count symbols in one row. */
    struct symbol_count_read_resources {
        MemoryContext caller_context = nullptr;
        MemoryContext scratch = nullptr;
        bioseq::storage::seq_caches* caches = nullptr;
        column_read_state* row_read_state = nullptr;
    };

    /** @brief Closes symbol-count readers before deleting their scratch memory context. */
    void release_symbol_count_read_resources_checked(symbol_count_read_resources* resources) {
        if (resources == nullptr) {
            return;
        }
        if (resources->caller_context != nullptr &&
            CurrentMemoryContext != resources->caller_context) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [resources] { MemoryContextSwitchTo(resources->caller_context); });
        }
        if (resources->row_read_state != nullptr) {
            column_read_state* finished = resources->row_read_state;
            bioseq::postgres::support::invoke_pg_as_cpp([finished] { end_column_read(finished); });
            resources->row_read_state = nullptr;
        }
        if (resources->caches != nullptr) {
            bioseq::storage::seq_caches* finished = resources->caches;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] { finished->close_all(/*commit=*/false); });
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] { bioseq::storage::destroy_seq_caches(finished); });
            resources->caches = nullptr;
        }
        if (resources->scratch != nullptr) {
            MemoryContext finished = resources->scratch;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] { MemoryContextDelete(finished); });
            resources->scratch = nullptr;
        }
    }

    /** @brief Drops C++ cache contents and leaves PostgreSQL resources for error cleanup. */
    void abandon_symbol_count_read_resources_after_error(
        symbol_count_read_resources* resources) noexcept {
        if (resources == nullptr) {
            return;
        }
        // Leave readers, relations and PostgreSQL memory to transaction cleanup.
        // Only release C++ containers here, so cleanup cannot replace the original error.
        resources->row_read_state = nullptr;
        if (resources->caches != nullptr) {
            bioseq::storage::seq_caches* abandoned = std::exchange(resources->caches, nullptr);
            for (auto& [key, entry] : abandoned->entries) {
                (void)key;
                entry.rel = nullptr;
                entry.write_state = nullptr;
                entry.read_state = nullptr;
                entry.read_snapshot_source = nullptr;
                entry.pending_bytes_delta = 0;
                entry.pending_total_payload_delta = 0;
            }
            abandoned->entries.clear();
            abandoned->~seq_caches();
        }
        resources->scratch = nullptr;
    }

    /** @brief Raises a PostgreSQL error that the surrounding C++ cleanup code can catch. */
    [[noreturn]] void symbol_count_error_as_cpp(int sqlstate, std::string_view message) {
        bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message] {
            ereport(ERROR,
                    (errcode(sqlstate),
                     errmsg("%.*s", static_cast<int>(message.size()), message.data())));
        });
        pg_unreachable();
    }

    /**
     * @brief Fills symbol_count results for one stored row.
     *
     * Uses saved counts when index reads are enabled and the sequence has a locator.
     * Otherwise counts symbols from the sequence. Missing rows and NULL sequences
     * produce NULL placeholders.
     * @param registered_snapshot Registered MVCC snapshot for all storage reads.
     */
    void populate_symbol_count_placeholders(placeholder_constants& consts,
                                            Relation main,
                                            int64_t rowno,
                                            Snapshot registered_snapshot,
                                            const bool read_sequence_index) {
        if (consts.symbol_count_sites.empty()) {
            return;
        }
        symbol_count_read_resources* resources = nullptr;
        ErrorData* exact_error = nullptr;
        bool cpp_error = false;
        bool unknown_cpp_error = false;
        std::array<char, 512> detail{"std::exception without detail"};
        try {
            if (registered_snapshot == nullptr || !IsMVCCSnapshot(registered_snapshot) ||
                registered_snapshot->regd_count == 0) {
                symbol_count_error_as_cpp(
                    ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                    "bioseq symbol-count read requires a registered MVCC executor snapshot");
            }
            // Marks every installed placeholder null when no physical row can be read.
            auto set_all_null = [&consts]() {
                for (auto& s : consts.symbol_count_sites)
                    if (s.placeholder != nullptr)
                        s.placeholder->constisnull = true;
            };
            if (rowno < 0 || main == nullptr) {
                set_all_null();
                return;
            }

            namespace bsi = bioseq::storage::indexing;
            const Oid relid = RelationGetRelid(main);
            const auto rid = bsi::index_relation_identity(relid);
            TupleDesc td = RelationGetDescr(main);

            /* Keep resources in one record so success can close them in order
             * and error handling can preserve the original PostgreSQL error. */
            resources = bioseq::postgres::support::invoke_pg_as_cpp([] {
                return static_cast<symbol_count_read_resources*>(
                    palloc0(sizeof(symbol_count_read_resources)));
            });
            resources->caller_context = CurrentMemoryContext;
            resources->scratch = bioseq::postgres::support::invoke_pg_as_cpp([] {
                return AllocSetContextCreate(
                    CurrentMemoryContext, "bioseq_symcount", ALLOCSET_SMALL_SIZES);
            });
            resources->caches = bioseq::postgres::support::invoke_pg_as_cpp(
                [resources] { return bioseq::storage::alloc_seq_caches(resources->scratch); });
            resources->caches->side = bioseq::storage::seq_caches::access_side::read;
            resources->caches->lock_mode = AccessShareLock;
            resources->caches->read_state_mctx = resources->scratch;
            resources->caches->main_database_oid = rid.db_oid;
            resources->caches->main_tablespace_oid = rid.tablespace_oid;
            resources->caches->main_relfilenode = rid.relfilenode;

            auto* vals = bioseq::postgres::support::invoke_pg_as_cpp([resources, td] {
                return static_cast<Datum*>(MemoryContextAllocZero(
                    resources->scratch, sizeof(Datum) * static_cast<std::size_t>(td->natts)));
            });
            auto* nulls = bioseq::postgres::support::invoke_pg_as_cpp([resources, td] {
                return static_cast<bool*>(MemoryContextAllocZero(
                    resources->scratch, sizeof(bool) * static_cast<std::size_t>(td->natts)));
            });
            /* Read each requested column once. Keep the column list and reader
             * state in the scratch context. */
            MemoryContext previous_context = bioseq::postgres::support::invoke_pg_as_cpp(
                [resources] { return MemoryContextSwitchTo(resources->scratch); });
            List* projected = bioseq::postgres::support::invoke_pg_as_cpp([&consts] {
                List* result = NIL;
                for (auto& site : consts.symbol_count_sites) {
                    if (site.placeholder != nullptr &&
                        !list_member_int(result, site.column_attno)) {
                        result = lappend_int(result, site.column_attno);
                    }
                }
                return result;
            });
            resources->row_read_state = bioseq::postgres::support::invoke_pg_as_cpp(
                [main, td, projected, resources, registered_snapshot] {
                    return begin_column_read(main,
                                             td,
                                             projected,
                                             NIL,
                                             resources->scratch,
                                             registered_snapshot,
                                             /*randomAccess=*/true,
                                             nullptr);
                });
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] { MemoryContextSwitchTo(previous_context); });
            const bool row_ok =
                bioseq::postgres::support::invoke_pg_as_cpp([resources, rowno, &vals, &nulls] {
                    return read_column_row_by_number(
                        resources->row_read_state, static_cast<uint64_t>(rowno), vals, nulls);
                });
            /* The row reader owns the values in vals. Keep it open until all
             * symbol-count calls have consumed their sequence values. */

            for (auto& site : consts.symbol_count_sites) {
                if (site.placeholder == nullptr)
                    continue;
                if (!row_ok || site.rank < 0 || nulls[site.column_attno - 1]) {
                    site.placeholder->constisnull = true;
                    continue;
                }
                MemoryContext old = bioseq::postgres::support::invoke_pg_as_cpp(
                    [resources] { return MemoryContextSwitchTo(resources->scratch); });
                bioseq::pg_types::seq* hdr = bioseq::postgres::support::invoke_pg_as_cpp(
                    [vals, column_attno = site.column_attno] {
                        return reinterpret_cast<bioseq::pg_types::seq*>(
                            PG_DETOAST_DATUM(vals[column_attno - 1]));
                    });
                bioseq::postgres::support::invoke_pg_as_cpp([old] { MemoryContextSwitchTo(old); });
                const std::uint8_t alpha = hdr->alphabet_code;
                const bool stub = bioseq::storage::is_locator_stub(hdr);
                const std::uint8_t bw = stub ? bioseq::storage::logical_storage_unit_byte_width(hdr)
                                             : hdr->storage_unit_byte_width;
                bioseq::storage::sequence_locator loc{};
                if (stub)
                    loc = bioseq::storage::decode_locator_from_stub(hdr);

                /* The index-read setting controls only this stored-count lookup. */
                std::optional<std::int64_t> stored;
                if (read_sequence_index && stub) {
                    auto store =
                        bsi::open_sequence_storage(relid,
                                                   rid.db_oid,
                                                   rid.tablespace_oid,
                                                   rid.relfilenode,
                                                   site.column_attno,
                                                   bioseq::indexing::symbol_counts_index<
                                                       bsi::sequence_index_storage>::index_name,
                                                   loc,
                                                   static_cast<uint64_t>(rowno),
                                                   resources->caches,
                                                   resources->scratch);
                    try {
                        stored = bioseq::indexing::symbol_counts_index<
                            bsi::sequence_index_storage>::read_count(store,
                                                                     static_cast<std::size_t>(
                                                                         site.rank));
                        store.close_checked();
                    } catch (...) {
                        store.abandon_after_pg_error_noexcept();
                        throw;
                    }
                }
                if (stored.has_value()) {
                    site.placeholder->constvalue = bioseq::postgres::support::pg_datum::from(
                                                       static_cast<std::int64_t>(*stored))
                                                       .raw();
                    site.placeholder->constisnull = false;
                    continue;
                }

                /* Recompute when no stored count is available, even with index reads disabled. */
                std::int64_t cnt = 0;
                bioseq::pg_types::seq::dispatch_instance(
                    alpha,
                    bw,
                    [stub,
                     rid,
                     column_attno = site.column_attno,
                     rank = site.rank,
                     loc,
                     registered_snapshot,
                     caches = resources->caches,
                     scratch = resources->scratch,
                     &cnt,
                     hdr]<class p_sequence>() {
                        if (stub) {
                            using source_binding_type =
                                bioseq::storage::postgres_lazy_sequence_binding<p_sequence>;
                            const source_binding_type source_binding{
                                rid.db_oid,
                                rid.tablespace_oid,
                                rid.relfilenode,
                                column_attno,
                                loc,
                                source_binding_type::expected_last_fragment_padding(loc),
                                registered_snapshot,
                                caches,
                                scratch};
                            bioseq::storage::with_postgres_lazy_sequence<p_sequence>(
                                source_binding, [rank, &cnt](auto lazy_view) {
                                    auto counts = bioseq::indexing::tally_symbol_counts(lazy_view);
                                    if (static_cast<std::size_t>(rank) < counts.size())
                                        cnt = counts[rank];
                                });
                        } else {
                            compact_sequence_range<p_sequence> range{*hdr};
                            auto lazy = bioseq::storage::make_resident_lazy_sequence(range);
                            const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
                            auto counts = bioseq::indexing::tally_symbol_counts(lazy_view);
                            if (static_cast<std::size_t>(rank) < counts.size())
                                cnt = counts[rank];
                        }
                    });
                site.placeholder->constvalue = bioseq::postgres::support::pg_datum::from(cnt).raw();
                site.placeholder->constisnull = false;
            }
            release_symbol_count_read_resources_checked(resources);
            bioseq::postgres::support::invoke_pg_as_cpp([resources] { pfree(resources); });
            resources = nullptr;
        } catch (bioseq::postgres::support::postgres_error& error) {
            exact_error = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(detail.data(),
                    error.what() == nullptr ? "std::exception without detail" : error.what(),
                    detail.size());
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }
        if (exact_error != nullptr || cpp_error || unknown_cpp_error) {
            abandon_symbol_count_read_resources_after_error(resources);
        }
        if (exact_error != nullptr) {
            ReThrowError(exact_error);
        }
        if (cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq symbol-count read raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        if (unknown_cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq symbol-count read raised an unknown C++ exception")));
        }
    }

} // export custom-scan partition declarations
