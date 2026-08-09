/**-------------------------------------------------------------------------
 *
 * highway_ksw_target_test_support.cppm
 *
 * Test-indexes-only SQL receipt for exact KSW2 parity across the production
 * Highway target registry. The caller selects one target through the existing
 * set_highway_target_for_test() seam before requesting this receipt.
 *
 *-------------------------------------------------------------------------
 */
module;

#if defined(BIOSEQ_TEST_INDEXES)

#include "common.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#endif

export module bioseq.highway_ksw_target_test_support;

#if defined(BIOSEQ_TEST_INDEXES)

import bioseq.alignment.contracts;
import bioseq.codegen.sql.annotation_model;
import bioseq.common.context;
import bioseq.common.sequan_compat;
import bioseq.highway.dispatch_contract;
import bioseq.pg_utils;
import bioseq.sequences;
import bioseq.storage.lazy;

namespace {

using dna4_u64 = ::sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = ::compact_sequence_range<dna4_u64>;
using alignment_table_t = bioseq::simd::AlignmentKernels<seq::dna4>;

inline constexpr std::array<std::size_t, 9> kBoundaryLengths{
    15u, 16u, 17u,
    31u, 32u, 33u,
    63u, 64u, 65u,
};

[[nodiscard]] const alignment_table_t& selected_alignment_table() {
    const auto dispatch =
        bioseq::simd_dispatch::registered_highway_dispatch_info();
    const auto runnable = static_cast<std::uint64_t>(
        dispatch.runnable_targets);
    if (dispatch.runnable_targets <= 0
        || !std::has_single_bit(runnable)
        || dispatch.planned_target != dispatch.runnable_targets) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INTERNAL_ERROR,
            {
                .message = "KSW2 boundary receipt requires one forced Highway target",
                .detail =
                    "Call bioseq_internal.set_highway_target_for_test(bit) "
                    "with one baseline-runnable target before requesting the receipt.",
            });
    }

    const auto& binding =
        bioseq::simd_dispatch::registered_highway_target_binding(
            dispatch.planned_target);
    if (binding.target != dispatch.planned_target
        || binding.kernels == nullptr
        || !binding.kernels->complete()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INTERNAL_ERROR,
            {
                .message = "Selected Highway target has no complete KSW2 table",
            });
    }

    const auto* identity =
        binding.kernels->resolve_alphabet_identity<seq::dna4>();
    if (identity == nullptr
        || binding.kernels->resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INTERNAL_ERROR,
            {
                .message = "Selected Highway target has no canonical DNA4 identity",
            });
    }
    for (const auto& entry : binding.kernels->alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code
            && entry.typed_table_complete
            && entry.typed_table != nullptr) {
            return *static_cast<const alignment_table_t*>(entry.typed_table);
        }
    }
    bioseq::pg_utils::raise_sql_error(
        ERRCODE_INTERNAL_ERROR,
        {
            .message = "Selected Highway target has no complete DNA4 KSW2 table",
        });
}

[[nodiscard]] std::vector<seq::dna4> make_query(
    const std::size_t length, std::uint64_t seed) {
    std::vector<seq::dna4> sequence(length);
    for (std::size_t index = 0u; index < length; ++index) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        sequence[index].assign_rank(
            static_cast<decltype(sequence[index].to_rank())>(
                (seed >> 33u) & 3u));
    }
    return sequence;
}

[[nodiscard]] std::vector<seq::dna4> make_target(
    const std::vector<seq::dna4>& query) {
    std::vector<seq::dna4> target = query;
    const std::array<std::size_t, 4> mutation_positions{
        query.size() / 5u,
        query.size() / 3u,
        query.size() / 2u,
        (query.size() * 4u) / 5u,
    };
    for (const std::size_t position : mutation_positions) {
        const auto rank = target[position].to_rank();
        target[position].assign_rank(
            static_cast<decltype(rank)>((rank + 1u) & 3u));
    }
    return target;
}

