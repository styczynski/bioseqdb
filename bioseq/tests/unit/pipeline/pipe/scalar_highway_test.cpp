/**
 * @file scalar_highway_test.cpp
 * @brief Exercises scalar and Highway pipeline routes.
 */
#include <catch2/matchers/catch_matchers.hpp>

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

namespace bioseq::simd::pipe {

using namespace ::bioseq::tests::pipeline::pipe_support;

TEST_CASE("Prefilter stats aggregation preserves runtime identity and fails closed on overflow",
          "[component:bioseq][layer:unit][feature:prefilter]") {
    using bioseq::pipeline_stats::pipeline_execution_stats;
    using bioseq::pipeline_stats::pipeline_operational_stats;
    using bioseq::prefilter::prefilter_backend;
    using bioseq::prefilter::prefilter_execution_stats;
    using bioseq::prefilter::resolved_prefilter_algorithm;

    pipeline_execution_stats aggregate{};
    for (std::uint64_t index = 0u; index < 3u; ++index) {
        pipeline_operational_stats batch{};
        prefilter_execution_stats stats{};
        const bool rejected = index == 2u;
        stats.record_batch(
            resolved_prefilter_algorithm::gatekeeper,
            prefilter_backend::vulkan,
            1u, 1u, 0u, 0u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            rejected ? 0u : 1u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            0u,
            1u);
        batch.record_prefilter_execution_stats(stats);
        aggregate.merge_operational(batch);
    }

    REQUIRE(aggregate.operational.prefilter_execution_stats_available);
    const auto& stats = aggregate.operational.prefilter_stats;
    CHECK(stats.dispatches == 3u);
    CHECK(stats.gatekeeper_dispatches == 3u);
    CHECK(stats.vulkan_dispatches == 3u);
    CHECK(stats.jobs == 3u);
    CHECK(stats.evaluated_jobs == 3u);
    CHECK(stats.length_64_jobs == 3u);
    CHECK(stats.rejected_jobs == 1u);
    CHECK(stats.rejected_regions == 1u);
    CHECK(stats.surviving_regions == 2u);
    CHECK(stats.mask_rejection_candidates == 1u);
    CHECK(stats.exact_guard_activations == 1u);
    CHECK(stats.guard_confirmed_rejections == 1u);
    CHECK(stats.hamming_exact_rejections == 0u);
    CHECK(stats.is_internally_consistent());
    CHECK(std::string_view{stats.resolved_algorithm_name()} == "gatekeeper");
    CHECK(std::string_view{stats.resolved_backend_name()} == "vulkan");
    CHECK_FALSE(aggregate.operational.are_stats_saturated);

    pipeline_operational_stats valid_then_malformed{};
    prefilter_execution_stats valid_single_batch{};
    valid_single_batch.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    valid_then_malformed.record_prefilter_execution_stats(
        valid_single_batch);
    REQUIRE(valid_then_malformed.prefilter_stats
                .is_internally_consistent());
    valid_then_malformed.record_prefilter_execution_stats(
        prefilter_execution_stats{});
    CHECK(valid_then_malformed.prefilter_stats.is_saturated);
    CHECK_FALSE(valid_then_malformed.prefilter_stats
                    .is_internally_consistent());
    CHECK(valid_then_malformed.are_stats_saturated);

    pipeline_execution_stats at_limit{};
    pipeline_operational_stats maximum_batch{};
    prefilter_execution_stats maximum{};
    maximum.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u,
        (std::numeric_limits<std::uint64_t>::max)());
    maximum_batch.record_prefilter_execution_stats(maximum);
    at_limit.merge_operational(maximum_batch);
    pipeline_execution_stats increment{};
    pipeline_operational_stats increment_batch{};
    prefilter_execution_stats one_more{};
    one_more.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    increment_batch.record_prefilter_execution_stats(one_more);
    increment.merge_operational(increment_batch);
    at_limit.merge(increment);
    CHECK(at_limit.operational.prefilter_stats.is_saturated);
    CHECK_FALSE(at_limit.operational.prefilter_stats
                    .is_internally_consistent());
    CHECK(at_limit.operational.are_stats_saturated);
}

TEST_CASE("Prefilter algorithm resolver admits only the measured Vulkan matrix",
          "[component:bioseq][layer:unit][feature:prefilter]") {
    using bioseq::prefilter::prefilter_backend;
    using bioseq::prefilter::resolved_prefilter_algorithm;
    using bioseq::prefilter::prefilter_job;

    const std::array<prefilter_job, 2u> measured{
        prefilter_job{.query_length = 32u, .target_length = 32u,
                     .max_edits = 2u},
        prefilter_job{.query_length = 64u, .target_length = 64u,
                     .max_edits = 4u},
    };
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::vulkan, measured)
        == resolved_prefilter_algorithm::gatekeeper);
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::simd, measured)
        == resolved_prefilter_algorithm::sneaky_snake);

    auto outside = measured;
    outside[0].query_length = 48u;
    outside[0].target_length = 48u;
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::vulkan, outside)
        == resolved_prefilter_algorithm::sneaky_snake);
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::vulkan,
        std::span<const prefilter_job>{})
        == resolved_prefilter_algorithm::sneaky_snake);
}

