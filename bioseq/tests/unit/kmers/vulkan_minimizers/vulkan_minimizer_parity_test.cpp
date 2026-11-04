/**
 * @file vulkan_minimizer_parity_test.cpp
 * @brief Compare Vulkan and, scalar minimizers, and check reasons for rejected requests.
 */
#include <hwy/highway.h>
#include <cstring>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <libassert/assert.hpp>
#include <libassert/assert-catch2.hpp>

import std;
import bioseq.tests.unit.kmers.vulkan_minimizers_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

using namespace vulkan_minimizers_test_support;

TEST_CASE("vulkan minimizer: dna4 parity equals scalar mm-sketch",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    for (auto [w, k] :
         std::vector<std::pair<std::size_t, std::size_t>>{{10, 15}, {5, 7}, {10, 27}}) {
        const auto check_parity = [w, k]<class p_record>() {
            const auto cpu = triples(cpu_sketch<p_record>(dna4_fixture, w, k));
            const auto gpu = triples(gpu_sketch<p_record>(dna4_fixture, w, k));
            ASSERT(!gpu.empty(),
                   fmt::format("GPU produced no dna4 minimizers at (w={},k={})", w, k),
                   w,
                   k);
            ASSERT(cpu == gpu,
                   fmt::format("dna4 GPU sketch != scalar at (w={},k={}): cpu={} gpu={}",
                               w,
                               k,
                               cpu.size(),
                               gpu.size()),
                   cpu.size(),
                   gpu.size());
        };
        if (bioseq::minimizers::exact_minimizer_shape_fits(
                bioseq::minimizers::minimizer_exact_rank_bits<seq::dna4>, k,
                minimap_sketch_data::layout_type::k_hash_bits)) {
            check_parity.template operator()<minimap_sketch_data>();
        } else {
            check_parity.template operator()<minimizer_representation<63u, 64u>>();
        }
    }

    // One dense native input exercises recovery at the runtime and wrapper boundaries.
    using record_type = minimap_sketch_data;
    using sequence_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using wire = vk::minimizer_wire_traits<record_type>;
    constexpr std::size_t letter_count = 1024u;
    constexpr std::uint32_t window_size = 10u;
    constexpr std::uint32_t kmer_size = 15u;
    constexpr std::uint64_t expected_count = letter_count - kmer_size + 1u;
    seq::dna4 repeated_letter{};
    seqan3::assign_char_to('A', repeated_letter);
    std::vector<seq::dna4> dense_data(letter_count, repeated_letter);
    const auto scalar = cpu_sketch<record_type>(dense_data, window_size, kmer_size);
    REQUIRE(scalar.size() == expected_count);
    auto sequence = compact_sequence_range<sequence_type>{dense_data};
    const auto upload = vk::make_sequence_upload_view(sequence);
    const auto rank_lut = vk::make_vulkan_rank_lut<decltype(sequence)>(
        vk::vulkan_minimizer_seed_policy_id<typename decltype(sequence)::traits>(kmer_size));
    REQUIRE(rank_lut.seed_policy_id == 0u);
    const auto estimated_count = vk::estimate_vulkan_minimizer_capacity(
        upload.bp_size, window_size, rank_lut.seed_policy_id);
    REQUIRE(estimated_count == 498u);
    REQUIRE(expected_count > estimated_count);
    REQUIRE(upload.byte_size == upload.padded_byte_size);

    auto& runtime = vk::runtime::instance();
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto previous_cap = admission.index_memory_cap_bytes();
    {
        struct restore_cap_guard {
            bioseq::parallelism::memory_admission_controller& admission;
            std::uint64_t previous;
            ~restore_cap_guard() { admission.set_index_memory_cap_bytes(previous); }
        } restore{admission, previous_cap};
        const auto baseline = admission.reserved_bytes();
        const auto cache_before = runtime.pipeline_cache_metadata_reserved_bytes();
        const auto plan = vk::make_gpu_sketch_memory_plan(
            upload.bp_size, upload.padded_byte_size, sizeof(vk::vulkan_rank_lut),
            estimated_count, vk::vulkan_sketch_resident_bytes<record_type>(estimated_count),
            vk::gpu_sketch_purpose::minimizer_words, kmer_size, window_size,
            upload.letters_per_unit, runtime.features().max_workgroup_count_x);
        REQUIRE(plan.is_valid);
        const auto initial_requirements = runtime.sketch_batch_allocation_requirements(
            upload.padded_byte_size, plan.tiles.tile_count,
            static_cast<std::uint32_t>(wire::k_word_kind),
            wire::k_words32_per_minimizer, estimated_count);
        const auto exact_requirements = runtime.sketch_batch_allocation_requirements(
            upload.padded_byte_size, plan.tiles.tile_count,
            static_cast<std::uint32_t>(wire::k_word_kind),
            wire::k_words32_per_minimizer, expected_count);
        REQUIRE(initial_requirements.valid);
        REQUIRE(exact_requirements.valid);
        const auto physical_arena_bytes =
            runtime.gpu_arena_allocation_requirement(plan.arena_scaffold_bytes);
        const auto retained_host_bytes = vk::vulkan_exact_host_allocation_admission_bytes(
            plan.count_submit_host_transient_bytes);
        const auto initial_peak = vk::vulkan_sketch_dispatch_admission_bytes(
            physical_arena_bytes, retained_host_bytes, 0u,
            initial_requirements.pessimistic_output_allocation_bytes);
        const auto exact_peak = vk::vulkan_sketch_dispatch_admission_bytes(
            physical_arena_bytes, retained_host_bytes, 0u,
            exact_requirements.pessimistic_output_allocation_bytes);
        const auto host_headroom = bioseq::common::saturating_mul(
            expected_count * sizeof(record_type),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const auto operation_headroom = bioseq::common::saturating_add(
            bioseq::common::saturating_add(initial_peak, exact_peak), host_headroom);
        admission.set_index_memory_cap_bytes(
            bioseq::common::saturating_add(baseline, operation_headroom));
        REQUIRE(initial_requirements.pessimistic_output_requested_bytes <=
                runtime.features().max_storage_buffer_range);
        REQUIRE(exact_requirements.pessimistic_output_requested_bytes <=
                runtime.features().max_storage_buffer_range);

        const vk::vulkan_sketch_params params{
            .bp_size = static_cast<std::uint32_t>(upload.bp_size),
            .window_size = window_size,
            .kmer_size = kmer_size,
            .hash_bits = wire::k_hash_bits,
            .hash_shift = wire::k_hash_shift,
            .pos_bits = wire::k_position_bits,
            .minimizer_word_kind = static_cast<std::uint32_t>(wire::k_word_kind),
            .minimizer_words32 = wire::k_words32_per_minimizer,
            .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
            .unit_bits = upload.unit_bits,
            .bits_per_char = upload.bits_per_char,
            .letters_per_unit = upload.letters_per_unit,
            .alphabet_size = upload.alphabet_size,
            .seq_word_offset = 0u,
            .char_mask = upload.char_mask,
            .padding_letters = upload.padding_letters,
        };
        {
            auto peak_permit = admission.try_reserve(initial_peak);
            REQUIRE(peak_permit.has_value());
            REQUIRE(peak_permit->bytes() == initial_peak);
            // Explicit preallocated mode proves recovery, independently of OS sampling.
            auto recovered = runtime.run_sketch_roundtrip(
                {static_cast<const std::byte*>(upload.bytes),
                 static_cast<std::size_t>(upload.padded_byte_size)},
                rank_lut, params, estimated_count, {}, 0u,
                vk::vulkan_sketch_output_allocation::preallocated,
                std::addressof(*peak_permit), retained_host_bytes);
            REQUIRE_FALSE(recovered.overflow);
            REQUIRE_FALSE(recovered.execution_peak_admission_failure.has_failure());
            REQUIRE(recovered.completed_roundtrips == 2u);
            REQUIRE(recovered.count == expected_count);
            REQUIRE(recovered.arena_trimmed_before_return);
            REQUIRE(recovered.minimizer_words.is_valid());
            REQUIRE(recovered.minimizer_words.mapped != nullptr);
            REQUIRE(recovered.minimizer_words.bytes ==
                    wire::device_bytes_for_count(expected_count));
            REQUIRE(recovered.minimizer_words.allocation_bytes ==
                    exact_requirements.pessimistic_output_allocation_bytes);
            REQUIRE(peak_permit->bytes() >= exact_peak);
            bioseq_log<logging_level::info>(
                "[vulkan recovery fixture] initial_count={} actual_count={} "
                "initial_output_allocation={} recovered_output_allocation={} "
                "initial_permit={} retained_permit={}", estimated_count, recovered.count,
                initial_requirements.pessimistic_output_allocation_bytes,
                recovered.minimizer_words.allocation_bytes, initial_peak, peak_permit->bytes());
            if (exact_requirements.pessimistic_output_allocation_bytes >
                initial_requirements.pessimistic_output_allocation_bytes) {
                REQUIRE(peak_permit->bytes() > initial_peak);
            }
            const auto recovered_host = vk::decode_host_minimizer_bytes<record_type>(
                {static_cast<const std::byte*>(recovered.minimizer_words.mapped),
                 static_cast<std::size_t>(recovered.minimizer_words.bytes)}, recovered.count);
            REQUIRE(triples(recovered_host) == triples(scalar));
            // Buffer destruction precedes release of its retained peak reservation.
        }
        const auto cache_after_runtime = runtime.pipeline_cache_metadata_reserved_bytes();
        REQUIRE(cache_after_runtime >= cache_before);
        REQUIRE(admission.reserved_bytes() == baseline + cache_after_runtime - cache_before);

        // Same input through automatic ownership verifies handoff, not its initial mode.
        auto result = vk::sketch_uploaded_window_result<record_type>(
            upload, rank_lut, window_size, kmer_size);
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->overflow);
        REQUIRE(result->dispatch_token.is_internally_consistent());
        REQUIRE(result->dispatch_token.completed_roundtrips == 2u);
        REQUIRE(result->device != nullptr);
        REQUIRE(result->device->words.is_valid());
        REQUIRE(result->device->count == expected_count);
        REQUIRE(result->device->capacity == expected_count);
        REQUIRE(result->device->permit.bytes() == result->device->words.allocation_bytes);
        REQUIRE(result->host_permit.has_value());
        const auto host_capacity_bytes = result->host_minimizers.capacity() * sizeof(record_type);
        REQUIRE(result->host_permit->bytes() == host_capacity_bytes);
        REQUIRE(triples(result->host_minimizers) == triples(scalar));
        REQUIRE(result->arena_trimmed_before_host_materialization);
        REQUIRE_FALSE(runtime.reusable_gpu_arena().device_buffer().is_valid());
        REQUIRE_FALSE(runtime.reusable_gpu_arena().upload_buffer().is_valid());
        REQUIRE_FALSE(runtime.reusable_gpu_arena().download_buffer().is_valid());
        const auto cache_after_wrapper = runtime.pipeline_cache_metadata_reserved_bytes();
        REQUIRE(cache_after_wrapper >= cache_after_runtime);
        REQUIRE(admission.reserved_bytes() == baseline + cache_after_wrapper - cache_before +
                result->device->words.allocation_bytes + host_capacity_bytes);
        result.reset();
        REQUIRE(admission.reserved_bytes() == baseline + cache_after_wrapper - cache_before);
    }
    REQUIRE(admission.index_memory_cap_bytes() == previous_cap);

}

