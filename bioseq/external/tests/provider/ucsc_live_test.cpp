/**
 * @file external/tests/provider/ucsc_live_test.cpp
 * @brief Live UCSC provider query and sequence-delivery tests.
 */

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <stop_token>
#include <variant>
#include <vector>

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.model;
import bioseq.external.provider.messages;
import bioseq.external.provider.query;
import bioseq.external.provider.budget;
import bioseq.external.provider.runtime_services;
import bioseq.external.providers.ucsc;
import bioseq.common.context;
import bioseq.parallelism.pool;

using namespace bioseq::external::provider;
using bioseq::external::providers::ucsc_plan;
using bioseq::external::providers::ucsc_provider;

namespace {

// A caller-thread collector satisfying the owning-message push protocol.
struct catalog_collector {
    std::vector<owning_catalog_row> catalogs;
    std::vector<owning_provider_diagnostic> diagnostics;

    std::expected<void, external_error> push(owning_provider_message&& message) {
        if (auto* row = std::get_if<owning_catalog_row>(&message)) {
            catalogs.push_back(std::move(*row));
        } else if (auto* diag =
                       std::get_if<owning_provider_diagnostic>(&message)) {
            diagnostics.push_back(std::move(*diag));
        }
        return {};
    }
};

provider_message_sink_ref make_sink(catalog_collector& collector) {
    return provider_message_sink_ref{
        .object = &collector,
        .push_fn = [](void* self, owning_provider_message&& message) {
            return static_cast<catalog_collector*>(self)->push(
                std::move(message));
        }};
}

}  // namespace

TEST_CASE("UCSC provider session normalizes live genome catalog",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();

    ucsc_plan plan;
    plan.target = query_target::catalog;
    plan.operation_id = "listUcscGenomes";
    plan.limit = 200;

    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = "ucsc"};

    auto session =
        ucsc_provider::open(memory, pool, runtime, std::nullopt, plan, budget);
    REQUIRE(session.has_value());

    catalog_collector collector;
    auto sink = make_sink(collector);
    std::stop_source stop;

    const auto stats = session->run(sink, stop.get_token());
    REQUIRE(stats.has_value());
    REQUIRE(stats->requests == 1);
    REQUIRE(stats->is_complete);
    REQUIRE(stats->wire_bytes > 0);

    // Live UCSC lists hundreds of assemblies; the normalized catalog must be
    // non-empty and contain the human hg38 assembly with its provider database
    // key set (proving the common model preserved the UCSC-specific field).
    REQUIRE_FALSE(collector.catalogs.empty());
    REQUIRE(stats->rows == collector.catalogs.size());

    bool found_hg38 = false;
    for (const auto& row : collector.catalogs) {
        REQUIRE(row.provider.value == "ucsc");
        REQUIRE(row.kind == entity_kind::assembly);
        REQUIRE(row.is_retrievable);
        if (row.key.accession.view() == "hg38") {
            found_hg38 = true;
            REQUIRE(row.assembly_database.has_value());
            REQUIRE(row.assembly_database->view() == "hg38");
        }
    }
    REQUIRE(found_hg38);
}