TEST_CASE("prefilter_stage composes reverse, unequal, aggregate, and stable routes",
          "[component:bioseq][layer:unit][feature:prefilter]") {
    using bioseq::prefilter::prefilter_backend;
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    using ::bioseq::chaining::anchor;
    using vulkan_pipeline_test_support::dna4_u64;
    using vulkan_pipeline_test_support::range_t;

    const auto dna4_from_ascii = [](std::string_view text) {
        std::vector<seq::dna4> result(text.size());
        for (std::size_t index = 0; index < text.size(); ++index) {
            result[index].assign_char(text[index]);
        }
        return result;
    };
    const auto make_anchor = [](
        std::uint64_t query_position,
        std::uint64_t ref_position,
        bool reverse) {
        return anchor{
            .ref_position = ref_position,
            .query_position = query_position,
            .is_reverse = static_cast<std::uint32_t>(reverse),
            .query_span = 8u,
            .flags = 0u,
            .segment_id = 0u,
        };
    };

    std::string query_text(224u, 'A');
    constexpr std::string_view reverse_source =
        "ACGTTGCAAGTCCTAGGATCCGATTTACGGCATGCAATCG";
    static_assert(reverse_source.size() == 40u);
    query_text.replace(16u, reverse_source.size(), reverse_source);
    std::string target_text = query_text;
    // Region 1 owns two individually accepted, equal-length gaps. Their
    // lower bounds aggregate to two and reject only at the region boundary.
    target_text[72] = 'C';
    target_text[104] = 'C';
    // Region 3 is reverse-strand.  Its query-coordinate window [168, 208)
    // mirrors to the non-palindromic forward source [16, 56), so this checks
    // coordinate mirroring and reversal as well as base complementation.
    const auto complement_ascii = [](char base) {
        switch (base) {
            case 'A': return 'T';
            case 'C': return 'G';
            case 'G': return 'C';
            case 'T': return 'A';
            default: return base;
        }
    };
    for (std::size_t offset = 0; offset < reverse_source.size(); ++offset) {
        target_text[168u + offset] = complement_ascii(
            reverse_source[reverse_source.size() - 1u - offset]);
    }

    range_t query_range(dna4_from_ascii(query_text));
    range_t target_range(dna4_from_ascii(target_text));
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view =
        bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_target);

    const std::vector<anchor> anchors{
        make_anchor(15u, 15u, false),
        make_anchor(31u, 31u, false),
        make_anchor(63u, 63u, false),
        make_anchor(95u, 95u, false),
        make_anchor(127u, 127u, false),
        make_anchor(143u, 143u, false),
        make_anchor(159u, 164u, false),
        make_anchor(175u, 175u, true),
        make_anchor(207u, 207u, true),
    };

    for (const prefilter_backend backend : {
             prefilter_backend::scalar,
             prefilter_backend::simd}) {
        CAPTURE(static_cast<unsigned>(backend));
        minimap2_parameters params;
        bind_fixed_highway(params);
        params.prefilter_enabled = true;
        params.compute_edit_distance = true;
        params.prefilter_backend = backend;
        params.prefilter_max_edits = 1u;
        params.min_ksw_len = 1;

        std::vector<alignment_region> regions(4u);
        regions[0].anchor_start = 0;
        regions[0].seed_count = 2;
        regions[1].anchor_start = 2;
        regions[1].seed_count = 3;
        regions[2].anchor_start = 5;
        regions[2].seed_count = 2;
        regions[3].anchor_start = 7;
        regions[3].seed_count = 2;
        regions[3].is_reverse = true;

        match_result_collection stats;
        std::vector<alignment_region> observed;
        int on_batch_count = 0;
        prefilter_probe_sink probe{&observed, &on_batch_count};
        prefilter_stage<seq::dna4> stage{
            {}, params, query_view, target_view, &stats};
        auto bound = stage.template attach<cep_type>(probe);
        bound.on_batch_pair(
            std::span<alignment_region>{regions},
            std::span<const anchor>{anchors});

        REQUIRE(on_batch_count == 1);
        REQUIRE(observed.size() == 2u);
        CHECK(observed[0].anchor_start == 0);
        CHECK(observed[1].anchor_start == 7);
        REQUIRE(stats.prefilter_execution_stats_available);
        const auto& route = stats.prefilter_stats;
        CHECK(route.dispatches == 1u);
        CHECK(route.sneaky_snake_dispatches == 1u);
        CHECK(route.gatekeeper_dispatches == 0u);
        CHECK(route.scalar_dispatches
              == (backend == prefilter_backend::scalar ? 1u : 0u));
        CHECK(route.simd_dispatches
              == (backend == prefilter_backend::simd ? 1u : 0u));
        CHECK(route.jobs == 5u);
        CHECK(route.evaluated_jobs == 4u);
        CHECK(route.length_difference_jobs == 1u);
        CHECK(route.unsupported_jobs == 0u);
        CHECK(route.rejected_jobs == 1u);
        CHECK(route.rejected_regions == 2u);
        CHECK(route.surviving_regions == 2u);
        CHECK(route.mask_rejection_candidates == 0u);
        CHECK(route.exact_guard_activations == 0u);
        CHECK(route.guard_confirmed_rejections == 0u);
        CHECK(route.hamming_exact_rejections == 0u);
        CHECK_FALSE(route.is_saturated);
    }
}

