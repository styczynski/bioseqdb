/**
 * @file
 * @brief Encodes stable planner semantics and validates cached custom-scan payloads.
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

export module bioseq.postgres.table:custom_scan_semantic_payload;

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

using namespace bioseq::postgres::support;

export {

    /*
     * Versioned, self-describing result-semantic snapshot for BioseqScan.
     *
     * The payload is one named List embedded in CustomScan.custom_private:
     *
     *   ["semantic_payload", INT4(version), INT4(field_count), fields...]
     *
     * Every scalar is a typed PostgreSQL Const so copyObject(), prepared-plan
     * storage, parallel plan transfer, and node serialization retain the exact
     * value without any process-local C++ pointer.  Floating-point parameters are
     * stored as their exact IEEE-754 binary32 bits in an INT8 Const.
     */
    inline constexpr auto k_bioseq_semantic_payload_name = std::to_array("semantic_payload");
    inline constexpr std::int32_t k_bioseq_semantic_payload_format_version = 1;
    inline constexpr std::int32_t k_bioseq_semantic_payload_field_count = 42;
    inline constexpr int k_bioseq_semantic_payload_header_nodes = 3;

    enum class bioseq_semantic_field : int {
        kmer_size = 0,
        window_size,
        max_gap,
        max_gap_ref,
        bandwidth,
        max_chain_skip,
        max_chain_iter,
        min_seed_count,
        min_chain_score,
        chain_gap_scale,
        chain_skip_scale,
        mask_level,
        mask_len,
        pri_ratio,
        best_n,
        match_score,
        mismatch_pen,
        gap_open1,
        gap_ext1,
        gap_open2,
        gap_ext2,
        sc_ambi,
        zdrop,
        end_bonus,
        max_occ,
        mid_occ,
        block_min_size,
        block_max_size,
        bandwidth_long,
        chaining_algorithm,
        base_alignment_algorithm,
        minimizer_algorithm,
        vulkan_enabled,
        vulkan_seed_hits_enabled,
        vulkan_index_cache_enabled,
        vulkan_fused_frontend_enabled,
        vulkan_gpu_index_build_enabled,
        vulkan_extend_enabled,
        vulkan_ordered_sketch_tile_bp,
        vulkan_radix_digit_bits,
        read_sequence_index,
        index_read_requirement,
    };

    static_assert(static_cast<int>(bioseq_semantic_field::index_read_requirement) + 1 ==
                  k_bioseq_semantic_payload_field_count);

    struct frozen_scan_semantics {
        bioseq::simd::minimap2_parameters params{};
        bool read_sequence_index = false;
        bioseq::indexing::index_read_requirement index_read_requirement =
            bioseq::indexing::index_read_requirement::allow;
    };

    [[nodiscard]] Const* make_semantic_int4_const(const std::int32_t value) {
        return makeConst(
            INT4OID, -1, InvalidOid, sizeof(std::int32_t), Int32GetDatum(value), false, true);
    }

    [[nodiscard]] Const* make_semantic_int8_const(const std::int64_t value) {
        return makeConst(INT8OID,
                         -1,
                         InvalidOid,
                         sizeof(std::int64_t),
                         Int64GetDatum(value),
                         false,
                         FLOAT8PASSBYVAL);
    }

    [[nodiscard]] Const* make_semantic_bool_const(const bool value) {
        return makeConst(BOOLOID, -1, InvalidOid, sizeof(bool), BoolGetDatum(value), false, true);
    }

    [[nodiscard]] Const* make_semantic_float_bits_const(const float value) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        return make_semantic_int8_const(static_cast<std::int64_t>(bits));
    }

    [[nodiscard]] List* make_bioseq_semantic_payload(const frozen_scan_semantics& semantics) {
        const auto& params = semantics.params;
        List* payload =
            list_make3(makeString(pstrdup(k_bioseq_semantic_payload_name.data())),
                       make_semantic_int4_const(k_bioseq_semantic_payload_format_version),
                       make_semantic_int4_const(k_bioseq_semantic_payload_field_count));
        payload = lappend(payload, make_semantic_int8_const(params.kmer_size));
        payload = lappend(payload, make_semantic_int8_const(params.window_size));
        payload = lappend(payload, make_semantic_int8_const(params.max_gap));
        payload = lappend(payload, make_semantic_int8_const(params.max_gap_ref));
        payload = lappend(payload, make_semantic_int8_const(params.bandwidth));
        payload = lappend(payload, make_semantic_int8_const(params.max_chain_skip));
        payload = lappend(payload, make_semantic_int8_const(params.max_chain_iter));
        payload = lappend(payload, make_semantic_int8_const(params.min_seed_count));
        payload = lappend(payload, make_semantic_int8_const(params.min_chain_score));
        payload = lappend(payload, make_semantic_float_bits_const(params.chain_gap_scale));
        payload = lappend(payload, make_semantic_float_bits_const(params.chain_skip_scale));
        payload = lappend(payload, make_semantic_float_bits_const(params.mask_level));
        payload = lappend(payload, make_semantic_int8_const(params.mask_len));
        payload = lappend(payload, make_semantic_float_bits_const(params.pri_ratio));
        payload = lappend(payload, make_semantic_int8_const(params.best_n));
        payload = lappend(payload, make_semantic_int8_const(params.match_score));
        payload = lappend(payload, make_semantic_int8_const(params.mismatch_pen));
        payload = lappend(payload, make_semantic_int8_const(params.gap_open1));
        payload = lappend(payload, make_semantic_int8_const(params.gap_ext1));
        payload = lappend(payload, make_semantic_int8_const(params.gap_open2));
        payload = lappend(payload, make_semantic_int8_const(params.gap_ext2));
        payload = lappend(payload, make_semantic_int8_const(params.sc_ambi));
        payload = lappend(payload, make_semantic_int8_const(params.zdrop));
        payload = lappend(payload, make_semantic_int8_const(params.end_bonus));
        payload = lappend(payload, make_semantic_int8_const(params.max_occ));
        payload = lappend(payload, make_semantic_int8_const(params.mid_occ));
        payload = lappend(
            payload, make_semantic_int8_const(static_cast<std::int64_t>(params.block_min_size)));
        payload = lappend(
            payload, make_semantic_int8_const(static_cast<std::int64_t>(params.block_max_size)));
        payload = lappend(payload, make_semantic_int8_const(params.bandwidth_long));
        payload =
            lappend(payload,
                    make_semantic_int4_const(static_cast<std::int32_t>(params.chaining_algorithm)));
        payload = lappend(
            payload,
            make_semantic_int4_const(static_cast<std::int32_t>(params.base_alignment_algorithm)));
        payload = lappend(
            payload,
            make_semantic_int4_const(static_cast<std::int32_t>(params.minimizer_algorithm)));
        payload = lappend(payload, make_semantic_bool_const(params.vulkan_enabled));
        payload = lappend(payload, make_semantic_bool_const(params.vulkan_seed_hits_enabled));
        payload = lappend(payload, make_semantic_bool_const(params.vulkan_index_cache_enabled));
        payload = lappend(payload, make_semantic_bool_const(params.vulkan_fused_frontend_enabled));
        payload = lappend(payload, make_semantic_bool_const(params.vulkan_gpu_index_build_enabled));
        payload = lappend(payload, make_semantic_bool_const(params.vulkan_extend_enabled));
        payload = lappend(payload,
                          make_semantic_int8_const(
                              static_cast<std::int64_t>(params.vulkan_ordered_sketch_tile_bp)));
        payload = lappend(
            payload,
            make_semantic_int8_const(static_cast<std::int64_t>(params.vulkan_radix_digit_bits)));
        payload = lappend(payload, make_semantic_bool_const(semantics.read_sequence_index));
        payload = lappend(
            payload,
            make_semantic_int4_const(static_cast<std::int32_t>(semantics.index_read_requirement)));
        Assert(list_length(payload) ==
               k_bioseq_semantic_payload_header_nodes + k_bioseq_semantic_payload_field_count);
        return payload;
    }

    [[nodiscard]] bool semantic_const_metadata_valid(const Const* constant,
                                                     const Oid type_oid,
                                                     const int16_t type_length,
                                                     const bool by_value) noexcept {
        return constant != nullptr && constant->consttype == type_oid &&
               constant->consttypmod == -1 && constant->constcollid == InvalidOid &&
               constant->constlen == type_length && constant->constbyval == by_value &&
               !constant->constisnull &&
               (by_value || DatumGetPointer(constant->constvalue) != nullptr);
    }

    [[nodiscard]] bool decode_semantic_int4(const Node* node, std::int32_t* output) noexcept {
        const Const* constant =
            node != nullptr && IsA(node, Const) ? reinterpret_cast<const Const*>(node) : nullptr;
        if (output == nullptr ||
            !semantic_const_metadata_valid(constant, INT4OID, sizeof(std::int32_t), true)) {
            return false;
        }
        *output = DatumGetInt32(constant->constvalue);
        return true;
    }

    [[nodiscard]] bool decode_semantic_int8(const Node* node, std::int64_t* output) noexcept {
        const Const* constant =
            node != nullptr && IsA(node, Const) ? reinterpret_cast<const Const*>(node) : nullptr;
        if (output == nullptr || !semantic_const_metadata_valid(
                                     constant, INT8OID, sizeof(std::int64_t), FLOAT8PASSBYVAL)) {
            return false;
        }
        *output = DatumGetInt64(constant->constvalue);
        return true;
    }

    [[nodiscard]] bool decode_semantic_bool(const Node* node, bool* output) noexcept {
        const Const* constant =
            node != nullptr && IsA(node, Const) ? reinterpret_cast<const Const*>(node) : nullptr;
        if (output == nullptr ||
            !semantic_const_metadata_valid(constant, BOOLOID, sizeof(bool), true) ||
            (constant->constvalue != BoolGetDatum(false) &&
             constant->constvalue != BoolGetDatum(true))) {
            return false;
        }
        *output = DatumGetBool(constant->constvalue);
        return true;
    }

    [[nodiscard]] const Node* semantic_field_node(const List* payload,
                                                  const bioseq_semantic_field field) noexcept {
        if (payload == nullptr)
            return nullptr;
        const int index = k_bioseq_semantic_payload_header_nodes + static_cast<int>(field);
        return index >= 0 && index < list_length(payload)
                   ? reinterpret_cast<const Node*>(list_nth(payload, index))
                   : nullptr;
    }

    [[nodiscard]] bool decode_semantic_int32_field(const List* payload,
                                                   const bioseq_semantic_field field,
                                                   std::int32_t* output) noexcept {
        std::int64_t decoded = 0;
        if (!decode_semantic_int8(semantic_field_node(payload, field), &decoded) ||
            decoded < (std::numeric_limits<std::int32_t>::min)() ||
            decoded > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        *output = static_cast<std::int32_t>(decoded);
        return true;
    }

    [[nodiscard]] bool decode_semantic_size_field(const List* payload,
                                                  const bioseq_semantic_field field,
                                                  std::size_t* output) noexcept {
        std::int64_t decoded = 0;
        if (!decode_semantic_int8(semantic_field_node(payload, field), &decoded) || decoded < 0 ||
            static_cast<std::uint64_t>(decoded) >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            return false;
        }
        *output = static_cast<std::size_t>(decoded);
        return true;
    }

    [[nodiscard]] bool decode_semantic_uint32_field(const List* payload,
                                                    const bioseq_semantic_field field,
                                                    std::uint32_t* output) noexcept {
        std::int64_t decoded = 0;
        if (!decode_semantic_int8(semantic_field_node(payload, field), &decoded) || decoded < 0 ||
            static_cast<std::uint64_t>(decoded) > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        *output = static_cast<std::uint32_t>(decoded);
        return true;
    }

    [[nodiscard]] bool decode_semantic_float_field(const List* payload,
                                                   const bioseq_semantic_field field,
                                                   float* output) noexcept {
        std::int64_t decoded = 0;
        if (!decode_semantic_int8(semantic_field_node(payload, field), &decoded) || decoded < 0 ||
            static_cast<std::uint64_t>(decoded) > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        *output = std::bit_cast<float>(static_cast<std::uint32_t>(decoded));
        return true;
    }

    [[nodiscard]] const List* find_bioseq_semantic_payload(const List* custom_private) noexcept {
        const List* found = nullptr;
        for (const Node* candidate : postgres_list<Node>(const_cast<List*>(custom_private))) {
            if (candidate == nullptr || !IsA(candidate, List))
                continue;
            const List* list = reinterpret_cast<const List*>(candidate);
            const Node* name_node =
                list == NIL ? nullptr : reinterpret_cast<const Node*>(linitial(list));
            if (name_node == nullptr || !IsA(name_node, String) || strVal(name_node) == nullptr ||
                std::strcmp(strVal(name_node), k_bioseq_semantic_payload_name.data()) != 0) {
                continue;
            }
            if (found != nullptr)
                return nullptr;
            found = list;
        }
        return found;
    }

    [[nodiscard]] bool decode_bioseq_semantic_payload(const List* custom_private,
                                                      frozen_scan_semantics* output) noexcept {
        if (output == nullptr)
            return false;
        const List* payload = find_bioseq_semantic_payload(custom_private);
        if (payload == nullptr ||
            list_length(payload) !=
                k_bioseq_semantic_payload_header_nodes + k_bioseq_semantic_payload_field_count) {
            return false;
        }
        std::int32_t version = 0;
        std::int32_t field_count = 0;
        if (!decode_semantic_int4(reinterpret_cast<const Node*>(list_nth(payload, 1)), &version) ||
            !decode_semantic_int4(reinterpret_cast<const Node*>(list_nth(payload, 2)),
                                  &field_count) ||
            version != k_bioseq_semantic_payload_format_version ||
            field_count != k_bioseq_semantic_payload_field_count) {
            return false;
        }

        frozen_scan_semantics decoded{};
        auto& params = decoded.params;
        if (!decode_semantic_int32_field(
                payload, bioseq_semantic_field::kmer_size, &params.kmer_size) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::window_size, &params.window_size) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::max_gap, &params.max_gap) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::max_gap_ref, &params.max_gap_ref) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::bandwidth, &params.bandwidth) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::max_chain_skip, &params.max_chain_skip) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::max_chain_iter, &params.max_chain_iter) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::min_seed_count, &params.min_seed_count) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::min_chain_score, &params.min_chain_score) ||
            !decode_semantic_float_field(
                payload, bioseq_semantic_field::chain_gap_scale, &params.chain_gap_scale) ||
            !decode_semantic_float_field(
                payload, bioseq_semantic_field::chain_skip_scale, &params.chain_skip_scale) ||
            !decode_semantic_float_field(
                payload, bioseq_semantic_field::mask_level, &params.mask_level) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::mask_len, &params.mask_len) ||
            !decode_semantic_float_field(
                payload, bioseq_semantic_field::pri_ratio, &params.pri_ratio) ||
            !decode_semantic_int32_field(payload, bioseq_semantic_field::best_n, &params.best_n) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::match_score, &params.match_score) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::mismatch_pen, &params.mismatch_pen) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::gap_open1, &params.gap_open1) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::gap_ext1, &params.gap_ext1) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::gap_open2, &params.gap_open2) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::gap_ext2, &params.gap_ext2) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::sc_ambi, &params.sc_ambi) ||
            !decode_semantic_int32_field(payload, bioseq_semantic_field::zdrop, &params.zdrop) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::end_bonus, &params.end_bonus) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::max_occ, &params.max_occ) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::mid_occ, &params.mid_occ) ||
            !decode_semantic_size_field(
                payload, bioseq_semantic_field::block_min_size, &params.block_min_size) ||
            !decode_semantic_size_field(
                payload, bioseq_semantic_field::block_max_size, &params.block_max_size) ||
            !decode_semantic_int32_field(
                payload, bioseq_semantic_field::bandwidth_long, &params.bandwidth_long)) {
            return false;
        }

        std::int32_t chaining = 0;
        std::int32_t base_alignment = 0;
        std::int32_t minimizer = 0;
        if (!decode_semantic_int4(
                semantic_field_node(payload, bioseq_semantic_field::chaining_algorithm),
                &chaining) ||
            !decode_semantic_int4(
                semantic_field_node(payload, bioseq_semantic_field::base_alignment_algorithm),
                &base_alignment) ||
            !decode_semantic_int4(
                semantic_field_node(payload, bioseq_semantic_field::minimizer_algorithm),
                &minimizer)) {
            return false;
        }
        using chaining_type = bioseq::simd::chaining_algorithm;
        using base_alignment_type = bioseq::simd::base_alignment_algorithm;
        using minimizer_type = bioseq::simd::minimizer_algorithm;
        switch (static_cast<chaining_type>(chaining)) {
        case chaining_type::scalar_minimap2:
        case chaining_type::simd_minimap2:
        case chaining_type::range_minimum_query:
        case chaining_type::vulkan_tiled_dynamic_programming:
            params.chaining_algorithm = static_cast<chaining_type>(chaining);
            break;
        default:
            return false;
        }
        switch (static_cast<base_alignment_type>(base_alignment)) {
        case base_alignment_type::ksw2:
        case base_alignment_type::block_aligner:
        case base_alignment_type::ksw2_batched:
            params.base_alignment_algorithm = static_cast<base_alignment_type>(base_alignment);
            break;
        default:
            return false;
        }
        switch (static_cast<minimizer_type>(minimizer)) {
        case minimizer_type::scalar:
        case minimizer_type::simd_minimap2_reference:
        case minimizer_type::two_stack_nthash:
        case minimizer_type::vulkan_minimap_integer:
        case minimizer_type::bounded_nthash_simd:
            params.minimizer_algorithm = static_cast<minimizer_type>(minimizer);
            break;
        default:
            return false;
        }

        if (!decode_semantic_bool(
                semantic_field_node(payload, bioseq_semantic_field::vulkan_enabled),
                &params.vulkan_enabled) ||
            !decode_semantic_bool(
                semantic_field_node(payload, bioseq_semantic_field::vulkan_seed_hits_enabled),
                &params.vulkan_seed_hits_enabled) ||
            !decode_semantic_bool(
                semantic_field_node(payload, bioseq_semantic_field::vulkan_index_cache_enabled),
                &params.vulkan_index_cache_enabled) ||
            !decode_semantic_bool(
                semantic_field_node(payload, bioseq_semantic_field::vulkan_fused_frontend_enabled),
                &params.vulkan_fused_frontend_enabled) ||
            !decode_semantic_bool(
                semantic_field_node(payload, bioseq_semantic_field::vulkan_gpu_index_build_enabled),
                &params.vulkan_gpu_index_build_enabled) ||
            !decode_semantic_bool(
                semantic_field_node(payload, bioseq_semantic_field::vulkan_extend_enabled),
                &params.vulkan_extend_enabled) ||
            !decode_semantic_uint32_field(payload,
                                          bioseq_semantic_field::vulkan_ordered_sketch_tile_bp,
                                          &params.vulkan_ordered_sketch_tile_bp) ||
            !decode_semantic_uint32_field(payload,
                                          bioseq_semantic_field::vulkan_radix_digit_bits,
                                          &params.vulkan_radix_digit_bits) ||
            !decode_semantic_bool(
                semantic_field_node(payload, bioseq_semantic_field::read_sequence_index),
                &decoded.read_sequence_index)) {
            return false;
        }
        std::int32_t requirement = 0;
        if (!decode_semantic_int4(
                semantic_field_node(payload, bioseq_semantic_field::index_read_requirement),
                &requirement) ||
            !bioseq::indexing::index_read_requirement_ordinal_valid(requirement)) {
            return false;
        }
        decoded.index_read_requirement =
            static_cast<bioseq::indexing::index_read_requirement>(requirement);
        *output = decoded;
        return true;
    }

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

    /*
     * Context passed to VarInjectorMutator through the void* context arg.
     * Bundles the allocated placeholder Consts with the resolved OIDs so
     * the mutator can dispatch on funcid -> Const* without a global.
     *
     * `current_relation` is used exclusively when rewriting
     * `symbol_count(Var, 'L')` call sites to resolve the column name
     * (attname) referenced by the Var — so the EXPLAIN output can say
     * "Fetched from meta: seq.symbol_count('A') AS ...".
     */
    struct var_injector_context {
        placeholder_constants* consts;
        const bioseq_stub_oids* oids;
        Relation current_relation;
        Index match_sequence_varno;
        AttrNumber match_sequence_attno;
        Node* match_target_recipe;
        List* match_target_lineage;
        List* match_helper_sites;
        int stable_match_id;
    };

    /* functions to cost paths in-place */
    void cost_bioseq_column_paths(PlannerInfo* root, RelOptInfo* rel, Oid relation_id);
    void cost_bioseq_column_index_path(PlannerInfo* root,
                                       RelOptInfo* rel,
                                       Oid relation_id,
                                       IndexPath* index_path);
    void cost_bioseq_column_seq_path(RelOptInfo* rel, Oid relation_id, Path* path);
    void adjust_bioseq_column_parallel_scan_cost(Path* path);
    void cost_bioseq_column_scan(PlannerInfo* root,
                                 RelOptInfo* rel,
                                 Oid relation_id,
                                 CustomPath* cpath,
                                 int number_of_columns_read,
                                 int n_clauses);

    /* functions to add new paths */
    void add_bioseq_column_scan_paths(PlannerInfo* root, RelOptInfo* rel, RangeTblEntry* rte);
    Path* add_bioseq_column_scan_path(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      RangeTblEntry* rte,
                                      Relids required_relids);

    /* helper functions to be used when costing paths or altering them */
    void remove_paths_by_predicate(RelOptInfo* rel, path_predicate remove_path_predicate);
    bool is_not_index_path(Path* path);
    Cost bioseq_column_index_scan_additional_cost(PlannerInfo* root,
                                                  RelOptInfo* rel,
                                                  Oid relation_id,
                                                  IndexPath* index_path);
    int relation_id_get_number_of_attributes(Oid relation_id);
    Cost bioseq_column_per_stripe_scan_cost(RelOptInfo* rel,
                                            Oid relation_id,
                                            int number_of_columns_read);
    uint64_t bioseq_column_table_stripe_count(Oid relation_id);
    Path* create_bioseq_column_seq_scan_path(PlannerInfo* root, RelOptInfo* rel, Oid relation_id);
    void add_bioseq_column_scan_paths_rec(PlannerInfo* root,
                                          RelOptInfo* rel,
                                          RangeTblEntry* rte,
                                          Relids param_relids,
                                          Relids candidate_relids,
                                          int depth_limit);

    /* hooks and callbacks */
    void bioseq_column_set_rel_pathlist_hook(PlannerInfo* root,
                                             RelOptInfo* rel,
                                             Index rti,
                                             RangeTblEntry* rte);
    void bioseq_column_get_relation_info_hook(PlannerInfo* root,
                                              Oid relation_object_id,
                                              bool inhparent,
                                              RelOptInfo* rel);
    void bioseq_batch_join_set_join_pathlist_hook(PlannerInfo* root,
                                                  RelOptInfo* joinrel,
                                                  RelOptInfo* outerrel,
                                                  RelOptInfo* innerrel,
                                                  JoinType jointype,
                                                  JoinPathExtraData* extra);

    Plan* bioseq_column_scan_path_plan_custom_path(PlannerInfo* root,
                                                   RelOptInfo* rel,
                                                   CustomPath* best_path,
                                                   List* tlist,
                                                   List* clauses,
                                                   List* custom_plans);
    List* bioseq_column_scan_path_reparameterize_custom_path_by_child(PlannerInfo* root,
                                                                      List* custom_private,
                                                                      RelOptInfo* child_rel);
    Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan);
    Plan* bioseq_batch_join_plan_custom_path(PlannerInfo* root,
                                             RelOptInfo* rel,
                                             CustomPath* best_path,
                                             List* tlist,
                                             List* clauses,
                                             List* custom_plans);
    Node* bioseq_batch_join_create_custom_scan_state(CustomScan* cscan);
    Plan* bioseq_per_row_join_plan_custom_path(PlannerInfo* root,
                                               RelOptInfo* rel,
                                               CustomPath* best_path,
                                               List* tlist,
                                               List* clauses,
                                               List* custom_plans);
    List* bioseq_per_row_join_path_reparameterize_custom_path_by_child(PlannerInfo* root,
                                                                       List* custom_private,
                                                                       RelOptInfo* child_rel);

    void bioseq_column_scan_begin_custom_scan(CustomScanState* node, EState* estate, int eflags);

    TupleTableSlot* custom_exec_scan(scan_state* node,
                                     ExecScanAccessMtd access_mtd,
                                     ExecScanRecheckMtd recheck_mtd);
    TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node);
    void bioseq_column_scan_end_custom_scan(CustomScanState* node);
    void bioseq_column_scan_rescan_custom_scan(CustomScanState* node);
    void bioseq_column_scan_explain_custom_scan(CustomScanState* node,
                                                List* ancestors,
                                                ExplainState* es);
    Size bioseq_column_estimate_dsm_custom_scan(CustomScanState* node, ParallelContext* pcxt);
    void bioseq_column_initialize_dsm_custom_scan(CustomScanState* node,
                                                  ParallelContext* pcxt,
                                                  void* coordinate);
    void bioseq_column_reinitialize_dsm_custom_scan(CustomScanState* node,
                                                    ParallelContext* pcxt,
                                                    void* coordinate);
    void bioseq_column_initialize_worker_custom_scan(CustomScanState* node,
                                                     shm_toc* toc,
                                                     void* coordinate);
    void bioseq_batch_join_begin_custom_scan(CustomScanState* node, EState* estate, int eflags);
    TupleTableSlot* bioseq_batch_join_exec_custom_scan(CustomScanState* node);
    void bioseq_batch_join_end_custom_scan(CustomScanState* node);
    void bioseq_batch_join_rescan_custom_scan(CustomScanState* node);
    void
    bioseq_batch_join_explain_custom_scan(CustomScanState* node, List* ancestors, ExplainState* es);
    void bioseq_per_row_join_begin_custom_scan(CustomScanState* node, EState* estate, int eflags);
    TupleTableSlot* bioseq_per_row_join_exec_custom_scan(CustomScanState* node);
    void bioseq_per_row_join_end_custom_scan(CustomScanState* node);
    void bioseq_per_row_join_rescan_custom_scan(CustomScanState* node);
    void bioseq_per_row_join_explain_custom_scan(CustomScanState* node,
                                                 List* ancestors,
                                                 ExplainState* es);
    [[nodiscard]] constexpr bioseq::simd::pipe::query_sketch_route query_sketch_plan_route_for(
        const bioseq::indexing::index_read_requirement requirement) noexcept {
        return requirement == bioseq::indexing::index_read_requirement::require_compatible_persisted
                   ? bioseq::simd::pipe::query_sketch_route::compatible_persisted_only
                   : bioseq::simd::pipe::query_sketch_route::recompute_capable;
    }
    void
    explain_pipeline_execution_stats(const bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                     ExplainState* es,
                                     const bioseq::simd::minimap2_parameters* aligner_params,
                                     const bioseq::simd::pipe::pipeline_description* pipeline_desc,
                                     bioseq::simd::pipe::query_sketch_route query_sketch_route,
                                     bool highway_dispatch_already_rendered);

    /* helper functions to build strings for EXPLAIN */
    inline const char* bioseq_column_pushdown_clauses_str(List* context, List* clauses);
    inline const char* bioseq_column_projected_columns_str(List* context, List* projected_columns);
    List* set_deparse_context_planstate(List* dpcontext, Node* node, List* ancestors);

    /* other helpers */
    List* bioseq_column_var_needed(scan_state* bioseq_scan_state);

    /* saved hook value in case of unload */
    set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = nullptr;
    set_join_pathlist_hook_type previous_set_join_pathlist_hook = nullptr;
    get_relation_info_hook_type previous_get_relation_info_hook = nullptr;

    [[nodiscard]] int planner_debug_level() noexcept {
        return static_cast<int>(bioseq::settings::planner::diagnostics::log_level.get_value());
    }

