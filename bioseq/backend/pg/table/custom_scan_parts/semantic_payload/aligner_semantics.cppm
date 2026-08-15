/**
 * @file
 * @brief Validates aligner overrides and final SIMD chaining policy.
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

export module bioseq.postgres.table:custom_scan_semantic_payload_aligner_semantics;

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
import :custom_scan_oid_expression_support;

import :custom_scan_semantic_payload_payload_codec;

using namespace bioseq::postgres::support;

export {
    [[nodiscard]] bool mmsearch_plans_admitted_packed_regions(
        bioseq::simd::minimap2_parameters const& params) noexcept {
        using algorithm_type = bioseq::simd::minimizer_algorithm;
        switch (params.minimizer_algorithm) {
        case algorithm_type::scalar:
        case algorithm_type::simd_minimap2_reference:
            return false;
        case algorithm_type::two_stack_nthash:
        case algorithm_type::bounded_nthash_simd:
        case algorithm_type::vulkan_minimap_integer:
            return true;
        }
        return false;
    }

    [[nodiscard]] Const* require_aligner_options_const(FuncExpr* mmsearch_fe,
                                                       const bioseq_stub_oids& oids) {
        if (mmsearch_fe == nullptr) {
            return nullptr;
        }
        const int arity = list_length(mmsearch_fe->args);
        if (mmsearch_fe->funcid == oids.mmsearch_oid && arity == 2) {
            return nullptr;
        }
        if (arity != 3 || mmsearch_fe->funcid != oids.mmsearch_with_opts_oid) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("mmsearch aligner-options call has an invalid function shape"),
                     errdetail("arity=%d funcid=%u expected_funcid=%u",
                               arity,
                               mmsearch_fe->funcid,
                               oids.mmsearch_with_opts_oid)));
        }
        postgres_list<Node> args(mmsearch_fe->args);
        Node* opt_node = args.get_third();
        Const* opt_const = try_cast_pg_node<Const>(opt_node);
        if (opt_const == nullptr) {
            const std::string_view node_tag_name = get_pg_node_tag_name(get_pg_node_tag(opt_node));
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("mmsearch aligner_options must be a plan-time constant"),
                     errdetail("third argument node tag is %.*s",
                               static_cast<int>(node_tag_name.size()),
                               node_tag_name.data()),
                     errhint("Pass aligner_options(...) directly; parameterized or column-valued "
                             "option records are not supported.")));
        }
        if (opt_const->consttype != oids.bioseq_aligner_options_type_oid) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("mmsearch aligner_options constant has the wrong type"),
                     errdetail("consttype=%u expected=%u",
                               opt_const->consttype,
                               oids.bioseq_aligner_options_type_oid)));
        }
        return opt_const;
    }

    /* The dispatch buffer owns one minimap2_parameters object for every query attached
     * to a scan.  Consequently the bounded Param-primary/Const-extra exception can
     * share that buffer only when the primary changes no biological option and the
     * extra requests the default option set.  The sole permitted difference is an
     * explicit require-absent index policy on the primary: the extra's inherited
     * `allow` policy admits the same no-index execution, so this does not alter its
     * Boolean result or error contract. */
    [[nodiscard]] bool bounded_batch_fallback_options_are_supported(FuncExpr* primary,
                                                                    FuncExpr* extra,
                                                                    const bioseq_stub_oids& oids) {
        if (primary == nullptr || extra == nullptr ||
            primary->funcid != oids.mmsearch_with_opts_oid || list_length(primary->args) != 3 ||
            extra->funcid != oids.mmsearch_oid || list_length(extra->args) != 2)
            return false;
        Const* options =
            try_cast_pg_node<Const>(reinterpret_cast<Node*>(list_nth(primary->args, 2)));
        if (options == nullptr || options->constisnull ||
            options->consttype != oids.bioseq_aligner_options_type_oid) {
            return false;
        }

        HeapTupleHeader header =
            reinterpret_cast<HeapTupleHeader>(PG_DETOAST_DATUM(options->constvalue));
        auto free_detoasted_header = [header, options] {
            if (reinterpret_cast<void*>(header) !=
                reinterpret_cast<void*>(DatumGetPointer(options->constvalue))) {
                pfree(header);
            }
        };
        const Oid row_typeoid = HeapTupleHeaderGetTypeId(header);
        if (row_typeoid != oids.bioseq_aligner_options_type_oid) {
            free_detoasted_header();
            return false;
        }
        TupleDesc tupdesc = lookup_rowtype_tupdesc(row_typeoid, HeapTupleHeaderGetTypMod(header));
        constexpr std::size_t k_expected_option_attrs = aligner_options_codec::field_count;
        if (tupdesc->natts != static_cast<int>(k_expected_option_attrs)) {
            ReleaseTupleDesc(tupdesc);
            free_detoasted_header();
            return false;
        }
        for (const bioseq::postgres::support::row_descriptor_field& expected :
             aligner_options_codec::expected_descriptor) {
            const Form_pg_attribute attribute =
                TupleDescAttr(tupdesc, expected.attribute_number - 1);
            if (attribute->attnum != expected.attribute_number || attribute->attisdropped ||
                attribute->atttypid != expected.type_oid ||
                attribute->attnotnull != !expected.nullable ||
                std::string_view{NameStr(attribute->attname)} != expected.name) {
                ReleaseTupleDesc(tupdesc);
                free_detoasted_header();
                return false;
            }
        }
        HeapTupleData tuple{};
        tuple.t_len = HeapTupleHeaderGetDatumLength(header);
        tuple.t_data = header;
        ItemPointerSetInvalid(&tuple.t_self);
        tuple.t_tableOid = InvalidOid;
        std::array<Datum, k_expected_option_attrs> values{};
        std::array<bool, k_expected_option_attrs> nulls{};
        heap_deform_tuple(&tuple, tupdesc, values.data(), nulls.data());
        constexpr std::size_t index_policy_index =
            static_cast<std::size_t>(aligner_options_codec::attribute_number<
                                         &aligner_options_catalog_row::index_read_requirement>() -
                                     1);
        bool only_index_policy_is_explicit = true;
        for (std::size_t index = 0; index < k_expected_option_attrs; ++index) {
            if (index != index_policy_index) {
                only_index_policy_is_explicit = only_index_policy_is_explicit && nulls[index];
            }
        }
        const bool requires_absent =
            !nulls[index_policy_index] &&
            DatumGetInt32(values[index_policy_index]) ==
                static_cast<int32>(bioseq::indexing::index_read_requirement::require_absent);
        ReleaseTupleDesc(tupdesc);
        free_detoasted_header();
        return only_index_policy_is_explicit && requires_absent;
    }

    /*
     * If the third argument of a mmsearch FuncExpr is a row-typed Const
     * representing aligner_options(), decode its fields and overlay any
     * non-NULL values on top of `params`. NULL field values signal "inherit
     * the GUC" so they are skipped here.
     *
     * The generated row codec validates and decodes the exact
     * bioseq_aligner_options composite layout.
     */
    [[nodiscard]] bool
    apply_aligner_options_override(FuncExpr* mmsearch_fe,
                                   bioseq::simd::minimap2_parameters& params,
                                   bioseq::indexing::index_read_requirement& index_read_requirement,
                                   const bioseq_stub_oids& oids,
                                   bool* vulkan_enabled_explicit = nullptr) {
        if (vulkan_enabled_explicit != nullptr) {
            *vulkan_enabled_explicit = false;
        }
        Const* opt_const = require_aligner_options_const(mmsearch_fe, oids);
        if (opt_const == nullptr) {
            return false;
        }
        if (opt_const->constisnull) {
            return false;
        }
        /*
         * The composite comes through as a HeapTupleHeader varlena. Deconstruct
         * it via heap_deform_tuple against the pg_type tupdesc.
         */
        HeapTupleHeader header =
            reinterpret_cast<HeapTupleHeader>(PG_DETOAST_DATUM(opt_const->constvalue));
        Oid row_typeoid = HeapTupleHeaderGetTypeId(header);
        int32 row_typmod = HeapTupleHeaderGetTypMod(header);
        auto free_detoasted_header = [header, opt_const] {
            if (reinterpret_cast<void*>(header) !=
                reinterpret_cast<void*>(DatumGetPointer(opt_const->constvalue))) {
                pfree(header);
            }
        };
        if (row_typeoid != oids.bioseq_aligner_options_type_oid) {
            const Oid actual_row_typeoid = row_typeoid;
            free_detoasted_header();
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("mmsearch aligner_options payload has the wrong row type"),
                     errdetail("header_typeoid=%u expected=%u",
                               actual_row_typeoid,
                               oids.bioseq_aligner_options_type_oid)));
        }
        TupleDesc tupdesc = lookup_rowtype_tupdesc(row_typeoid, row_typmod);

        HeapTupleData heap_tuple{};
        heap_tuple.t_len = HeapTupleHeaderGetDatumLength(header);
        heap_tuple.t_data = header;
        ItemPointerSetInvalid(&heap_tuple.t_self);
        heap_tuple.t_tableOid = InvalidOid;

        std::optional<aligner_options_catalog_row> decoded_options;
        try {
            decoded_options.emplace(
                aligner_options_codec::deform(&heap_tuple, tupdesc, CurrentMemoryContext));
        } catch (const bioseq::postgres::support::row_codec_error& error) {
            const std::string detail{error.what()};
            ReleaseTupleDesc(tupdesc);
            free_detoasted_header();
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bioseq_aligner_options catalog layout is incompatible"),
                     errdetail("%s", detail.c_str())));
        }
        const aligner_options_catalog_row& options = *decoded_options;
        bool index_read_requirement_present = false;
        if (options.kmer_size)
            params.kmer_size = *options.kmer_size;
        if (options.window_size)
            params.window_size = *options.window_size;
        if (options.max_gap)
            params.max_gap = *options.max_gap;
        if (options.max_gap_ref)
            params.max_gap_ref = *options.max_gap_ref;
        if (options.bandwidth)
            params.bandwidth = *options.bandwidth;
        if (options.bandwidth_long) {
            params.bandwidth_long = *options.bandwidth_long;
        }
        if (options.max_chain_skip) {
            params.max_chain_skip = *options.max_chain_skip;
        }
        if (options.max_chain_iter) {
            params.max_chain_iter = *options.max_chain_iter;
        }
        if (options.min_seed_count) {
            params.min_seed_count = *options.min_seed_count;
        }
        if (options.min_chain_score) {
            params.min_chain_score = *options.min_chain_score;
        }
        if (options.chain_gap_scale) {
            params.chain_gap_scale = static_cast<float>(*options.chain_gap_scale);
        }
        if (options.chain_skip_scale) {
            params.chain_skip_scale = static_cast<float>(*options.chain_skip_scale);
        }
        if (options.mask_level) {
            params.mask_level = static_cast<float>(*options.mask_level);
        }
        if (options.mask_len)
            params.mask_len = *options.mask_len;
        if (options.pri_ratio) {
            params.pri_ratio = static_cast<float>(*options.pri_ratio);
        }
        if (options.best_n)
            params.best_n = *options.best_n;
        if (options.match_score)
            params.match_score = *options.match_score;
        if (options.mismatch_pen)
            params.mismatch_pen = *options.mismatch_pen;
        if (options.gap_open1)
            params.gap_open1 = *options.gap_open1;
        if (options.gap_ext1)
            params.gap_ext1 = *options.gap_ext1;
        if (options.gap_open2)
            params.gap_open2 = *options.gap_open2;
        if (options.gap_ext2)
            params.gap_ext2 = *options.gap_ext2;
        if (options.sc_ambi)
            params.sc_ambi = *options.sc_ambi;
        if (options.zdrop)
            params.zdrop = *options.zdrop;
        if (options.end_bonus)
            params.end_bonus = *options.end_bonus;
        if (options.max_occ)
            params.max_occ = *options.max_occ;
        if (options.mid_occ)
            params.mid_occ = *options.mid_occ;
        if (options.block_min_size) {
            params.block_min_size = static_cast<size_t>(*options.block_min_size);
        }
        if (options.block_max_size) {
            params.block_max_size = static_cast<size_t>(*options.block_max_size);
        }
        if (options.chaining_algorithm) {
            /*
             * Range-check before the enum cast — otherwise a crafted caller
             * (aligner_options(chaining_algorithm => 42)) injects an invalid
             * enum value into params.chaining_algorithm. Supported
             * ordinals are 0=SCALAR_MM2, 1=SIMD_MM2, 2=RMQ, and
             * 3=VULKAN_TILED_DP.
             */
            const std::int32_t raw = *options.chaining_algorithm;
            if (raw >= 0 && raw <= 3) {
                params.chaining_algorithm = static_cast<bioseq::simd::chaining_algorithm>(raw);
            } else {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("aligner_options.chaining_algorithm out of range: %d (expected 0..3)",
                            raw)));
            }
        }
        if (options.base_alignment_algorithm) {
            const std::int32_t raw = *options.base_alignment_algorithm;
            if (raw >= 0 && raw <= 1) {
                params.base_alignment_algorithm =
                    static_cast<bioseq::simd::base_alignment_algorithm>(raw);
            } else {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("aligner_options.base_alignment_algorithm out of range: %d "
                                "(expected 0..1)",
                                raw)));
            }
        }
        // Known-good minimizer ordinals are 0 through 4.
        if (options.minimizer_algorithm) {
            const std::int32_t raw = *options.minimizer_algorithm;
            if (raw >= 0 && raw <= 4) {
                params.minimizer_algorithm = static_cast<bioseq::simd::minimizer_algorithm>(raw);
            } else {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("aligner_options.minimizer_algorithm out of range: %d (expected 0..4)",
                            raw)));
            }
        }
        if (options.vulkan_enabled) {
            params.vulkan_enabled = *options.vulkan_enabled;
            if (vulkan_enabled_explicit != nullptr) {
                *vulkan_enabled_explicit = true;
            }
        }
        if (options.vulkan_seed_hits_enabled) {
            params.vulkan_seed_hits_enabled = *options.vulkan_seed_hits_enabled;
        }
        if (options.vulkan_index_cache_enabled) {
            params.vulkan_index_cache_enabled = *options.vulkan_index_cache_enabled;
        }
        if (options.index_read_requirement) {
            const std::int32_t raw = *options.index_read_requirement;
            if (!bioseq::indexing::index_read_requirement_ordinal_valid(raw)) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg(
                         "aligner_options.index_read_requirement out of range: %d (expected 0..2)",
                         raw)));
            }
            index_read_requirement = static_cast<bioseq::indexing::index_read_requirement>(raw);
            index_read_requirement_present = true;
        }
        ReleaseTupleDesc(tupdesc);
        /* Free the detoasted copy if PG_DETOAST_DATUM actually copied. */
        free_detoasted_header();
        return index_read_requirement_present;
    }

    /*
     * Resolve the SIMD chaining policy only after the final per-call override.
     * The C++ API deliberately defaults to exact_visited.  PostgreSQL promotes
     * the explicitly selected SIMD_MM2 route to the named bounded candidate so
     * integration and performance gates exercise the real production choice.
     * Every non-SIMD algorithm is reset to exact: a stale bounded value must not
     * leak through a copied parameter object, and no unsupported request may be
     * translated into a different biological algorithm.
     */
    void finalize_simd_chaining_policy(bioseq::simd::minimap2_parameters& params) {
        using bioseq::simd::chaining_algorithm;
        using bioseq::simd::simd_chaining_policy;
        switch (params.chaining_algorithm) {
        case chaining_algorithm::simd_minimap2:
            params.simd_chaining_policy = simd_chaining_policy::bounded_batch_approximate;
            return;
        case chaining_algorithm::scalar_minimap2:
        case chaining_algorithm::range_minimum_query:
        case chaining_algorithm::vulkan_tiled_dynamic_programming:
            params.simd_chaining_policy = simd_chaining_policy::exact_visited;
            return;
        }
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("final SIMD chaining policy cannot be selected"),
             errdetail("chaining_algorithm has unknown ordinal %u",
                       static_cast<unsigned>(params.chaining_algorithm)),
             errhint("Select scalar_minimap2, simd_minimap2, range_minimum_query, or "
                     "vulkan_tiled_dynamic_programming explicitly; no fallback is performed.")));
    }

} // export custom-scan partition declarations