TEST_CASE("reference SIMD pipe runtime and constexpr dispatch use fixed target table",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using namespace ::bioseq::simd;
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using collector_type = basic_sketch_collector<minimap_sketch_data>;
    using pipe::minimizer_kernel_kind;

    auto data = vulkan_pipeline_test_support::make_dna4(4096u, 0x51AD51ADu);
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto* table = fixed_highway_kernels();
    REQUIRE(table->has_kernel(
        ::bioseq::simd_dispatch::highway_kernel::
            minimizer_minimap2_reference));

    collector_type runtime_collector;
    (void)pipe::minimizer_dispatch<
        minimizer_kernel_kind::simd, minimap_sketch_data>(
            runtime_collector, view, 10u, 15u, table);

    collector_type constexpr_collector;
    (void)pipe::minimizer_dispatch_constexpr<
        minimizer_kernel_kind::simd, 10u, 15u, minimap_sketch_data>(
            constexpr_collector, view, 10u, 15u, table);

    const auto& runtime = runtime_collector.extract_sketches_data();
    const auto& specialized = constexpr_collector.extract_sketches_data();
    REQUIRE(runtime.size() == specialized.size());
    for (std::size_t index = 0u; index < runtime.size(); ++index) {
        CAPTURE(index);
        REQUIRE(runtime[index].raw_value() == specialized[index].raw_value());
    }

    collector_type scalar_collector;
    (void)pipe::minimizer_dispatch<
        minimizer_kernel_kind::scalar, minimap_sketch_data>(
            scalar_collector, view, 10u, 15u, table);
    const auto normalized_runtime =
        normalized_raw_minimizers(runtime_collector);
    const auto normalized_scalar =
        normalized_raw_minimizers(scalar_collector);
    REQUIRE_FALSE(normalized_runtime.empty());
    REQUIRE(normalized_runtime == normalized_scalar);
}

