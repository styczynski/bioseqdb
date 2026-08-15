/**
 * @file
 * @brief Encodes and decodes the ordered frozen scan semantic payload.
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

export module bioseq.postgres.table:custom_scan_semantic_payload_payload_codec;

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

} // export custom-scan partition declarations
