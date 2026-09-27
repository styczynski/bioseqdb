/**
 * @file
 * @brief Looks up and caches the SQL function and type OIDs used by Bioseq scans.
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
export import :custom_scan_oid_expression_support_oid_catalog_contracts;

export {

    /** @brief Returns the named type's OID, or InvalidOid if it is missing or still a shell type. */
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

    /** @brief Returns Bioseq's installation schema, or no value if it cannot be found. */
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

    /** @brief Checks that the types and exact function signatures needed by custom scans exist. */
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
     * @brief Looks up the SQL function and type OIDs required by custom scans.
     *
     * Find mmsearch and its options type in the extension's installation schema.
     * The sequence functions and match helpers use the bioseq schema.
     *
     * @throws PostgreSQL ERROR if a required type or function is missing.
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

    /** @brief Holds this backend's cached function and type OIDs and whether they are current. */
    struct cached_stub_oids {
        bioseq_stub_oids oids{};
        bool initialised = false;
    };
    /** @brief Caches Bioseq OIDs for this PostgreSQL backend. */
    cached_stub_oids g_cached_stub_oids{};

    /** @brief Marks the backend-local OID cache stale after relation invalidation. */
    void invalidate_cached_stub_oids(Datum /*arg*/, Oid /*relid*/) {
        g_cached_stub_oids.initialised = false;
    }

    /** @brief Marks the backend-local OID cache stale after procedure or type invalidation. */
    void
    invalidate_cached_stub_oids_syscache(Datum /*arg*/,
#if PG_VERSION_NUM >= 190000
                                        SysCacheIdentifier /*cacheid*/,
#else
                                        int /*cacheid*/,
#endif
                                        uint32 /*hashvalue*/) {
        g_cached_stub_oids.initialised = false;
    }

    /** @brief Checks that an OID still names a row in the requested syscache. */
    [[nodiscard]] bool oid_exists_in_syscache(SysCacheIdentifier cache_id, Oid oid) {
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
     * @brief Returns this backend's Bioseq function and type OIDs.
     *
     * Reload stale or missing catalog entries. Register catalog-invalidation
     * callbacks on first use so later lookups refresh the cache after changes.
     *
     * @throws PostgreSQL ERROR if required extension objects cannot be found.
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

    /**
     * @brief Returns the cached OIDs when the required extension objects are available.
     *
     * Return null during missing or incomplete extension setup. Otherwise refresh
     * the cache as needed.
     */
    [[nodiscard]] const bioseq_stub_oids* try_get_cached_stub_oids() {
        if (g_cached_stub_oids.initialised && cached_stub_oids_are_valid(g_cached_stub_oids.oids)) {
            return &g_cached_stub_oids.oids;
        }
        g_cached_stub_oids.initialised = false;
        if (!bioseq_stub_oid_surface_ready())
            return nullptr;
        return &get_cached_stub_oids();
    }

} // export custom-scan partition declarations
