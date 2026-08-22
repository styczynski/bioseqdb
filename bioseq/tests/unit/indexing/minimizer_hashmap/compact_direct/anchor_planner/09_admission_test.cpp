/**
 * @file compact_direct/anchor_planner/09_admission_test.cpp
 * @brief Anchor admission failure and owner reuse.
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
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;
import bioseq.tests.unit.indexing.minimizer_hashmap.compact_direct_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

using namespace compact_direct_test_support;

TEST_CASE("fragment anchor admission failure leaves reusable owners exact",
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
    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{std::span<const minimap_sketch_data>{query}, query_length, kmer_size},
    };

    bioseq::parallelism::memory_admission_controller admission;
    {
        auto reader = open_grouped_soa_fragment_query_reader(admission, *generation, sink.rows, 2u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        {
            auto warm = reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch);
            REQUIRE(warm.admitted_bytes() > 0u);
            REQUIRE_FALSE(warm.query_view(0u)
                              .ordered_fragment_seed_anchors(query, query_length, 2, kmer_size)
                              .empty());
            scratch->complete_wave_after_join();
        }
        const std::uint64_t retained_without_batch = admission.reserved_bytes();
        REQUIRE(retained_without_batch == reader->admitted_bytes() + scratch->admitted_bytes());
        REQUIRE(scratch->admitted_bytes() > 0u);
        admission.set_index_memory_cap_bytes(retained_without_batch +
                                             sizeof(prepared_query_anchors));

        REQUIRE_THROWS_AS(reader->prepare_fragment_anchor_batch(admission, queries, 2, *scratch),
                          persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
        REQUIRE(admission.reserved_bytes() == retained_without_batch);
        scratch->release_retained_capacity();
        REQUIRE(scratch->admitted_bytes() == 0u);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
