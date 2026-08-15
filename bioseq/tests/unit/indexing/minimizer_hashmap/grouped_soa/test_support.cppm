/**
 * @file test_support.cppm
 * @brief Provides grouped-SoA row fixtures for unit tests.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <hwy/highway.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <tests/support/common.hpp>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.tests.unit.indexing.minimizer_hashmap.grouped_soa_support;

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

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

export namespace grouped_soa_test_support {

/// Validate a captured row and return its move-only receipt.
[[nodiscard]] bioseq::indexing::validated_minimizer_generation_row_receipt
trusted_row_receipt(const collected_minimizer_row& row) {
    auto validated = bioseq::indexing::validate_minimizer_generation_row_receipt(row_view(row));
    REQUIRE(validated.has_value());
    return std::move(*validated);
}

/// Recompute the address-bound checksum after mutating a captured row.
void recompute_row_checksum(collected_minimizer_row& row) noexcept {
    row.row_checksum =
        bioseq::indexing::crc64_index_chunk_row(row.address.generation_id,
                                                row.address.position_segment,
                                                row.address.sub_key,
                                                row.address.chunk_no,
                                                static_cast<std::uint32_t>(row.bytes.size()),
                                                row.bytes);
}

} // namespace grouped_soa_test_support
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
