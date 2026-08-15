/**
 * @file highway_ksw_target_test_support.cppm
 * @brief Exposes test-only SQL receipts for KSW2 Highway target parity.
 */
module;

#if defined(BIOSEQ_TEST_INDEXES)

#include <bioseq/pg_compat.h>

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
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.sql_boundary;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;

namespace {

using dna4_u64 = ::sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t = ::compact_sequence_range<dna4_u64>;
using alignment_table_t = bioseq::simd::alignment_kernels<seq::dna4>;

inline constexpr std::array<std::size_t, 9> k_boundary_lengths{
    15u, 16u, 17u,
    31u, 32u, 33u,
    63u, 64u, 65u,
};

[[nodiscard]] auto selected_alignment_table() -> const alignment_table_t& {
    const auto dispatch =
        bioseq::simd_dispatch::registered_highway_dispatch_plan();
    const auto runnable = static_cast<std::uint64_t>(
        dispatch.runnable_targets);
    if (dispatch.runnable_targets <= 0
        || !std::has_single_bit(runnable)
        || dispatch.planned_target != dispatch.runnable_targets) {
        bioseq::postgres::support::raise_sql_error(
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
        || !binding.kernels->is_complete()) {
        bioseq::postgres::support::raise_sql_error(
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
        bioseq::postgres::support::raise_sql_error(
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
    bioseq::postgres::support::raise_sql_error(
        ERRCODE_INTERNAL_ERROR,
        {
            .message = "Selected Highway target has no complete DNA4 KSW2 table",
        });
}

[[nodiscard]] auto make_query(const std::size_t length, std::uint64_t seed)
    -> std::vector<seq::dna4> {
    std::vector<seq::dna4> sequence(length);
    for (std::size_t index = 0u; index < length; ++index) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        sequence[index].assign_rank(
            static_cast<decltype(sequence[index].to_rank())>(
                (seed >> 33u) & 3u));
    }
    return sequence;
}

[[nodiscard]] auto make_target(const std::vector<seq::dna4>& query) -> std::vector<seq::dna4> {
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
    const bioseq::simd::ksw_alignment_result& result) {
    if (result.score == bioseq::simd::k_ksw_negative_infinity
        || result.cigar.empty()) {
        bioseq::postgres::support::raise_sql_error(
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
    append_unsigned(receipt, result.is_z_dropped ? 1u : 0u);
    append_unsigned(receipt, result.has_reached_end ? 1u : 0u);
    append_unsigned(receipt, result.edit_distance);
    append_unsigned(receipt, result.is_edit_distance_valid ? 1u : 0u);
    append_unsigned(receipt, result.cigar.size());
    for (const std::uint32_t operation : result.cigar) {
        append_unsigned(receipt, operation);
    }
    receipt.push_back(';');
}

[[nodiscard]] auto make_boundary_receipt() -> std::string {
    const alignment_table_t& table = selected_alignment_table();
    const auto scoring =
        bioseq::simd::scoring_matrix<dna4_u64>::generate(2, 4, 1);
    const bioseq::simd::ksw_single_affine_parameters z2_params{
        .gap_open = 4,
        .gap_extend = 2,
        .band_width = -1,
        .z_drop = -1,
        .end_bonus = 0,
        .flags = {},
    };
    const bioseq::simd::ksw_dual_affine_parameters d2_params{
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
    std::string receipt{"ksw-boundary;revision=1;"};
    receipt.reserve(16u * 1024u);

    for (const std::size_t length : k_boundary_lengths) {
        auto query_data = make_query(
            length, 0x9E3779B97F4A7C15ULL ^ length);
        auto target_data = make_target(query_data);
        range_t query_range{query_data};
        range_t target_range{target_data};
        const auto query_view =
            bioseq::storage::make_lazy_sequence_view(query_range);
        const auto target_view =
            bioseq::storage::make_lazy_sequence_view(target_range);

        for (const bool reversed : std::array{false, true}) {
            append_result(
                receipt, 'Z', reversed, length,
                table.align_ksw2_single_affine(
                    reversed, query_view, target_view,
                    0u, length, 0u, length, scoring, z2_params));
        }
        for (const bool reversed : std::array{false, true}) {
            append_result(
                receipt, 'D', reversed, length,
                table.align_ksw2_dual_affine(
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