TEST_CASE("neutral minimizer sink projects active K15 layouts without truncation",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using namespace ::bioseq::simd;
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using output_type = minimizer_representation<33u, 15u>;
    using collector_type = basic_sketch_collector<output_type>;
    using pipe::minimizer_kernel_kind;

    auto data = vulkan_pipeline_test_support::make_dna4(32'769u, 0x15AB1C0Du);
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto* table = fixed_highway_kernels();

    const auto require_raw_parity = [](const collector_type& selected,
                                       const collector_type& scalar) {
        const auto& selected_records = selected.extract_sketches_data();
        const auto& scalar_records = scalar.extract_sketches_data();
        REQUIRE_FALSE(selected_records.empty());
        REQUIRE(selected_records.size() == scalar_records.size());
        for (std::size_t index = 0u; index < selected_records.size(); ++index) {
            CAPTURE(index);
            REQUIRE(selected_records[index].raw_value()
                    == scalar_records[index].raw_value());
        }
    };

    SECTION("reference SIMD preserves the normalized K15 record set") {
        collector_type reference_collector;
        (void)pipe::minimizer_dispatch<minimizer_kernel_kind::simd, output_type>(
            reference_collector, view, 10u, 15u, table);
        collector_type reference_scalar_collector;
        (void)pipe::minimizer_dispatch<minimizer_kernel_kind::scalar, output_type>(
            reference_scalar_collector, view, 10u, 15u, table);
        const auto reference =
            normalized_raw_minimizers(reference_collector);
        const auto scalar =
            normalized_raw_minimizers(reference_scalar_collector);
        REQUIRE_FALSE(reference.empty());
        REQUIRE(reference == scalar);
    }

    SECTION("exact SIMD preserves raw K15 record identity") {
        collector_type two_stack_collector;
        const auto two_stack_stats =
            pipe::minimizer_dispatch<minimizer_kernel_kind::exact_simd, output_type>(
                two_stack_collector, view, 10u, 15u, table);
        REQUIRE(two_stack_stats.is_internally_consistent(true));
        collector_type two_stack_scalar_collector;
        (void)pipe::minimizer_dispatch<minimizer_kernel_kind::scalar, output_type>(
            two_stack_scalar_collector, view, 10u, 15u, table);
        require_raw_parity(
            two_stack_collector, two_stack_scalar_collector);
    }
}

TEST_CASE("neutral minimizer batches retain the caller's exact wide representation",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using namespace ::bioseq::simd;
    using output_type = minimizer_representation<63u, 15u>;
    using collector_type = basic_sketch_collector<output_type>;

    STATIC_REQUIRE(sizeof(output_type) == 2u * sizeof(std::uint64_t));
    STATIC_REQUIRE(output_type::k_hash_field_mask
                   > minimap_sketch_data::k_hash_field_mask);
    STATIC_REQUIRE(output_type::k_position_field_mask
                   > minimap_sketch_data::k_position_field_mask);

    constexpr std::array<output_type, 3u> expected{
        output_type{
            (std::uint64_t{1u} << 40u) | 0x12345u,
            (std::uint64_t{1u} << 28u) | 17u,
            1u},
        output_type{
            (std::uint64_t{1u} << 41u) | 0x23456u,
            (std::uint64_t{1u} << 28u) | 29u,
            0u},
        output_type{
            (std::uint64_t{1u} << 42u) | 0x34567u,
            (std::uint64_t{1u} << 28u) | 43u,
            1u},
    };
    STATIC_REQUIRE(expected[0].kmer()
                   > minimap_sketch_data::k_hash_field_mask);
    STATIC_REQUIRE(expected[0].last_pos()
                   > minimap_sketch_data::k_position_field_mask);

    collector_type collector;
    pipe::highway_minimizer_sink_state<output_type, collector_type> adapter{
        collector};
    auto sink = adapter.sink();
    REQUIRE(sink.acquire != nullptr);
    REQUIRE(sink.commit != nullptr);

    auto first_batch = sink.acquire(sink.state, 2u);
    REQUIRE(first_batch.data != nullptr);
    REQUIRE(first_batch.capacity >= 2u);
    REQUIRE(first_batch.layout
            == ::bioseq::minimizers::k_minimizer_output_layout<output_type>);
    REQUIRE(first_batch.record_size == sizeof(output_type));
    REQUIRE(first_batch.record_alignment == alignof(output_type));
    auto* first_records = static_cast<output_type*>(first_batch.data);
    std::construct_at(first_records, expected[0]);
    std::construct_at(first_records + 1u, expected[1]);
    sink.commit(sink.state, 2u);

    auto second_batch = sink.acquire(sink.state, 1u);
    REQUIRE(second_batch.data != nullptr);
    REQUIRE(second_batch.capacity >= 1u);
    auto* second_records = static_cast<output_type*>(second_batch.data);
    std::construct_at(second_records, expected[2]);
    sink.commit(sink.state, 1u);

    ::bioseq::minimizers::minimizer_kernel_result result{};
    result.emitted = expected.size();
    REQUIRE_NOTHROW(adapter.validate(result));

    const auto& records = collector.extract_sketches_data();
    REQUIRE(records.size() == expected.size());
    for (std::size_t index = 0u; index < records.size(); ++index) {
        CAPTURE(index);
        REQUIRE(records[index].kmer() == expected[index].kmer());
        REQUIRE(records[index].last_pos() == expected[index].last_pos());
        REQUIRE(records[index].strand() == expected[index].strand());
        REQUIRE(records[index].span() == 15u);
    }
}

TEST_CASE("production minimizer sink keeps protocol faults distinct from collector admission",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using output_type = ::bioseq::simd::minimap_sketch_data;

    struct controlled_collector final {
        bool accept_batches = true;
        bool healthy = true;
        bool throw_from_batch = false;

        [[nodiscard]] bool append_records(
                std::span<const output_type>) {
            if (throw_from_batch) {
                throw std::runtime_error{"production sink callback failure"};
            }
            if (!accept_batches) {
                healthy = false;
                return false;
            }
            return true;
        }

        void collect(const output_type&) noexcept {}
        void collect(output_type&&) noexcept {}

        [[nodiscard]] bool is_ok() const noexcept { return healthy; }
    };

    constexpr output_type record{0x12345u, 17u, 1u};

    SECTION("sticky collector failure remains available to the outer scheduler") {
        controlled_collector collector;
        collector.accept_batches = false;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        auto sink = adapter.sink();
        auto batch = sink.acquire(sink.state, 1u);
        REQUIRE(batch.data != nullptr);
        std::construct_at(static_cast<output_type*>(batch.data), record);
        sink.commit(sink.state, 1u);

        ::bioseq::minimizers::minimizer_kernel_result result{};
        result.emitted = 1u;
        REQUIRE_FALSE(collector.is_ok());
        REQUIRE_NOTHROW(adapter.validate(result));
    }

    SECTION("healthy count mismatch remains a transport error") {
        controlled_collector collector;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        ::bioseq::minimizers::minimizer_kernel_result result{};
        result.emitted = 1u;
        REQUIRE_THROWS_WITH(
            adapter.validate(result),
            "selected Highway minimizer sink rejected a committed batch");
    }

    SECTION("acquire without commit fails closed even at a zero count") {
        controlled_collector collector;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        auto sink = adapter.sink();
        REQUIRE(sink.acquire(sink.state, 1u).data != nullptr);
        ::bioseq::minimizers::minimizer_kernel_result result{};
        REQUIRE_THROWS_WITH(
            adapter.validate(result),
            "selected Highway minimizer sink left a batch uncommitted");
    }

    SECTION("callback exception is rethrown after the noexcept commit boundary") {
        controlled_collector collector;
        collector.throw_from_batch = true;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        auto sink = adapter.sink();
        auto batch = sink.acquire(sink.state, 1u);
        REQUIRE(batch.data != nullptr);
        std::construct_at(static_cast<output_type*>(batch.data), record);
        sink.commit(sink.state, 1u);

        ::bioseq::minimizers::minimizer_kernel_result result{};
        result.emitted = 1u;
        REQUIRE_THROWS_WITH(
            adapter.validate(result), "production sink callback failure");
    }
}

TEST_CASE("reference SIMD preserves DNA4 topology and scalar-routes DNA5",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    check_reference_minimizer_topology_case<seq::dna4, std::uint32_t>();
    check_reference_minimizer_topology_case<seq::dna4, std::uint64_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint8_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint16_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint32_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint64_t>();
}

TEST_CASE("source-owned two-stack topology closes DNA4/DNA5 by four widths",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint8_t>();
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint16_t>();
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint32_t>();
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint64_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint8_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint16_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint32_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint64_t>();
}

TEST_CASE("exact SIMD pipe admits whole non-u32 ranges into resident u32 vector_stream",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using non_u32_sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using non_u32_range_type = compact_sequence_range<non_u32_sequence_type>;
    using minimizer_type =
        ::bioseq::simd::minimizer_representation<33u, 15u>;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::exact_simd_range_route;
    using ::bioseq::simd::pipe::minimizer_kernel_kind;
    using ::bioseq::simd::pipe::minimizer_dispatch_constexpr;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;
    using ::bioseq::simd::pipe::
        exact_simd_stats_share_selection_route;
    using ::bioseq::simd::pipe::
        minimizer_range_stats_matches_backend;

    auto data = vulkan_pipeline_test_support::make_dna4(4096u, 0x51A1D5EEDu);
    range_type resident(data);
    auto lazy_resident = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto resident_view =
        bioseq::storage::make_lazy_sequence_view(lazy_resident);
    const auto* table = fixed_highway_kernels();

    collector_type whole_collector;
    ::bioseq::simd::two_stack_execution_stats whole_stats;
    try {
        whole_stats = minimizer_dispatch_constexpr<
            minimizer_kernel_kind::exact_simd, 10u, 15u, minimizer_type>(
                whole_collector, resident_view, 10u, 15u, table);
    } catch (const ::bioseq::simd::exact_simd_runtime_error& error) {
        REQUIRE(error.status() == ::bioseq::simd::
            two_stack_continuation_status::unsupported_vector_shape);
        REQUIRE(whole_collector.extract_sketches_data().empty());
        SKIP("generated fixed target lacks the required vector shape");
    }
    REQUIRE(whole_stats.is_internally_consistent(true));
    REQUIRE(whole_stats.resident_vector_stream_kernel_calls == 1u);
    REQUIRE(whole_stats.resident_vector_stream_dual_calls == 1u);
    REQUIRE(whole_stats.resident_vector_stream_single_calls == 0u);
    REQUIRE(whole_stats.generic_u64_kernel_calls == 0u);
    REQUIRE(whole_stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0u);

    // Range dispatch is also used by indexed query preparation. Resident
    // vector_stream is therefore an explicit paired/no-index opt-in, never a
    // decision inferred from the query's storage shape alone.
    collector_type paired_resident_collector;
    const auto paired_resident_stats =
        minimizer_dispatch_range<minimizer_type>(
            paired_resident_collector,
            resident_view,
            0u,
            resident.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_resident_vector_stream,
            table);
    REQUIRE(paired_resident_stats.exact_simd_available);
    REQUIRE(paired_resident_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(paired_resident_stats.exact_simd
        .resident_vector_stream_kernel_calls == 1u);
    REQUIRE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        paired_resident_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_resident_vector_stream));

    // Resident vector_stream is an exact-u32 topology. A different resident
    // width is therefore admitted into an owned u32 region before the same
    // The fixed-target route executes; source width is never relabelled in place.
    non_u32_range_type non_u32_resident(data);
    auto lazy_non_u32 = bioseq::storage::make_resident_lazy_sequence(non_u32_resident);
    const auto non_u32_view =
        bioseq::storage::make_lazy_sequence_view(lazy_non_u32);
    const auto non_u32_access = bioseq::storage::plan_sequence_access(
        non_u32_view,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        non_u32_view.bp_size(),
        sizeof(std::uint32_t));
    REQUIRE(non_u32_access.route
        == bioseq::storage::sequence_access_route::materialize_admitted);
    REQUIRE_FALSE(non_u32_access.resident.has_value());
    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t admission_before = admission.reserved_bytes();
    collector_type admitted_non_u32_collector;
    const auto admitted_non_u32_stats =
        minimizer_dispatch_range<minimizer_type>(
            admitted_non_u32_collector,
            non_u32_view,
            0u,
            non_u32_view.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_resident_vector_stream,
            table);
    REQUIRE(admission.reserved_bytes() == admission_before);
    REQUIRE(admitted_non_u32_stats.exact_simd_available);
    REQUIRE(admitted_non_u32_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(admitted_non_u32_stats.exact_simd
        .resident_vector_stream_kernel_calls == 1u);
    REQUIRE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        admitted_non_u32_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_resident_vector_stream));
    const auto& admitted_records =
        admitted_non_u32_collector.extract_sketches_data();
    const auto& resident_records =
        paired_resident_collector.extract_sketches_data();
    REQUIRE(admitted_records.size() == resident_records.size());
    for (std::size_t index = 0u; index < resident_records.size(); ++index) {
        CAPTURE(index);
        REQUIRE(admitted_records[index].raw_value()
            == resident_records[index].raw_value());
    }

    // This is the fail-closed resident-stats boundary: resident stats cannot
    // be relabelled as the continuation/B8 scheme used by persisted indexes.
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        paired_resident_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_continuation));

    collector_type persisted_compatible_collector;
    const auto persisted_compatible_stats =
        minimizer_dispatch_range<minimizer_type>(
            persisted_compatible_collector,
            resident_view,
            0u,
            resident.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_continuation,
            table);
    REQUIRE(persisted_compatible_stats.exact_simd_available);
    REQUIRE(persisted_compatible_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(persisted_compatible_stats.exact_simd
        .resident_vector_stream_kernel_calls == 0u);
    REQUIRE((persisted_compatible_stats.exact_simd
            .generic_u64_kernel_calls > 0u
        || persisted_compatible_stats.exact_simd
            .kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0u));
    REQUIRE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        persisted_compatible_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_continuation));
    REQUIRE_FALSE(exact_simd_stats_share_selection_route(
        paired_resident_stats.exact_simd,
        persisted_compatible_stats.exact_simd));
    REQUIRE(exact_simd_stats_share_selection_route(
        paired_resident_stats.exact_simd,
        whole_stats));

    collector_type rejected_resident_subrange_collector;
    REQUIRE_THROWS_AS(
        minimizer_dispatch_range<minimizer_type>(
            rejected_resident_subrange_collector,
            resident_view,
            0u,
            resident.bp_size() - 1u,
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_resident_vector_stream,
            table),
        std::invalid_argument);
    REQUIRE(rejected_resident_subrange_collector
        .extract_sketches_data().empty());

    collector_type subrange_collector;
    const auto subrange_stats = minimizer_dispatch_range<minimizer_type>(
        subrange_collector,
        resident_view,
        0u,
        resident.bp_size() - 1u,
        10u,
        15u,
        minimizer_sketch_backend::exact_simd,
        exact_simd_range_route::require_continuation,
        table);
    REQUIRE(subrange_stats.exact_simd_available);
    REQUIRE(subrange_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(subrange_stats.exact_simd
        .resident_vector_stream_kernel_calls == 0u);
    REQUIRE((subrange_stats.exact_simd.generic_u64_kernel_calls > 0u
        || subrange_stats.exact_simd
            .kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0u));
}