#ifdef BIOSEQ_TEST_INDEXES
    enum slice_project_dsm_test_fixture : int {
        k_slice_dsm_test_none = 0,
        k_slice_dsm_test_incomplete = 1,
        k_slice_dsm_test_malformed = 2,
    };

    /* Backend-local, one-shot diagnostic state.  The test hook arms one
     * mode; the next analyzed SliceProject takes it before running any fixture
     * code, so ERROR during the fixture cannot leave a latent injection behind. */
    int pending_slice_dsm_test_fixture = k_slice_dsm_test_none;
#endif

    /** @brief Arm one backend-local SliceProject DSM diagnostic fixture. */
    void bioseq_arm_slice_dsm_fixture_for_test(std::string_view mode) {
#ifdef BIOSEQ_TEST_INDEXES
        int requested_mode = k_slice_dsm_test_none;
        if (mode == "incomplete") {
            requested_mode = k_slice_dsm_test_incomplete;
        } else if (mode == "malformed") {
            requested_mode = k_slice_dsm_test_malformed;
        } else if (mode != "none") {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("unknown SliceProject DSM diagnostic fixture"),
                     errdetail("fixture=%.*s", static_cast<int>(mode.size()), mode.data()),
                     errhint("Expected incomplete, malformed, or none.")));
        }
        if (requested_mode == k_slice_dsm_test_none) {
            pending_slice_dsm_test_fixture = k_slice_dsm_test_none;
            return;
        }
        if (pending_slice_dsm_test_fixture != k_slice_dsm_test_none) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("a SliceProject DSM diagnostic fixture is already armed"),
                     errhint("Run the analyzed SliceProject or disarm it with fixture 'none'.")));
        }
        pending_slice_dsm_test_fixture = requested_mode;
