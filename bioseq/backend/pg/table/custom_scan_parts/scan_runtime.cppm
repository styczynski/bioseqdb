/**
 * @file
 * @brief Executes Bioseq base scans, source promotion, projection, rescan, and cleanup.
 */
module;
// Anchor Highway in this global module fragment so target-specific definitions
// imported through multiple module graphs share one translation-unit context.
#include <bioseq/pg_compat.h>
#include <hwy/highway.h>

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

export module bioseq.postgres.table:custom_scan_scan_runtime;

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
import bioseq.highway.dispatch_contract;
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
import bioseq.postgres.storage.indexing.admin;
import bioseq.instrumentation.summary_writer;
import bioseq.common.seqan_adapter;
import bioseq.postgres.table.batch_sort_scan;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;
import :custom_scan_match_ownership;
import :custom_scan_join_execution;

using namespace bioseq::postgres::support;

export {

    /*
     * Per-tuple population for every `bioseq.symbol_count(Var, 'L')`
     * placeholder. For the current output row (resolved by columnar rowno) it
     * random-access reads the SEQ column header, then for each site:
     *   (2) if bioseq.index.sequence_read.enabled = on, tries the stored INLINE
     *       symbol_counts_index chunk (one heap PK lookup, the only thing the GUC
     *       gates here);
     *   (3) otherwise UNCONDITIONALLY recomputes tally_symbol_counts over the row
     *       (scanner_pg-streamed for a stored stub, in-memory for an inline value).
     * The recompute guarantees a non-NULL answer for every present row regardless
     * of the GUC — the GUC only chooses whether the stored chunk is preferred.
     * Replaces the deleted meta_sidecar / 49-stub path.
     */
    struct symbol_count_read_resources {
        MemoryContext caller_context = nullptr;
        MemoryContext scratch = nullptr;
        bioseq::storage::seq_caches* caches = nullptr;
        column_read_state* row_read_state = nullptr;
    };

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

    void abandon_symbol_count_read_resources_after_error(
        symbol_count_read_resources* resources) noexcept {
        if (resources == nullptr) {
            return;
        }
        // PostgreSQL's transaction/ResourceOwner will release readers, relations,
        // and palloc contexts.  Do not issue any PG call while preserving the
        // original ErrorData; only discharge ordinary C++ heap ownership.
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

    [[noreturn]] void symbol_count_error_as_cpp(int sqlstate, std::string_view message) {
        bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message] {
            ereport(ERROR,
                    (errcode(sqlstate),
                     errmsg("%.*s", static_cast<int>(message.size()), message.data())));
        });
        pg_unreachable();
    }

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

            /* Mutable ownership lives in this record so ordinary C++ catches can either
             * checked-close it on success or abandon PG handles without replacing the
             * original ErrorData. */
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
            /* Project every distinct site column and keep the transient List plus
             * direct read state inside scratch. Catch restores caller_context if an
             * ereport jumps over this explicit context switch. */
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
            /* vals contains by-reference Datums owned by row_read_state's
             * stripeReadContext.  Keep the reader alive until every symbol-count
             * site has detoasted/consumed those values; the unified release below
             * ends it on both success and ERROR. */

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

                /* (2) stored read — the ONLY thing read_sequence_index gates here. */
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

                /* (3) unconditional recompute fallback (never gated by the GUC). */
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

    void promote_slice_project_sources(scan_state* state,
                                       TupleTableSlot* slot,
                                       MemoryContext result_context,
                                       const bool allow_locator_passthrough,
                                       const slice_project_source_slot_layout layout,
                                       TupleDesc expected_descriptor) {
        bioseq_scan_runtime& runtime = bioseq_runtime(state);
        if (!runtime.lazy_source_projection ||
            (runtime.locator_passthrough && !allow_locator_passthrough))
            return;
        if (runtime.lazy_source_provider == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqSliceProject source provider is not initialized")));
        }
        if (slot == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqSliceProject source slot is unavailable")));
        }
        slot_getallattrs(slot);
        const Oid seq_oid = get_cached_stub_oids().bioseq_seq_type_oid;
        if (result_context == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqSliceProject result context is unavailable")));
        }
        TupleDesc actual_descriptor = slot->tts_tupleDescriptor;
        if (actual_descriptor == nullptr || expected_descriptor == nullptr ||
            (actual_descriptor != expected_descriptor &&
             !equalTupleDescs(actual_descriptor, expected_descriptor))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqSliceProject source slot layout changed"),
                     errdetail("layout=%s",
                               layout == slice_project_source_slot_layout::physical_relation
                                   ? "physical relation"
                                   : "projected child")));
        }
        const std::size_t required_entries = static_cast<std::size_t>(actual_descriptor->natts + 1);
        if ((layout == slice_project_source_slot_layout::physical_relation &&
             (runtime.lazy_source_physical_producer_ids.size() != required_entries ||
              runtime.lazy_source_physical_bound.size() != required_entries)) ||
            (layout == slice_project_source_slot_layout::projected_child &&
             runtime.lazy_source_projected_bindings.size() != required_entries)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject source binding layout changed")));
        }
        for (int index = 0; index < slot->tts_tupleDescriptor->natts; ++index) {
            const AttrNumber resno = static_cast<AttrNumber>(index + 1);
            std::uint32_t producer_id = 0u;
            AttrNumber column_attno = InvalidAttrNumber;
            bool bound = false;
            if (layout == slice_project_source_slot_layout::physical_relation) {
                bound = runtime.lazy_source_physical_bound[resno] != 0u;
                producer_id = runtime.lazy_source_physical_producer_ids[resno];
                column_attno = resno;
            } else {
                auto const& binding = runtime.lazy_source_projected_bindings[resno];
                bound = binding.physical_attno != InvalidAttrNumber;
                producer_id = binding.producer_id;
                column_attno = binding.physical_attno;
            }
            if (!bound)
                continue;
            if (TupleDescAttr(actual_descriptor, index)->atttypid != seq_oid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqSliceProject bound source is not a sequence"),
                         errdetail("layout_attribute=%d physical_attribute=%d",
                                   static_cast<int>(resno),
                                   static_cast<int>(column_attno))));
            }
            if (slot->tts_isnull[index])
                continue;
            Datum source = slot->tts_values[index];
            if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(source))) {
                if (producer_id != 0u && runtime.lazy_trace_attachment != nullptr &&
                    runtime.lazy_trace_attachment->runtime != nullptr &&
                    runtime.lazy_trace_attachment->runtime->collection_enabled &&
                    !attach_slice_trace_provenance(
                        source, runtime.lazy_trace_attachment, producer_id)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqSliceProject expanded source lost provenance")));
                }
                continue;
            }
            auto* value = reinterpret_cast<const bioseq::pg_types::seq*>(DatumGetPointer(source));
            if (!bioseq::storage::is_locator_stub(value)) {
                /* A small inline sequence has no EOH metadata field on which to keep
                 * the planned producer identity.  Under ANALYZE only, adopt that flat
                 * value once into the row context and attach observation metadata before
                 * the lazy expression runs.  The backing copy is a real source-adoption
                 * allocation (and remains visible to the allocator probe); it is not a
                 * SliceProject slot-publication materialization. */
                if (producer_id == 0u || runtime.lazy_trace_attachment == nullptr ||
                    runtime.lazy_trace_attachment->runtime == nullptr ||
                    !runtime.lazy_trace_attachment->runtime->collection_enabled) {
                    continue;
                }
                MemoryContext previous = MemoryContextSwitchTo(result_context);
                try {
                    source = bioseq::postgres::support::invoke_pg_as_cpp(
                        [source, trace = runtime.lazy_trace_attachment, producer_id] {
                            auto adopted = acquire_writable_pg_sequence(source, trace, producer_id);
                            return MakeExpandedObjectReadOnly(
                                pg_sequence_rw_datum(adopted.value), false, -1);
                        });
                    MemoryContextSwitchTo(previous);
                } catch (...) {
                    MemoryContextSwitchTo(previous);
                    throw;
                }
                slot->tts_values[index] = source;
                continue;
            }
            if (column_attno <= 0) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject locator lost its physical column"),
                         errdetail("result_attribute=%d", static_cast<int>(resno))));
            }
            slot->tts_values[index] =
                bioseq::storage::promote_pg_sequence_locator(runtime.lazy_source_provider,
                                                             column_attno,
                                                             source,
                                                             result_context,
                                                             runtime.lazy_trace_attachment,
                                                             producer_id);
        }
    }

    enum class mmsearch_source_capture_role : std::uint8_t {
        evaluated_operand,
        output_projection,
    };

    struct mmsearch_source_restore_entry {
        AttrNumber attno = InvalidAttrNumber;
        Datum datum{};
    };

    /* Four physical sources cover the ordinary target/query/output case without
     * a per-match_result allocation.  Wider projections spill into the executor
     * per-tuple MemoryContext, which PostgreSQL releases even when ERROR skips C++
     * destructors.  The object is populated before PG_TRY and never mutated in
     * its setjmp region, so either storage form is safe to read from PG_CATCH. */
    struct mmsearch_source_restore {
        static constexpr std::size_t inline_capacity = 4u;
        std::array<mmsearch_source_restore_entry, inline_capacity> inline_entries{};
        mmsearch_source_restore_entry* overflow_entries = nullptr;
        std::size_t count = 0u;

        [[nodiscard]] mmsearch_source_restore_entry& entry(const std::size_t index) noexcept {
            return overflow_entries == nullptr ? inline_entries[index] : overflow_entries[index];
        }

        [[nodiscard]] const mmsearch_source_restore_entry&
        entry(const std::size_t index) const noexcept {
            return overflow_entries == nullptr ? inline_entries[index] : overflow_entries[index];
        }
    };

    void mark_mmsearch_physical_sequence_source(std::vector<std::uint8_t>& roles,
                                                TupleDesc relation_descriptor,
                                                const AttrNumber attno,
                                                const Oid sequence_type_oid) {
        if (relation_descriptor == nullptr)
            return;
        const int natts = relation_descriptor->natts;
        if (attno == 0) {
            for (int index = 0; index < natts; ++index) {
                Form_pg_attribute attribute = TupleDescAttr(relation_descriptor, index);
                if (attribute != nullptr && !attribute->attisdropped &&
                    attribute->atttypid == sequence_type_oid) {
                    roles[static_cast<std::size_t>(index + 1)] = 1u;
                }
            }
            return;
        }
        if (attno <= 0 || attno > natts)
            return;
        Form_pg_attribute attribute = TupleDescAttr(relation_descriptor, attno - 1);
        if (attribute != nullptr && !attribute->attisdropped &&
            attribute->atttypid == sequence_type_oid) {
            roles[static_cast<std::size_t>(attno)] = 1u;
        }
    }

    /* Mark physical sequence Vars from one finalized executor expression tree.
     * This mirrors collect_needed_column_attributes' Var/whole-row rules, but records only
     * sequence columns and keeps the lifetime role separate from read demand. */
    void mark_mmsearch_sequence_sources_from_expression(std::vector<std::uint8_t>& roles,
                                                        TupleDesc relation_descriptor,
                                                        Node* expression,
                                                        const Oid sequence_type_oid) {
        if (expression == nullptr)
            return;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* variables = pull_var_clause(expression, pull_flags);
        for (Var* variable : postgres_list<Var>(variables)) {
            if (variable != nullptr)
                mark_mmsearch_physical_sequence_source(
                    roles, relation_descriptor, variable->varattno, sequence_type_oid);
        }
        list_free(variables);
    }

    /* Derive timing from the final executor trees, not the broader BSS3 provider
     * tag.  newtargetlist is the exact per-match_result projection after helper
     * injection; plan.qual is the executable residual frontier.  Operand lineage
     * is added because evaluated submission executes both operand ExprStates even
     * when only one side is syntactically transformed. */
    void derive_mmsearch_source_lifetime_roles(bioseq_scan_runtime& runtime,
                                               Plan* plan,
                                               TupleDesc relation_descriptor) {
        runtime.mmsearch_pre_eval_sources_present = false;
        if (relation_descriptor == nullptr)
            return;
        const std::size_t role_count = static_cast<std::size_t>(relation_descriptor->natts + 1);
        runtime.mmsearch_pre_eval_source_bound.assign(role_count, 0u);
        runtime.mmsearch_output_source_bound.assign(role_count, 0u);
        const Oid sequence_type_oid = get_cached_stub_oids().bioseq_seq_type_oid;
        mark_mmsearch_sequence_sources_from_expression(
            runtime.mmsearch_output_source_bound,
            relation_descriptor,
            reinterpret_cast<Node*>(runtime.newtargetlist),
            sequence_type_oid);
        if (plan != nullptr)
            mark_mmsearch_sequence_sources_from_expression(runtime.mmsearch_pre_eval_source_bound,
                                                           relation_descriptor,
                                                           reinterpret_cast<Node*>(plan->qual),
                                                           sequence_type_oid);
        if (runtime.match_evaluated_submission) {
            for (Var* source : postgres_list<Var>(runtime.match_target_lineage)) {
                if (source != nullptr)
                    mark_mmsearch_physical_sequence_source(runtime.mmsearch_pre_eval_source_bound,
                                                           relation_descriptor,
                                                           source->varattno,
                                                           sequence_type_oid);
            }
            if (runtime.match_query_evaluated) {
                for (Var* source : postgres_list<Var>(runtime.match_query_lineage)) {
                    if (source != nullptr)
                        mark_mmsearch_physical_sequence_source(
                            runtime.mmsearch_pre_eval_source_bound,
                            relation_descriptor,
                            source->varattno,
                            sequence_type_oid);
                }
            }
        }
        runtime.mmsearch_pre_eval_sources_present =
            std::any_of(runtime.mmsearch_pre_eval_source_bound.begin(),
                        runtime.mmsearch_pre_eval_source_bound.end(),
                        [](const std::uint8_t bound) { return bound != 0u; });
    }

    [[nodiscard]] bool
    mmsearch_source_matches_capture_role(const bioseq_scan_runtime& runtime,
                                         const AttrNumber attno,
                                         const mmsearch_source_capture_role role) noexcept {
        const auto& roles = role == mmsearch_source_capture_role::output_projection
                                ? runtime.mmsearch_output_source_bound
                                : runtime.mmsearch_pre_eval_source_bound;
        const std::size_t index = static_cast<std::size_t>(attno);
        return index < roles.size() && roles[index] != 0u;
    }

    /* Capture each physical attribute at most once.  Iterating the slot's attno
     * namespace forms the exact union required by the role: evaluated operand
     * leaves before ExprState execution, or finalized output-projection sources
     * before per-match_result projection. */
    [[nodiscard]] mmsearch_source_restore
    capture_mmsearch_sources(bioseq_scan_runtime& runtime,
                             TupleTableSlot* slot,
                             MemoryContext restore_context,
                             const mmsearch_source_capture_role role) {
        mmsearch_source_restore restore{};
        if (runtime.lazy_source_provider == nullptr || slot == nullptr ||
            slot->tts_tupleDescriptor == nullptr || restore_context == nullptr)
            return restore;
        slot_getallattrs(slot);
        const int natts = slot->tts_tupleDescriptor->natts;
        std::size_t count = 0u;
        for (int index = 0; index < natts; ++index) {
            const AttrNumber attno = static_cast<AttrNumber>(index + 1);
            if (!slot->tts_isnull[index] &&
                mmsearch_source_matches_capture_role(runtime, attno, role)) {
                ++count;
            }
        }
        if (count == 0u)
            return restore;
        restore.count = count;
        if (count > mmsearch_source_restore::inline_capacity) {
            restore.overflow_entries = static_cast<mmsearch_source_restore_entry*>(
                MemoryContextAlloc(restore_context, count * sizeof(mmsearch_source_restore_entry)));
        }
        std::size_t captured = 0u;
        for (int index = 0; index < natts; ++index) {
            const AttrNumber attno = static_cast<AttrNumber>(index + 1);
            if (slot->tts_isnull[index] ||
                !mmsearch_source_matches_capture_role(runtime, attno, role)) {
                continue;
            }
            restore.entry(captured) = mmsearch_source_restore_entry{
                .attno = attno,
                .datum = slot->tts_values[index],
            };
            ++captured;
        }
        return restore;
    }

    [[nodiscard]] std::size_t promote_mmsearch_sources(bioseq_scan_runtime& runtime,
                                                       TupleTableSlot* slot,
                                                       MemoryContext output_context,
                                                       const mmsearch_source_restore& restore) {
        if (runtime.lazy_source_provider == nullptr || slot == nullptr ||
            output_context == nullptr) {
            return 0u;
        }
        std::size_t promoted_count = 0u;
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const AttrNumber attno = restore.entry(index).attno;
            if (attno <= 0 || attno > slot->tts_tupleDescriptor->natts)
                continue;
            const Datum source = restore.entry(index).datum;
            if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(source)))
                continue;

            /* ExecCopySlotMinimalTuple may short-pack this STORAGE=extended value.
             * Promotion and the locator ABI require the ordinary four-byte varlena
             * header, so detoast in the output lifetime before inspecting it. */
            MemoryContext previous = MemoryContextSwitchTo(output_context);
            bioseq::pg_types::seq* flat = nullptr;
            try {
                flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(source));
                MemoryContextSwitchTo(previous);
            } catch (...) {
                MemoryContextSwitchTo(previous);
                throw;
            }
            if (static_cast<std::size_t>(VARSIZE(flat)) < bioseq::pg_types::seq::header_bytes()) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("buffered bioseq output source is truncated"),
                         errdetail("attribute=%d bytes=%zu",
                                   static_cast<int>(attno),
                                   static_cast<std::size_t>(VARSIZE(flat)))));
            }
            if (!bioseq::storage::is_locator_stub(flat))
                continue;
            std::uint32_t producer_id = 0u;
            slice_trace_attachment* trace_attachment = nullptr;
            if (runtime.lazy_source_projection) {
                const std::size_t binding_index = static_cast<std::size_t>(attno);
                if (binding_index < runtime.lazy_source_physical_bound.size() &&
                    binding_index < runtime.lazy_source_physical_producer_ids.size() &&
                    runtime.lazy_source_physical_bound[binding_index] != 0u) {
                    trace_attachment = runtime.lazy_trace_attachment;
                    producer_id = runtime.lazy_source_physical_producer_ids[binding_index];
                }
            }
            slot->tts_values[attno - 1] =
                bioseq::storage::promote_pg_sequence_locator(runtime.lazy_source_provider,
                                                             attno,
                                                             PointerGetDatum(flat),
                                                             output_context,
                                                             trace_attachment,
                                                             producer_id);
            ++promoted_count;
        }
        return promoted_count;
    }

    void restore_mmsearch_sources(TupleTableSlot* slot,
                                  const mmsearch_source_restore& restore) noexcept {
        if (slot == nullptr)
            return;
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const AttrNumber attno = restore.entry(index).attno;
            if (attno > 0 && attno <= slot->tts_tupleDescriptor->natts) {
                slot->tts_values[attno - 1] = restore.entry(index).datum;
            }
        }
    }

    [[nodiscard]] bool
    mmsearch_sources_restored_exactly(TupleTableSlot* slot,
                                      const mmsearch_source_restore& restore) noexcept {
        if (slot == nullptr || slot->tts_tupleDescriptor == nullptr) {
            return restore.count == 0u;
        }
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const AttrNumber attno = restore.entry(index).attno;
            if (attno <= 0 || attno > slot->tts_tupleDescriptor->natts ||
                slot->tts_values[attno - 1] != restore.entry(index).datum) {
                return false;
            }
        }
        return true;
    }

    struct mmsearch_row_evaluation {
        bool qual_passed = false;
        Datum target{};
        bool target_is_null = true;
        Datum query{};
        bool query_is_null = true;
    };

    /* Evaluate the residual frontier and, when configured, both mmsearch operand
     * ExprStates while only their physical sources are promoted.  The scan slot is
     * restored on normal completion, C++ unwind, and PostgreSQL longjmp before a
     * caller can buffer it. */
    [[nodiscard]] mmsearch_row_evaluation
    evaluate_mmsearch_input_row(bioseq_scan_runtime& runtime,
                                TupleTableSlot* slot,
                                ExprState* qual,
                                ExprContext* expression_context) {
        mmsearch_row_evaluation result{};
        if (!runtime.match_evaluated_submission && !runtime.mmsearch_pre_eval_sources_present) {
            result.qual_passed = qual == nullptr || ExecQual(qual, expression_context);
            return result;
        }
        const mmsearch_source_restore source_restore =
            capture_mmsearch_sources(runtime,
                                     slot,
                                     expression_context->ecxt_per_tuple_memory,
                                     mmsearch_source_capture_role::evaluated_operand);
        if (source_restore.count == 0u && !runtime.match_evaluated_submission) {
            result.qual_passed = qual == nullptr || ExecQual(qual, expression_context);
            return result;
        }
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* exact_pg_error = nullptr;
        {
            std::exception_ptr cpp_error;
            ErrorData* volatile pg_error = nullptr;
            PG_TRY();
            {
                try {
                    (void)promote_mmsearch_sources(
                        runtime, slot, expression_context->ecxt_per_tuple_memory, source_restore);
                    result.qual_passed = qual == nullptr || ExecQual(qual, expression_context);
                    if (result.qual_passed && runtime.match_evaluated_submission) {
                        if (runtime.match_target_expr_state == nullptr) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_DATA_CORRUPTED),
                                     errmsg("BioseqScan evaluated target ExprState is missing")));
                        }
                        result.target = ExecEvalExprSwitchContext(runtime.match_target_expr_state,
                                                                  expression_context,
                                                                  &result.target_is_null);
                        if (runtime.match_query_evaluated) {
                            if (runtime.match_query_expr_state == nullptr) {
                                ereport(
                                    ERROR,
                                    (errcode(ERRCODE_DATA_CORRUPTED),
                                     errmsg("BioseqScan evaluated query ExprState is missing")));
                            }
                            /* Preserve SQL argument order even when the target is NULL. */
                            result.query = ExecEvalExprSwitchContext(runtime.match_query_expr_state,
                                                                     expression_context,
                                                                     &result.query_is_null);
                        }
                    }
                    restore_mmsearch_sources(slot, source_restore);
                } catch (...) {
                    MemoryContextSwitchTo(saved_context);
                    restore_mmsearch_sources(slot, source_restore);
                    cpp_error = std::current_exception();
                }
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(saved_context);
                restore_mmsearch_sources(slot, source_restore);
                pg_error = CopyErrorData();
                FlushErrorState();
            }
            PG_END_TRY();
            if (pg_error != nullptr)
                exact_pg_error = pg_error;
            if (cpp_error != nullptr)
                std::rethrow_exception(cpp_error);
        }
        /* Re-enter PostgreSQL only after std::exception_ptr has been destroyed. */
        if (exact_pg_error != nullptr)
            ReThrowError(exact_pg_error);
        return result;
    }

    /*
     * VectorAgg consumes one vector_column_storage pointer per projected attribute.
     * The ordinary scan path extracts scalar rows, so build a projected
     * vector batch explicitly when the planner attached VectorAgg.
     */
    static TupleTableSlot* next_vectorized_aggregate_batch(scan_state* bioseq_scan_state,
                                                           ExecScanAccessMtd access_mtd,
                                                           ExecScanRecheckMtd recheck_mtd) {
        ScanState* node = reinterpret_cast<ScanState*>(&bioseq_scan_state->custom_scan_state.ss);
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
        ExprContext* econtext = node->ps.ps_ExprContext;
        ExprState* qual = node->ps.qual;
        TupleTableSlot* result_slot = bioseq_scan_state->vectorization.result_vector_slot;
        auto* result_vector = reinterpret_cast<vector_tuple_table_slot_storage*>(result_slot);
        TupleTableSlot* scalar_scan_slot = node->ss_ScanTupleSlot;
        const bool projects_attributes = result_slot->tts_tupleDescriptor->natts != 0;
        ProjectionInfo* projection =
            projects_attributes ? ExecBuildProjectionInfo(runtime.newtargetlist,
                                                          econtext,
                                                          node->ps.ps_ResultTupleSlot,
                                                          &node->ps,
                                                          scalar_scan_slot->tts_tupleDescriptor)
                                : nullptr;

        for (;;) {
            TupleTableSlot* slot = exec_scan_fetch(node, access_mtd, recheck_mtd);
            if (TupIsNull(slot)) {
                runtime.source_exhausted = true;
                return ExecClearTuple(result_slot);
            }
            if (!is_vector_tuple_table_slot(slot)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("VectorAgg input is not a vector tuple slot")));
            }

            auto* scan_vector = reinterpret_cast<vector_tuple_table_slot_storage*>(slot);
            if (!bioseq_scan_state->vectorization.qualifiers.is_empty()) {
                if (bioseq_scan_state->vectorization.constructed_qualifiers.is_empty()) {
                    bioseq_scan_state->vectorization.constructed_qualifiers =
                        bioseq::postgres::support::postgres_list<Node>{
                            compile_vector_qualifier_list(
                                slot, bioseq_scan_state->vectorization.qualifiers.get())};
                }
                bool* result_qual = evaluate_vectorized_qualifiers(
                    slot,
                    bioseq_scan_state->vectorization.constructed_qualifiers.get(),
                    AND_EXPR,
                    econtext);
                memcpy(scan_vector->row_is_active.data(),
                       result_qual,
                       scan_vector->row_count * sizeof(bool));
            }

            std::uint32_t result_dimension = 0;
            for (std::uint32_t row_index = 0; row_index < scan_vector->row_count; ++row_index) {
                if (!scan_vector->row_is_active[row_index])
                    continue;

                ResetExprContext(econtext);
                ExecClearTuple(scalar_scan_slot);
                extract_tuple_from_vector_slot(
                    scalar_scan_slot,
                    scan_vector,
                    static_cast<std::int32_t>(row_index),
                    bioseq_scan_state->vectorization.attributes_needed.get());
                scalar_scan_slot->tts_tid =
                    row_number_to_tid(scan_vector->source_row_numbers[row_index]);
                econtext->ecxt_scantuple = scalar_scan_slot;

                if (qual != nullptr && !ExecQual(qual, econtext)) {
                    InstrCountFiltered1(node, 1);
                    continue;
                }

                if (projects_attributes) {
                    TupleTableSlot* projected = ExecProject(projection);
                    write_tuple_to_vector_slot(
                        projected, result_vector, static_cast<std::int32_t>(result_dimension));
                }
                ++result_dimension;
            }

            bioseq_scan_state->vectorization.pending_row_number = 0;
            bioseq_scan_state->vectorization.row_index = scan_vector->row_count;
            if (result_dimension == 0)
                continue;

            result_vector->row_count = result_dimension;
            memset(result_vector->row_is_active.data(), true, result_dimension);
            ExecStoreVirtualTuple(result_slot);
            return result_slot;
        }
    }

    static TupleTableSlot* inner_custom_exec_scan(scan_state* bioseq_scan_state,
                                                  ExecScanAccessMtd access_mtd,
                                                  ExecScanRecheckMtd recheck_mtd) {
        bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() entered");
        ExprContext* econtext;
        ExprState* qual;

        ScanState* node = reinterpret_cast<ScanState*>(&bioseq_scan_state->custom_scan_state.ss);
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);

        qual = node->ps.qual;
        econtext = node->ps.ps_ExprContext;

        if (bioseq_scan_state->vectorization.is_aggregate) {
            ExecClearTuple(bioseq_scan_state->vectorization.result_vector_slot);
            return next_vectorized_aggregate_batch(bioseq_scan_state, access_mtd, recheck_mtd);
        }

        /*
         * Two-stage state machine:
         *
         *   Writing stage: pull tuples from the access method, run qual,
         *                      buffer passing tuples via buffer.submit_scan.
         *                      When source is exhausted, run batch processor
         *                      and transition.
         *   Reading stage: drain buffered output entries, setting
         *                      placeholder const nodes from match_results and
         *                      projecting via ExecProject.
         *
         * buffer.has_finished_writing() flips from false → true at the
         * transition.
         */
        for (;;) {
            /* ---- Reading stage: drain buffered output ---- */
            if (runtime.buffer.has_finished_writing()) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() draining output cursor");
                /* The parent has consumed the previously returned result before it
                 * invokes this scan again.  Drop that tuple-visible EOH first, then
                 * release every detoasted locator and source-backed output allocated
                 * in the ordinary executor per-tuple context.  Buffered MinimalTuple
                 * slots and match_results have independent lifetimes. */
                ExecClearTuple(node->ps.ps_ResultTupleSlot);
                ResetExprContext(econtext);
                econtext->ecxt_scantuple = nullptr;
                econtext->ecxt_outertuple = nullptr;
                econtext->ecxt_innertuple = nullptr;
                auto next = runtime.buffer.read_next_output();
                if (!next) {
                    if (runtime.source_exhausted) {
                        bioseq::postgres::table::alignment_execution::
                            mark_alignment_pass_fully_drained(runtime.alignment_registration);
                        bioseq_log<logging_level::debug>(
                            "[bioseq] pg: Inner_CustomExecScan() final output cursor drained; "
                            "returning empty slot (EOF)");
                        return ExecClearTuple(node->ps.ps_ResultTupleSlot);
                    }
                    /* A capacity rollover completed. Drop only that physical
                     * batch and resume the still-live access-method scan. */
                    runtime.buffer.clear_slots();
                    continue;
                }
                bioseq::storage::note_mmsearch_output_context_reset_for_test();

                if (runtime.edit_predicate_active &&
                    (runtime.edit_exclusive_upper_bound <= 0 ||
                     next->match->edit_distance >=
                         static_cast<std::uint64_t>(runtime.edit_exclusive_upper_bound))) {
                    /* SneakySnake is only a lower-bound gate.  Every survivor is
                     * aligned, and this exact comparison is the authoritative SQL
                     * predicate (strict `<`, including the boundary case). */
                    InstrCountFiltered1(node, 1);
                    continue;
                }

                /*
                 * Update every placeholder Const from the match_result. The
                 * subsequent ExecProject reads the current constvalue of
                 * every Const node referenced in newtargetlist.
                 */
                populate_placeholder_consts(runtime.placeholder_consts, *next->match);

                /*
                 * symbol_count placeholders are independent of match_result;
                 * they are resolved from meta_sidecar by (relid, rowno) so
                 * we populate them alongside the per-match fields.
                 */
                populate_symbol_count_placeholders(runtime.placeholder_consts,
                                                   node->ss_currentRelation,
                                                   next->sequence_rowno,
                                                   bioseq_scan_state->snapshot,
                                                   runtime.aligner_state.read_sequence_index);

                /* The alignment path retains locator stubs in its buffered row. Promote
                 * only for this output projection, then restore the stable locator so a
                 * second match from the same row never points into a reset per-tuple
                 * context. */
                const mmsearch_source_restore source_restore =
                    capture_mmsearch_sources(runtime,
                                             next->scan_slot_copy,
                                             econtext->ecxt_per_tuple_memory,
                                             mmsearch_source_capture_role::output_projection);
                TupleTableSlot* projected = nullptr;
                std::exception_ptr projection_error;
                MemoryContext saved_context = CurrentMemoryContext;
                ErrorData* volatile projection_pg_error = nullptr;

                /* PostgreSQL ERROR skips C++ destructors. Keep the immutable restore
                 * snapshot outside the setjmp region and restore the locator slot on
                 * normal return, C++ unwind, and PG longjmp alike. */
                PG_TRY();
                {
                    try {
                        const std::size_t promoted_source_count =
                            promote_mmsearch_sources(runtime,
                                                     next->scan_slot_copy,
                                                     econtext->ecxt_per_tuple_memory,
                                                     source_restore);
                        bioseq::storage::note_mmsearch_output_locator_promotion_for_test(
                            promoted_source_count != 0u);
                        bioseq::storage::inject_mmsearch_output_after_promotion_failure_for_test();

                        /* Re-install the buffered scan slot as the current scan
                         * tuple so Var references resolve off the right row. */
                        econtext->ecxt_scantuple = next->scan_slot_copy;

                        ProjectionInfo* proj =
                            ExecBuildProjectionInfo(runtime.newtargetlist,
                                                    econtext,
                                                    node->ps.ps_ResultTupleSlot,
                                                    &(node->ps),
                                                    next->scan_slot_copy->tts_tupleDescriptor);
                        projected = ExecProject(proj);
                        restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                    } catch (...) {
                        MemoryContextSwitchTo(saved_context);
                        restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                        projection_error = std::current_exception();
                    }
                }
                PG_CATCH();
                {
                    MemoryContextSwitchTo(saved_context);
                    restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                    bioseq::storage::note_mmsearch_output_pg_error_restore_for_test(
                        mmsearch_sources_restored_exactly(next->scan_slot_copy, source_restore));
                    projection_pg_error = CopyErrorData();
                    FlushErrorState();
                }
                PG_END_TRY();
                if (projection_pg_error != nullptr) {
                    ErrorData* exact_error = projection_pg_error;
                    throw bioseq::postgres::support::postgres_error{exact_error};
                }
                if (projection_error != nullptr) {
                    std::rethrow_exception(projection_error);
                }
                return projected;
            }

            /* ---- Writing stage ---- */
            TupleTableSlot* slot = nullptr;

            /*
             * Scalar fetch path: pull one scan tuple and run qual on it.
             */
            if (!bioseq_scan_state->vectorization.is_enabled) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() scalar fetch via ExecScanFetch");
                slot = exec_scan_fetch(node, access_mtd, recheck_mtd);

                if (TupIsNull(slot)) {
                    runtime.source_exhausted = true;
                    bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() source "
                                                     "exhausted; running batch processor");
                    if (runtime.buffer.run_batch_and_build_cursors()) {
                        bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                            runtime.alignment_registration, runtime.buffer.execution_stats());
                    }
                    continue; /* Next iteration enters the reading stage. */
                }
            }
            /*
             * Vectorized fetch path: pull a vector slot, apply vectorized
             * qual mask, then extract a single scalar tuple per iteration
             * until the vector is exhausted.
             */
            else {
                /*
                 * First vector pull or after previous vector drained.
                 */
                if (bioseq_scan_state->vectorization.pending_row_number == 0) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() "
                                                     "vectorized fetch (initial vector pull)");
                    slot = exec_scan_fetch(node, access_mtd, recheck_mtd);

                    if (TupIsNull(slot)) {
                        runtime.source_exhausted = true;
                        bioseq_log<logging_level::debug>(
                            "[bioseq] pg: Inner_CustomExecScan() vectorized source exhausted; "
                            "running batch processor");
                        if (runtime.buffer.run_batch_and_build_cursors()) {
                            bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                                runtime.alignment_registration, runtime.buffer.execution_stats());
                        }
                        continue;
                    }

                    if (!bioseq_scan_state->vectorization.qualifiers.is_empty()) {
                        if (bioseq_scan_state->vectorization.constructed_qualifiers.is_empty()) {
                            bioseq_scan_state->vectorization.constructed_qualifiers =
                                bioseq::postgres::support::postgres_list<Node>{
                                    compile_vector_qualifier_list(
                                        slot, bioseq_scan_state->vectorization.qualifiers.get())};
                        }

                        vector_tuple_table_slot_storage* vector_slot =
                            reinterpret_cast<vector_tuple_table_slot_storage*>(slot);
                        bool* result_qual = evaluate_vectorized_qualifiers(
                            slot,
                            bioseq_scan_state->vectorization.constructed_qualifiers.get(),
                            AND_EXPR,
                            econtext);
                        memcpy(vector_slot->row_is_active.data(),
                               result_qual,
                               vector_column_capacity * sizeof(bool));
                    }
                }

                /*
                 * Extract one kept row from the vector slot into the scalar
                 * scan slot. Skip inactive rows.
                 */
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() vectorized extraction");
                vector_tuple_table_slot_storage* vector_slot =
                    reinterpret_cast<vector_tuple_table_slot_storage*>(
                        bioseq_scan_state->vectorization.scan_vector_slot);

                bool row_found = false;
                while (bioseq_scan_state->vectorization.pending_row_number != 0 && !row_found) {
                    if (vector_slot->row_is_active[bioseq_scan_state->vectorization.row_index]) {
                        slot = bioseq_scan_state->custom_scan_state.ss.ss_ScanTupleSlot;
                        ExecClearTuple(slot);
                        extract_tuple_from_vector_slot(
                            slot,
                            vector_slot,
                            bioseq_scan_state->vectorization.row_index,
                            bioseq_scan_state->vectorization.attributes_needed.get());

                        uint64_t row_number =
                            vector_slot
                                ->source_row_numbers[bioseq_scan_state->vectorization.row_index];
                        slot->tts_tid = row_number_to_tid(row_number);
                        row_found = true;
                    }
                    bioseq_scan_state->vectorization.pending_row_number--;
                    bioseq_scan_state->vectorization.row_index++;
                }

                /*
                 * Vector exhausted without finding a kept row: go back to
                 * pull another vector or finalize.
                 */
                if (!row_found)
                    continue;
            }

            /* Every fetched input row owns a fresh expression/root arena.  BSS3
             * promotion and the authoritative operand ExprStates retain only until
             * submit_scan_evaluated copies their small EOH wrappers into the batch
             * arena. */
            ResetExprContext(econtext);
            econtext->ecxt_scantuple = slot;

            /*
             * The scan runs with defer_seq_hydration=true, so `slot`
             * carries 40-byte locator stubs. When there is an mmsearch qual the
             * row is buffered (submit_scan captures the locator; the batch path
             * bulk-hydrates per bucket), so we leave the stub in place. Without an
             * mmsearch qual the row is projected immediately below, so hydrate the
             * SEQ columns now (force=true overrides the defer flag) — this covers
             * both the scalar and vectorized convergence and is the analogue of
             * the inline hydration bioseq_getnextslot performs on non-deferred
             * scan paths.
             */
            if (!runtime.buffer.mmsearch_active() && !runtime.locator_passthrough &&
                !runtime.lazy_source_projection) {
                hydrate_scan_slot(node->ss_currentScanDesc, slot, /*force=*/true);
            }
            /* A non-mmsearch scan projects immediately and may promote every BSS3
             * source.  Alignment scans instead retain stable locators in their
             * buffered row.  Evaluated operand leaves are promoted temporarily at
             * their ExprState boundary below; output-only sources are promoted once
             * per match_result under the PG_TRY restoration boundary above. */
            if (runtime.lazy_source_projection && !runtime.locator_passthrough &&
                !runtime.buffer.mmsearch_active()) {
                promote_slice_project_sources(bioseq_scan_state,
                                              slot,
                                              econtext->ecxt_per_tuple_memory,
                                              /*allow_locator_passthrough=*/false,
                                              slice_project_source_slot_layout::physical_relation,
                                              slot->tts_tupleDescriptor);
            }

            /*
             * Place the current tuple into the expr context and run the
             * non-vectorized qual (e.g. mmsearch()). Both scalar and
             * vectorized paths converge here.
             */
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: Inner_CustomExecScan() applying qual filter");
            mmsearch_row_evaluation mmsearch_evaluation{};
            bool qual_passed = false;
            if (runtime.buffer.mmsearch_active()) {
                mmsearch_evaluation =
                    evaluate_mmsearch_input_row(runtime, econtext->ecxt_scantuple, qual, econtext);
                qual_passed = mmsearch_evaluation.qual_passed;
            } else {
                qual_passed = qual == nullptr || ExecQual(qual, econtext);
            }
            if (qual_passed) {
                if (runtime.buffer.mmsearch_active()) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() qual "
                                                     "passed; buffering scan tuple");
                    if (runtime.match_evaluated_submission) {
                        if (runtime.mmsearch_null_input)
                            continue;
                        runtime.buffer.submit_scan_evaluated(econtext->ecxt_scantuple,
                                                             mmsearch_evaluation.target,
                                                             mmsearch_evaluation.target_is_null,
                                                             runtime.match_query_evaluated,
                                                             mmsearch_evaluation.query,
                                                             mmsearch_evaluation.query_is_null);
                    } else {
                        if (runtime.mmsearch_null_input)
                            continue;
                        bool target_is_null = false;
                        (void)slot_getattr(econtext->ecxt_scantuple,
                                           runtime.match_sequence_attno,
                                           &target_is_null);
                        if (target_is_null)
                            continue;
                        runtime.buffer.submit_scan(econtext->ecxt_scantuple);
                    }
                    if (runtime.buffer.ready_to_dispatch()) {
                        if (runtime.buffer.run_batch_and_build_cursors()) {
                            bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                                runtime.alignment_registration, runtime.buffer.execution_stats());
                        }
                    }
                } else {
                    if (runtime.mmsearch_null_input) {
                        /* A NULL global Const/Param leaves the buffer inactive.  An
                         * evaluated target still has SQL effects and must run once. */
                        if (runtime.match_evaluated_submission &&
                            runtime.match_target_expr_state != nullptr) {
                            bool ignored_null = false;
                            (void)ExecEvalExprSwitchContext(
                                runtime.match_target_expr_state, econtext, &ignored_null);
                        }
                        continue;
                    }
                    /*
                     * No WHERE mmsearch(...) qual installed -> there is nothing
                     * to batch. Project the scan tuple to the CustomScan target
                     * list so the output slot layout matches what an outer Result
                     * node expects. PlanCustomPath filters this target list to
                     * plain Vars plus bioseq-owned placeholders; ordinary
                     * expressions such as seq::text and bioseq.len(seq) remain in
                     * the outer Result and evaluate against this projected tuple.
                     */
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: Inner_CustomExecScan() qual passed; no mmsearch qual");

                    /*
                     * symbol_count fast-path requires the row number; the
                     * access method set it on slot->tts_tid above. Convert
                     * once per projected row rather than stashing a
                     * (relid, rowno) in shared state.
                     */
                    {
                        int64_t row_number = -1;
                        if (ItemPointerIsValid(&slot->tts_tid)) {
                            row_number = static_cast<int64_t>(tid_to_row_number(slot->tts_tid));
                        }
                        populate_symbol_count_placeholders(
                            runtime.placeholder_consts,
                            node->ss_currentRelation,
                            row_number,
                            bioseq_scan_state->snapshot,
                            runtime.aligner_state.read_sequence_index);
                    }

                    ProjectionInfo* proj = ExecBuildProjectionInfo(runtime.newtargetlist,
                                                                   econtext,
                                                                   node->ps.ps_ResultTupleSlot,
                                                                   &(node->ps),
                                                                   slot->tts_tupleDescriptor);
                    TupleTableSlot* projected = ExecProject(proj);
                    if (runtime.locator_passthrough) {
                        if (!ItemPointerIsValid(&slot->tts_tid) ||
                            runtime.locator_rowno_resno <= 0 ||
                            runtime.locator_rowno_resno > projected->tts_tupleDescriptor->natts) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_INTERNAL_ERROR),
                                     errmsg("Bioseq locator passthrough row-number projection is "
                                            "invalid")));
                        }
                        Form_pg_attribute rowno_attribute = TupleDescAttr(
                            projected->tts_tupleDescriptor, runtime.locator_rowno_resno - 1);
                        if (rowno_attribute->atttypid != INT8OID) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                                     errmsg("Bioseq locator passthrough row-number column is not "
                                            "bigint")));
                        }
                        slot_getallattrs(projected);
                        projected->tts_values[runtime.locator_rowno_resno - 1] = Int64GetDatum(
                            static_cast<std::int64_t>(tid_to_row_number(slot->tts_tid)));
                        projected->tts_isnull[runtime.locator_rowno_resno - 1] = false;
                    }
                    return projected;
                }
            } else {
                InstrCountFiltered1(node, 1);
            }
        }
    }

    TupleTableSlot* custom_exec_scan(scan_state* bioseq_scan_state,
                                     ExecScanAccessMtd access_mtd,
                                     ExecScanRecheckMtd recheck_mtd) {

        // Testing custom exec scan
        bioseq_log<logging_level::debug>("[bioseq] pg: CustomExecScan() entered");

        ScanState* node = reinterpret_cast<ScanState*>(&bioseq_scan_state->custom_scan_state.ss);
        PlanState ps = node->ps;
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);

        bioseq_log<logging_level::debug>("[bioseq] pg: CustomExecScan() reading plan targetlist");
        auto targetlist = ps.plan->targetlist;
        bioseq_log<logging_level::debug>("[bioseq] pg: CustomExecScan() targetlist_is_null={}",
                                         targetlist == nullptr);
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: CustomExecScan() targetlist iteration complete");

        if (runtime.placeholder_consts.dp_score_node == nullptr) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: CustomExecScan() allocating placeholder const nodes");

            /* INT4: byval, len=4, no collation. Used for the int32_t match_result
             * fields (dp_score, mapq, chain_score, dp_max_score, seed_count,
             * secondary_chain_score). */
            const auto int_placeholder = []() {
                return makeConst(INT4OID,
                                 -1,
                                 InvalidOid,
                                 4,
                                 static_cast<Datum>(static_cast<int32_t>(0)),
                                 /*constisnull=*/true,
                                 /*constbyval=*/true);
            };
            /* INT8: byval on LP64 (pg_type.typbyval for bigint is 't'), len=8.
             * Used for the eight sequence_size_t (uint64) coordinate/length
             * fields (query_len … block_length) so positions in sequences longer
             * than INT32_MAX are not truncated. Matches the SQL
             * DDL where those eight projections RETURN BIGINT. */
            const auto bigint_placeholder = []() {
                return makeConst(
                    INT8OID,
                    -1,
                    InvalidOid,
                    8,
                    bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(0)).raw(),
                    /*constisnull=*/true,
                    /*constbyval=*/true);
            };
            /* BOOL: byval, len=1, no collation. */
            const auto bool_placeholder = []() {
                return makeConst(BOOLOID,
                                 -1,
                                 InvalidOid,
                                 1,
                                 bioseq::postgres::support::pg_datum::from(false).raw(),
                                 /*constisnull=*/true,
                                 /*constbyval=*/true);
            };
            /* FLOAT4: byval, len=4, no collation. */
            const auto float_placeholder = []() {
                return makeConst(FLOAT4OID,
                                 -1,
                                 InvalidOid,
                                 4,
                                 bioseq::postgres::support::pg_datum::from(0.0F).raw(),
                                 /*constisnull=*/true,
                                 /*constbyval=*/true);
            };
            /* TEXT: byref, len=-1 (varlena), default collation. */
            const auto text_placeholder = []() {
                return makeConst(TEXTOID,
                                 -1,
                                 DEFAULT_COLLATION_OID,
                                 -1,
                                 bioseq::postgres::support::pg_datum::from_pointer(nullptr).raw(),
                                 /*constisnull=*/true,
                                 /*constbyval=*/false);
            };

            runtime.placeholder_consts.symbol_count_sites.clear();
            runtime.placeholder_consts.dp_score_node = int_placeholder();
            runtime.placeholder_consts.query_len_node = bigint_placeholder();
            runtime.placeholder_consts.query_start_node = bigint_placeholder();
            runtime.placeholder_consts.query_end_node = bigint_placeholder();
            runtime.placeholder_consts.target_len_node = bigint_placeholder();
            runtime.placeholder_consts.target_start_node = bigint_placeholder();
            runtime.placeholder_consts.target_end_node = bigint_placeholder();
            runtime.placeholder_consts.num_matches_node = bigint_placeholder();
            runtime.placeholder_consts.block_length_node = bigint_placeholder();
            runtime.placeholder_consts.edit_distance_node = bigint_placeholder();
            runtime.placeholder_consts.mapq_node = int_placeholder();
            runtime.placeholder_consts.chain_score_node = int_placeholder();
            runtime.placeholder_consts.dp_max_score_node = int_placeholder();
            runtime.placeholder_consts.seed_count_node = int_placeholder();
            runtime.placeholder_consts.secondary_chain_score_node = int_placeholder();
            runtime.placeholder_consts.is_reverse_node = bool_placeholder();
            runtime.placeholder_consts.is_primary_node = bool_placeholder();
            runtime.placeholder_consts.has_cigar_node = bool_placeholder();
            runtime.placeholder_consts.divergence_node = float_placeholder();
            runtime.placeholder_consts.cigar_node = text_placeholder();

            /*
             * Reset only the slot buffer; keep the mmsearch_context that
             * BeginCustomScan configured earlier for this scan.
             */
            runtime.buffer.clear_slots();

            const bioseq_stub_oids& oids_snapshot = get_cached_stub_oids();
            var_injector_context injector_ctx;
            injector_ctx.consts = &runtime.placeholder_consts;
            injector_ctx.oids = &oids_snapshot;
            injector_ctx.current_relation =
                bioseq_scan_state->custom_scan_state.ss.ss_currentRelation;
            injector_ctx.match_sequence_varno = runtime.match_sequence_varno;
            injector_ctx.match_sequence_attno = runtime.match_sequence_attno;
            injector_ctx.match_target_recipe = runtime.match_target_recipe;
            injector_ctx.match_target_lineage = runtime.match_target_lineage;
            injector_ctx.match_helper_sites = runtime.match_helper_sites;
            injector_ctx.stable_match_id = runtime.stable_match_id;

            runtime.newtargetlist = reinterpret_cast<List*>(
                var_injector(reinterpret_cast<Node*>(targetlist), &injector_ctx));
            derive_mmsearch_source_lifetime_roles(
                runtime,
                ps.plan,
                bioseq_scan_state->custom_scan_state.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: CustomExecScan() dispatching to Inner_CustomExecScan");

        auto tuple_slot = inner_custom_exec_scan(bioseq_scan_state, access_mtd, recheck_mtd);
        return tuple_slot;
    }

    TupleTableSlot* bioseq_column_scan_next(scan_state* bioseq_scan_state) {
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScanNext() entered");
        CustomScanState* node = bioseq_scan_state->as_custom_scan_state();

        /*
         * get information from the estate and scan state
         */
        TableScanDesc scandesc = node->ss.ss_currentScanDesc;
        EState* estate = node->ss.ps.state;
        ScanDirection direction = estate->es_direction;
        const bool is_vectorization_enabled = bioseq_scan_state->vectorization.is_enabled;

        TupleTableSlot* slot = nullptr;

        if (is_vectorization_enabled)
            slot = bioseq_scan_state->vectorization.scan_vector_slot;
        else
            slot = node->ss.ss_ScanTupleSlot;

        if (scandesc != nullptr && scandesc->rs_rd == nullptr) {
            bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScanNext() found stale scan "
                                             "descriptor with null relation; reopening");
            node->ss.ss_currentScanDesc = nullptr;
            scandesc = nullptr;
        }

        if (scandesc == nullptr) {
            /* the columnar access method does not use the flags, they are specific to heap */
            uint32_t flags = 0;

            /*
             * We reach here if the scan is not parallel, or if we're serially
             * executing a scan that was planned to be parallel.
             */
            bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
            const bool needs_deferred_hydration =
                (runtime.locator_passthrough || runtime.lazy_source_projection ||
                 runtime.buffer.mmsearch_active() || runtime.param_rebind.active) &&
                !runtime.buffer.per_row_self_alignment();

            /*
             * The batched mmsearch and locator-passthrough paths
             * consume locator stubs directly.  A lazy SliceProject source also stays
             * deferred, then promotes only its bound columns through the owning
             * provider immediately before projection.  Plain scans may be projected
             * by an upper Result node, so they use the ordinary table-AM scan and
             * return fully hydrated bioseq.seq varlenas. Same-column self-alignment
             * (`mmsearch(seq, seq)`) stays non-deferred: submit_scan can copy the
             * hydrated value once as both target and per-row query, avoiding the cold
             * post-load descriptor path while preserving deferred hydration for real
             * target/query scans.
             */
            if (needs_deferred_hydration) {
                scandesc = bioseq_beginscan_deferred_c(node->ss.ss_currentRelation,
                                                       bioseq_scan_state->snapshot,
                                                       0,
                                                       nullptr,
                                                       nullptr,
                                                       flags,
                                                       bioseq_scan_state->attributes_needed.get(),
                                                       bioseq_scan_state->qualifiers.get(),
                                                       bioseq_scan_state->parallel_scan,
                                                       is_vectorization_enabled);
            } else {
                scandesc = bioseq_beginscan_extended_c(node->ss.ss_currentRelation,
                                                       bioseq_scan_state->snapshot,
                                                       0,
                                                       nullptr,
                                                       nullptr,
                                                       flags,
                                                       bioseq_scan_state->attributes_needed.get(),
                                                       bioseq_scan_state->qualifiers.get(),
                                                       bioseq_scan_state->parallel_scan,
                                                       is_vectorization_enabled);
            }

            node->ss.ss_currentScanDesc = scandesc;
        }

        if (scandesc == nullptr || scandesc->rs_rd == nullptr) {
            ereport(ERROR,
                    (errmsg("bioseq: failed to initialize columnar scan descriptor "
                            "for relation %u",
                            node->ss.ss_currentRelation == nullptr
                                ? InvalidOid
                                : RelationGetRelid(node->ss.ss_currentRelation))));
        }

        if (table_scan_getnextslot(scandesc, direction, slot)) {
            /*
             * Setup custom scan state for reading tuples directly from stored
             * vector tuple table slot
             */
            if (is_vectorization_enabled) {
                bioseq_scan_state->vectorization.pending_row_number =
                    reinterpret_cast<vector_tuple_table_slot_storage*>(slot)->row_count;
                bioseq_scan_state->vectorization.row_index = 0;
            }
            return slot;
        }

        return nullptr;
    }

    /*
     * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
     */
    bool bioseq_column_scan_recheck(scan_state* node, TupleTableSlot* slot) {
        return true;
    }

    TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node) {
        TupleTableSlot* result = nullptr;
        ErrorData* exact_error = nullptr;
        bool cpp_error = false;
        bool unknown_cpp_error = false;
        std::array<char, 512> detail{"std::exception without detail"};
        try {
            scan_state* state = scan_state::from_custom_scan_state(node);
            bioseq_scan_runtime& runtime = bioseq_runtime(state);
            if (runtime.mmsearch_null_input && !runtime.match_evaluated_submission) {
                /* A raw target with a NULL owned query is statement-empty.  An
                 * evaluated target must still run once for every residual-admitted row
                 * before strict NULL semantics discard the result, so only the raw
                 * owner may bypass the table scan here. */
                if (runtime.alignment_registration.is_registered()) {
                    bioseq::postgres::table::alignment_execution::mark_alignment_pass_fully_drained(
                        runtime.alignment_registration);
                }
                return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
            }
            bioseq::postgres::table::alignment_execution::ensure_alignment_node_registered(
                runtime.alignment_registration);
            result =
                custom_exec_scan(state,
                                 reinterpret_cast<ExecScanAccessMtd>(bioseq_column_scan_next),
                                 reinterpret_cast<ExecScanRecheckMtd>(bioseq_column_scan_recheck));
            return result;
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

        scan_state* state = scan_state::from_custom_scan_state(node);
        if (state != nullptr && state->runtime_state != nullptr) {
            auto* runtime = static_cast<bioseq_scan_runtime*>(state->runtime_state);
            runtime->buffer.abandon_join_runtime_after_pg_error_noexcept();
        }
        if (exact_error != nullptr) {
            ReThrowError(exact_error);
        }
        if (cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Exec raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        if (unknown_cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Exec raised an unknown C++ exception")));
        }
        return result;
    }

    void bioseq_column_scan_end_custom_scan(CustomScanState* node) {
        TableScanDesc scan_desc = node->ss.ss_currentScanDesc;
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);

        /*
         * Profiler summary lives here (not inside the per-batch dispatch
         * fork) so multi-batch scans emit exactly one block of summary
         * lines. After dumping, reset_all() clears the persistent
         * per-process aggregate so the next scan starts with fresh counts.
         * Part 48.5 / 48.8 of NEW_PROFILING_RESEARCH.md.
         */
        if (bioseq::aligner::settings::profiling::enabled) {
            bioseq::instrumentation::summary::write_summary_to_log();
            bioseq::instrumentation::summary::reset_metrics();
        }

        /*
         * Free buffered scan slots HERE, while the per-query ResourceOwner
         * that pinned their TupleDesc refs is still active. Deferring this
         * to the next query's init would leak TupleDesc refcounts onto a
         * dead ResourceOwner and produce
         *   "WARNING: resource was not closed: TupleDesc ..."
         * followed by
         *   "ERROR: tupdesc reference ... is not owned by resource owner Portal"
         * when the next query tries to release them.
         *
         * After clear_slots, free the bioseq_scan_runtime itself. Owning the
         * std::vector / std::map storage on the C++ heap (rather than in a
         * Postgres MemoryContext) means we have to reach the destructor
         * here — Postgres' MemoryContext teardown wouldn't run them.
         */
        if (bioseq_scan_state->runtime_state != nullptr) {
            bioseq_scan_runtime* runtime_ptr =
                static_cast<bioseq_scan_runtime*>(bioseq_scan_state->runtime_state);
            /*
             * Clear MinimalTuple slots while the per-query ResourceOwner
             * is still alive — Postgres tuple-desc refcounts are tracked
             * against it. The MemoryContext reset callback fires later
             * (during es_query_cxt teardown) and is forbidden from
             * touching slots, so we MUST do this here.
             *
             * We deliberately do NOT delete the runtime here. The
             * MemoryContextCallback struct that bioseq_runtime_cleanup_callback
             * uses lives inside the runtime; freeing the runtime now would
             * dangle the callback pointer in es_query_cxt's reset chain
             * and SIGSEGV on context teardown. The callback handles the
             * delete safely once the context begins to tear down.
             */
            /* Drop every tuple-visible reference before resetting the expression
             * contexts that own promoted EOHs. Result and scan slots may alias in a
             * projection-free shape, so clear the physical slot exactly once. */
            clear_recipe_scan_parent_tuple_references(node);

            /* Locator promotion uses the ordinary executor ExprContext. The private
             * runtime context can independently retain evaluated Params, so drain it
             * too while the exact source provider remains live. */
            ExprContext* expression_context = node->ss.ps.ps_ExprContext;
            if (bioseq_scan_state->runtime_expression_context != nullptr &&
                bioseq_scan_state->runtime_expression_context != expression_context) {
                ReScanExprContext(bioseq_scan_state->runtime_expression_context);
            }

            /* Tuple-visible EOH references are gone; the retained batch arena may
             * now release its wrappers and source-domain edges. */
            runtime_ptr->buffer.clear_slots();

            if (runtime_ptr->lazy_source_provider != nullptr) {
                bioseq::storage::close_pg_sequence_source_provider(
                    runtime_ptr->lazy_source_provider, /*is_commit=*/true);
                runtime_ptr->lazy_source_provider = nullptr;
            }
            runtime_ptr->buffer.clear_sequence_source_provider_binding();
            if (runtime_ptr->lazy_trace_attachment != nullptr) {
                release_slice_trace_attachment(runtime_ptr->lazy_trace_attachment);
                runtime_ptr->lazy_trace_attachment = nullptr;
            }
            runtime_ptr->cleanup_done = true;
        }

        /*
         * Cleanup BMS of selected scan attributes
         */
        bioseq_scan_state->attributes_needed.clear();

        /* A failed/partial Begin may not have allocated runtime state. */
        if (bioseq_scan_state->runtime_state == nullptr) {
            TupleTableSlot* result_slot = node->ss.ps.ps_ResultTupleSlot;
            TupleTableSlot* scan_slot = node->ss.ss_ScanTupleSlot;
            if (result_slot != nullptr)
                ExecClearTuple(result_slot);
            if (scan_slot != nullptr && scan_slot != result_slot) {
                ExecClearTuple(scan_slot);
            }
        }

        /*
         * close heap scan
         */
        if (scan_desc != nullptr) {
            table_endscan(scan_desc);
            node->ss.ss_currentScanDesc = nullptr;
        }

        /* Unregister snapshot */
        if (bioseq_scan_state->owns_snapshot_registration) {
            UnregisterSnapshot(bioseq_scan_state->snapshot);
        }
    }

    void bioseq_column_scan_rescan_custom_scan(CustomScanState* node) {
        CustomScan* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
        bioseq::postgres::table::alignment_execution::record_alignment_rescan(
            runtime.alignment_registration);

        clear_recipe_scan_parent_tuple_references(node);
        if (bioseq_scan_state->runtime_expression_context != node->ss.ps.ps_ExprContext) {
            ResetExprContext(bioseq_scan_state->runtime_expression_context);
        }
        List* all_clauses = postgres_list<List>(cscan->custom_exprs).get_second();
        List* storage_clauses =
            storage_prunable_quals_without_mmsearch(all_clauses, get_cached_stub_oids());
        bioseq_scan_state->qualifiers = bioseq::postgres::support::postgres_list<Node>{
            reinterpret_cast<List*>(eval_params(reinterpret_cast<Node*>(storage_clauses),
                                                bioseq_scan_state->runtime_expression_context))};

        /*
         * Drop every buffered tuple, output cursor and match map left over
         * from the previous scan iteration. Without this, an inner-side
         * scan that drained its cursor on iteration N would re-enter the reading
         * stage immediately on iteration N+1 (because output_position stayed
         * pointing past the cursor end) and return EOF without ever pulling
         * a tuple from the access method. ReScan is the natural reset
         * point: BeginCustomScan only fires once per query, so we cannot
         * defer this to there.
         */
        runtime.buffer.clear_slots();
        runtime.source_exhausted = false;

        /*
         * Param-driven mmsearch rebind: when the planner rewrote the
         * second mmsearch argument as a Param (parameterized inner-side
         * scan in a Nested Loop), each rescan needs to install the current
         * Param value as the new query. The Param lives in the scan
         * node's PlanState ExprContext (filled by the NL parent before
         * triggering rescan), so we resolve it via EvalParamsMutator
         * against a copy of the original FuncExpr argument.
         *
         * mmsearch_parameter_rebind retains the exact plan FuncExpr because the copied
         * storage qual has already folded Params and planner order may put a Const
         * extra before the Param-driven primary.
         */
        if (runtime.param_rebind.active) {
            /* The stashed FuncExpr identifies the exact primary Param even when
             * planner qual ordering puts a Const extra first.  NULL clears only
             * the primary context; BMO rejects NULL Const extras during planning,
             * while registered non-NULL extras survive for a later non-NULL rescan. */
            (void)resolve_mmsearch_param_query(
                runtime, node->ss.ps.ps_ExprContext, "ReScanCustomScan");
        }

        TableScanDesc scan_desc = node->ss.ss_currentScanDesc;

        if (scan_desc != nullptr) {
            /* XXX: hack to pass quals as scan keys */
            ScanKey scan_keys = reinterpret_cast<ScanKey>(bioseq_scan_state->qualifiers.get());
            table_rescan(node->ss.ss_currentScanDesc, scan_keys);
        }
    }

} // export custom-scan partition declarations
