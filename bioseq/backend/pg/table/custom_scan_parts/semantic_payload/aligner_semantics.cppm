/**
 * @file
 * @brief Validates aligner overrides and final SIMD chaining policy.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "storage/itemptr.h"
#include "utils/errcodes.h"
#include "utils/typcache.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

export module bioseq.postgres.table:custom_scan_semantic_payload_aligner_semantics;

import bioseq.indexing.type;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.postgres.support;

import :custom_scan_aligner_options_codec_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
export import :custom_scan_planner_aligner_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Reports whether the selected minimizer accepts packed sequence regions. */
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

    /**
     * @brief Returns the optional plan-time aligner-options constant from an mmsearch call.
     * @throws PostgreSQL ERROR when the call shape or option expression is invalid.
     */
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

    /**
     * @brief Checks whether two mmsearch calls can share one scan's parameters.
     *
     * The parameterized primary call must explicitly require an absent index and
     * leave every other option NULL. The extra constant-query call must use the
     * two-argument form. Both then use the same alignment settings, and the extra
     * call's default index policy permits the primary's choice to recompute.
     */
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
        // Releases only the temporary copy produced by PostgreSQL detoasting.
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
                bioseq::postgres::support::tuple_descriptor_attribute(tupdesc, expected.attribute_number - 1);
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

    /**
     * @brief Applies fields explicitly set in a constant aligner_options record.
     *
     * NULL fields keep the values already in @p params. The generated decoder checks
     * the SQL row layout, and algorithm selectors are checked before conversion.
     * @return True when the record explicitly sets the index-read requirement.
     * @throws PostgreSQL ERROR for an invalid call, record layout or selector.
     */
    [[nodiscard]] bool
    apply_aligner_options_override(FuncExpr* mmsearch_fe,
                                   bioseq::simd::minimap2_parameters& params,
                                   bioseq::indexing::index_read_requirement& index_read_requirement,
                                   const bioseq_stub_oids& oids,
                                   bool* vulkan_enabled_explicit) {
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
        /* Decode the composite value using its registered PostgreSQL row layout. */
        HeapTupleHeader header =
            reinterpret_cast<HeapTupleHeader>(PG_DETOAST_DATUM(opt_const->constvalue));
        Oid row_typeoid = HeapTupleHeaderGetTypeId(header);
        int32 row_typmod = HeapTupleHeaderGetTypMod(header);
        // Releases only the temporary copy produced by PostgreSQL detoasting.
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
            /* Reject unknown chaining selectors before converting the SQL integer
             * to an enum. For example, chaining_algorithm => 42 is invalid. */
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
        // Valid minimizer selectors are 0 through 4.
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
        /* Release the temporary detoasted copy, if one was allocated. */
        free_detoasted_header();
        return index_read_requirement_present;
    }

    /**
     * @brief Chooses the chaining policy after applying SQL overrides.
     *
     * SIMD minimap2 uses bounded_batch_approximate. Other supported chaining
     * algorithms use exact_visited, replacing any policy left in a copied parameter
     * object. An unknown algorithm raises a PostgreSQL error.
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
