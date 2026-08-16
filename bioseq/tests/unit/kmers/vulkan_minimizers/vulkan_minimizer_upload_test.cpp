/**
 * @file vulkan_minimizer_upload_test.cpp
 * @brief Vulkan minimizer upload and typed failure tests.
 */
#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.tests.unit.kmers.vulkan_minimizers_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

using namespace vulkan_minimizers_test_support;

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("Vulkan canonical u32 preparation borrows stable resident storage",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    constexpr std::size_t source_size = 1'000'003u;
    std::vector<seq::dna4> letters(source_size);
    for (std::size_t index = 0u; index < letters.size(); ++index) {
        letters[index].assign_rank(static_cast<std::uint8_t>((index * 11u + index / 19u) & 3u));
    }
    using config = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<config> range{letters};
    auto lazy = bioseq::storage::make_resident_lazy_sequence(range);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto resident = view.try_resident_packed(0u, view.bp_size(), sizeof(std::uint32_t));
    REQUIRE(resident.has_value());
    REQUIRE(resident->leading_symbol_offset == 0u);

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    vk::arm_vulkan_admitted_window_test_seam();
    struct reset_window_seam {
        ~reset_window_seam() {
            vk::reset_vulkan_admitted_window_test_seam();
        }
    } reset;
    vk::vulkan_sketch_decline decline{};
    auto window = vk::admitted_sequence_detail::prepare_canonical_u32_window(
        view, 0u, view.bp_size(), &decline);
    REQUIRE(window.has_value());
    REQUIRE_FALSE(window->owner.has_value());
    REQUIRE(window->borrow_kind ==
            bioseq::storage::sequence_resident_borrow_kind::native_typed_storage);
    REQUIRE(window->descriptor.units == resident->units);
    REQUIRE(window->descriptor.unit_count == resident->unit_count);
    REQUIRE(window->descriptor.logical_length == view.bp_size());
    const auto snapshot = vk::vulkan_admitted_window_test_snapshot();
    REQUIRE(snapshot.admitted_materializations == 0u);
    REQUIRE(snapshot.live_admitted_windows == 0u);
    REQUIRE(snapshot.live_admitted_permit_bytes == 0u);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("Vulkan byte upload borrows u64 storage without claiming typed u32",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    constexpr std::size_t source_size = 65'551u;
    std::vector<seq::dna4> letters(source_size);
    for (std::size_t index = 0u; index < letters.size(); ++index) {
        letters[index].assign_rank(static_cast<std::uint8_t>((index * 29u + index / 7u) & 3u));
    }

    using source_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<source_config> source_range{letters};
    auto lazy = bioseq::storage::make_resident_lazy_sequence(source_range);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto native = view.try_resident_packed(0u, view.bp_size(), sizeof(std::uint64_t));
    REQUIRE(native.has_value());

    const auto typed_plan = bioseq::storage::plan_sequence_access(
        view,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        view.bp_size(),
        sizeof(std::uint32_t));
    REQUIRE(typed_plan.route == bioseq::storage::sequence_access_route::materialize_admitted);
    REQUIRE_FALSE(typed_plan.resident.has_value());
    REQUIRE(typed_plan.resident_borrow_kind ==
            bioseq::storage::sequence_resident_borrow_kind::none);

    const auto upload_plan = bioseq::storage::plan_sequence_access(
        view,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        view.bp_size(),
        sizeof(std::uint32_t),
        bioseq::storage::sequence_resident_borrow_policy::byte_upload_compatible);
    REQUIRE(upload_plan.route == bioseq::storage::sequence_access_route::borrow_resident);
    REQUIRE(upload_plan.resident.has_value());
    REQUIRE(upload_plan.resident_borrow_kind ==
            bioseq::storage::sequence_resident_borrow_kind::reinterpreted_bytes);
    REQUIRE(upload_plan.resident->units == native->units);
    REQUIRE(upload_plan.resident->unit_count == (letters.size() + 15u) / 16u);

    using control_config = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<control_config> control_range{letters};
    auto control_lazy = bioseq::storage::make_resident_lazy_sequence(control_range);
    const auto control_view = bioseq::storage::make_lazy_sequence_view(control_lazy);
    const auto control =
        control_view.try_resident_packed(0u, control_view.bp_size(), sizeof(std::uint32_t));
    REQUIRE(control.has_value());
    REQUIRE(control->unit_count == upload_plan.resident->unit_count);
    REQUIRE(std::memcmp(upload_plan.resident->units,
                        control->units,
                        control->unit_count * sizeof(std::uint32_t)) == 0);

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    vk::arm_vulkan_admitted_window_test_seam();
    struct reset_window_seam {
        ~reset_window_seam() {
            vk::reset_vulkan_admitted_window_test_seam();
        }
    } reset;
    vk::vulkan_sketch_decline decline{};
    auto window = vk::admitted_sequence_detail::prepare_canonical_u32_window(
        view, 0u, view.bp_size(), &decline);
    REQUIRE(window.has_value());
    REQUIRE_FALSE(window->owner.has_value());
    REQUIRE(window->borrow_kind ==
            bioseq::storage::sequence_resident_borrow_kind::reinterpreted_bytes);
    REQUIRE(window->descriptor.units == native->units);

    bool visited_byte_segment = false;
    vk::admitted_sequence_detail::with_canonical_u32_upload_source<seq::dna4>(
        *window, [&visited_byte_segment, &native, &control](auto& upload_source) {
            using source_type = std::remove_cvref_t<decltype(upload_source)>;
            if constexpr (requires { source_type::is_byte_addressed_upload_only; }) {
                visited_byte_segment = true;
                const vk::segment_upload_descriptor segment =
                    vk::compute_segment_descriptor(upload_source, 1u, 5u);
                auto begin = upload_source.cbegin();
                begin += segment.lo_unit;
                const auto* base = upload_source.read_cptr(begin);
                STATIC_REQUIRE(std::same_as<decltype(base), const std::byte*>);
                const vk::sequence_upload_view upload =
                    vk::make_upload_view_from_window(upload_source, base, segment);
                const auto* native_bytes = static_cast<const std::byte*>(native->units);
                const auto* control_bytes = static_cast<const std::byte*>(control->units);
                REQUIRE(upload.bytes == native_bytes + sizeof(std::uint32_t));
                REQUIRE(upload.byte_size == 4u * sizeof(std::uint32_t));
                REQUIRE(upload.unit_bits == 32u);
                REQUIRE(upload.bp_size == 64u);
                REQUIRE(std::memcmp(upload.bytes,
                                    control_bytes + sizeof(std::uint32_t),
                                    static_cast<std::size_t>(upload.byte_size)) == 0);
            }
        });
    REQUIRE(visited_byte_segment);
    const auto snapshot = vk::vulkan_admitted_window_test_snapshot();
    REQUIRE(snapshot.admitted_materializations == 0u);
    REQUIRE(snapshot.live_admitted_windows == 0u);
    REQUIRE(snapshot.live_admitted_permit_bytes == 0u);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("Vulkan delayed fence retains admitted or stable borrowed source on faults",
          "[component:bioseq][layer:unit][feature:gpu_runtime][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; delayed-fence fixture skipped");
        return;
    }

    static constexpr std::size_t source_size = 8192u;
    static constexpr bioseq::storage::sequence_size_t selected_from = 64u;
    static constexpr bioseq::storage::sequence_size_t selected_to = 8128u;
    std::vector<seq::dna4> prewarm_letters(source_size);
    for (std::size_t index = 0u; index < prewarm_letters.size(); ++index) {
        prewarm_letters[index].assign_rank(
            static_cast<std::uint8_t>((index * 13u + index / 17u) & 3u));
    }
    using prewarm_config = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<prewarm_config> prewarm_range{prewarm_letters};
    auto prewarm_owner = bioseq::storage::make_resident_lazy_sequence(prewarm_range);
    basic_sketch_collector<minimap_sketch_data> prewarm_collector;
    vk::collect_minimizers_vulkan_segment_or_throw<minimap_sketch_data>(
        prewarm_collector,
        bioseq::storage::make_lazy_sequence_view(prewarm_owner),
        selected_from,
        selected_to,
        10u,
        15u);

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();

    const auto run_fault = [&admission, baseline](vk::vulkan_submission_test_fault fault,
                                                  std::string_view expected_error,
                                                  bool expose_resident) {
        std::atomic<std::uint64_t> destructions{0u};
        auto source = std::make_shared<vulkan_lifetime_source_state>();
        source->expose_resident = expose_resident;
        source->backend_thread = std::this_thread::get_id();
        source->destructions = std::addressof(destructions);
        source->ranks.resize(source_size);
        source->packed.resize((source_size + 15u) / 16u);
        for (std::size_t index = 0u; index < source->ranks.size(); ++index) {
            source->ranks[index] = static_cast<std::uint8_t>((index * 13u + index / 17u) & 3u);
            source->packed[index / 16u] |= static_cast<std::uint32_t>(source->ranks[index])
                                           << (2u * (index % 16u));
        }
        const std::weak_ptr<vulkan_lifetime_source_state> weak_source = source;
        const auto source_view = vulkan_lifetime_view(*source);

        struct observation {
            bool reached_delayed_submit = false;
            bool source_released_before_wait = false;
            bool permit_charged_before_wait = false;
            bool one_admitted_window_live = false;
            bool backend_materialized = false;
            bool reached_post_drain_gate = false;
            bool same_admitted_owner_after_wait = false;
            bool permit_charged_after_wait = false;
            std::uint64_t admitted_permit_bytes = 0u;
            std::uintptr_t admitted_buffer_address = 0u;
            std::uint64_t source_reads = 0u;
            std::uint64_t resident_probes = 0u;
            std::uint64_t resident_borrows = 0u;
            bool wrong_thread = false;
        } observed{};
        std::atomic<bool> call_finished{false};

        vk::arm_vulkan_admitted_window_test_seam();
        vk::arm_vulkan_submission_test_seam(fault);
        struct reset_seams {
            ~reset_seams() {
                vk::release_vulkan_submission_test_fence();
                vk::release_vulkan_submission_test_delivery();
                vk::reset_vulkan_submission_test_seam();
                vk::reset_vulkan_admitted_window_test_seam();
            }
        } reset_seams;

        std::thread observer{[&admission,
                              baseline,
                              weak_source,
                              &destructions,
                              &observed,
                              expose_resident,
                              &call_finished,
                              source = std::move(source)]() mutable {
            const auto submit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            auto submission = vk::capture_vulkan_submission_test_snapshot();
            while (!submission.waiting_after_submit &&
                   !call_finished.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < submit_deadline) {
                std::this_thread::yield();
                submission = vk::capture_vulkan_submission_test_snapshot();
            }
            observed.reached_delayed_submit =
                submission.waiting_after_submit && submission.submissions == 1u;
            if (!observed.reached_delayed_submit) {
                vk::release_vulkan_submission_test_fence();
                vk::release_vulkan_submission_test_delivery();
                while (!call_finished.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return;
            }

            const auto admitted = vk::vulkan_admitted_window_test_snapshot();
            observed.admitted_permit_bytes = admitted.live_admitted_permit_bytes;
            observed.admitted_buffer_address = admitted.live_admitted_buffer_address;
            const std::uint64_t reserved_before_wait = admission.reserved_bytes();
            observed.permit_charged_before_wait =
                expose_resident
                    ? admitted.live_admitted_permit_bytes == 0u &&
                          admitted.live_admitted_buffer_address == 0u
                    : admitted.live_admitted_permit_bytes > 0u &&
                          admitted.live_admitted_buffer_address != 0u &&
                          reserved_before_wait >= baseline &&
                          reserved_before_wait - baseline >= admitted.live_admitted_permit_bytes;
            observed.one_admitted_window_live = expose_resident
                                                    ? admitted.admitted_materializations == 0u &&
                                                          admitted.live_admitted_windows == 0u
                                                    : admitted.admitted_materializations == 1u &&
                                                          admitted.live_admitted_windows == 1u;
            observed.backend_materialized = expose_resident
                                                ? admitted.backend_thread_materializations == 0u &&
                                                      admitted.off_backend_materializations == 0u
                                                : admitted.backend_thread_materializations == 1u &&
                                                      admitted.off_backend_materializations == 0u;

            observed.source_reads = source->read_calls.load(std::memory_order_relaxed);
            observed.resident_probes = source->resident_probes.load(std::memory_order_relaxed);
            observed.resident_borrows = source->resident_borrows.load(std::memory_order_relaxed);
            observed.wrong_thread = source->wrong_thread.load(std::memory_order_relaxed);
            source.reset();
            observed.source_released_before_wait =
                weak_source.expired() && destructions.load(std::memory_order_relaxed) == 1u;
            vk::release_vulkan_submission_test_fence();

            const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            auto drained = vk::capture_vulkan_submission_test_snapshot();
            while (!drained.waiting_after_drain && !call_finished.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < drain_deadline) {
                std::this_thread::yield();
                drained = vk::capture_vulkan_submission_test_snapshot();
            }
            observed.reached_post_drain_gate =
                drained.waiting_after_drain && drained.fence_drains == 1u;
            if (observed.reached_post_drain_gate) {
                const auto admitted_after_wait = vk::vulkan_admitted_window_test_snapshot();
                observed.same_admitted_owner_after_wait =
                    expose_resident ? admitted_after_wait.live_admitted_windows == 0u &&
                                          admitted_after_wait.live_admitted_permit_bytes == 0u &&
                                          admitted_after_wait.live_admitted_buffer_address == 0u &&
                                          weak_source.expired() &&
                                          destructions.load(std::memory_order_relaxed) == 1u
                                    : admitted_after_wait.live_admitted_windows == 1u &&
                                          admitted_after_wait.live_admitted_permit_bytes ==
                                              observed.admitted_permit_bytes &&
                                          admitted_after_wait.live_admitted_buffer_address ==
                                              observed.admitted_buffer_address &&
                                          weak_source.expired() &&
                                          destructions.load(std::memory_order_relaxed) == 1u;
                const std::uint64_t reserved_after_wait = admission.reserved_bytes();
                observed.permit_charged_after_wait =
                    expose_resident
                        ? admitted_after_wait.live_admitted_permit_bytes == 0u
                        : reserved_after_wait >= baseline &&
                              reserved_after_wait - baseline >= observed.admitted_permit_bytes;
            }
            vk::release_vulkan_submission_test_delivery();
        }};

        std::string failure;
        std::exception_ptr unexpected{};
        try {
            basic_sketch_collector<minimap_sketch_data> collector;
            vk::collect_minimizers_vulkan_segment_or_throw<minimap_sketch_data>(
                collector, source_view, selected_from, selected_to, 10u, 15u);
        } catch (const vk::vulkan_runtime_error& error) {
            try {
                failure = error.what();
            } catch (...) {
                unexpected = std::current_exception();
            }
        } catch (...) {
            unexpected = std::current_exception();
        }
        call_finished.store(true, std::memory_order_release);
        vk::release_vulkan_submission_test_fence();
        vk::release_vulkan_submission_test_delivery();
        observer.join();
        if (unexpected) {
            std::rethrow_exception(unexpected);
        }

        const auto submission = vk::capture_vulkan_submission_test_snapshot();
        const auto admitted = vk::vulkan_admitted_window_test_snapshot();

        REQUIRE(observed.reached_delayed_submit);
        REQUIRE(observed.source_released_before_wait);
        REQUIRE(observed.permit_charged_before_wait);
        REQUIRE(observed.one_admitted_window_live);
        REQUIRE(observed.backend_materialized);
        REQUIRE(observed.reached_post_drain_gate);
        REQUIRE(observed.same_admitted_owner_after_wait);
        REQUIRE(observed.permit_charged_after_wait);
        REQUIRE((observed.admitted_permit_bytes > 0u) != expose_resident);
        REQUIRE((observed.admitted_buffer_address != 0u) != expose_resident);
        REQUIRE(submission.upload_latches >= 1u);
        REQUIRE(submission.submissions == 1u);
        REQUIRE(submission.fence_drains == 1u);
        REQUIRE(submission.delivered_faults == 1u);
        REQUIRE(submission.cancellation_requested ==
                (fault == vk::vulkan_submission_test_fault::cancellation_after_submit));
        REQUIRE(submission.upload_failure_latched ==
                (fault == vk::vulkan_submission_test_fault::upload_failure_after_submit));
        REQUIRE(failure.find(expected_error) != std::string::npos);
        REQUIRE(admitted.admitted_materializations == (expose_resident ? 0u : 1u));
        REQUIRE(admitted.live_admitted_windows == 0u);
        REQUIRE(admitted.live_admitted_permit_bytes == 0u);
        REQUIRE(admitted.live_admitted_buffer_address == 0u);
        REQUIRE(admitted.backend_thread_materializations == (expose_resident ? 0u : 1u));
        REQUIRE(admitted.off_backend_materializations == 0u);
        REQUIRE(observed.resident_probes == (expose_resident ? 1u : 2u));
        REQUIRE(observed.resident_borrows == (expose_resident ? 1u : 0u));
        REQUIRE((expose_resident ? observed.source_reads == 0u : observed.source_reads > 0u));
        REQUIRE_FALSE(observed.wrong_thread);
        REQUIRE(weak_source.expired());
        REQUIRE(destructions.load(std::memory_order_relaxed) == 1u);
        REQUIRE(admission.reserved_bytes() == baseline);
    };

    for (const bool expose_resident : std::array<bool, 2>{false, true}) {
        INFO("resident source=" << (expose_resident ? "borrowable" : "absent"));
        run_fault(vk::vulkan_submission_test_fault::cancellation_after_submit,
                  "cancellation after fence drain",
                  expose_resident);
        run_fault(vk::vulkan_submission_test_fault::upload_failure_after_submit,
                  "upload failure after fence drain",
                  expose_resident);
    }
}
#endif

TEST_CASE("vulkan minimizer: canonical u32 admission removes raw tail bytes with "
          "exact parity",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    using packed8_type = sequence_custom_alphabet<seq::dna4, std::uint8_t>;
    using packed64_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    for (const std::size_t letter_count : std::array<std::size_t, 3>{65u, 69u, 73u}) {
        std::vector<seq::dna4> letters(letter_count);
        for (std::size_t index = 0u; index < letters.size(); ++index) {
            letters[index].assign_rank(static_cast<std::uint8_t>(index & 3u));
        }
        compact_sequence_range<packed64_type> packed64{letters};
        auto packed64_owner = bioseq::storage::make_resident_lazy_sequence(packed64);
        const auto packed64_view = bioseq::storage::make_lazy_sequence_view(packed64_owner);
        auto baseline = vk::build_gpu_index_on_device<minimap_sketch_data>(packed64_view, 10u, 15u);
        REQUIRE(baseline.has_value());
        REQUIRE(baseline->valid);
        const std::size_t slot_bytes = static_cast<std::size_t>(baseline->occupied_slots) * 16u;
        const std::size_t position_bytes = static_cast<std::size_t>(baseline->position_count) * 8u;
        std::vector<std::byte> expected_slots(slot_bytes);
        std::vector<std::byte> expected_positions(position_bytes);
        std::memcpy(expected_slots.data(), baseline->slots.mapped, slot_bytes);
        std::memcpy(expected_positions.data(), baseline->positions.mapped, position_bytes);
        const std::uint32_t expected_slot_count = baseline->occupied_slots;
        const std::uint32_t expected_position_count = baseline->position_count;
        baseline.reset();

        compact_sequence_range<packed8_type> packed8{letters};
        const vk::sequence_upload_view upload = vk::make_sequence_upload_view(packed8);
        REQUIRE(upload.byte_size % 4u != 0u);
        auto packed8_owner = bioseq::storage::make_resident_lazy_sequence(packed8);
        const auto packed8_view = bioseq::storage::make_lazy_sequence_view(packed8_owner);
        REQUIRE_FALSE(
            packed8_view.try_resident_packed(0u, packed8_view.bp_size(), sizeof(std::uint32_t)));
        auto candidate = vk::build_gpu_index_on_device<minimap_sketch_data>(packed8_view, 10u, 15u);
        REQUIRE(candidate.has_value());
        REQUIRE(candidate->valid);
        REQUIRE(candidate->ownership.sequence_padding_zeroed_bytes == 0u);
        REQUIRE(candidate->occupied_slots == expected_slot_count);
        REQUIRE(candidate->position_count == expected_position_count);
        REQUIRE(std::memcmp(
                    candidate->slots.mapped, expected_slots.data(), expected_slots.size()) == 0);
        REQUIRE(std::memcmp(candidate->positions.mapped,
                            expected_positions.data(),
                            expected_positions.size()) == 0);
    }
}

TEST_CASE("vulkan minimizer: truncated query gate fails before target-index dispatch",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    const std::array<std::uint32_t, 1> truncated_words{1u};
    const vk::vulkan_sketch_query_gate truncated_gate{
        std::span<const std::uint32_t>{truncated_words}, 31u};
    REQUIRE_THROWS_AS(
        vk::build_gpu_index_on_device<minimap_sketch_data>(sequence_view, 10u, 15u, truncated_gate),
        vk::vulkan_runtime_error);
}

TEST_CASE("vulkan minimizer: selected segment preserves typed strict cause",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using configuration_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<configuration_type>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_type = bioseq::storage::chunked_span_sequence_scanner<
        storage_unit,
        bioseq::storage::default_fragment_storage_units>;
    using lazy_type = bioseq::storage::lazy_sequence<configuration_type, scanner_type>;

    range_type range(dna4_fixture);
    lazy_type fragmented{scanner_type::from_range(range)};
    const auto fragmented_view = bioseq::storage::make_lazy_sequence_view(fragmented);
    basic_sketch_collector<minimap_sketch_data> collector;
    bool caught = false;
    try {
        vk::collect_minimizers_vulkan_segment_or_throw<minimap_sketch_data>(
            collector, fragmented_view, 0u, fragmented.bp_size(), 10u, 40u);
    } catch (const vk::vulkan_runtime_error& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("Vulkan segment minimizer sketch selected") != std::string::npos);
        REQUIRE(message.find("reason=kmer_size_unsupported") != std::string::npos);
        REQUIRE(message.find("w=10 k=40") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("vulkan minimizer: explicit output overflow has typed identity",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; output-overflow dispatch skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    const auto upload = vk::make_sequence_upload_view(sequence);
    const auto rank_lut = vk::make_vulkan_rank_lut<decltype(sequence)>();
    bool capacity_overflowed = false;
    vk::vulkan_sketch_decline decline{};
    auto result = vk::sketch_uploaded_window_result<minimap_sketch_data>(
        upload, rank_lut, 10u, 15u, 1u, &capacity_overflowed, {}, &decline);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(capacity_overflowed);
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::output_capacity_overflow);
    REQUIRE(decline.max_minimizers == 1u);
}

TEST_CASE("vulkan minimizer: software device declines selected paths",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!software_vulkan_ready()) {
        SUCCEED("No software Vulkan device detected; hardware GPU cases cover the selected path");
        return;
    }

    using configuration_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<configuration_type>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_type = bioseq::storage::chunked_span_sequence_scanner<
        storage_unit,
        bioseq::storage::default_fragment_storage_units>;
    using lazy_type = bioseq::storage::lazy_sequence<configuration_type, scanner_type>;

    range_type range(dna4_fixture);
    auto range_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto range_view = bioseq::storage::make_lazy_sequence_view(range_owner);
    auto result = vk::collect_minimizers_vulkan_result<minimap_sketch_data>(range_view, 10, 15);
    REQUIRE_FALSE(result.has_value());

    lazy_type fragmented{scanner_type::from_range(range)};
    const auto fragmented_view = bioseq::storage::make_lazy_sequence_view(fragmented);
    REQUIRE_THROWS_AS(vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                          fragmented_view,
                          0,
                          fragmented.bp_size(),
                          0,
                          0,
                          static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
                          10,
                          15),
                      vk::vulkan_runtime_error);
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
