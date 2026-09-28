/**
 * @file compact_direct/reader_codec/06_page_advice_test.cpp
 * @brief Checks page ranges requested for index reads and merges awjacent ranges.
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

TEST_CASE("compact-direct selected-page advice is exact coalesced and fail open",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;

    SECTION("overlap and touch merge while real gaps flush") {
        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        compact_direct_selected_page_advice_accumulator accumulator;
        REQUIRE(accumulator.accumulate(adviser, {.begin = 100u, .end = 200u}));
        REQUIRE(accumulator.accumulate(adviser, {.begin = 150u, .end = 240u}));
        REQUIRE(accumulator.accumulate(adviser, {.begin = 240u, .end = 300u}));
        REQUIRE(recorder.record_count.load() == 0u);

        REQUIRE(accumulator.accumulate(adviser, {.begin = 400u, .end = 450u}));
        REQUIRE(recorder.record_count.load() == 1u);
        REQUIRE(recorder.records[0u] == compact_direct_selected_page_range{
                                            .begin = 100u,
                                            .end = 300u,
                                        });
        accumulator.flush(adviser);
        REQUIRE(recorder.record_count.load() == 2u);
        REQUIRE(recorder.records[1u] == compact_direct_selected_page_range{
                                            .begin = 400u,
                                            .end = 450u,
                                        });
        REQUIRE_FALSE(accumulator.pending);
        REQUIRE_FALSE(accumulator.accumulate(adviser, {.begin = 500u, .end = 500u}));
    }

    SECTION("a callback which drops advice cannot affect control flow") {
        selected_page_advice_recorder recorder;
        recorder.drop_advice.store(true);
        const auto adviser = recorder.token();
        compact_direct_selected_page_advice_accumulator accumulator;
        REQUIRE(accumulator.accumulate(adviser, {.begin = 100u, .end = 200u}));
        accumulator.flush(adviser);
        REQUIRE_FALSE(accumulator.pending);
        REQUIRE(recorder.record_count.load() == 0u);
    }

    SECTION("alignment and arithmetic overflow remain release active") {
        const auto aligned = compact_direct_align_selected_page_range(0x1003u, 2u, 4096u);
        REQUIRE(aligned.has_value());
        REQUIRE(*aligned == compact_direct_selected_page_range{
                                .begin = 0x1000u,
                                .end = 0x2000u,
                            });
        const auto overflow = compact_direct_align_selected_page_range(
            (std::numeric_limits<std::uintptr_t>::max)() - 1u, 2u, 4096u);
        REQUIRE_FALSE(overflow.has_value());
        REQUIRE(overflow.error() == direct_inverted_index_error::arithmetic_overflow);
        const auto empty = compact_direct_align_selected_page_range(0x1000u, 0u, 4096u);
        REQUIRE_FALSE(empty.has_value());
        REQUIRE(empty.error() == direct_inverted_index_error::query_plan);
    }

    SECTION("P32 inline descriptors are excluded and posting bounds reject") {
        fragment_compact_direct_source source{
            .runs =
                {
                    fragment_compact_direct_run{
                        .header =
                            compact_hash32_sorted_run_header{
                                .hash = 7u,
                                .global_count = 2u,
                                .target_strand_zero_count = 1u,
                            },
                        .encoded_positions = {20u, 23u},
                    },
                    fragment_compact_direct_run{
                        .header =
                            compact_hash32_sorted_run_header{
                                .hash = 8u,
                                .global_count = 1u,
                                .target_strand_zero_count = 1u,
                            },
                        .encoded_positions = {40u},
                    },
                },
        };
        const direct_index_identity identity{
            .generation_id = 1u,
            .source_identity = 2u,
            .storage_contract_checksum = 3u,
            .target_length = 400u,
        };
        const compact_hash32_geometry geometry{
            .hash_domain_bits = 9u,
            .max_hash = 511u,
        };
        const compact_hash32_build_options options{
            .identity = identity,
            .max_occ_cap = 4u,
            .geometry = geometry,
        };
        const compact_hash32_sorted_run_source source_ref{
            .context = &source,
            .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
        };
        auto image = build_compact_hash32_direct_index(source_ref, options);
        REQUIRE(image.has_value());
        const auto direct =
            validate_compact_hash32_direct_index(*image, identity, options.max_occ_cap, geometry);
        REQUIRE(direct.has_value());
        REQUIRE(direct->header.posting_width_bytes == 4u);
        REQUIRE(direct->header.postings_bytes == 8u);

        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        const auto posting_span =
            direct->image.subspan(static_cast<std::size_t>(direct->header.postings_offset), 8u);
        const auto selected = compact_direct_select_page_range(adviser, *direct, posting_span);
        REQUIRE(selected.has_value());
        REQUIRE(selected->has_value());
        REQUIRE(**selected == compact_direct_selected_page_range{
                                  .begin = reinterpret_cast<std::uintptr_t>(posting_span.data()),
                                  .end = reinterpret_cast<std::uintptr_t>(posting_span.data()) +
                                         posting_span.size(),
                              });

        const auto inline_descriptor =
            direct->image.subspan(static_cast<std::size_t>(direct->header.descriptors_offset +
                                                           direct->header.descriptor_width_bytes),
                                  4u);
        const auto excluded = compact_direct_select_page_range(adviser, *direct, inline_descriptor);
        REQUIRE(excluded.has_value());
        REQUIRE_FALSE(excluded->has_value());

        const auto outside_sections =
            compact_direct_select_page_range(adviser, *direct, direct->image.first(1u));
        REQUIRE_FALSE(outside_sections.has_value());
        REQUIRE(outside_sections.error() == direct_inverted_index_error::query_plan);
        auto malformed = *direct;
        malformed.header.postings_bytes = malformed.image.size();
        const auto rejected_bounds =
            compact_direct_select_page_range(adviser, malformed, posting_span);
        REQUIRE_FALSE(rejected_bounds.has_value());
        REQUIRE(rejected_bounds.error() == direct_inverted_index_error::section_layout);
    }

    SECTION("P64 singleton postings remain selected") {
        fragment_compact_direct_source source{
            .runs =
                {
                    fragment_compact_direct_run{
                        .header =
                            compact_hash32_sorted_run_header{
                                .hash = 7u,
                                .global_count = 1u,
                                .target_strand_zero_count = 1u,
                            },
                        .encoded_positions = {20u},
                    },
                },
        };
        const direct_index_identity identity{
            .generation_id = 4u,
            .source_identity = 5u,
            .storage_contract_checksum = 6u,
            .target_length = (std::uint64_t{1u} << 31u) + 1u,
        };
        const compact_hash32_geometry geometry{
            .hash_domain_bits = 9u,
            .max_hash = 511u,
        };
        const compact_hash32_build_options options{
            .identity = identity,
            .max_occ_cap = 4u,
            .geometry = geometry,
        };
        const compact_hash32_sorted_run_source source_ref{
            .context = &source,
            .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
        };
        auto image = build_compact_hash32_direct_index(source_ref, options);
        REQUIRE(image.has_value());
        const auto direct =
            validate_compact_hash32_direct_index(*image, identity, options.max_occ_cap, geometry);
        REQUIRE(direct.has_value());
        REQUIRE(direct->header.posting_width_bytes == 8u);
        REQUIRE(direct->header.postings_bytes == 8u);

        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        const auto posting_span =
            direct->image.subspan(static_cast<std::size_t>(direct->header.postings_offset), 8u);
        const auto selected = compact_direct_select_page_range(adviser, *direct, posting_span);
        REQUIRE(selected.has_value());
        REQUIRE(selected->has_value());
        REQUIRE((**selected).end - (**selected).begin == 8u);
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
