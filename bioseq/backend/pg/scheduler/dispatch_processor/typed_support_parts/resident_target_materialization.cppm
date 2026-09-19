/**
 * @file typed_support_parts/resident_target_materialization.cppm
 * @brief Admits and materializes one immutable target for segment workers.
 */
module;

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.resident_target_materialization;

import std;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.common.logging;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.parallelism.memory_admission;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Owns the admitted packed target shared by segment workers. */
template <sequence_alphabet p_alphabet> struct resident_target_for_workers {
    bioseq::storage::admitted_packed_sequence<p_alphabet> owner;
};

/**
 * Materialize a bounded-random-access target when memory admission succeeds.
 *
 * A rejected admission returns an empty optional so the established serial
 * scanner route can handle the row.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<resident_target_for_workers<p_alphabet>>
try_materialize_target_for_segment_workers(
    bioseq::storage::lazy_sequence_view<p_alphabet> target_view,
    std::uint8_t requested_width,
    std::uint64_t sequence_rowno) {
    const auto plan = bioseq::storage::plan_sequence_access(
        target_view,
        bioseq::storage::sequence_access_requirement::bounded_random_window,
        0u,
        target_view.bp_size(),
        requested_width);
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        plan.admission_bytes);
    if (!permit) {
        bioseq_log<logging_level::debug>(
            "[bioseq] resident target: row={} target_bp={} admitted_bytes={} "
            "not admitted; using scanner_pg serial fallback",
            static_cast<unsigned long long>(sequence_rowno),
            static_cast<unsigned long long>(target_view.bp_size()),
            static_cast<unsigned long long>(plan.admission_bytes));
        return std::nullopt;
    }

    const std::uint64_t materialize_started_ns = dispatch_host_now_ns();
    auto resident_owner = bioseq::storage::materialize_packed_sequence(
        target_view, plan.from, plan.to, requested_width, std::move(*permit));
    const std::uint64_t materialize_elapsed_ns = elapsed_dispatch_host_ns(materialize_started_ns);
    const auto& descriptor = resident_owner.descriptor();
    const std::uint64_t packed_bytes =
        static_cast<std::uint64_t>(descriptor.unit_count) * descriptor.storage_unit_width;

    bioseq_log<logging_level::debug>(
        "[bioseq] resident target: row={} target_bp={} packed_bytes={} "
        "materialize_ns={} materialized for segment workers",
        static_cast<unsigned long long>(sequence_rowno),
        static_cast<unsigned long long>(target_view.bp_size()),
        static_cast<unsigned long long>(packed_bytes),
        static_cast<unsigned long long>(materialize_elapsed_ns));

    return resident_target_for_workers<p_alphabet>{std::move(resident_owner)};
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
