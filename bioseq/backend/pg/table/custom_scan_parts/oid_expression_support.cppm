/**
 * @file
 * @brief Resolves Bioseq catalog identities and validates planner expression ownership.
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

export module bioseq.postgres.table:custom_scan_oid_expression_support;

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

import :custom_scan_runtime_support;

using namespace bioseq::postgres::support;

export {

    /*
     * OIDs looked up from pg_proc for the bioseq extension's SQL stubs.
     * Resolved on demand via LookupStubOids() and cached by
     * get_cached_stub_oids() for the lifetime of a backend. The cache is
     * flushed on any relcache invalidation (which covers DROP/CREATE
     * EXTENSION re-registering the pg_proc rows with new OIDs).
     */
    struct bioseq_stub_oids {
        Oid mmsearch_oid;
        Oid mmsearch_with_opts_oid; // mmsearch(bioseq.seq, bioseq.seq, bioseq_aligner_options)
        Oid bioseq_seq_type_oid;    // resolves to bioseq.seq pg_type row
        Oid bioseq_aligner_options_type_oid;

        Oid score_oid; // bioseq.score (alias for dp_score)
        Oid dp_score_oid;
        Oid query_len_oid;
        Oid query_start_oid;
        Oid query_end_oid;
        Oid target_len_oid;
        Oid target_start_oid;
        Oid target_end_oid;
        Oid num_matches_oid;
        Oid block_length_oid;
        Oid edit_oid;
        Oid mapq_oid;
        Oid chain_score_oid;
        Oid dp_max_score_oid;
        Oid seed_count_oid;
        Oid secondary_chain_score_oid;
        Oid is_reverse_oid;
        Oid is_primary_oid;
        Oid has_cigar_oid;
        Oid divergence_oid;
        Oid cigar_oid;
        Oid symbol_count_oid;
        /* Lazy sequence projection functions are resolved by exact
         * signatures; the planner never classifies an operation by its name. */
        Oid seq_concat_oid;
        Oid seq_concat_text_oid;
        Oid text_concat_seq_oid;
        Oid seq_concat_varchar_oid;
        Oid varchar_concat_seq_oid;
        Oid seq_concat_text_array_oid;
        Oid text_array_concat_seq_oid;
        Oid seq_concat_letter_array_oid;
        Oid letter_array_concat_seq_oid;
        Oid seq_reverse_oid;
        Oid seq_reverse_complement_oid;
        Oid seq_array_cat_oid;
        Oid seq_array_length_oid;
        Oid seq_cardinality_oid;
        Oid seq_to_text_oid;
        Oid seq_to_varchar_oid;
        Oid seq_to_array_oid;
        Oid seq_from_letter_array_infer_oid;
        Oid seq_from_letter_array_named_oid;
        Oid seq_len_oid;
        Oid seq_cast_oid;
    };

    /*
     * The create_upper_paths hook is live while CREATE EXTENSION is still
     * executing.  Its catalog surface is therefore allowed to be absent or only
     * partially installed.  These missing-ok probes are deliberately separate
     * from LookupStubOids(): the executor/planner hot paths retain the exact,
     * throwing lookup once the complete surface is validated and ready.
     */
    [[nodiscard]] Oid lookup_defined_type_missing_ok(const char* schema_name,
                                                     const char* type_name) {
        const Oid namespace_oid = get_namespace_oid(schema_name, /*missing_ok=*/true);
        if (!OidIsValid(namespace_oid))
            return InvalidOid;
        const Oid type_oid = GetSysCacheOid2(TYPENAMENSP,
                                             Anum_pg_type_oid,
                                             PointerGetDatum(type_name),
                                             ObjectIdGetDatum(namespace_oid));
        if (!OidIsValid(type_oid))
            return InvalidOid;
        HeapTuple tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
        if (!HeapTupleIsValid(tuple))
            return InvalidOid;
        const bool is_defined = reinterpret_cast<Form_pg_type>(GETSTRUCT(tuple))->typisdefined;
        ReleaseSysCache(tuple);
        return is_defined ? type_oid : InvalidOid;
    }

    [[nodiscard]] std::optional<std::string> bioseq_extension_schema_missing_ok() {
        const Oid extension_oid = get_extension_oid("bioseq", /*missing_ok=*/true);
        if (!OidIsValid(extension_oid))
            return std::nullopt;
        const Oid namespace_oid = get_extension_schema(extension_oid);
        if (!OidIsValid(namespace_oid))
            return std::nullopt;
        char* raw_name = get_namespace_name(namespace_oid);
        if (raw_name == nullptr)
            return std::nullopt;
        std::string result{raw_name};
        pfree(raw_name);
        return result;
    }

    [[nodiscard]] Oid exact_function_oid(const char* schema_name,
                                         const char* function_name,
                                         std::initializer_list<Oid> argument_types,
                                         bool missing_ok) {
        for (const Oid argument_type : argument_types) {
            if (!OidIsValid(argument_type))
                return InvalidOid;
        }
        List* qualified_name =
            list_make2(makeString(pstrdup(schema_name)), makeString(pstrdup(function_name)));
        const Oid function_oid = LookupFuncName(qualified_name,
                                                static_cast<int>(argument_types.size()),
                                                argument_types.begin(),
                                                missing_ok);
        list_free_deep(qualified_name);
        return function_oid;
    }

    [[nodiscard]] bool exact_function_exists_missing_ok(const char* schema_name,
                                                        const char* function_name,
                                                        std::initializer_list<Oid> argument_types) {
        return OidIsValid(exact_function_oid(schema_name,
                                             function_name,
                                             argument_types,
                                             /*missing_ok=*/true));
    }

    [[nodiscard]] bool bioseq_stub_oid_surface_ready() {
        const std::optional<std::string> extension_schema = bioseq_extension_schema_missing_ok();
        if (!extension_schema)
            return false;
        const Oid seq_oid = lookup_defined_type_missing_ok("bioseq", "seq");
        const Oid letter_oid = lookup_defined_type_missing_ok("bioseq", "letter");
        const Oid options_oid =
            lookup_defined_type_missing_ok(extension_schema->c_str(), "bioseq_aligner_options");
        const Oid letter_array_oid =
            OidIsValid(letter_oid) ? get_array_type(letter_oid) : InvalidOid;
        if (!OidIsValid(seq_oid) || !OidIsValid(letter_oid) || !OidIsValid(options_oid) ||
            !OidIsValid(letter_array_oid)) {
            return false;
        }

        if (!exact_function_exists_missing_ok(
                extension_schema->c_str(), "mmsearch", {seq_oid, seq_oid}) ||
            !exact_function_exists_missing_ok(
                extension_schema->c_str(), "mmsearch", {seq_oid, seq_oid, options_oid}) ||
            !exact_function_exists_missing_ok("bioseq", "symbol_count", {seq_oid, TEXTOID})) {
            return false;
        }

        constexpr std::array<const char*, 21> projection_functions{
            "score",       "dp_score",     "query_len",    "query_start",
            "query_end",   "target_len",   "target_start", "target_end",
            "num_matches", "block_length", "edit",         "mapq",
            "chain_score", "dp_max_score", "seed_count",   "secondary_chain_score",
            "is_reverse",  "is_primary",   "has_cigar",    "divergence",
            "cigar"};
        for (const char* function_name : projection_functions) {
            if (!exact_function_exists_missing_ok("bioseq", function_name, {ANYELEMENTOID})) {
                return false;
            }
        }

        return exact_function_exists_missing_ok("bioseq", "seq_concat", {seq_oid, seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "seq_concat_text", {seq_oid, TEXTOID}) &&
               exact_function_exists_missing_ok("bioseq", "text_concat_seq", {TEXTOID, seq_oid}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "seq_concat_varchar", {seq_oid, VARCHAROID}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "varchar_concat_seq", {VARCHAROID, seq_oid}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "seq_concat_text_array", {seq_oid, TEXTARRAYOID}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "text_array_concat_seq", {TEXTARRAYOID, seq_oid}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "seq_concat_letter_array", {seq_oid, letter_array_oid}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "letter_array_concat_seq", {letter_array_oid, seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "reverse", {seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "reverse_complement", {seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "array_cat", {seq_oid, seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "array_length", {seq_oid, INT4OID}) &&
               exact_function_exists_missing_ok("bioseq", "cardinality", {seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "to_text", {seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "to_varchar", {seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "to_array", {seq_oid}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "from_array", {letter_array_oid, INT4OID}) &&
               exact_function_exists_missing_ok(
                   "bioseq", "from_array", {letter_array_oid, TEXTOID, INT4OID}) &&
               exact_function_exists_missing_ok("bioseq", "len", {seq_oid}) &&
               exact_function_exists_missing_ok("bioseq", "seq_cast", {seq_oid, INT4OID, BOOLOID});
    }

    /*
     * LookupStubOids resolves all bioseq stub OIDs via regprocedurein,
     * which raises ereport(ERROR, ...) if any function is missing. We
     * assume CREATE EXTENSION bioseq has already succeeded before any
     * path below runs. Prefer get_cached_stub_oids() on the hot path —
     * this builder is kept public so the cache can repopulate on
     * relcache invalidation.
     */
    [[nodiscard]] bioseq_stub_oids lookup_stub_oids() {
        bioseq_stub_oids result{};
        auto resolve = [](const char* signature) {
            return DatumGetObjectId(
                DirectFunctionCall1(regprocedurein, CStringGetDatum(signature)));
        };
        auto resolve_type = [](const char* type_name) {
            return DatumGetObjectId(DirectFunctionCall1(regtypein, CStringGetDatum(type_name)));
        };
        const std::optional<std::string> extension_schema = bioseq_extension_schema_missing_ok();
        if (!extension_schema) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("bioseq extension installation schema is unavailable")));
        }
        const Oid seq_type_oid = resolve_type("bioseq.seq");
        const Oid aligner_options_type_oid =
            lookup_defined_type_missing_ok(extension_schema->c_str(), "bioseq_aligner_options");
        if (!OidIsValid(aligner_options_type_oid)) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("bioseq aligner options type is unavailable")));
        }
        result.mmsearch_oid = exact_function_oid(extension_schema->c_str(),
                                                 "mmsearch",
                                                 {seq_type_oid, seq_type_oid},
                                                 /*missing_ok=*/false);
        result.mmsearch_with_opts_oid =
            exact_function_oid(extension_schema->c_str(),
                               "mmsearch",
                               {seq_type_oid, seq_type_oid, aligner_options_type_oid},
                               /*missing_ok=*/false);
        result.bioseq_seq_type_oid = seq_type_oid;
        result.bioseq_aligner_options_type_oid = aligner_options_type_oid;
        result.score_oid = resolve("bioseq.score(anyelement)");
        result.dp_score_oid = resolve("bioseq.dp_score(anyelement)");
        result.query_len_oid = resolve("bioseq.query_len(anyelement)");
        result.query_start_oid = resolve("bioseq.query_start(anyelement)");
        result.query_end_oid = resolve("bioseq.query_end(anyelement)");
        result.target_len_oid = resolve("bioseq.target_len(anyelement)");
        result.target_start_oid = resolve("bioseq.target_start(anyelement)");
        result.target_end_oid = resolve("bioseq.target_end(anyelement)");
        result.num_matches_oid = resolve("bioseq.num_matches(anyelement)");
        result.block_length_oid = resolve("bioseq.block_length(anyelement)");
        result.edit_oid = resolve("bioseq.edit(anyelement)");
        result.mapq_oid = resolve("bioseq.mapq(anyelement)");
        result.chain_score_oid = resolve("bioseq.chain_score(anyelement)");
        result.dp_max_score_oid = resolve("bioseq.dp_max_score(anyelement)");
        result.seed_count_oid = resolve("bioseq.seed_count(anyelement)");
        result.secondary_chain_score_oid = resolve("bioseq.secondary_chain_score(anyelement)");
        result.is_reverse_oid = resolve("bioseq.is_reverse(anyelement)");
        result.is_primary_oid = resolve("bioseq.is_primary(anyelement)");
        result.has_cigar_oid = resolve("bioseq.has_cigar(anyelement)");
        result.divergence_oid = resolve("bioseq.divergence(anyelement)");
        result.cigar_oid = resolve("bioseq.cigar(anyelement)");
        result.symbol_count_oid = resolve("bioseq.symbol_count(bioseq.seq, text)");
        result.seq_concat_oid = resolve("bioseq.seq_concat(bioseq.seq,bioseq.seq)");
        result.seq_concat_text_oid = resolve("bioseq.seq_concat_text(bioseq.seq,text)");
        result.text_concat_seq_oid = resolve("bioseq.text_concat_seq(text,bioseq.seq)");
        result.seq_concat_varchar_oid =
            resolve("bioseq.seq_concat_varchar(bioseq.seq,character varying)");
        result.varchar_concat_seq_oid =
            resolve("bioseq.varchar_concat_seq(character varying,bioseq.seq)");
        result.seq_concat_text_array_oid =
            resolve("bioseq.seq_concat_text_array(bioseq.seq,text[])");
        result.text_array_concat_seq_oid =
            resolve("bioseq.text_array_concat_seq(text[],bioseq.seq)");
        result.seq_concat_letter_array_oid =
            resolve("bioseq.seq_concat_letter_array(bioseq.seq,bioseq.letter[])");
        result.letter_array_concat_seq_oid =
            resolve("bioseq.letter_array_concat_seq(bioseq.letter[],bioseq.seq)");
        result.seq_reverse_oid = resolve("bioseq.reverse(bioseq.seq)");
        result.seq_reverse_complement_oid = resolve("bioseq.reverse_complement(bioseq.seq)");
        result.seq_array_cat_oid = resolve("bioseq.array_cat(bioseq.seq,bioseq.seq)");
        result.seq_array_length_oid = resolve("bioseq.array_length(bioseq.seq,integer)");
        result.seq_cardinality_oid = resolve("bioseq.cardinality(bioseq.seq)");
        result.seq_to_text_oid = resolve("bioseq.to_text(bioseq.seq)");
        result.seq_to_varchar_oid = resolve("bioseq.to_varchar(bioseq.seq)");
        result.seq_to_array_oid = resolve("bioseq.to_array(bioseq.seq)");
        result.seq_from_letter_array_infer_oid =
            resolve("bioseq.from_array(bioseq.letter[],integer)");
        result.seq_from_letter_array_named_oid =
            resolve("bioseq.from_array(bioseq.letter[],text,integer)");
        result.seq_len_oid = resolve("bioseq.len(bioseq.seq)");
        result.seq_cast_oid = resolve("bioseq.seq_cast(bioseq.seq,integer,boolean)");
        return result;
    }

    /*
     * Per-backend cache for bioseq_stub_oids. The prior implementation called
     * LookupStubOids() from every planner path entry (AddBioseqColumnScanPath,
     * CustomExecScan first-call init, ExplainCustomScan) — that is ~22
     * regprocedurein syscache hits per query. The cache is backed by pg_proc and
     * pg_type syscache invalidation, plus relcache invalidation for relation-driven
     * replans, so DROP/CREATE EXTENSION in one backend flushes the resolved OIDs
     * before the next lookup.
     */
    struct cached_stub_oids {
        bioseq_stub_oids oids{};
        bool initialised = false;
    };
    cached_stub_oids g_cached_stub_oids{};

    void invalidate_cached_stub_oids(Datum /*arg*/, Oid /*relid*/) {
        g_cached_stub_oids.initialised = false;
    }

    void
    invalidate_cached_stub_oids_syscache(Datum /*arg*/, int /*cacheid*/, uint32 /*hashvalue*/) {
        g_cached_stub_oids.initialised = false;
    }

    [[nodiscard]] bool oid_exists_in_syscache(int cache_id, Oid oid) {
        if (!OidIsValid(oid)) {
            return false;
        }
        HeapTuple tuple =
            SearchSysCache1(cache_id, bioseq::postgres::support::pg_datum::from_oid(oid).raw());
        if (!HeapTupleIsValid(tuple)) {
            return false;
        }
        ReleaseSysCache(tuple);
        return true;
    }

    [[nodiscard]] bool cached_stub_oids_are_valid(const bioseq_stub_oids& o) {
        const auto func_oids = std::to_array<Oid>({
            o.mmsearch_oid,
            o.mmsearch_with_opts_oid,
            o.score_oid,
            o.dp_score_oid,
            o.query_len_oid,
            o.query_start_oid,
            o.query_end_oid,
            o.target_len_oid,
            o.target_start_oid,
            o.target_end_oid,
            o.num_matches_oid,
            o.block_length_oid,
            o.edit_oid,
            o.mapq_oid,
            o.chain_score_oid,
            o.dp_max_score_oid,
            o.seed_count_oid,
            o.secondary_chain_score_oid,
            o.is_reverse_oid,
            o.is_primary_oid,
            o.has_cigar_oid,
            o.divergence_oid,
            o.cigar_oid,
            o.symbol_count_oid,
            o.seq_concat_oid,
            o.seq_concat_text_oid,
            o.text_concat_seq_oid,
            o.seq_concat_varchar_oid,
            o.varchar_concat_seq_oid,
            o.seq_concat_text_array_oid,
            o.text_array_concat_seq_oid,
            o.seq_concat_letter_array_oid,
            o.letter_array_concat_seq_oid,
            o.seq_reverse_oid,
            o.seq_reverse_complement_oid,
            o.seq_array_cat_oid,
            o.seq_array_length_oid,
            o.seq_cardinality_oid,
            o.seq_to_text_oid,
            o.seq_to_varchar_oid,
            o.seq_to_array_oid,
            o.seq_from_letter_array_infer_oid,
            o.seq_from_letter_array_named_oid,
            o.seq_len_oid,
            o.seq_cast_oid,
        });
        for (const Oid oid : func_oids) {
            if (!oid_exists_in_syscache(PROCOID, oid)) {
                return false;
            }
        }
        return oid_exists_in_syscache(TYPEOID, o.bioseq_seq_type_oid) &&
               oid_exists_in_syscache(TYPEOID, o.bioseq_aligner_options_type_oid);
    }

    [[nodiscard]] const bioseq_stub_oids& get_cached_stub_oids() {
        if (g_cached_stub_oids.initialised && cached_stub_oids_are_valid(g_cached_stub_oids.oids)) {
            return g_cached_stub_oids.oids;
        }
        static bool invalidation_registered = false;
        if (!invalidation_registered) {
            CacheRegisterRelcacheCallback(invalidate_cached_stub_oids, PointerGetDatum(nullptr));
            CacheRegisterSyscacheCallback(
                PROCOID, invalidate_cached_stub_oids_syscache, PointerGetDatum(nullptr));
            CacheRegisterSyscacheCallback(
                TYPEOID, invalidate_cached_stub_oids_syscache, PointerGetDatum(nullptr));
            invalidation_registered = true;
        }
        g_cached_stub_oids.oids = lookup_stub_oids();
        g_cached_stub_oids.initialised = true;
        return g_cached_stub_oids.oids;
    }

    /* Planner hooks use this entrypoint while CREATE/DROP EXTENSION can expose a
     * partial catalog surface. A partial probe neither calls the throwing lookup
     * nor marks the cache initialized. */
    [[nodiscard]] const bioseq_stub_oids* try_get_cached_stub_oids() {
        if (g_cached_stub_oids.initialised && cached_stub_oids_are_valid(g_cached_stub_oids.oids)) {
            return &g_cached_stub_oids.oids;
        }
        g_cached_stub_oids.initialised = false;
        if (!bioseq_stub_oid_surface_ready())
            return nullptr;
        return &get_cached_stub_oids();
    }

    /* Bitmapsets of relation ids contain only concrete range-table indexes.
     * PostgreSQL 17 represents INNER/OUTER/INDEX/ROWID_VAR with negative special
     * varnos (stored through the unsigned Index type), so every Var consumer must
     * apply this predicate before passing varno to bms_is_member() or rt_fetch(). */
    [[nodiscard]] constexpr bool bioseq_is_concrete_rte_varno(const Index varno) noexcept {
        return varno != 0 && !IS_SPECIAL_VARNO(varno);
    }
    static_assert(bioseq_is_concrete_rte_varno(1));
    static_assert(!bioseq_is_concrete_rte_varno(0));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(INNER_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(OUTER_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(INDEX_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(ROWID_VAR)));

    /*
     * Classifier used by planner ownership and BeginCustomScan discovery. Returns
     * true if funcid
     * matches any of the bioseq projection stubs (i.e. anything that
     * VarInjectorMutator will rewrite into a placeholder Const).
     */
    /*
     * Accepts either the 2-arg mmsearch(seq, seq) or the 3-arg
     * mmsearch(seq, seq, bioseq_aligner_options). Call sites care about
     * "is this a match qual" rather than the arity, so funnelling both
     * overloads here avoids scattering the same disjunction everywhere.
     */
    [[nodiscard]] inline bool is_mmsearch_funcid(Oid funcid, const bioseq_stub_oids& o) {
        return funcid == o.mmsearch_oid || funcid == o.mmsearch_with_opts_oid;
    }

    struct raw_mmsearch_options_validation {
        const bioseq_stub_oids* oids = nullptr;
    };

    [[nodiscard]] inline bool is_volatile_sql_function(const Oid function_oid) {
        HeapTuple procedure_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(function_oid));
        if (!HeapTupleIsValid(procedure_tuple))
            return false;
        const Form_pg_proc procedure = reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple));
        const bool result =
            procedure->provolatile == PROVOLATILE_VOLATILE && procedure->prolang == SQLlanguageId;
        ReleaseSysCache(procedure_tuple);
        return result;
    }

    inline bool contains_volatile_sql_function_node(Node* node, void* context);

    [[nodiscard]] inline bool contains_volatile_sql_function_query(Query* query, void* context) {
        return query != nullptr &&
               query_tree_walker(
                   query, contains_volatile_sql_function_node, context, QTW_EXAMINE_RTES_BEFORE);
    }

    [[nodiscard]] inline bool contains_volatile_sql_function_node(Node* node, void* context) {
        if (node == nullptr)
            return false;
        if (FuncExpr* expression = try_cast_pg_node<FuncExpr>(node);
            expression != nullptr && is_volatile_sql_function(expression->funcid))
            return true;
        if (IsA(node, RangeTblEntry)) {
            /* query_tree_walker owns traversal of RTE expressions and subqueries. */
            return false;
        }
        if (Query* query = try_cast_pg_node<Query>(node)) {
            return contains_volatile_sql_function_query(query, context);
        }
        return expression_tree_walker(node, contains_volatile_sql_function_node, context);
    }

    /* Identify only volatile SQL functions whose wrapper provenance can be lost
     * when standard_planner simplifies their options subtree.  This catalog walk
     * inspects nodes without invoking any expression. */
    [[nodiscard]] inline bool contains_volatile_sql_function(Node* expression) {
        return contains_volatile_sql_function_node(expression, nullptr);
    }

    inline bool validate_raw_mmsearch_options_node(Node* node,
                                                   raw_mmsearch_options_validation* validation);

    inline void validate_raw_mmsearch_options_query(Query* query,
                                                    raw_mmsearch_options_validation* validation) {
        if (query == nullptr) {
            return;
        }
        (void)query_tree_walker(
            query, validate_raw_mmsearch_options_node, validation, QTW_EXAMINE_RTES_BEFORE);
    }

    inline bool validate_raw_mmsearch_options_node(Node* node,
                                                   raw_mmsearch_options_validation* validation) {
        if (node == nullptr) {
            return false;
        }
        if (FuncExpr* expression = try_cast_pg_node<FuncExpr>(node);
            expression != nullptr && is_mmsearch_funcid(expression->funcid, *validation->oids) &&
            expression->funcid == validation->oids->mmsearch_with_opts_oid &&
            list_length(expression->args) == 3) {
            Node* options = reinterpret_cast<Node*>(list_nth(expression->args, 2));
            if (contains_volatile_sql_function(options)) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("mmsearch qualifier is not owned by a Bioseq executor"),
                     errdetail("Volatile aligner options cannot be reduced to a statement-stable "
                               "executor-owned value."),
                     errhint(
                         "Pass a constant bioseq_aligner_options value as the third argument.")));
            }
        }
        if (IsA(node, RangeTblEntry)) {
            /* query_tree_walker owns traversal of RTE expressions and subqueries. */
            return false;
        }
        if (IsA(node, Query)) {
            validate_raw_mmsearch_options_query(castNode(Query, node), validation);
            return false;
        }
        return expression_tree_walker(node, validate_raw_mmsearch_options_node, validation);
    }

    /** @brief Reject volatile raw options before planner simplification removes their provenance.
     *
     * Called by the top-level planner hook while the analyzed Query still retains
     * SQL-function volatility.  standard_planner simplification may erase that
     * provenance by replacing the wrapper with its constant body. */
    void bioseq_validate_raw_mmsearch_options(Query* query) {
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (query == nullptr || oids == nullptr) {
            return;
        }
        raw_mmsearch_options_validation validation{oids};
        validate_raw_mmsearch_options_query(query, &validation);
    }

    struct mmsearch_qualifier_walker {
        const bioseq_stub_oids* oids = nullptr;
    };

    struct mmsearch_qualifier_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              mmsearch_qualifier_walker& data) const {
            if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                if (is_mmsearch_funcid(fe->funcid, *data.oids)) {
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool contains_mmsearch_qualifier(Node* node, const bioseq_stub_oids& oids) {
        mmsearch_qualifier_walker data{&oids};
        mmsearch_qualifier_visitor visitor;
        return walk_pg_expression_including_root(node, data, visitor);
    }

    struct mmsearch_occurrence_counter {
        const bioseq_stub_oids* oids = nullptr;
        std::size_t count = 0;
    };

    struct mmsearch_occurrence_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              mmsearch_occurrence_counter& data) const {
            if (FuncExpr* fe = node.try_cast<FuncExpr>();
                fe != nullptr && is_mmsearch_funcid(fe->funcid, *data.oids)) {
                ++data.count;
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] std::size_t count_mmsearch_occurrences(Node* node, const bioseq_stub_oids& oids) {
        mmsearch_occurrence_counter data{&oids, 0};
        mmsearch_occurrence_visitor visitor;
        walk_pg_expression_including_root(node, data, visitor);
        return data.count;
    }

    struct owned_mmsearch_restriction {
        RestrictInfo* restriction = nullptr;
        FuncExpr* normalized_expr = nullptr;
        List* extra_restrictions = NIL;
        List* extra_normalized_exprs = NIL;
    };

    /* Cheap prefilter used by the early clause-ownership pass.  The complete
     * grammar/nulling/type validation lives beside the per-row recipe validator
     * (which
     * is declared later in this translation unit); this predicate only keeps a
     * potentially base-local sequence expression alive until that validation can
     * run in AddBioseqColumnScanPath.  Volatility inside an operand is deliberately
     * not rejected: the expression remains at its original baserel frontier and is
     * evaluated once by its authoritative ExprState. */
    [[nodiscard]] bool coarse_base_mmsearch_operand_candidate(Node* expression,
                                                              RelOptInfo* rel,
                                                              const bioseq_stub_oids& oids,
                                                              const bool require_sequence_var) {
        if (expression == nullptr || rel == nullptr ||
            exprType(expression) != oids.bioseq_seq_type_oid ||
            expression_returns_set(expression) || contain_subplans(expression))
            return false;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* variables = pull_var_clause(expression, pull_flags);
        bool saw_sequence_var = false;
        bool valid = true;
        for (Var* variable : postgres_list<Var>(variables)) {
            if (variable == nullptr || !bioseq_is_concrete_rte_varno(variable->varno) ||
                variable->varno != rel->relid || variable->varattno <= 0 ||
                variable->varlevelsup != 0 || !bms_is_empty(variable->varnullingrels)) {
                valid = false;
                break;
            }
            if (variable->vartype == oids.bioseq_seq_type_oid) {
                saw_sequence_var = true;
            }
        }
        list_free(variables);
        return valid && (!require_sequence_var || saw_sequence_var);
    }

    [[nodiscard]] bool bounded_batch_fallback_options_are_supported(FuncExpr* primary,
                                                                    FuncExpr* extra,
                                                                    const bioseq_stub_oids& oids);

    /*
     * Choose the bounded RestrictInfo set the custom scan is allowed to execute:
     * either one semantic call, or the narrow BatchJoin-fallback shape consisting
     * of an ordinary-relation outer Var primary plus one same-target Const base
     * filter.  The decision is deliberately made against the original planner
     * nodes: their pointer identities later select exactly which restrictions are
     * removed from executable quals and which competing paths
     * would execute them.
     */
    [[nodiscard]] owned_mmsearch_restriction
    choose_owned_mmsearch_restriction(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      RangeTblEntry* rte,
                                      List* restrictions,
                                      List* ppi_restrictions,
                                      Relids param_relids,
                                      const bioseq_stub_oids& oids) {
        std::size_t semantic_calls = 0;
        List* semantic_restrictions = NIL;
        for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(restrictions)) {
            const std::size_t in_clause =
                count_mmsearch_occurrences(reinterpret_cast<Node*>(rinfo->clause), oids);
            semantic_calls += in_clause;
            if (in_clause != 0)
                semantic_restrictions = lappend(semantic_restrictions, rinfo);
        }
        if (semantic_calls == 0 || semantic_restrictions == NIL) {
            return {};
        }
        if (semantic_calls != static_cast<std::size_t>(list_length(semantic_restrictions))) {
            /* More than one occurrence inside one Boolean expression is never the
             * narrow Param-primary/Const-extra ownership shape below. */
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("stacked mmsearch qualifiers are unsupported"),
                     errhint("Split the alignment predicates into separate query levels.")));
        }

        /* Fail closed under row-level security, security-barrier quals, leaky
         * ordering, pseudoconstants, or volatile siblings. */
        if (root == nullptr || root->parse == nullptr || root->parse->hasRowSecurity ||
            rte == nullptr || rte->securityQuals != NIL) {
            return {};
        }
        for (RestrictInfo* sibling : postgres_list<RestrictInfo>(restrictions)) {
            if (!list_member_ptr(semantic_restrictions, sibling) &&
                contain_volatile_functions(reinterpret_cast<Node*>(sibling->clause))) {
                return {};
            }
        }

        /* A parameterized baserel candidate can rendezvous restrictions from any
         * number of higher owners.  Provenance, not operand syntax, decides whether
         * this candidate may classify them together. */
        if (semantic_calls > 1u) {
            /* Walk PostgreSQL Lists directly.  Do not retain a std::vector (or any
             * other nontrivial C++ owner) across ereport(ERROR): PostgreSQL's longjmp
             * would skip its destructor in the backend. */
            for (ListCell* current_cell = list_head(semantic_restrictions); current_cell != nullptr;
                 current_cell = lnext(semantic_restrictions, current_cell)) {
                RestrictInfo* current = reinterpret_cast<RestrictInfo*>(lfirst(current_cell));
                FuncExpr* current_expression =
                    current == nullptr
                        ? nullptr
                        : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(current->clause));
                const bool exact_expression =
                    current_expression != nullptr &&
                    is_mmsearch_funcid(current_expression->funcid, oids) &&
                    ((current_expression->funcid == oids.mmsearch_oid &&
                      list_length(current_expression->args) == 2) ||
                     (current_expression->funcid == oids.mmsearch_with_opts_oid &&
                      list_length(current_expression->args) == 3));
                const bool current_base =
                    current != nullptr && list_member_ptr(rel->baserestrictinfo, current);
                const bool current_ppi =
                    current != nullptr && list_member_ptr(ppi_restrictions, current);
                if (!exact_expression || current_base == current_ppi ||
                    current->required_relids == nullptr) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("stacked mmsearch qualifiers are unsupported"),
                         errhint("Split the alignment predicates into separate query levels.")));
                }

                for (ListCell* prior_cell = list_head(semantic_restrictions);
                     prior_cell != current_cell;
                     prior_cell = lnext(semantic_restrictions, prior_cell)) {
                    RestrictInfo* prior = reinterpret_cast<RestrictInfo*>(lfirst(prior_cell));
                    const bool prior_base = list_member_ptr(rel->baserestrictinfo, prior);
                    if (current_base == prior_base &&
                        bms_equal(current->required_relids, prior->required_relids)) {
                        ereport(
                            ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("stacked mmsearch qualifiers are unsupported"),
                             errdetail("Two semantic mmsearch calls share one ownership frontier."),
                             errhint(
                                 "Split the alignment predicates into separate query levels.")));
                    }
                }
            }

            bool bounded_fallback_candidate = false;
            if (semantic_calls == 2u) {
                RestrictInfo* first =
                    reinterpret_cast<RestrictInfo*>(linitial(semantic_restrictions));
                RestrictInfo* second =
                    reinterpret_cast<RestrictInfo*>(lsecond(semantic_restrictions));
                const bool first_base = list_member_ptr(rel->baserestrictinfo, first);
                const bool second_base = list_member_ptr(rel->baserestrictinfo, second);
                if (first_base != second_base) {
                    RestrictInfo* base_restriction = first_base ? first : second;
                    RestrictInfo* ppi_restriction = first_base ? second : first;
                    FuncExpr* base_expression = try_cast_pg_node<FuncExpr>(
                        reinterpret_cast<Node*>(base_restriction->clause));
                    FuncExpr* ppi_expression = try_cast_pg_node<FuncExpr>(
                        reinterpret_cast<Node*>(ppi_restriction->clause));
                    Node* base_target = reinterpret_cast<Node*>(linitial(base_expression->args));
                    Node* base_query = reinterpret_cast<Node*>(lsecond(base_expression->args));
                    Node* ppi_target = reinterpret_cast<Node*>(linitial(ppi_expression->args));
                    Node* ppi_query = reinterpret_cast<Node*>(lsecond(ppi_expression->args));
                    Var* ppi_query_var = try_cast_pg_node<Var>(ppi_query);
                    Const* base_query_const = try_cast_pg_node<Const>(base_query);
                    bounded_fallback_candidate =
                        try_cast_pg_node<Var>(base_target) != nullptr &&
                        try_cast_pg_node<Var>(ppi_target) != nullptr &&
                        equal(base_target, ppi_target) && base_query_const != nullptr &&
                        base_query_const->consttype == oids.bioseq_seq_type_oid &&
                        ppi_query_var != nullptr && ppi_query_var->varlevelsup == 0 &&
                        ppi_query_var->varattno > 0 &&
                        ppi_query_var->vartype == oids.bioseq_seq_type_oid &&
                        bms_is_empty(ppi_query_var->varnullingrels) &&
                        bms_is_member(static_cast<int>(ppi_query_var->varno), param_relids);
                }
            }
            if (!bounded_fallback_candidate)
                return {};
        }

        RestrictInfo* primary_restriction = nullptr;
        FuncExpr* primary_expr = nullptr;
        RestrictInfo* constant_restriction = nullptr;
        FuncExpr* constant_expr = nullptr;
        for (RestrictInfo* semantic_restriction :
             postgres_list<RestrictInfo>(semantic_restrictions)) {
            /* Nested Boolean/CASE/NOT shapes are not equivalent to a consumed,
             * strict top-level predicate and therefore remain executable SQL quals. */
            FuncExpr* original =
                try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(semantic_restriction->clause));
            if (original == nullptr || !is_mmsearch_funcid(original->funcid, oids) ||
                semantic_restriction->pseudoconstant || semantic_restriction->security_level != 0 ||
                !restriction_is_securely_promotable(semantic_restriction, rel))
                return {};
            const int arity = list_length(original->args);
            if ((original->funcid == oids.mmsearch_oid && arity != 2) ||
                (original->funcid == oids.mmsearch_with_opts_oid && arity != 3))
                return {};
            pg_first_two_node_arguments args =
                get_pg_first_two_arguments_if_arity_between(*original, 2, 3);
            Node* target_node = args.is_present() ? args.first_argument : nullptr;
            Var* target = try_cast_pg_node<Var>(target_node);
            const bool raw_target =
                target != nullptr && target->varlevelsup == 0 && target->varno == rel->relid &&
                target->varattno > 0 && bms_is_empty(target->varnullingrels) &&
                target->vartype == oids.bioseq_seq_type_oid && target->vartypmod >= 0;
            if (!raw_target &&
                !coarse_base_mmsearch_operand_candidate(target_node,
                                                        rel,
                                                        oids,
                                                        /*require_sequence_var=*/true))
                return {};

            Node* query = args.second_argument;
            Const* query_constant = try_cast_pg_node<Const>(query);
            const bool constant_query = query_constant != nullptr &&
                                        query_constant->consttype == oids.bioseq_seq_type_oid &&
                                        query_constant->consttypmod >= 0;
            bool parameterized_query = false;
            if (!constant_query) {
                if (Param* param = try_cast_pg_node<Param>(query)) {
                    parameterized_query =
                        param->paramtype == oids.bioseq_seq_type_oid &&
                        (param->paramkind == PARAM_EXEC || param->paramkind == PARAM_EXTERN) &&
                        (param->paramtypmod >= 0 || param->paramkind == PARAM_EXTERN);
                } else if (Var* query_var = try_cast_pg_node<Var>(query)) {
                    const bool concrete_varno = bioseq_is_concrete_rte_varno(query_var->varno);
                    const bool same_relation = concrete_varno && query_var->varno == rel->relid;
                    const bool available_outer =
                        concrete_varno &&
                        bms_is_member(static_cast<int>(query_var->varno), param_relids);
                    parameterized_query = query_var->varlevelsup == 0 && query_var->varattno > 0 &&
                                          query_var->vartype == oids.bioseq_seq_type_oid &&
                                          query_var->vartypmod >= 0 &&
                                          (same_relation || available_outer);
                    /* A same-relation per-row query remains a valid sole owner, but
                     * cannot be combined with a constant extra: the dispatcher has one
                     * dynamic primary slot and the bounded stacked contract is specifically
                     * the parameterized join case. */
                    if (same_relation && semantic_calls == 2) {
                        parameterized_query = false;
                    }
                } else {
                    /* A complete base-local query recipe (slice/concat/reverse,
                     * CASE/COALESCE, dynamic bounds, or an opaque effectful arm) is
                     * evaluated once per row after the target.  The later validation
                     * checks every Var/nulling frontier and derives its bound type. */
                    parameterized_query =
                        semantic_calls == 1 &&
                        coarse_base_mmsearch_operand_candidate(query,
                                                               rel,
                                                               oids,
                                                               /*require_sequence_var=*/false);
                }
            }
            if (!constant_query && !parameterized_query) {
                if (semantic_calls == 2) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("stacked mmsearch qualifiers are unsupported"),
                             errdetail("Only one parameterized mmsearch plus one constant filter "
                                       "on the same target column can share a Bioseq scan.")));
                }
                return {};
            }

            FuncExpr* normalized = copyObject(original);
            if (arity == 3) {
                Node* option = reinterpret_cast<Node*>(list_nth(normalized->args, 2));
                if (contain_volatile_functions(option))
                    return {};
                Node* folded = eval_const_expressions(root, option);
                Const* option_const = try_cast_pg_node<Const>(folded);
                if (option_const == nullptr ||
                    option_const->consttype != oids.bioseq_aligner_options_type_oid)
                    return {};
                lfirst(list_nth_cell(normalized->args, 2)) = folded;
            }
            if (constant_query) {
                if (constant_expr != nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("stacked mmsearch qualifiers are unsupported"),
                             errdetail("Two constant mmsearch filters cannot share one "
                                       "match_result owner.")));
                }
                constant_restriction = semantic_restriction;
                constant_expr = normalized;
            } else {
                if (primary_expr != nullptr) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("stacked mmsearch qualifiers are unsupported"),
                         errdetail(
                             "Two dynamic mmsearch filters cannot share one match_result owner.")));
                }
                primary_restriction = semantic_restriction;
                primary_expr = normalized;
            }
        }

        if (semantic_calls == 1) {
            return primary_expr != nullptr
                       ? owned_mmsearch_restriction{primary_restriction, primary_expr, NIL, NIL}
                       : owned_mmsearch_restriction{constant_restriction, constant_expr, NIL, NIL};
        }
        Node* primary_target = primary_expr == nullptr
                                   ? nullptr
                                   : reinterpret_cast<Node*>(linitial(primary_expr->args));
        Node* constant_target = constant_expr == nullptr
                                    ? nullptr
                                    : reinterpret_cast<Node*>(linitial(constant_expr->args));
        Node* primary_query = primary_expr == nullptr || list_length(primary_expr->args) < 2
                                  ? nullptr
                                  : reinterpret_cast<Node*>(lsecond(primary_expr->args));
        Node* constant_query = constant_expr == nullptr || list_length(constant_expr->args) < 2
                                   ? nullptr
                                   : reinterpret_cast<Node*>(lsecond(constant_expr->args));
        Var* primary_query_var = try_cast_pg_node<Var>(primary_query);
        Const* constant_query_value = try_cast_pg_node<Const>(constant_query);
        RangeTblEntry* primary_query_rte =
            primary_query_var == nullptr ||
                    !bioseq_is_concrete_rte_varno(primary_query_var->varno) ||
                    primary_query_var->varno > static_cast<Index>(list_length(root->parse->rtable))
                ? nullptr
                : rt_fetch(primary_query_var->varno, root->parse->rtable);
        /* Preserve the established general stacked-qualifier rejection.  The sole
         * admitted exception is the BatchJoin fallback contract: an explicitly
         * require-absent primary over an outer Var from an ordinary relation becomes
         * PARAM_EXEC for a Bioseq target scan whose same target column also has one
         * non-NULL, default-options Const filter.  Both calls therefore share the
         * exact biological parameters, while require-absent is a valid execution of
         * the extra's default "allow" index policy.  A second Bioseq relation,
         * PARAM_EXTERN, same-row Var, view/subquery, security-qualified provider,
         * NULL extra, or independent option set remains unsupported; accepting those
         * would silently broaden the generic multi-owner surface beyond the one
         * executor shape that has an AND-stack runtime and integration contract. */
        const bool bounded_batch_fallback =
            primary_query_var != nullptr &&
            bms_is_member(static_cast<int>(primary_query_var->varno), param_relids) &&
            constant_query_value != nullptr && !constant_query_value->constisnull &&
            primary_query_rte != nullptr && primary_query_rte->rtekind == RTE_RELATION &&
            primary_query_rte->securityQuals == NIL && OidIsValid(primary_query_rte->relid) &&
            !relation_oid_uses_column_table_access_method(primary_query_rte->relid) &&
            bounded_batch_fallback_options_are_supported(primary_expr, constant_expr, oids);
        if (primary_restriction == nullptr || primary_expr == nullptr ||
            constant_restriction == nullptr || constant_expr == nullptr ||
            !equal(primary_target, constant_target) || !bounded_batch_fallback) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("stacked mmsearch qualifiers are unsupported"),
                     errdetail("Only one parameterized mmsearch plus one constant filter on the "
                               "same target column can share a Bioseq scan."),
                     errhint("Split the alignment predicates into separate query levels.")));
        }
        return owned_mmsearch_restriction{primary_restriction,
                                          primary_expr,
                                          list_make1(constant_restriction),
                                          list_make1(constant_expr)};
    }

    [[nodiscard]] List* restrictions_without_owned_mmsearch(List* restrictions,
                                                            List* owned,
                                                            bool restrictions_are_private_copies) {
        postgres_list<RestrictInfo> result;
        for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(restrictions)) {
            bool is_owned = false;
            for (RestrictInfo* owner : postgres_list<RestrictInfo>(owned)) {
                if (rinfo == owner || (restrictions_are_private_copies && owner != nullptr &&
                                       equal(rinfo->clause, owner->clause))) {
                    is_owned = true;
                    break;
                }
            }
            if (is_owned)
                continue;
            result.append(rinfo);
        }
        return result.release();
    }

    /*
     * The owned mmsearch is removed before these storage quals are built. Keep a
     * defensive filter at the reader boundary so an unowned/nested rendezvous
     * expression can never be mistaken for a min/max pruning predicate.
     */
    [[nodiscard]] List* storage_prunable_quals_without_mmsearch(List* quals,
                                                                const bioseq_stub_oids& oids) {
        postgres_list<Node> filtered;
        for (auto qual : postgres_list<Node>(quals)) {
            if (qual == nullptr) {
                continue;
            }
            if (contains_mmsearch_qualifier(qual, oids)) {
                continue;
            }
            filtered.append(qual);
        }
        return filtered.release();
    }

    [[nodiscard]] inline bool is_bioseq_projection_funcid(Oid funcid, const bioseq_stub_oids& o) {
        return funcid == o.score_oid || funcid == o.dp_score_oid || funcid == o.query_len_oid ||
               funcid == o.query_start_oid || funcid == o.query_end_oid ||
               funcid == o.target_len_oid || funcid == o.target_start_oid ||
               funcid == o.target_end_oid || funcid == o.num_matches_oid ||
               funcid == o.block_length_oid || funcid == o.edit_oid || funcid == o.mapq_oid ||
               funcid == o.chain_score_oid || funcid == o.dp_max_score_oid ||
               funcid == o.seed_count_oid || funcid == o.secondary_chain_score_oid ||
               funcid == o.is_reverse_oid || funcid == o.is_primary_oid ||
               funcid == o.has_cigar_oid || funcid == o.divergence_oid || funcid == o.cigar_oid;
    }

    /* `anyelement` match helpers traditionally accept a target-owned row marker
     * such as the primary key.  When bioseq.edit is instead given a bioseq.seq
     * Var, that Var carries biological meaning and must be the exact sequence
     * column aligned by mmsearch; silently substituting another column's helper
     * with the shared match_result edit count would return a plausible wrong value. */
    [[nodiscard]] bool
    edit_sequence_argument_matches_target(const FuncExpr* function,
                                          Index target_relid,
                                          AttrNumber target_sequence_attno,
                                          const bioseq_stub_oids& oids) noexcept {
        if (function == nullptr || function->funcid != oids.edit_oid)
            return true;
        if (list_length(function->args) != 1)
            return false;
        Node* argument_node = reinterpret_cast<Node*>(linitial(function->args));
        if (argument_node == nullptr || exprType(argument_node) != oids.bioseq_seq_type_oid) {
            /* Accept the target-owned row-marker helper form. */
            return true;
        }
        Var* argument = try_cast_pg_node<Var>(argument_node);
        if (argument == nullptr) {
            /* A sequence-valued wrapper (COALESCE, cast, PHV expression, etc.)
             * has biological identity that the shared match_result cannot safely
             * infer.  Only the exact mmsearch target Var is supported. */
            return false;
        }
        return target_relid != 0 && target_sequence_attno > 0 && argument->varno == target_relid &&
               argument->varattno == target_sequence_attno && argument->varlevelsup == 0 &&
               bms_is_empty(argument->varnullingrels);
    }

    /* A transformed target has biological identity in its complete canonical
     * recipe, not merely in the first storage Var below that recipe.  Preserve the
     * raw-Var check above for base scans, while allowing bioseq.edit() to
     * name the exact owned transformed expression. */
    [[nodiscard]] bool
    edit_sequence_argument_matches_recipe(const FuncExpr* function,
                                          Node* target_expression,
                                          Index target_relid,
                                          AttrNumber target_sequence_attno,
                                          const bioseq_stub_oids& oids) noexcept {
        if (function == nullptr || function->funcid != oids.edit_oid)
            return true;
        if (list_length(function->args) != 1 || target_expression == nullptr) {
            return false;
        }
        Node* argument = reinterpret_cast<Node*>(linitial(function->args));
        /* A sequence-valued helper argument is biological identity, not a
         * row marker.  It must name the complete owned recipe; accepting the raw
         * storage Var of a sliced/reversed/concatenated target would silently report
         * the transformed match as if it belonged to the whole stored sequence. */
        if (argument != nullptr && exprType(argument) == oids.bioseq_seq_type_oid) {
            return equal(argument, target_expression);
        }
        /* Only non-sequence row-marker spellings reach the target-owned
         * fallback.  The surrounding helper registration has already validated
         * that such a Var belongs to target_relid. */
        return edit_sequence_argument_matches_target(
            function, target_relid, target_sequence_attno, oids);
    }

    /*
     * Walk a target list for bioseq.* function calls and set the matching
     * feature-flags. We also recurse into arg subtrees so "bioseq.cigar(a) + 1"
     * is still counted. The shape used for detection matches the shape used by
     * VarInjectorMutator so the flags agree with which placeholders actually
     * populate at execution time.
     */
    struct bioseq_feature_detector {
        const bioseq_stub_oids* oids = nullptr;
        scan_feature_flags flags;
    };

    struct feature_flag_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              bioseq_feature_detector& detector) const {
            if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                const bioseq_stub_oids* o = detector.oids;
                if (is_mmsearch_funcid(fe->funcid, *o)) {
                    detector.flags.has_mmsearch = true;
                }
                if (fe->funcid == o->cigar_oid || fe->funcid == o->has_cigar_oid) {
                    detector.flags.uses_cigar = true;
                }
                if (fe->funcid == o->edit_oid) {
                    detector.flags.uses_edit = true;
                }
                if (fe->funcid == o->divergence_oid) {
                    detector.flags.uses_divergence = true;
                }
                if (fe->funcid == o->mapq_oid) {
                    detector.flags.uses_mapq = true;
                }
                if (is_bioseq_projection_funcid(fe->funcid, *o)) {
                    detector.flags.any_match_projection = true;
                }
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] scan_feature_flags
    detect_feature_flags(List* targetlist, List* qual, const bioseq_stub_oids& oids) {
        bioseq_feature_detector detector{&oids, scan_feature_flags{}};
        feature_flag_visitor visitor;
        for (auto tgt : postgres_list<TargetEntry>(targetlist)) {
            if (tgt == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(to_pg_node(tgt->expr), detector, visitor);
        }
        for (auto q : postgres_list<Node>(qual)) {
            if (q == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(q, detector, visitor);
        }
        return detector.flags;
    }

    inline constexpr int k_bioseq_edit_predicate_magic = 0x42534531; /* BSE1 */
    inline constexpr int k_bioseq_edit_predicate_version = 1;

    struct strict_edit_predicate {
        std::int64_t exclusive_upper_bound = 0;
        AttrNumber sequence_attno = InvalidAttrNumber;
    };

    struct edit_function_probe {
        const bioseq_stub_oids* oids = nullptr;
        bool found = false;
    };

    struct edit_function_probe_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor, pg_node_view node, edit_function_probe& probe) const {
            if (FuncExpr* function = node.try_cast<FuncExpr>();
                function != nullptr && function->funcid == probe.oids->edit_oid) {
                probe.found = true;
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool contains_bioseq_edit(Node* expression, const bioseq_stub_oids& oids) {
        if (expression == nullptr)
            return false;
        edit_function_probe probe{&oids, false};
        edit_function_probe_visitor visitor;
        walk_pg_expression_including_root(expression, probe, visitor);
        return probe.found;
    }

    /* Supported SQL contract: one top-level `bioseq.edit(Var) < integer Const`.
     * The predicate is removed from the pre-alignment qual and enforced against
     * match_result::edit_distance while draining aligned output. */
    [[nodiscard]] Node* strict_edit_operand(Node* operand, Index scan_relid) noexcept {
        PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(operand);
        if (placeholder == nullptr)
            return operand;
        /* bioseq_placeholder_support introduces this exact wrapper for aggregate
         * and join plans.  Accept only a scan-owned, non-nulled PHV; unwrapping a
         * generic or outer-join-nullable PHV would discard planner semantics. */
        if (!bioseq_is_concrete_rte_varno(scan_relid) || placeholder->phid == 0 ||
            placeholder->phlevelsup != 0 || !bms_is_empty(placeholder->phnullingrels) ||
            bms_num_members(placeholder->phrels) != 1 ||
            !bms_is_member(static_cast<int>(scan_relid), placeholder->phrels)) {
            return nullptr;
        }
        return reinterpret_cast<Node*>(placeholder->phexpr);
    }

    [[nodiscard]] std::optional<strict_edit_predicate> decode_strict_edit_upper_bound(
        Node* expression, Index scan_relid, const bioseq_stub_oids& oids) {
        OpExpr* comparison = try_cast_pg_node<OpExpr>(expression);
        if (comparison == nullptr || comparison->opresulttype != BOOLOID ||
            list_length(comparison->args) != 2) {
            return std::nullopt;
        }
        postgres_list<Node> arguments(comparison->args);
        FuncExpr* edit =
            try_cast_pg_node<FuncExpr>(strict_edit_operand(arguments.get_first(), scan_relid));
        Const* upper = try_cast_pg_node<Const>(arguments.get_second());
        if (edit == nullptr || edit->funcid != oids.edit_oid || edit->funcresulttype != INT8OID ||
            list_length(edit->args) != 1 || upper == nullptr || upper->constisnull) {
            return std::nullopt;
        }
        /* Operator names are schema-overloadable.  A bare SQL literal such as 10
         * is int4, so PostgreSQL resolves BIGINT < 10 to its built-in int84lt
         * implementation rather than inserting an int8 cast.  Accept only the
         * three exact built-in RHS integer widths and normalize their Const datum;
         * an arbitrary operator merely named `<` must remain fail-closed. */
        const Oid opcode = get_opcode(comparison->opno);
        std::int64_t exclusive_upper_bound = 0;
        switch (upper->consttype) {
        case INT8OID:
            if (opcode != F_INT8LT)
                return std::nullopt;
            exclusive_upper_bound = DatumGetInt64(upper->constvalue);
            break;
        case INT4OID:
            if (opcode != F_INT84LT)
                return std::nullopt;
            exclusive_upper_bound = static_cast<std::int64_t>(DatumGetInt32(upper->constvalue));
            break;
        case INT2OID:
            if (opcode != F_INT82LT)
                return std::nullopt;
            exclusive_upper_bound = static_cast<std::int64_t>(DatumGetInt16(upper->constvalue));
            break;
        default:
            return std::nullopt;
        }
        Var* sequence = try_cast_pg_node<Var>(reinterpret_cast<Node*>(linitial(edit->args)));
        if (sequence == nullptr || sequence->varno != scan_relid || sequence->varattno <= 0 ||
            sequence->varlevelsup != 0 || !bms_is_empty(sequence->varnullingrels) ||
            sequence->vartype != oids.bioseq_seq_type_oid) {
            return std::nullopt;
        }
        return strict_edit_predicate{
            .exclusive_upper_bound = exclusive_upper_bound,
            .sequence_attno = sequence->varattno,
        };
    }

    [[nodiscard]] List* quals_without_bioseq_edit(List* quals, const bioseq_stub_oids& oids) {
        postgres_list<Node> filtered;
        for (Node* qual : postgres_list<Node>(quals)) {
            if (qual != nullptr && !contains_bioseq_edit(qual, oids)) {
                filtered.append(qual);
            }
        }
        return filtered.release();
    }

    /* Planner baserestrictinfo is a List<RestrictInfo>, not an expression list.
     * Expression walkers deliberately do not know how to descend RestrictInfo
     * (PostgreSQL reports "unrecognized node type"), so unwrap only the exact
     * clause field and fail closed if a future planner shape violates that ABI. */
    [[nodiscard]] std::optional<scan_feature_flags>
    detect_restrictinfo_feature_flags(List* restrictions, const bioseq_stub_oids& oids) {
        bioseq_feature_detector detector{&oids, scan_feature_flags{}};
        feature_flag_visitor visitor;
        for (Node* node : postgres_list<Node>(restrictions)) {
            RestrictInfo* restriction = try_cast_pg_node<RestrictInfo>(node);
            if (restriction == nullptr || restriction->clause == nullptr) {
                return std::nullopt;
            }
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(restriction->clause), detector, visitor);
        }
        return detector.flags;
    }

    /*
     * Plan-time detection of an mmsearch typmod-coalesce (same alphabet,
     * different storage width between the bioseq.seq column and the query literal).
     *
     * `EXPLAIN` without `ANALYZE` has no populated runtime buffer, while the
     * coalesce decision is fully determined at plan time by the column Var's
     * typmod versus the literal's typmod. Derive it from the plan qual here using
     * the same BeginCustomScan
     * typmod logic (same `compact_sequence_stdc_type_params::decode`), reading no
     * runtime state. A mismatch we cannot classify simply yields `active=false`
     * (no hint, never a crash).
     */
    struct explain_typmod_coalesce {
        bool active = false;
        uint8_t col_alpha = 0;
        uint8_t col_byte_width = 0;
    };
    struct explain_coalesce_walk_context {
        const bioseq_stub_oids* oids;
        explain_typmod_coalesce result;
    };

    struct typmod_coalesce_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              explain_coalesce_walk_context& ctx) const {
            if (!ctx.result.active) {
                if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                    if (is_mmsearch_funcid(fe->funcid, *ctx.oids) && list_length(fe->args) >= 2) {
                        Node* arg0 =
                            reinterpret_cast<Node*>(linitial(fe->args)); // bioseq.seq column
                        Node* arg1 = reinterpret_cast<Node*>(
                            lsecond(fe->args)); // query literal / param / var
                        if (arg0 != nullptr && arg1 != nullptr &&
                            pg_node_view{arg0}.is_type<Var>()) {
                            const int32 col_typmod = expect_pg_node<Var>(arg0)->vartypmod;
                            int32 arg1_typmod = -1;
                            if (Const* arg1_const = try_cast_pg_node<Const>(arg1)) {
                                arg1_typmod = arg1_const->consttypmod;
                            } else if (Param* arg1_param = try_cast_pg_node<Param>(arg1)) {
                                arg1_typmod = arg1_param->paramtypmod;
                            } else if (Var* arg1_var = try_cast_pg_node<Var>(arg1)) {
                                arg1_typmod = arg1_var->vartypmod;
                            }
                            if (col_typmod >= 0 && arg1_typmod >= 0) {
                                const auto col =
                                    compact_sequence_stdc_type_params::decode(col_typmod);
                                const auto qry =
                                    compact_sequence_stdc_type_params::decode(arg1_typmod);
                                if (col.alphabet_code == qry.alphabet_code &&
                                    col.storage_unit_byte_width != qry.storage_unit_byte_width) {
                                    ctx.result.active = true;
                                    ctx.result.col_alpha = col.alphabet_code;
                                    ctx.result.col_byte_width = col.storage_unit_byte_width;
                                }
                            }
                        }
                    }
                }
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] explain_typmod_coalesce
    explain_detect_typmod_coalesce(List* qual, const bioseq_stub_oids& oids) {
        explain_coalesce_walk_context ctx{&oids, explain_typmod_coalesce{}};
        typmod_coalesce_visitor visitor;
        for (auto q : postgres_list<Node>(qual)) {
            if (q != nullptr) {
                walk_pg_expression_including_root(q, ctx, visitor);
            }
        }
        return ctx.result;
    }

    /*
     * Snapshot the bioseq.aligner.* GUCs into a minimap2_parameters struct.
     * A subsequent pass (parse_aligner_options_override) may overlay per-call
     * fields that the user supplied via aligner_options() in the mmsearch
     * FuncExpr. Keeping the two passes separate makes the precedence order
     * obvious: GUC → aligner_options() override → size-heuristic → pipeline.
     */
    [[nodiscard]] bioseq::simd::minimap2_parameters build_params_from_gucs() {
        bioseq::simd::minimap2_parameters params;
        params.kmer_size = bioseq::aligner::settings::minimizer::kmer_size;
        params.window_size = bioseq::aligner::settings::minimizer::window_size;
        params.max_gap = bioseq::aligner::settings::chaining::maximum_gap;
        params.max_gap_ref = bioseq::aligner::settings::chaining::maximum_reference_gap;
        params.bandwidth = bioseq::aligner::settings::chaining::bandwidth;
        params.max_chain_skip = bioseq::aligner::settings::chaining::maximum_skipped_anchors;
        params.max_chain_iter = bioseq::aligner::settings::chaining::maximum_predecessors;
        params.min_seed_count = bioseq::aligner::settings::chaining::minimum_seed_count;
        params.min_chain_score = bioseq::aligner::settings::chaining::minimum_score;
        params.chain_gap_scale =
            static_cast<float>(bioseq::aligner::settings::chaining::gap_penalty_scale);
        params.chain_skip_scale =
            static_cast<float>(bioseq::aligner::settings::chaining::skip_penalty_scale);
        params.mask_level =
            static_cast<float>(bioseq::aligner::settings::secondary::overlap_threshold);
        params.mask_len = bioseq::aligner::settings::secondary::uncovered_length_limit;
        params.pri_ratio =
            static_cast<float>(bioseq::aligner::settings::secondary::score_ratio_threshold);
        params.best_n = bioseq::aligner::settings::secondary::maximum_count;
        params.match_score = bioseq::aligner::settings::scoring::match_score;
        params.mismatch_pen = bioseq::aligner::settings::scoring::mismatch_penalty;
        params.gap_open1 = bioseq::aligner::settings::scoring::short_gap_open_penalty;
        params.gap_ext1 = bioseq::aligner::settings::scoring::short_gap_extension_penalty;
        params.gap_open2 = bioseq::aligner::settings::scoring::long_gap_open_penalty;
        params.gap_ext2 = bioseq::aligner::settings::scoring::long_gap_extension_penalty;
        params.sc_ambi = bioseq::aligner::settings::scoring::ambiguous_base_penalty;
        params.zdrop = bioseq::aligner::settings::scoring::z_drop_threshold;
        params.end_bonus = bioseq::aligner::settings::scoring::end_bonus;
        params.max_occ = bioseq::aligner::settings::seeding::maximum_occurrence;
        params.mid_occ = bioseq::aligner::settings::seeding::intermediate_occurrence_threshold;
        params.block_min_size =
            static_cast<size_t>(bioseq::aligner::settings::block_alignment::minimum_block_size);
        params.block_max_size =
            static_cast<size_t>(bioseq::aligner::settings::block_alignment::maximum_block_size);
        params.bandwidth_long = bioseq::aligner::settings::chaining::long_join_bandwidth;
        params.chaining_algorithm = static_cast<bioseq::simd::chaining_algorithm>(
            static_cast<int>(bioseq::aligner::settings::chaining::algorithm.get_value()));
        params.base_alignment_algorithm = static_cast<bioseq::simd::base_alignment_algorithm>(
            static_cast<int>(bioseq::aligner::settings::base_alignment::algorithm.get_value()));
        params.minimizer_algorithm = static_cast<bioseq::simd::minimizer_algorithm>(
            static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()));
        params.enable_profiling = bioseq::aligner::settings::profiling::enabled;
        params.vulkan_enabled = bioseq::aligner::settings::vulkan::enabled;
        params.vulkan_seed_hits_enabled = bioseq::aligner::settings::vulkan::seed_lookup::enabled;
        params.vulkan_index_cache_enabled = bioseq::aligner::settings::vulkan::index_cache::enabled;
        /* Resident two-submit frontend and GPU index construction. */
        params.vulkan_fused_frontend_enabled =
            bioseq::aligner::settings::vulkan::fused_frontend::enabled;
        params.vulkan_gpu_index_build_enabled =
            bioseq::aligner::settings::vulkan::index_build::enabled;
        params.vulkan_extend_enabled =
            bioseq::aligner::settings::vulkan::alignment_extension::enabled;
        params.vulkan_ordered_sketch_tile_bp =
            static_cast<uint32_t>(bioseq::aligner::settings::vulkan::ordered_sketch_tile_bp);
        params.vulkan_radix_digit_bits =
            static_cast<uint32_t>(bioseq::aligner::settings::vulkan::radix_sort_digit_bits);
        return params;
    }

} // export custom-scan partition declarations
