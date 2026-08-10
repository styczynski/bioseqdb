#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

import bioseq.common.sink_control;
import bioseq.common.checked_arithmetic;
import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.query;
import bioseq.external.provider.capabilities;
import bioseq.external.provider.budget;

using namespace bioseq::external::provider;

TEST_CASE("bounded_string enforces its compile-time cap",
          "[component:external_api][layer:unit][feature:external_source]") {
    const auto ok = bounded_string<8>::make("abc");
    REQUIRE(ok.has_value());
    REQUIRE(ok->view() == "abc");
    REQUIRE(ok->measure_deep_size_bytes() >= sizeof(bounded_string<8>));

    const auto too_long = bounded_string<2>::make("abcdef");
    REQUIRE_FALSE(too_long.has_value());
    REQUIRE(too_long.error() == bounded_string_error::too_long);

    const auto clamped = bounded_string<3>::make_truncated("abcdef");
    REQUIRE(clamped.view() == "abc");
}

TEST_CASE("checked/saturating arithmetic is overflow-safe",
          "[component:external_api][layer:unit][feature:external_source]") {
    using namespace bioseq::common;
    REQUIRE(checked_add(1U, 2U).value() == 3U);
    REQUIRE_FALSE(checked_add(UINT64_MAX, 1U).has_value());
    REQUIRE(checked_mul(4U, 5U).value() == 20U);
    REQUIRE_FALSE(checked_mul(UINT64_MAX, 2U).has_value());
    REQUIRE(saturating_add(UINT64_MAX, 5U) == UINT64_MAX);
    REQUIRE(saturating_mul(UINT64_MAX, 2U) == UINT64_MAX);

    const expansion_ratio_limit ratio{.numerator = 3, .denominator = 1,
                                       .grace_bytes = 10};
    REQUIRE(scaled_limit(100U, ratio).value() == 310U);
    const expansion_ratio_limit zero_den{.numerator = 3, .denominator = 0,
                                         .grace_bytes = 0};
    REQUIRE_FALSE(scaled_limit(100U, zero_den).has_value());
}

TEST_CASE("external_error is built through checked factories",
          "[component:external_api][layer:unit][feature:external_source]") {
    const auto err = make_external_error(external_error_kind::parse, "bad json");
    REQUIRE(err.kind == external_error_kind::parse);
    REQUIRE(err.message.view() == "bad json");
    REQUIRE_FALSE(err.is_retryable);
    REQUIRE(to_string(external_error_kind::parse) == "parse");

    const provider_id ncbi{.value = "ncbi"};
    const auto with_provider = make_external_error(
        external_error_kind::http, ncbi, "genome_dataset_report", "404");
    REQUIRE(with_provider.known_provider.has_value());
    REQUIRE(with_provider.known_provider->value == "ncbi");
    REQUIRE(with_provider.provider_name.has_value());
    REQUIRE(with_provider.provider_name->view() == "ncbi");
    REQUIRE(with_provider.operation.view() == "genome_dataset_report");
}

TEST_CASE("identity views and enum text mapping",
          "[component:external_api][layer:unit][feature:external_source]") {
    REQUIRE(bioseq::sink_control::continue_ != bioseq::sink_control::stop);
    REQUIRE(to_string(molecule_kind::protein) == "protein");
    REQUIRE(to_string(entity_kind::assembly) == "assembly");

    const provider_id a{.value = "ncbi"};
    const provider_id b{.value = "ncbi"};
    const provider_id c{.value = "uniprot"};
    REQUIRE(a == b);
    REQUIRE(a != c);
}

namespace {
// Helper: a sequence/metadata budget request over `providers` with `transfers`
// in-flight transfers and the given delivery mode. The span aliases the caller's
// array, which must outlive every make() call below.
external_budget_request make_budget_request(std::span<const provider_id> providers,
                                            std::size_t transfers,
                                            sequence_delivery_mode delivery) {
    external_budget_request r;
    r.delivery = delivery;
    r.selected_providers = providers;
    r.maximum_wire_bytes = 64U * 1024U * 1024U;
    r.maximum_decoded_bytes = 64U * 1024U * 1024U;
    r.maximum_record_bytes = 8U * 1024U * 1024U;
    r.requested_queue_bytes = 4U * 1024U * 1024U;
    r.requested_queue_items = 128U;
    r.requested_in_flight_transfers = transfers;
    return r;
}
}  // namespace