void append_signed(std::string& receipt, const std::int64_t value) {
    receipt.append(std::to_string(value));
    receipt.push_back(',');
}

void append_unsigned(std::string& receipt, const std::uint64_t value) {
    receipt.append(std::to_string(value));
    receipt.push_back(',');
}

void append_result(
    std::string& receipt,
    const char mode,
    const bool reversed,
    const std::size_t length,
    const bioseq::simd::KswAlignResult& result) {
    if (result.score == bioseq::simd::kKswNegInf || result.cigar.empty()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INTERNAL_ERROR,
            {
                .message = "KSW2 boundary receipt observed no alignment work",
            });
    }

    receipt.push_back(mode);
    receipt.push_back(reversed ? 'R' : 'F');
    receipt.push_back(':');
    append_unsigned(receipt, length);
    append_signed(receipt, result.max);
    append_signed(receipt, result.score);
    append_signed(receipt, result.mqe);
    append_signed(receipt, result.mte);
    append_signed(receipt, result.max_q);
    append_signed(receipt, result.max_t);
    append_signed(receipt, result.mqe_t);
    append_signed(receipt, result.mte_q);
    append_unsigned(receipt, result.zdropped ? 1u : 0u);
    append_unsigned(receipt, result.reach_end ? 1u : 0u);
    append_unsigned(receipt, result.edit_distance);
    append_unsigned(receipt, result.edit_distance_valid ? 1u : 0u);
    append_unsigned(receipt, result.cigar.size());
    for (const std::uint32_t operation : result.cigar) {
        append_unsigned(receipt, operation);
    }
    receipt.push_back(';');
}

[[nodiscard]] std::string make_boundary_receipt() {
    const alignment_table_t& table = selected_alignment_table();
    const auto scoring =
        bioseq::simd::ScoringMatrix<dna4_u64>::generate(2, 4, 1);
    const bioseq::simd::KswZ2Params z2_params{
        .gap_open = 4,
        .gap_extend = 2,
        .band_width = -1,
        .z_drop = -1,
        .end_bonus = 0,
        .flags = {},
    };
    const bioseq::simd::KswD2Params d2_params{
        .gap_open_short = 4,
        .gap_extend_short = 2,
        .gap_open_long = 24,
        .gap_extend_long = 1,
        .band_width = -1,
        .z_drop = -1,
        .end_bonus = 0,
        .flags = {},
    };

    context::context memory;
    context::installed_context_scope installed{memory};
    std::string receipt{"ksw-boundary-v1;"};
    receipt.reserve(16u * 1024u);

    for (const std::size_t length : kBoundaryLengths) {
        auto query_data = make_query(
            length, 0x9E3779B97F4A7C15ULL ^ length);
        auto target_data = make_target(query_data);
        range_t query_range{query_data};
        range_t target_range{target_data};
        auto query = bioseq::storage::make_lazy_in_memory(query_range);
        auto target = bioseq::storage::make_lazy_in_memory(target_range);
        const auto query_view =
            bioseq::storage::as_lazy_sequence_view(query);
        const auto target_view =
            bioseq::storage::as_lazy_sequence_view(target);

        for (const bool reversed : std::array{false, true}) {
            append_result(
                receipt, 'Z', reversed, length,
                table.ksw2_z2(
                    reversed, query_view, target_view,
                    0u, length, 0u, length, scoring, z2_params));
        }
        for (const bool reversed : std::array{false, true}) {
            append_result(
                receipt, 'D', reversed, length,
                table.ksw2_d2(
                    reversed, query_view, target_view,
                    0u, length, 0u, length, scoring, d2_params));
        }
    }
    return receipt;
}

}  // namespace

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
std::string highway_ksw_boundary_receipt_for_test() {
    return make_boundary_receipt();
}

}  // namespace bioseq::pg::sql_routines

#endif
