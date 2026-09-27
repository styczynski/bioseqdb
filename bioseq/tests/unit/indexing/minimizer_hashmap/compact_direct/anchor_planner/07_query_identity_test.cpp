/**
 * @file compact_direct/anchor_planner/07_query_identity_test.cpp
 * @brief Checks the checksum of sorted query hashes and search parameters.
 */

#include <cstdint>
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

TEST_CASE("compact-direct query-set checksum preserves canonical-u32 identity",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint32_t kmer_size = 15u;
    constexpr std::uint32_t window_size = 10u;
    constexpr std::uint32_t max_occ = 500u;
    std::vector<std::uint32_t> duplicate_heavy{(std::numeric_limits<std::uint32_t>::max)(),
                                               0u,
                                               1u,
                                               0u,
                                               1u,
                                               1u,
                                               (std::numeric_limits<std::uint32_t>::max)(),
                                               0x7fffffffu,
                                               0x7fffffffu,
                                               0u};
    std::sort(duplicate_heavy.begin(), duplicate_heavy.end());
    duplicate_heavy.erase(std::unique(duplicate_heavy.begin(), duplicate_heavy.end()),
                          duplicate_heavy.end());
    const std::vector<std::uint32_t> expected_unique{
        0u, 1u, 0x7fffffffu, (std::numeric_limits<std::uint32_t>::max)()};
    REQUIRE(duplicate_heavy == expected_unique);

    const auto reference_mix = [](std::uint64_t hash, std::uint64_t value) noexcept {
        for (int byte_index = 0; byte_index < 8; ++byte_index) {
            hash ^= value & UINT64_C(0xff);
            hash *= UINT64_C(1099511628211);
            value >>= 8u;
        }
        return hash;
    };
    std::uint64_t reference_checksum = UINT64_C(1469598103934665603);
    reference_checksum = reference_mix(reference_checksum, kmer_size);
    reference_checksum = reference_mix(reference_checksum, window_size);
    reference_checksum = reference_mix(reference_checksum, max_occ);
    for (const std::uint32_t hash : duplicate_heavy) {
        reference_checksum = reference_mix(reference_checksum, hash);
    }
    REQUIRE(compact_direct_query_set_checksum(duplicate_heavy, kmer_size, window_size, max_occ) ==
            reference_checksum);
    REQUIRE(compact_direct_query_set_checksum(
                std::span<const std::uint32_t>{}, kmer_size, window_size, max_occ) ==
            compact_direct_query_set_checksum_prefix(kmer_size, window_size, max_occ));
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