TEST_CASE("Indexed dna4 u64 queries use admitted typed u32 B8 continuation with scalar-identical records",
          "[component:bioseq][layer:unit][feature:seeding][resource:highway]") {
    using namespace ::bioseq::simd;
    using query_sequence = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using query_range = compact_sequence_range<query_sequence>;

    minimap2_parameters params;
    bind_fixed_highway(params);
    params.minimizer_algorithm = minimizer_algorithm::two_stack_nthash;

    const auto assert_case = [&params](std::vector<seq::dna4> data,
                                      std::string_view fixture) {
        CAPTURE(fixture, data.size());
        query_range resident(data);
        auto lazy_query = bioseq::storage::make_resident_lazy_sequence(resident);
        const auto query_view =
            bioseq::storage::make_lazy_sequence_view(lazy_query);
        const auto native = query_view.try_resident_packed(
            0u, query_view.bp_size(), sizeof(std::uint64_t));
        REQUIRE(native.has_value());
        const auto access = bioseq::storage::plan_sequence_access(
            query_view,
            bioseq::storage::sequence_access_requirement::
                contiguous_resident,
            0u,
            query_view.bp_size(),
            sizeof(std::uint32_t));
        REQUIRE(access.route
            == bioseq::storage::sequence_access_route::materialize_admitted);
        REQUIRE(access.resident_borrow_kind
            == bioseq::storage::sequence_resident_borrow_kind::none);
        REQUIRE_FALSE(access.resident.has_value());
        REQUIRE(access.admission_bytes
            == bioseq::storage::packed_sequence_admission_bytes<seq::dna4>(
                data.size(), sizeof(std::uint32_t)));
        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        const std::uint64_t admission_baseline = admission.reserved_bytes();
        {
            auto permit = admission.try_reserve(access.admission_bytes);
            REQUIRE(permit.has_value());
            auto typed_owner = bioseq::storage::materialize_packed_sequence(
                query_view,
                0u,
                query_view.bp_size(),
                sizeof(std::uint32_t),
                std::move(*permit));
            REQUIRE(admission.reserved_bytes()
                == admission_baseline + access.admission_bytes);
            REQUIRE(typed_owner.descriptor().storage_unit_width
                == sizeof(std::uint32_t));
            REQUIRE(typed_owner.descriptor().unit_count
                == (data.size() + 15u) / 16u);
            REQUIRE(typed_owner.descriptor().units != native->units);
        }
        REQUIRE(admission.reserved_bytes() == admission_baseline);
        auto fast = sketch_query_for_index_selection<minimap_sketch_data>(
            query_view, params, 10u, 15u);
        REQUIRE(admission.reserved_bytes() == admission_baseline);

        basic_sketch_collector<minimap_sketch_data> scalar_collector;
        collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
            scalar_collector,
            resident,
            resident.begin(),
            resident.end(),
            10u,
            15u);
        const auto scalar =
            std::move(scalar_collector).extract_sketches_data();

        REQUIRE(fast.exact_simd_execution_stats_available);
        const two_stack_execution_stats& stats =
            fast.exact_simd_execution_stats;
        REQUIRE(stats.is_internally_consistent(/*require_nonempty_vector_work=*/true));
        REQUIRE(stats.bp_consumed == data.size());
        REQUIRE(stats.resident_vector_stream_kernel_calls == 0u);
        REQUIRE(stats.resident_vector_stream_single_calls == 0u);
        REQUIRE(stats.resident_vector_stream_dual_calls == 0u);
        REQUIRE(stats.generic_u64_kernel_calls == 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 1u);
        REQUIRE(stats.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks > 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_uint32_hash_lane_visits > 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_packed_22_groups > 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_packed_22_lane_visits > 0u);
        REQUIRE(fast.minimizers.size() == scalar.size());
        for (std::size_t i = 0; i < scalar.size(); ++i) {
            CAPTURE(i);
            REQUIRE(fast.minimizers[i].raw_value()
                == scalar[i].raw_value());
        }
    };

    assert_case(
        vulkan_pipeline_test_support::make_dna4(4'123u, 0xB8D0A57u),
        "non-periodic tail");
    std::vector<seq::dna4> repeated(4'123u);
    seq::dna4 adenine;
    adenine.assign_rank(
        static_cast<decltype(adenine.to_rank())>(0u));
    std::fill(repeated.begin(), repeated.end(), adenine);
    assert_case(std::move(repeated), "equal-hash tie windows");
}

TEST_CASE("KSW2 batched pipeline returns operation-originated nonempty stats",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;

    auto target = vulkan_pipeline_test_support::make_dna4(4'000u, 0xB47C4EDu);
    constexpr std::size_t truth_start = 700u;
    std::vector<seq::dna4> query(
        target.begin() + truth_start,
        target.begin() + truth_start + 900u);

    minimap2_parameters params{};
    bind_fixed_highway(params);
    params.minimizer_algorithm =
        minimizer_algorithm::two_stack_nthash;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.base_alignment_algorithm =
        base_alignment_algorithm::ksw2_batched;
    params.do_cigar = true;

    const auto result = vulkan_pipeline_test_support::run(query, target, params);
    REQUIRE(result.is_exact_simd_query_execution_consistent(true));
    REQUIRE(result.is_exact_simd_target_execution_consistent(true));
    REQUIRE(result.is_simd_chaining_execution_consistent(
        params.simd_chaining_policy,
        /*require_nonempty_vector_work=*/true));
    REQUIRE(result.is_ksw2_batched_execution_consistent(true));
    REQUIRE_FALSE(result.block_aligner_execution_stats_available);
    REQUIRE(result.scalar_pipeline_execution_stats_available);
    REQUIRE(result.scalar_pipeline_stats.seed_lookup_calls > 0u);
    REQUIRE(result.scalar_pipeline_stats.seed_query_minimizers > 0u);
    REQUIRE(result.scalar_pipeline_stats.query_minimizers
            == ::bioseq::simd::scalar_minimizer_execution_stats{});
    REQUIRE(result.scalar_pipeline_stats.target_minimizers
            == ::bioseq::simd::scalar_minimizer_execution_stats{});
    REQUIRE(result.query_sketch_host_wall_ns > 0u);
    REQUIRE(result.seed_lookup_host_wall_ns > 0u);
    REQUIRE(result.chain_score_host_wall_ns > 0u);
    REQUIRE(result.region_generation_host_wall_ns > 0u);
    REQUIRE(result.alignment_result_host_wall_ns > 0u);
    REQUIRE_FALSE(result.is_region_host_stage_active);
    REQUIRE_FALSE(result.is_alignment_result_host_stage_active);
    REQUIRE_FALSE(result.is_host_stage_timing_saturated);
    REQUIRE(result.scalar_pipeline_stats.chain_score_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.chain_backtrack_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.ksw2_left_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.ksw2_gap_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.ksw2_right_calls == 0u);

    auto missing = result;
    missing.ksw2_batched_execution_stats_available = false;
    REQUIRE_FALSE(missing.is_ksw2_batched_execution_consistent(true));

    auto tampered = result;
    ++tampered.ksw2_batched_stats.long_fill_calls;
    REQUIRE_FALSE(tampered.is_ksw2_batched_execution_consistent(true));
}

TEST_CASE("bounded ntHash materializes DNA4 slices and rejects every DNA5 width",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint8_t>();
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint16_t>();
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint32_t>();
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint64_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint8_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint16_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint32_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint64_t>();
}

TEST_CASE("fragmented bounded ntHash unwinds its vector and memory permit for every scanner PostgreSQL leaf failure",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using storage_unit_type = std::uint64_t;
    using packed_sequence = sequence_custom_alphabet<seq::dna4, storage_unit_type>;
    using range_type = compact_sequence_range<packed_sequence>;
    using scanner_type = injected_failure_scanner<storage_unit_type>;
    using lazy_type = bioseq::storage::lazy_sequence<
        packed_sequence, scanner_type>;
    using minimizer_type =
        ::bioseq::simd::minimap_sketch_data;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;

    auto bases = vulkan_pipeline_test_support::make_dna4(4'123, 0x51A9E5u);
    range_type range(bases);
    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();

    for (const injected_scanner_leaf point : {
             injected_scanner_leaf::row_read,
             injected_scanner_leaf::palloc,
             injected_scanner_leaf::interrupt}) {
        CAPTURE(static_cast<unsigned>(point));
        std::size_t reads = 0;
        scanner_type scanner{
            .backing = std::span<const storage_unit_type>{
                range.read_cptr(range.cbegin()),
                static_cast<std::size_t>(range.size())},
            .logical_bp_size = static_cast<bioseq::storage::sequence_size_t>(
                range.bp_size()),
            .padding = static_cast<std::size_t>(range.padding()),
            .point = point,
            .reads = &reads,
        };
        lazy_type fragmented{std::move(scanner)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);
        collector_type collector;
        const std::uint64_t reserved_before = admission.reserved_bytes();
        constexpr bioseq::storage::sequence_size_t letters_per_unit =
            range_type::traits::letters_per_unit;
        const bioseq::storage::sequence_size_t lo_bp =
            2u * letters_per_unit;
        const bioseq::storage::sequence_size_t hi_bp = std::min(
            static_cast<bioseq::storage::sequence_size_t>(
                (range.size() - 2u) * letters_per_unit),
            static_cast<bioseq::storage::sequence_size_t>(range.bp_size()));

        REQUIRE_THROWS_AS(
            minimizer_dispatch_range<minimizer_type>(
                collector,
                fragmented_view,
                lo_bp,
                hi_bp,
                10u,
                15u,
                minimizer_sketch_backend::bounded_nthash_simd,
                exact_simd_range_route::require_continuation,
                fixed_highway_kernels()),
            injected_scanner_leaf_error);
        REQUIRE(reads == 1u);
        REQUIRE(admission.reserved_bytes() == reserved_before);
    }
}

TEST_CASE("bounded ntHash SIMD pipe executes distinct sketch stats without fallback",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::base_alignment_algorithm;

    auto target = vulkan_pipeline_test_support::make_dna4(4000, 0x51A7C0DEu);
    constexpr std::size_t truth_start = 700;
    std::vector<seq::dna4> query(
        target.begin() + truth_start,
        target.begin() + truth_start + 900);

    minimap2_parameters params{};
    bind_fixed_highway(params);
    params.minimizer_algorithm =
        minimizer_algorithm::bounded_nthash_simd;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.base_alignment_algorithm =
        base_alignment_algorithm::block_aligner;
    params.do_cigar = true;

    const auto result = vulkan_pipeline_test_support::run(query, target, params);
    REQUIRE(result.is_bounded_nthash_query_execution_consistent(true));
    REQUIRE(result.is_bounded_nthash_target_execution_consistent(true));
    REQUIRE(result.is_simd_chaining_execution_consistent(
        params.simd_chaining_policy,
        /*require_nonempty_vector_work=*/true));
    REQUIRE(result.is_block_aligner_execution_consistent(true));
    REQUIRE(result.scalar_pipeline_execution_stats_available);
    REQUIRE(result.scalar_pipeline_stats.seed_lookup_calls > 0u);
    REQUIRE(result.scalar_pipeline_stats.seed_query_minimizers > 0u);
    REQUIRE(result.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.is_all_simd_execution_consistent(
        minimizer_algorithm::two_stack_nthash,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto mixed_stats = result;
    mixed_stats.exact_simd_query_execution_stats_available = true;
    REQUIRE_FALSE(mixed_stats.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto missing_cpu_seed_stats = result;
    missing_cpu_seed_stats.scalar_pipeline_stats = {};
    missing_cpu_seed_stats.scalar_pipeline_execution_stats_available = false;
    REQUIRE_FALSE(missing_cpu_seed_stats.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto presence_only_cpu_seed_stats = missing_cpu_seed_stats;
    presence_only_cpu_seed_stats.scalar_pipeline_execution_stats_available = true;
    REQUIRE_FALSE(presence_only_cpu_seed_stats.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto hidden_scalar_chain = result;
    hidden_scalar_chain.scalar_pipeline_stats
        .record_scalar_chain(1u);
    REQUIRE_FALSE(hidden_scalar_chain.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto saturated_cpu_seed = result;
    saturated_cpu_seed.scalar_pipeline_stats.is_saturated = true;
    REQUIRE_FALSE(saturated_cpu_seed.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(result.exact_simd_target_execution_stats_available);
    REQUIRE_FALSE(result.partial_vulkan_stats.has_any_dispatch());
    REQUIRE(result.partial_vulkan_stats
            == ::bioseq::simd::partial_vulkan_execution_stats{});

    auto vulkan_dispatch_tamper = result;
    vulkan_dispatch_tamper.partial_vulkan_stats
        .query_sketch_dispatches = 1;
    REQUIRE_FALSE(vulkan_dispatch_tamper
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto vulkan_metadata_tamper = result;
    vulkan_metadata_tamper.partial_vulkan_stats
        .gpu_index_unique_keys = 1;
    REQUIRE_FALSE(vulkan_metadata_tamper.partial_vulkan_stats
        .has_any_dispatch());
    REQUIRE_FALSE(vulkan_metadata_tamper
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto hidden_opposite_payload = result;
    hidden_opposite_payload.exact_simd_query_execution_stats.bp_consumed = 1;
    REQUIRE_FALSE(hidden_opposite_payload
        .exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(hidden_opposite_payload
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto persisted_stats = result;
    persisted_stats.bounded_nthash_target_execution_stats = {};
    persisted_stats.bounded_nthash_target_execution_stats_available = false;
    REQUIRE(persisted_stats.is_selected_simd_sketch_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        /*require_target_sketch=*/false,
        /*require_nonempty_vector_work=*/true));

    auto hidden_persisted_target_payload = persisted_stats;
    hidden_persisted_target_payload.bounded_nthash_target_execution_stats =
        result.bounded_nthash_target_execution_stats;
    REQUIRE_FALSE(hidden_persisted_target_payload
        .bounded_nthash_target_execution_stats_available);
    REQUIRE_FALSE(hidden_persisted_target_payload
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/false,
            /*require_nonempty_vector_work=*/true));

    auto target_presence_only = persisted_stats;
    target_presence_only.bounded_nthash_target_execution_stats_available = true;
    REQUIRE_FALSE(target_presence_only
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/false,
            /*require_nonempty_vector_work=*/true));

    const auto primary = std::ranges::find_if(
        result.results,
        [](const auto& match) { return match.is_primary; });
    REQUIRE(primary != result.results.end());
    REQUIRE(std::abs(
        static_cast<std::int64_t>(primary->target_start)
        - static_cast<std::int64_t>(truth_start)) < 15);
}

}  // namespace bioseq::simd::pipe