TEST_CASE("vulkan minimizer: dna5 with T parity (T is nt4 code 3)",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    const auto cpu = cpu_sketch<minimap_sketch_data>(dna5_with_t, w, k);
    const auto gpu = gpu_sketch<minimap_sketch_data>(dna5_with_t, w, k);
    ASSERT(!gpu.empty(), "GPU produced no dna5+T minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_parity(
        cpu,
        gpu,
        vk::make_sketch_tile_plan(
            dna5_with_t.size(), k, w, compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: dna5 N follows exact or bounded seed policy",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using range_type = compact_sequence_range<seq_type>;
    const auto tile_plan = vk::make_sketch_tile_plan(
        dna5_with_n.size(), k, w, range_type::traits::letters_per_unit);
    std::vector<std::size_t> n_positions;
    for (std::size_t position = 0u; position < dna5_with_n.size(); ++position) {
        if (seqan3::to_char(dna5_with_n[position]) == 'N') n_positions.push_back(position);
    }
    REQUIRE_FALSE(n_positions.empty());
    const auto contains_n = [&n_positions, k](const auto& minimizers) {
        for (const auto& minimizer : minimizers) {
            const auto last_position = minimizer.last_pos();
            REQUIRE(last_position + 1u >= k);
            const auto first_position = last_position + 1u - k;
            for (const auto n_position : n_positions) {
                if (n_position >= first_position && n_position <= last_position) return true;
            }
        }
        return false;
    };

    SECTION("exact profile excludes every N-containing k-mer") {
        const auto cpu_values = cpu_sketch<minimap_sketch_data>(dna5_with_n, w, k);
        const auto gpu_values = gpu_sketch<minimap_sketch_data>(dna5_with_n, w, k);
        REQUIRE_FALSE(cpu_values.empty());
        REQUIRE_FALSE(gpu_values.empty());
        require_gpu_minimizer_parity(cpu_values, gpu_values, tile_plan, k, w);
        REQUIRE_FALSE(contains_n(cpu_values));
        REQUIRE_FALSE(contains_n(gpu_values));
    }

    SECTION("explicit bounded profile seeds native N ranks") {
        auto sequence = range_type{dna5_with_n};
        basic_sketch_collector<minimap_sketch_data> collector;
        collect_minimizers_view<minimap_int_hasher, minimap_sketch_data,
            range_type, decltype(sequence.begin()), std::uint32_t,
            decltype(collector), bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash>(
                collector, sequence, sequence.begin(), sequence.end(), w, k);
        const auto cpu_values = std::move(collector).extract_sketches_data();
        const auto rank_lut = vk::make_vulkan_rank_lut<range_type>(
            bioseq::minimizers::k_bounded_nthash_scheme_id);
        auto result = vk::sketch_uploaded_window_result<minimap_sketch_data>(
            vk::make_sequence_upload_view(sequence), rank_lut, w, k);
        REQUIRE(result.has_value());
        require_gpu_minimizer_parity(cpu_values, result->host_minimizers, tile_plan, k, w);
        // One complete window has all candidate k-mers overlapping N.
        REQUIRE(contains_n(cpu_values));
        REQUIRE(contains_n(result->host_minimizers));
    }
}

TEST_CASE("vulkan minimizer: dna5 tile-boundary parity matches scalar multiset",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    constexpr std::size_t letter_count = 21u * 700u;
    std::vector<seq::dna5> data;
    data.reserve(letter_count);
    std::uint64_t lcg = 0x6a09e667f3bcc909ULL;
    for (std::size_t i = 0; i < letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna5 value{};
        value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % 4u));
        data.push_back(value);
    }
    for (const std::size_t n_pos : std::array<std::size_t, 12>{
             240u, 251u, 252u, 253u, 503u, 504u, 505u, 755u, 756u, 757u, 1007u, 1008u}) {
        seqan3::assign_char_to('N', data[n_pos]);
    }

    const auto cpu = cpu_sketch<minimap_sketch_data>(data, w, k);
    const auto gpu = gpu_sketch<minimap_sketch_data>(data, w, k);
    ASSERT(!gpu.empty(), "fused dna5 sketch produced no minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_parity(
        cpu,
        gpu,
        vk::make_sketch_tile_plan(
            data.size(), k, w, compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: generic alphabet functional superset",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint8_t>;
    constexpr std::uint32_t w = 5;
    constexpr std::uint32_t k = 7;
    std::vector<seq::dna5> data = dna5_with_t;
    auto seq = compact_sequence_range<seq_type>(data);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    auto result = vk::collect_minimizers_vulkan_result<minimap_sketch_data>(sequence_view, w, k);
    REQUIRE(result.has_value());
    auto cpu = cpu_sketch<minimap_sketch_data>(data, w, k);
    require_gpu_minimizer_parity(
        cpu,
        result->host_minimizers,
        vk::make_sketch_tile_plan(
            data.size(), k, w, compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: oversized k declines before dispatch (no throw)",
          "[component:bioseq][layer:unit][feature:seeding]") {
    // Unsupported k-mer and window sizes must return a reason before GPU work.
    // This test  can run without a Vulkan device
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto big_k =
        vk::collect_minimizers_vulkan_result<minimap_sketch_data>(sequence_view, 10, 40, &decline);
    ASSERT(!big_k.has_value(), "oversized k must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::kmer_size_unsupported);
    REQUIRE(decline.kmer_size == 40u);
    auto big_w =
        vk::collect_minimizers_vulkan_result<minimap_sketch_data>(sequence_view, 300, 15, &decline);
    ASSERT(!big_w.has_value(), "oversized window must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::window_size_unsupported);
    REQUIRE(decline.window_size == 300u);
}

TEST_CASE("vulkan minimizer: typed decline preserves admission snapshot and strict message",
          "[component:bioseq][layer:unit][feature:seeding]") {
    const vk::vulkan_sketch_decline decline{
        .reason = vk::vulkan_sketch_decline_reason::memory_admission_denied,
        .bp_size = 101u,
        .padded_sequence_bytes = 32u,
        .max_minimizers = 17u,
        .query_gate_words = 3u,
        .window_size = 10u,
        .kmer_size = 15u,
        .admission =
            {
                .reason = bioseq::parallelism::memory_admission_failure_reason::budget_exhausted,
                .requested_bytes = 701u,
                .reserved_bytes = 211u,
                .budget_ceiling_bytes = 503u,
                .allocatable_after_reservations_bytes = 292u,
                .process_footprint_bytes = 401u,
                .system_available_bytes = 601u,
                .system_total_bytes = 809u,
                .process_soft_cap_bytes = 307u,
            },
    };
    const std::string formatted = vk::format_vulkan_sketch_decline(decline);
    REQUIRE(formatted.find("reason=memory_admission_denied") != std::string::npos);
    REQUIRE(formatted.find("admission_reason=budget_exhausted") != std::string::npos);
    REQUIRE(formatted.find("required_bytes=701") != std::string::npos);
    REQUIRE(formatted.find("allocatable_after_reservations_bytes=292") != std::string::npos);
    REQUIRE(formatted.find("reserved_bytes=211") != std::string::npos);
    REQUIRE(formatted.find("budget_ceiling_bytes=503") != std::string::npos);
    REQUIRE(formatted.find("process_footprint_bytes=401") != std::string::npos);
    REQUIRE(formatted.find("system_available_bytes=601") != std::string::npos);
    REQUIRE(formatted.find("system_total_bytes=809") != std::string::npos);
    REQUIRE(formatted.find("process_soft_cap_bytes=307") != std::string::npos);

    bool caught = false;
    try {
        vk::throw_vulkan_strict_decline("typed target-index fixture", decline);
    } catch (const vk::vulkan_runtime_error& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("typed target-index fixture selected") != std::string::npos);
        REQUIRE(message.find("reason=memory_admission_denied") != std::string::npos);
        REQUIRE(message.find("admission_reason=budget_exhausted") != std::string::npos);
        REQUIRE(message.find("system_total_bytes=809") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("vulkan minimizer: only reducible declines permit segment retry",
          "[component:bioseq][layer:unit][feature:seeding]") {
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::memory_admission_denied));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::output_capacity_overflow));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::sequence_size_exceeds_u32));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::hardware_device_unavailable));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::kmer_size_unsupported));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::query_gate_exceeds_u32));
}

TEST_CASE("vulkan minimizer: target-index decline identity survives wrappers",
          "[component:bioseq][layer:unit][feature:index]") {
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::output_capacity_overflow) ==
                   vk::vulkan_sketch_decline_reason::output_capacity_overflow);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::memory_plan_invalid) ==
                   vk::vulkan_sketch_decline_reason::memory_plan_invalid);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::device_buffer_allocation_failed) ==
                   vk::vulkan_sketch_decline_reason::device_allocation_failed);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::count_peak_memory_admission_denied) ==
                   vk::vulkan_sketch_decline_reason::memory_admission_denied);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::execution_peak_memory_admission_denied) ==
                   vk::vulkan_sketch_decline_reason::memory_admission_denied);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::index_invariant_failed) ==
                   vk::vulkan_sketch_decline_reason::target_index_build_invariant_failed);

    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built =
        vk::build_gpu_index_on_device<minimap_sketch_data>(sequence_view, 10u, 40u, {}, &decline);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::kmer_size_unsupported);
    REQUIRE(decline.bp_size == sequence.bp_size());
    REQUIRE(decline.window_size == 10u);
    REQUIRE(decline.kmer_size == 40u);
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