#else
        (void)mode;
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("SliceProject DSM diagnostics require a test build"),
                 errdetail("The extension was built without BIOSEQ_TEST_INDEXES.")));
#endif
    }

    /** Returns the stable PostgreSQL path callback table for base Bioseq scans. */
    [[nodiscard]] const CustomPathMethods* bioseq_column_scan_path_methods() noexcept;

    /** Returns the stable PostgreSQL scan callback table for base Bioseq scans. */
    [[nodiscard]] const CustomScanMethods* bioseq_column_scan_scan_methods() noexcept;

    /** Returns the stable PostgreSQL path callback table for batched joins. */
    [[nodiscard]] const CustomPathMethods* bioseq_batch_join_path_methods() noexcept;

    /** Returns the stable PostgreSQL scan callback table for batched joins. */
    [[nodiscard]] const CustomScanMethods* bioseq_batch_join_scan_methods() noexcept;

    /** Returns the stable PostgreSQL executor callback table for batched joins. */
    [[nodiscard]] const CustomExecMethods* bioseq_batch_join_execute_methods() noexcept;

    /** Returns the stable PostgreSQL path callback table for per-row joins. */
    [[nodiscard]] const CustomPathMethods* bioseq_per_row_join_path_methods() noexcept;

    /** Returns the stable PostgreSQL executor callback table for base Bioseq scans. */
    [[nodiscard]] const CustomExecMethods* bioseq_column_scan_execute_methods() noexcept;

    /** @brief Return the stable base-scan callback table registered with PostgreSQL. */
    inline const CustomScanMethods* bioseq_customscan_methods(void) {
        return bioseq_column_scan_scan_methods();
    }

    /** @brief Report whether a base scan contains a decodable semantic snapshot. */
    bool bioseq_customscan_has_valid_semantic_payload(const CustomScan* scan) noexcept {
        if (scan == nullptr || scan->methods != bioseq_column_scan_scan_methods()) {
            return false;
        }
        frozen_scan_semantics semantics{};
        return decode_bioseq_semantic_payload(scan->custom_private, &semantics);
    }

    /** @brief Copy the cache-stable private payload for a valid base scan. */
    List* bioseq_customscan_cache_private_payload(const CustomScan* scan) {
        if (!bioseq_customscan_has_valid_semantic_payload(scan)) {
            return NIL;
        }
        auto* private_payload = static_cast<List*>(copyObject(scan->custom_private));
        List* locator_tag = nullptr;
        for (Node* possible_tag : postgres_list<Node>(private_payload)) {
            if (possible_tag == nullptr || !IsA(possible_tag, List))
                continue;
            auto* tag = reinterpret_cast<List*>(possible_tag);
            if (tag == NIL || !IsA(reinterpret_cast<Node*>(linitial(tag)), Integer) ||
                intVal(linitial(tag)) != k_bioseq_locator_passthrough_magic) {
                continue;
            }
            if (locator_tag != nullptr || list_length(tag) != 4 ||
                !IsA(list_nth(tag, 1), Integer) || !IsA(list_nth(tag, 2), Integer) ||
                !IsA(list_nth(tag, 3), Integer) ||
                intVal(list_nth(tag, 1)) != k_bioseq_batch_join_plan_version ||
                intVal(list_nth(tag, 2)) <= 0 || intVal(list_nth(tag, 3)) <= 0) {
                return NIL;
            }
            locator_tag = tag;
        }
        if (locator_tag != nullptr) {
            /* This token pairs a locator child with its parent only inside one
             * planned statement; the monotonic value is not result semantics. */
            lfirst(list_nth_cell(locator_tag, 2)) = makeInteger(0);
        }
        return private_payload;
    }

    /** @brief Return the stable BatchJoin callback table registered with PostgreSQL. */
    inline const CustomScanMethods* bioseq_batch_join_customscan_methods(void) {
        return bioseq_batch_join_scan_methods();
    }

    [[nodiscard]] bool bioseq_batch_join_private_integer(const List* nodes,
                                                         const int index) noexcept {
        if (nodes == nullptr || index < 0 || index >= list_length(nodes)) {
            return false;
        }
        const Node* node = reinterpret_cast<const Node*>(list_nth(nodes, index));
        return node != nullptr && IsA(node, Integer);
    }

    [[nodiscard]] bool
    bioseq_batch_join_target_list_is_structural(const List* target_list) noexcept {
        if (target_list == nullptr)
            return false;
        int expected_resno = 1;
        for (const TargetEntry* entry :
             postgres_list<TargetEntry>(const_cast<List*>(target_list))) {
            if (entry == nullptr || !IsA(entry, TargetEntry) || entry->expr == nullptr ||
                entry->resno != expected_resno++) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool
    bioseq_batch_join_descriptor_is_structural(const CustomScan* scan,
                                               const int descriptor_index) noexcept {
        if (scan == nullptr || descriptor_index < 0 ||
            descriptor_index >= list_length(scan->custom_private)) {
            return false;
        }
        const Node* descriptor_node =
            reinterpret_cast<const Node*>(list_nth(scan->custom_private, descriptor_index));
        if (descriptor_node == nullptr || !IsA(descriptor_node, List)) {
            return false;
        }
        const auto* descriptor = reinterpret_cast<const List*>(descriptor_node);
        if (list_length(descriptor) != k_bioseq_match_owner_descriptor_fields) {
            return false;
        }
        for (const int index : {k_match_owner_magic,
                                k_match_owner_version,
                                k_match_owner_stable_id,
                                k_match_owner_base_relid}) {
            if (!bioseq_batch_join_private_integer(descriptor, index)) {
                return false;
            }
        }
        const int stable_id = intVal(list_nth(descriptor, k_match_owner_stable_id));
        const Node* target_recipe =
            reinterpret_cast<const Node*>(list_nth(descriptor, k_match_owner_target_recipe));
        const Node* base_lineage =
            reinterpret_cast<const Node*>(list_nth(descriptor, k_match_owner_base_lineage));
        return intVal(list_nth(descriptor, k_match_owner_magic)) ==
                   k_bioseq_match_owner_descriptor_magic &&
               intVal(list_nth(descriptor, k_match_owner_version)) ==
                   k_bioseq_match_owner_descriptor_version &&
               (stable_id == 0 || stable_id == scan->scan.plan.plan_node_id) &&
               target_recipe != nullptr && (base_lineage == nullptr || IsA(base_lineage, List)) &&
               bioseq_is_concrete_rte_varno(
                   static_cast<Index>(intVal(list_nth(descriptor, k_match_owner_base_relid))));
    }

    /** @brief Report whether a BatchJoin contains the complete serialized executor contract. */
    bool bioseq_batch_join_customscan_has_valid_semantic_payload(const CustomScan* scan) noexcept {
        if (scan == nullptr || scan->methods != bioseq_batch_join_scan_methods() ||
            scan->scan.scanrelid != 0 || scan->scan.plan.plan_node_id < 0 ||
            outerPlan(&scan->scan.plan) != nullptr || innerPlan(&scan->scan.plan) != nullptr ||
            scan->custom_exprs != NIL || bms_is_empty(scan->custom_relids) ||
            !bioseq_batch_join_target_list_is_structural(scan->scan.plan.targetlist) ||
            !bioseq_batch_join_target_list_is_structural(scan->custom_scan_tlist) ||
            !bioseq_batch_join_private_integer(scan->custom_private, 0)) {
            return false;
        }
        const int version = intVal(linitial(scan->custom_private));
        /* V4 shares the PostgreSQL method table but has a different one-child
         * executor ABI.  Exact source elision is deliberately V3-only until that
         * ABI gets its own complete recipe contract. */
        if (version != k_bioseq_batch_join_plan_version ||
            list_length(scan->custom_private) != k_bioseq_batch_join_private_fields ||
            list_length(scan->custom_plans) != 2 ||
            !bioseq_batch_join_descriptor_is_structural(scan,
                                                        k_batch_join_match_owner_descriptor)) {
            return false;
        }
        for (const int index : {k_batch_join_version,
                                k_batch_join_token,
                                k_batch_join_target_relation_oid,
                                k_batch_join_target_storage_seq_attno,
                                k_batch_join_target_slot_seq_resno,
                                k_batch_join_target_slot_rowno_resno,
                                k_batch_join_query_slot_seq_resno,
                                k_batch_join_target_column_count,
                                k_batch_join_query_column_count,
                                k_batch_join_alphabet_code,
                                k_batch_join_storage_unit_byte_width,
                                k_batch_join_index_read_requirement}) {
            if (!bioseq_batch_join_private_integer(scan->custom_private, index)) {
                return false;
            }
        }
        const auto* target_child =
            reinterpret_cast<const CustomScan*>(linitial(scan->custom_plans));
        const Plan* query_child = static_cast<const Plan*>(lsecond(scan->custom_plans));
        const Node* sites = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_placeholder_sites));
        const Node* mmsearch = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_mmsearch_expr));
        const Node* honest_startup_cost = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_honest_startup_cost));
        const Node* honest_total_cost = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_honest_total_cost));
        const Node* incumbent_cost_floor = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_incumbent_cost_floor));
        return target_child != nullptr && IsA(target_child, CustomScan) &&
               target_child->methods == bioseq_column_scan_scan_methods() &&
               query_child != nullptr &&
               intVal(list_nth(scan->custom_private, k_batch_join_token)) > 0 &&
               OidIsValid(static_cast<Oid>(
                   intVal(list_nth(scan->custom_private, k_batch_join_target_relation_oid)))) &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_storage_seq_attno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_slot_seq_resno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_slot_rowno_resno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_query_slot_seq_resno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_column_count)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_query_column_count)) > 0 &&
               bioseq::indexing::index_read_requirement_ordinal_valid(
                   intVal(list_nth(scan->custom_private, k_batch_join_index_read_requirement))) &&
               (sites == nullptr || IsA(sites, List)) && mmsearch != nullptr &&
               IsA(mmsearch, FuncExpr) && honest_startup_cost != nullptr &&
               IsA(honest_startup_cost, Const) && honest_total_cost != nullptr &&
               IsA(honest_total_cost, Const) && incumbent_cost_floor != nullptr &&
               IsA(incumbent_cost_floor, Const);
    }

    /** @brief Copy result semantics while removing planner-only tokens and costs. */
    List* bioseq_batch_join_cache_plan_payload(const CustomScan* scan) {
        if (!bioseq_batch_join_customscan_has_valid_semantic_payload(scan)) {
            return NIL;
        }
        /* The token pairs one BatchJoin with its locator child only inside one
         * planned statement.  It is deliberately monotonic and therefore cannot
         * participate in a cross-planning cache identity.  Child plan semantics
         * are covered independently by the exact-plan provider walk. */
        auto* private_payload = static_cast<List*>(copyObject(scan->custom_private));
        lfirst(list_nth_cell(private_payload, k_batch_join_token)) = makeInteger(0);
        for (const int cost_index : {k_batch_join_honest_startup_cost,
                                     k_batch_join_honest_total_cost,
                                     k_batch_join_incumbent_cost_floor}) {
            /* Planner costs select a path but do not affect its result. */
            lfirst(list_nth_cell(private_payload, cost_index)) = makeInteger(0);
        }
        return list_make5(makeInteger(static_cast<int>(scan->flags)),
                          private_payload,
                          copyObject(scan->scan.plan.targetlist),
                          copyObject(scan->scan.plan.qual),
                          copyObject(scan->custom_scan_tlist));
    }

    /** @brief Reconstruct the frozen execution parameters for a valid BatchJoin. */
    List* bioseq_batch_join_cache_semantic_snapshot(const CustomScan* scan) {
        if (!bioseq_batch_join_customscan_has_valid_semantic_payload(scan)) {
            return NIL;
        }
        auto* mmsearch =
            reinterpret_cast<FuncExpr*>(list_nth(scan->custom_private, k_batch_join_mmsearch_expr));
        bioseq::simd::minimap2_parameters params = build_params_from_gucs();
        bioseq::indexing::index_read_requirement requirement =
            bioseq::indexing::index_read_requirement::allow;
        (void)apply_aligner_options_override(mmsearch, params, requirement, get_cached_stub_oids());
        finalize_simd_chaining_policy(params);
        if (static_cast<int>(requirement) !=
            intVal(list_nth(scan->custom_private, k_batch_join_index_read_requirement))) {
            return NIL;
        }

        /* Match BeginCustomScan exactly: expensive result columns are enabled by
         * the finalized placeholder ABI, not by the post-setrefs target list. */
        params.do_cigar = false;
        params.compute_divergence = false;
        params.compute_mapq = false;
        params.compute_edit_distance = false;
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        const Node* sites_node = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_placeholder_sites));
        const List* sites = sites_node == nullptr ? NIL : reinterpret_cast<const List*>(sites_node);
        Bitmapset* seen_resnos = nullptr;
        for (const List* site : postgres_list<List>(const_cast<List*>(sites))) {
            if (site == nullptr || !IsA(site, List) ||
                list_length(site) != k_bioseq_match_helper_site_fields) {
                bms_free(seen_resnos);
                return NIL;
            }
            for (const int index : {k_match_site_resno,
                                    k_match_site_funcid,
                                    k_match_site_typmod,
                                    k_match_site_collation,
                                    k_match_site_binding_kind,
                                    k_match_site_phid,
                                    k_match_site_stable_id}) {
                if (!bioseq_batch_join_private_integer(site, index)) {
                    bms_free(seen_resnos);
                    return NIL;
                }
            }
            const int resno = intVal(list_nth(site, k_match_site_resno));
            const Oid funcid = static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid)));
            const auto binding_kind = static_cast<match_helper_binding_kind>(
                intVal(list_nth(site, k_match_site_binding_kind)));
            const int stable_id = intVal(list_nth(site, k_match_site_stable_id));
            const Node* canonical_argument =
                reinterpret_cast<const Node*>(list_nth(site, k_match_site_canonical_argument));
            if (resno <= 0 || resno > list_length(scan->custom_scan_tlist) ||
                bms_is_member(resno, seen_resnos) || !is_bioseq_projection_funcid(funcid, oids) ||
                canonical_argument == nullptr ||
                (binding_kind != match_helper_binding_kind::exact_recipe &&
                 binding_kind != match_helper_binding_kind::unique_lineage) ||
                (stable_id != 0 && stable_id != scan->scan.plan.plan_node_id)) {
                bms_free(seen_resnos);
                return NIL;
            }
            seen_resnos = bms_add_member(seen_resnos, resno);
            params.do_cigar = params.do_cigar || funcid == oids.cigar_oid ||
                              funcid == oids.has_cigar_oid || funcid == oids.edit_oid;
            params.compute_edit_distance = params.compute_edit_distance || funcid == oids.edit_oid;
            params.compute_divergence = params.compute_divergence || funcid == oids.divergence_oid;
            params.compute_mapq = params.compute_mapq || funcid == oids.mapq_oid;
        }
        bms_free(seen_resnos);
        return make_bioseq_semantic_payload(frozen_scan_semantics{
            .params = params,
            .read_sequence_index = bioseq::settings::index::sequence_read::enabled,
            .index_read_requirement = requirement,
        });
    }

    /** @brief Tracks whether an expression contains a Bioseq projection or mmsearch call. */
    struct projection_presence_probe {
        const bioseq_stub_oids* stub_oids;
        bool found;
    };

    struct projection_presence_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              projection_presence_probe& data) const {
            if (data.found)
                return pg_walk_step::stop;
            if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                if (is_bioseq_projection_funcid(fe->funcid, *data.stub_oids) ||
                    is_mmsearch_funcid(fe->funcid, *data.stub_oids) ||
                    fe->funcid == data.stub_oids->symbol_count_oid) {
                    data.found = true;
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Report whether an expression depends on Bioseq scan-time projection state.
     *
     * True if any node in the expression tree references a bioseq.<...>
     * projection FuncExpr (cigar / score / target_*, or symbol_count) or
     * an mmsearch FuncExpr. The vectorised aggregator rewrite path uses
     * this to skip its rewrite when the agg input depends on any of
     * these — the projection FuncExprs are scan-time-rewritten into
     * placeholder Consts that the regular Postgres Aggregate node can
     * evaluate, but the vectorised aggregator expects vector-typed
     * scan output and crashes with a SIGSEGV when fed a regular slot
     * carrying our placeholder Consts.
     */
    bool bioseq_expr_uses_bioseq_projection(Node* node) {
        if (node == nullptr) {
            return false;
        }
        const bioseq_stub_oids* stub_oids = try_get_cached_stub_oids();
        if (stub_oids == nullptr)
            return false;
        projection_presence_probe probe{stub_oids, false};
        projection_presence_visitor visitor;
        walk_pg_expression_including_root(node, probe, visitor);
        return probe.found;
    }

} // export custom-scan partition declarations
