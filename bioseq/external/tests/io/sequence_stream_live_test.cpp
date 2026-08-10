// Exercise NCBI sequence streaming and deterministic bounded-channel
// accounting. Provider-backed cases select the network resource explicitly.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.model;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.provider.bounded_channel;
import bioseq.external.io.sequence_stream;

namespace prov = bioseq::external::provider;
namespace exio = bioseq::external::io;

namespace {

// Collect owning_sequence_payload rows through the provider sink protocol.
struct payload_collector {
    std::vector<prov::owning_sequence_payload> rows;

    static std::expected<void, prov::external_error> push(
        void* self, prov::owning_provider_message&& message) {
        auto* collector = static_cast<payload_collector*>(self);
        if (auto* payload =
                std::get_if<prov::owning_sequence_payload>(&message)) {
            collector->rows.push_back(std::move(*payload));
        }
        return {};
    }

    prov::provider_message_sink_ref ref() {
        return prov::provider_message_sink_ref{this, &payload_collector::push};
    }
};

}  // namespace

TEST_CASE("run_ncbi_sequence_stream chunks NC_012920.1 as one final chunk",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    payload_collector collector;
    auto stats = exio::run_ncbi_sequence_stream(
        "NC_012920.1", prov::query_target::sequence_chunk,
        exio::k_default_chunk_bases, collector.ref(), std::stop_token{});
    INFO("error: " << (stats.has_value() ? std::string{}
                                         : std::string(stats.error().message.view())));
    REQUIRE(stats.has_value());
    REQUIRE(collector.rows.size() == 1);
    REQUIRE(collector.rows[0].chunk_index == 0);
    REQUIRE(collector.rows[0].base_offset == 0);
    REQUIRE(collector.rows[0].bases.size() == 16569);
    REQUIRE(collector.rows[0].end_of_record == true);
}

TEST_CASE("run_ncbi_sequence_stream streams a 4.6 Mbp genome as ordered chunks",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    payload_collector collector;
    const std::uint64_t chunk_bases = exio::k_default_chunk_bases;  // 1048572
    auto stats = exio::run_ncbi_sequence_stream(
        "NC_000913.3", prov::query_target::sequence_chunk, chunk_bases,
        collector.ref(), std::stop_token{});
    INFO("error: " << (stats.has_value() ? std::string{}
                                         : std::string(stats.error().message.view())));
    REQUIRE(stats.has_value());
    // 4,641,652 bp / 1,048,572 => 4 full chunks + a 447,364 bp tail = 5 rows.
    REQUIRE(collector.rows.size() == 5);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < collector.rows.size(); ++i) {
        REQUIRE(collector.rows[i].chunk_index == i);
        REQUIRE(collector.rows[i].base_offset == total);
        total += collector.rows[i].bases.size();
        const bool is_last = (i + 1 == collector.rows.size());
        REQUIRE(collector.rows[i].end_of_record == is_last);
        if (!is_last) {
            REQUIRE(collector.rows[i].bases.size() == chunk_bases);
        }
    }
    REQUIRE(total == 4641652);
    REQUIRE(collector.rows.back().bases.size() == 447364);
}

TEST_CASE("run_ncbi_sequence_stream whole-record sequence target",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    payload_collector collector;
    auto stats = exio::run_ncbi_sequence_stream(
        "NC_012920.1", prov::query_target::sequence, exio::k_default_chunk_bases,
        collector.ref(), std::stop_token{});
    REQUIRE(stats.has_value());
    REQUIRE(collector.rows.size() == 1);
    REQUIRE(collector.rows[0].bases.size() == 16569);
    REQUIRE(collector.rows[0].end_of_record == true);
}

TEST_CASE("bounded_channel charges bytes/items and enforces backpressure",
          "[component:external_api][layer:unit][feature:external_source]") {
    SECTION("single-threaded charge accounting") {
        prov::bounded_channel<int> channel(/*bytes=*/1000, /*items=*/8);
        REQUIRE(channel.push(1, 100, std::stop_token{}).has_value());
        REQUIRE(channel.push(2, 100, std::stop_token{}).has_value());
        REQUIRE(channel.charged_bytes() == 200);
        auto first = channel.pop_for(std::chrono::milliseconds(10));
        REQUIRE(first.status == prov::pop_status::item);
        REQUIRE(first.item->value() == 1);
        // Charge stays until the lease is released.
        REQUIRE(channel.charged_bytes() == 200);
        first.item->release();
        REQUIRE(channel.charged_bytes() == 100);
        channel.close_done();
        auto second = channel.pop_for(std::chrono::milliseconds(10));
        REQUIRE(second.status == prov::pop_status::item);
        second.item.reset();
        auto end = channel.pop_for(std::chrono::milliseconds(10));
        REQUIRE(end.status == prov::pop_status::closed_done);
    }

    SECTION("producer blocks when the byte bound is reached") {
        prov::bounded_channel<int> channel(/*bytes=*/150, /*items=*/8);
        REQUIRE(channel.push(1, 100, std::stop_token{}).has_value());
        std::atomic<bool> second_pushed{false};
        std::thread producer([&] {
            // 100 more would exceed 150 with one item already charged; blocks.
            (void)channel.push(2, 100, std::stop_token{});
            second_pushed.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE_FALSE(second_pushed.load());  // backpressure held
        auto lease = channel.pop_for(std::chrono::milliseconds(10));
        REQUIRE(lease.status == prov::pop_status::item);
        lease.item->release();  // frees capacity -> producer proceeds
        producer.join();
        REQUIRE(second_pushed.load());
        REQUIRE(channel.high_water_bytes() <= 200);
    }
}
