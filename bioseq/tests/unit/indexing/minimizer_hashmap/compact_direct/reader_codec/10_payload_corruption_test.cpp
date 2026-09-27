/**
 * @file compact_direct/reader_codec/10_payload_corruption_test.cpp
 * @brief Checks that anchor preparation rejects corrupt counts in a mapped index.
 */

#include <hwy/highway.h>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.chaining.core;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.common.hashing; 

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;
import bioseq.tests.unit.indexing.minimizer_hashmap.compact_direct_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

using namespace compact_direct_test_support;

TEST_CASE("fragment anchor preparation rejects trusted payload corruption",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 1u);
    auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    const auto& entry = generation->directory.entries().front();
    const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                       generation->directory.profile().hash_width,
                                                       entry.shape_offset_width,
                                                       generation->directory.profile().count_width);
    REQUIRE(layout.has_value());
    REQUIRE(generation->directory.profile().count_width == minimizer_scalar_width::u16);
    test_write_le<std::uint16_t>(std::span<std::byte>{payload->data(), payload->size()},
                                 static_cast<std::size_t>(layout->count_offset),
                                 3u);

    bioseq::parallelism::memory_admission_controller admission;
    {
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::vector<minimap_sketch_data> query{
            minimap_sketch_data{7u, 20u, 0u},
        };
        const std::array queries{
            fragment_seed_query{std::span<const minimap_sketch_data>{query}, 100u, 15},
        };
        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 1, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
        scratch->release_retained_capacity();
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