TEST_CASE("external_execution_budget::make sums the full simultaneous peak",
          "[component:external_api][layer:unit][feature:external_source]") {
    const std::array<provider_id, 1> one{provider_id{.value = "ncbi"}};
    const std::array<provider_id, 2> two{provider_id{.value = "ncbi"},
                                         provider_id{.value = "ucsc"}};
    constexpr std::uint64_t k_huge_allocation = std::uint64_t{1} << 40;  // 1 TiB: always fits

    // Baseline: one provider, one transfer, chunked-record sequence delivery.
    const auto seq = external_execution_budget::make(
        make_budget_request(one, 1, sequence_delivery_mode::chunked_records),
        k_huge_allocation);
    REQUIRE(seq.has_value());
    // Sequence delivery reserves each simultaneous working-memory category.
    REQUIRE(seq->transfer_carry_bytes > 0);
    REQUIRE(seq->parser_scratch_bytes > 0);
    REQUIRE(seq->decoder_bytes > 0);
    REQUIRE(seq->archive_metadata_bytes > 0);
    REQUIRE(seq->reserved_capacity_bytes > 0);

    // Metadata-only delivery zeroes the sequence-path terms.
    const auto meta = external_execution_budget::make(
        make_budget_request(one, 1, sequence_delivery_mode::not_applicable),
        k_huge_allocation);
    REQUIRE(meta.has_value());
    REQUIRE(meta->transfer_carry_bytes == 0);
    REQUIRE(meta->decoder_bytes == 0);
    REQUIRE(meta->archive_metadata_bytes == 0);
    REQUIRE(meta->materialized_record_bytes == 0);

    // Adding a provider raises the peak by exactly one per-provider block
    // (private context + metadata page + staging) — proves the ×producer_count
    // multiplier and that nothing else scaled with provider count.
    const auto two_prov = external_execution_budget::make(
        make_budget_request(two, 1, sequence_delivery_mode::chunked_records),
        k_huge_allocation);
    REQUIRE(two_prov.has_value());
    const std::uint64_t per_provider_block =
        static_cast<std::uint64_t>(seq->private_context_bytes) +
        seq->metadata_page_bytes + seq->producer_staging_bytes;
    REQUIRE(two_prov->reserved_capacity_bytes - seq->reserved_capacity_bytes ==
            per_provider_block);

    // Adding an in-flight transfer raises the peak by exactly one carry buffer
    // — proves the ×transfer_count multiplier on curl carry only.
    const auto two_xfer = external_execution_budget::make(
        make_budget_request(one, 2, sequence_delivery_mode::chunked_records),
        k_huge_allocation);
    REQUIRE(two_xfer.has_value());
    REQUIRE(two_xfer->reserved_capacity_bytes - seq->reserved_capacity_bytes ==
            static_cast<std::uint64_t>(seq->transfer_carry_bytes));

    // Materialized-record delivery reserves the final datum once, on top of the
    // otherwise-identical sequence peak.
    const auto mat = external_execution_budget::make(
        make_budget_request(one, 1, sequence_delivery_mode::materialized_record),
        k_huge_allocation);
    REQUIRE(mat.has_value());
    REQUIRE(mat->materialized_record_bytes == 8U * 1024U * 1024U);
    REQUIRE(mat->reserved_capacity_bytes - seq->reserved_capacity_bytes ==
            8U * 1024U * 1024U);

    // A permit smaller than the peak is rejected as response_too_large.
    const auto tiny = external_execution_budget::make(
        make_budget_request(one, 1, sequence_delivery_mode::chunked_records),
        1U * 1024U);
    REQUIRE_FALSE(tiny.has_value());
    REQUIRE(tiny.error().kind == external_error_kind::response_too_large);
}

TEST_CASE("owning_external_budget_allocation::make keeps reserves disjoint",
          "[component:external_api][layer:unit][feature:external_source]") {
    const std::array<provider_id, 2> providers{provider_id{.value = "ncbi"},
                                               provider_id{.value = "ucsc"}};
    const auto request =
        make_budget_request(providers, 2, sequence_delivery_mode::chunked_records);

    // Enough for both per-slot fixed reserves plus a real shared gate pool.
    const std::uint64_t admitted = 128U * 1024U * 1024U;
    const auto owned = owning_external_budget_allocation::make(
        request, std::span<const provider_plan_estimate>{}, admitted);
    REQUIRE(owned.has_value());
    const auto view = owned->make_view();
    REQUIRE(view.slots.size() == 2);
    std::uint64_t sum_fixed = 0;
    for (const external_slot_budget& slot : view.slots) {
        sum_fixed = bioseq::common::saturating_add(sum_fixed,
                                                   slot.fixed_reserved_bytes);
        // Each slot floor includes page, staging, carry, and context storage.
        REQUIRE(slot.fixed_reserved_bytes >
                static_cast<std::uint64_t>(view.aggregate.private_context_bytes));
    }
    // Disjoint-reserve invariant: the fixed reserves alone fit under the permit,
    // leaving headroom for the shared gate.
    REQUIRE(sum_fixed <= admitted);

    // When the permit cannot even cover the per-slot fixed reserves, reject.
    const auto starved = owning_external_budget_allocation::make(
        request, std::span<const provider_plan_estimate>{}, 1U * 1024U * 1024U);
    REQUIRE_FALSE(starved.has_value());
    REQUIRE(starved.error().kind == external_error_kind::response_too_large);
}
