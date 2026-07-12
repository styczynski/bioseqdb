#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

import bioseq.common.sink_control;
import bioseq.common.checked_arithmetic;
import bioseq.external.provider.base;
import bioseq.external.provider.errors;

using namespace bioseq::external::provider;

TEST_CASE("bounded_string enforces its compile-time cap", "[external][provider][unit]") {
    const auto ok = bounded_string<8>::make("abc");
    REQUIRE(ok.has_value());
    REQUIRE(ok->view() == "abc");
    REQUIRE(ok->deep_size_bytes() >= sizeof(bounded_string<8>));

    const auto too_long = bounded_string<2>::make("abcdef");
    REQUIRE_FALSE(too_long.has_value());
    REQUIRE(too_long.error() == bounded_string_error::too_long);

    const auto clamped = bounded_string<3>::make_truncated("abcdef");
    REQUIRE(clamped.view() == "abc");
}

TEST_CASE("checked/saturating arithmetic is overflow-safe", "[external][provider][unit]") {
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

TEST_CASE("external_error is built through checked factories", "[external][provider][unit]") {
    const auto err = make_external_error(external_error_kind::parse, "bad json");
    REQUIRE(err.kind == external_error_kind::parse);
    REQUIRE(err.message.view() == "bad json");
    REQUIRE_FALSE(err.retryable);
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

TEST_CASE("identity views and enum text mapping", "[external][provider][unit]") {
    REQUIRE(bioseq::sink_control::continue_ != bioseq::sink_control::stop);
    REQUIRE(to_string(molecule_kind::protein) == "protein");
    REQUIRE(to_string(entity_kind::assembly) == "assembly");

    const provider_id a{.value = "ncbi"};
    const provider_id b{.value = "ncbi"};
    const provider_id c{.value = "uniprot"};
    REQUIRE(a == b);
    REQUIRE(a != c);
}
