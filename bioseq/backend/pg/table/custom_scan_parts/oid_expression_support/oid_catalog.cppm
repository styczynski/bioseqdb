/**
 * @file
 * @brief Owns Bioseq OID lookup, invalidation, and syscache validation.
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

export module bioseq.postgres.table:custom_scan_oid_expression_support_oid_catalog;

import std;
import bioseq.postgres.support.datums;

export {

    /** @brief Stores exact catalog OIDs for the SQL types and functions used by custom scans. */
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

    /** @brief Resolves a fully defined type without failing during partial extension setup. */
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

    /** @brief Returns the extension installation schema when the extension is available. */
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

    /** @brief Resolves a function by schema, name, and exact argument OIDs. */
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

    /** @brief Tests whether an exact function signature is present in the catalog. */
    [[nodiscard]] bool exact_function_exists_missing_ok(const char* schema_name,
                                                        const char* function_name,
                                                        std::initializer_list<Oid> argument_types) {
        return OidIsValid(exact_function_oid(schema_name,
                                             function_name,
                                             argument_types,
                                             /*missing_ok=*/true));
    }

    /** @brief Checks that the complete type and function surface needed by custom scans exists. */
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

    /**
     * @brief Resolves the complete Bioseq stub catalog after extension installation.
     * @throws PostgreSQL ERROR when a required type or function is absent.
     */
    [[nodiscard]] bioseq_stub_oids lookup_stub_oids() {
        bioseq_stub_oids result{};
        // Resolve a required SQL function from its exact regprocedure signature.
        auto resolve = [](const char* signature) {
            return DatumGetObjectId(
                DirectFunctionCall1(regprocedurein, CStringGetDatum(signature)));
        };
        // Resolve a required SQL type from its qualified name.
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

    /** @brief Holds the lazily initialized OID surface for one PostgreSQL backend. */
    struct cached_stub_oids {
        bioseq_stub_oids oids{};
        bool initialised = false;
    };
    /** @brief Backend-local OID cache invalidated by catalog and relation callbacks. */
    cached_stub_oids g_cached_stub_oids{};

    /** @brief Marks the backend-local OID cache stale after relation invalidation. */
    void invalidate_cached_stub_oids(Datum /*arg*/, Oid /*relid*/) {
        g_cached_stub_oids.initialised = false;
    }

    /** @brief Marks the backend-local OID cache stale after procedure or type invalidation. */
    void
    invalidate_cached_stub_oids_syscache(Datum /*arg*/, int /*cacheid*/, uint32 /*hashvalue*/) {
        g_cached_stub_oids.initialised = false;
    }

    /** @brief Checks that an OID still names a row in the requested syscache. */
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

    /** @brief Verifies that every cached function and type OID still exists. */
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

    /**
     * @brief Returns the backend-local stub OIDs, registering invalidation callbacks on first use.
     * @throws PostgreSQL ERROR when the complete extension catalog cannot be resolved.
     */
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

    /** @brief Returns cached stub OIDs only when the extension catalog surface is complete. */
    [[nodiscard]] const bioseq_stub_oids* try_get_cached_stub_oids() {
        if (g_cached_stub_oids.initialised && cached_stub_oids_are_valid(g_cached_stub_oids.oids)) {
            return &g_cached_stub_oids.oids;
        }
        g_cached_stub_oids.initialised = false;
        if (!bioseq_stub_oid_surface_ready())
            return nullptr;
        return &get_cached_stub_oids();
    }

    /** @brief Tests whether an Index is a concrete range-table identifier rather than a special Var. */
    [[nodiscard]] constexpr bool bioseq_is_concrete_rte_varno(const Index varno) noexcept {
        return varno != 0 && !IS_SPECIAL_VARNO(varno);
    }
    static_assert(bioseq_is_concrete_rte_varno(1));
    static_assert(!bioseq_is_concrete_rte_varno(0));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(INNER_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(OUTER_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(INDEX_VAR)));
    static_assert(!bioseq_is_concrete_rte_varno(static_cast<Index>(ROWID_VAR)));

    /** @brief Tests whether an OID names a match-result projection function. */
    [[nodiscard]] constexpr bool is_bioseq_projection_funcid(const Oid funcid,
                                                             const bioseq_stub_oids& o) noexcept {
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

} // export custom-scan partition declarations
